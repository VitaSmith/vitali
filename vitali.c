/*
  Vitali - Vita License database updater
  Copyright © 2017-2018 - VitaSmith

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#if !defined(__vita__)
#include <fcntl.h>
#if defined(_WIN32) || defined(__CYGWIN__)
#include <io.h>
#include <windows.h>
#endif
#else
#include <curl/curl.h>
#include <psp2/ctrl.h>
#include <psp2/sqlite.h>
#include <psp2/display.h>
#include <psp2/apputil.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/rtc.h> 
#include "console.h"
#endif

#include "sqlite3.h"
#include "zrif.h"
#include "puff.h"

#if defined(_WIN32)
#define msleep(msecs) Sleep(msecs)
static __inline uint64_t utime(void) {
    uint64_t ut;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ut = ft.dwHighDateTime;
    ut <<= 32;
    ut |= ft.dwLowDateTime;
    return (ut / 10) - 11644473600000000ULL;
}
#elif defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#define msleep(msecs) sceKernelDelayThread(1000*msecs)
static inline uint64_t utime(void) {
    SceRtcTick ut;
    sceRtcGetCurrentTick(&ut);
    return ut.tick;
}
#elif defined(__CYGWIN__) || defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#define msleep(msecs) usleep(1000*msecs)
static inline uint64_t utime(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000ULL + tv.tv_usec;
}
#endif

#define VERSION             "1.3"
#define MAX_QUERY_LENGTH    128
#define ZRIF_URI            "https://nopaystation.com/database/"
#define REFRESH_STEP        100000ULL

#if defined(__vita__)
#define ZRIF_TMP_PATH       "ux0:data/vitali.tmp"
#define LICENSE_DB_PATH     "ux0:license/license.db"
#define SHORTEN_SIZE        41
#undef  SEEK_SET
#undef  SEEK_CUR
#undef  SEEK_END
#define SEEK_SET            SCE_SEEK_SET
#define SEEK_CUR            SCE_SEEK_CUR
#define SEEK_END            SCE_SEEK_END
#define _open(path, flags)  sceIoOpen(path, flags, 0777)
#define _lseek              sceIoLseek
#define _read               sceIoRead
#define _close              sceIoClose
#define _O_RDONLY           SCE_O_RDONLY
#define _O_BINARY           0
#define perr(...)           printf(__VA_ARGS__)
#else
#define ZRIF_TMP_PATH       "vitali.tmp"
#define LICENSE_DB_PATH     "license.db"
#define SHORTEN_SIZE        62
#define perr(...)           fprintf(stderr, __VA_ARGS__)
#if defined(_WIN32) || defined(__CYGWIN__)
#define USE_VBSCRIPT_DOWNLOAD true
#else
#define USE_VBSCRIPT_DOWNLOAD false
#define _open               open
#define _lseek              lseek
#define _read               read
#define _close              close
#define _O_RDONLY           O_RDONLY
#define _O_BINARY           0
#endif
#endif

#define safe_close(fd)      if (fd > 0) { _close(fd); fd = 0; }

static const char* zrif_charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/+=";
static const char* schema =             \
    "CREATE TABLE Licenses ("           \
    "CONTENT_ID TEXT NOT NULL UNIQUE,"  \
    "RIF BLOB NOT NULL,"                \
    "PRIMARY KEY(CONTENT_ID)"           \
    ")";
#if !defined(__vita__)
static const char vbs[] = \
    "Set xHttp = createobject(\"Microsoft.XMLHTTP\")\n" \
    "Set bStrm = createobject(\"Adodb.Stream\")\n" \
    "Call xHttp.Open(\"GET\", WScript.Arguments(0), False)\n" \
    "Call xHttp.SetRequestHeader(\"If-None-Match\", \"some-random-string\")\n" \
    "Call xHttp.SetRequestHeader(\"Cache-Control\", \"no-cache,max-age=0\")\n" \
    "Call xHttp.SetRequestHeader(\"Pragma\", \"no-cache\")\n" \
    "Call xHttp.Send()\n" \
    "If Not xHttp.Status = 200 Then\n" \
    "  Call WScript.Quit(xHttp.Status)\n" \
    "End If\n" \
    "With bStrm\n" \
    "  .type = 1\n" \
    "  .open\n" \
    "  .write xHttp.responseBody\n" \
    "  .savetofile WScript.Arguments(1), 2\n" \
    "End With\n";
#endif

static bool separate_console()
{
#if defined(_WIN32) || defined(__CYGWIN__)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return 0;
    return ((!csbi.dwCursorPosition.X) && (!csbi.dwCursorPosition.Y));
#elif defined(__vita__)
    return true;
#else
    return false;
#endif
}

/* Custom shortening of URIs */
static char* shorten_uri(const char* uri, size_t max_size)
{
    size_t i, j;
    static char str[128];
    if ((max_size > strlen(uri)) || (max_size > sizeof(str) - 1))
        return (char*)uri;
    /* Shorten after the 3rd slash */
    for (i = 0, j = 0; (i < strlen(uri)) && (j < 3); i++) {
        if (uri[i] == '/')
            j++;
    }
    if ((i >= strlen(uri)) || (i >= max_size))
        return (char*)uri;
    strncpy(str, uri, i);
    strcat(str, "...");
    strcat(str, &uri[strlen(uri) - max_size + strlen(str)]);
    return str;
}

static char* unzip_xlsx(const char* in_buf, long in_size, long* out_size)
{
    const char* shared_strings = "xl/sharedStrings.xml";
    size_t shared_strings_len = strlen(shared_strings);
    char *pos, *out_buf = NULL;
    uint8_t* p;
    size_t compressed_size = 0, uncompressed_size = 0;

    pos = (char*) in_buf;
    /* Need to lookup the end table to get the filesize, since Microsoft decided
       to annoy everyone by removing them from the local table. WTF?!? */
    while ((pos = memchr(pos, 'P', in_size - ((intptr_t)pos - (intptr_t)in_buf))) != NULL) {
        if ((pos[1] != 'K') || (pos[2] != 0x01) || (pos[3] != 0x02)) {
            pos++;
            continue;
        }
        if ((((uint16_t*)pos)[14] != shared_strings_len) || (strncmp(&pos[0x2E], shared_strings, shared_strings_len) != 0)) {
            pos += 0x24;
            continue;
        }
        /* Vita doesn't seem to like casting to (uint32_t*) */
        p = (uint8_t*)&pos[20];
        compressed_size = p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24);
        p = (uint8_t*)&pos[24];
        uncompressed_size = p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24);
        break;
    }
    if (compressed_size == 0) {
        perr("Could not find '%s' in XLSX file\n", shared_strings);
        goto out;
    }

    /* Now that we have the sizes, we can process the compressed file */
    pos = (char*) in_buf;
    while ((pos = memchr(pos, 'P', in_size - ((intptr_t)pos - (intptr_t)in_buf))) != NULL) {
        if ((pos[1] != 'K') || (pos[2] != 0x03) || (pos[3] != 0x04)) {
            pos++;
            continue;
        }
        if ((((uint16_t*)pos)[13] != shared_strings_len) || (strncmp(&pos[0x1E], shared_strings, shared_strings_len) != 0)) {
            pos += 0x1e;
            continue;
        }
        pos += 0x1e + shared_strings_len;
        out_buf = malloc(uncompressed_size);
        if (out_buf == NULL) {
            perr("Could not allocate xlsx decompression buffer\n");
            goto out;
        }
        if (puff(0, (uint8_t*)out_buf, &uncompressed_size, (uint8_t*)pos, &compressed_size) != 0)
            perr("Could not decompress '%s' in XLSX file\n", shared_strings);
        goto out;
    }

out:
    *out_size = (long)((out_buf == NULL) ? 0 : uncompressed_size);
    return out_buf;
}

#if defined(__vita__)
static char* size_to_human_readable(uint64_t size)
{
    const char *suffix_table[] = { "KB", "MB", "GB", "PB" };
    static char str_size[32];
    double hr_size = (double)size;
    const double divider = 1024.0;
    int suffix;

    for (suffix = 0; suffix < 3; suffix++) {
        if (hr_size < divider)
            break;
        hr_size /= divider;
    }
    if (suffix == 0) {
        sprintf(str_size, "%d bytes", (int)hr_size);
    } else {
        sprintf(str_size, "%0.2f %s", hr_size, suffix_table[suffix - 1]);
    }
    return str_size;
}

static void http_init()
{
    const int size = 4 * 1024 * 1024;
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
    SceNetInitParam netInitParam;
    netInitParam.memory = malloc(size);
    netInitParam.size = size;
    netInitParam.flags = 0;
    sceNetInit(&netInitParam);
    sceNetCtlInit();
    sceHttpInit(size);
}

static void http_exit()
{
    sceHttpTerm();
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTP);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

static int curl_progress_function(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    static uint64_t last_tick = 0ULL;
    uint64_t cur_tick;

    /* Ensure that at least 200 ms have elapsed since last progress */
    cur_tick = utime();
    if (cur_tick - last_tick < REFRESH_STEP)
        return 0;
    last_tick = cur_tick;
    printf("\rDownloaded %s", size_to_human_readable(dlnow));
    return 0;
}

static size_t curl_write_function(void *ptr, size_t size, size_t nmemb, void *stream)
{
    int written = sceIoWrite(*(int*)stream, ptr, size * nmemb);
    if (written < 0) {
        perr("Error writing file: 0x%08X\n", (uint32_t)written);
        return 0;
    }
    return (size_t)written;
}

static bool download_file(const char *url, const char *dest)
{
    int fd = 0, http_status = 0;
    CURL *curl = NULL;
    CURLcode r = CURLE_RECV_ERROR;

    http_init();

    printf("Downloading '%s'...\n", shorten_uri(url, SHORTEN_SIZE));

    curl = curl_easy_init();
    if (curl == NULL) {
        perr("Could not open initialize download\n");
        goto out;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);

    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Vitali/" VERSION " (libcur/" LIBCURL_VERSION ")");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    /* We need TLS 1.2 support for nopaystation.com */
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_function);
    /* Set Curl to display some progress */
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_function);
    fd = sceIoOpen(dest, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd < 0) {
        perr("Could not open file '%s'\n", dest);
        goto out;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fd);

    r = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    if (r != CURLE_OK) {
        perr("Could not download file: %s\n", curl_easy_strerror(r));
        goto out;
    }
    printf("\n");

out:
    if (fd > 0)
        sceIoClose(fd);
    if (curl != NULL)
        curl_easy_cleanup(curl);
    http_exit();
    return (r == CURLE_OK);
}
#else
static bool download_file(const char* url, const char* file)
{
    bool use_vbscript = USE_VBSCRIPT_DOWNLOAD;
    char *vbs_tmp = "download.vbs";
    char cmd[1024];

    if (use_vbscript) {
        FILE *vbs_fd = fopen(vbs_tmp, "w");
        if (vbs_fd != NULL) {
            fwrite(vbs, 1, sizeof(vbs) - 1, vbs_fd);
            fclose(vbs_fd);
        }
    }

    printf("Downloading '%s'...\n", shorten_uri(url, SHORTEN_SIZE));

    fflush(stdout);
    if (use_vbscript)
        snprintf(cmd, sizeof(cmd), "cscript //nologo %s %s %s", vbs_tmp, url, file);
    else
        snprintf(cmd, sizeof(cmd), "curl %s -o %s || wget %s -O %s || exit 400",
            url, file, url, file);
    int sys_ret = system(cmd);
    if (sys_ret != 0)
        printf("Cannot download file - Error %d\n", sys_ret);
    if (use_vbscript)
        remove(vbs_tmp);
    return (sys_ret == 0);
}
#endif

int main(int argc, char** argv)
{
    int ret = 1, rc, processed = 0, added = 0, duplicate = 0, failed = 0;
    int fd = 0, rsize;
    long size;
    bool is_url, initialize_db = false, needs_keypress = separate_console();
    char *db_path = LICENSE_DB_PATH;
    char *zrif_tmp = ZRIF_TMP_PATH;
    char *zrif_uri = ZRIF_URI;
    char *content_id, *errmsg = NULL;
    char *buf = NULL, *zrif = NULL;
    char query[MAX_QUERY_LENGTH];
    uint8_t rif[1024];
    uint64_t last_tick = 0, cur_tick;
    size_t rif_len, zrif_len;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt;

#if defined(__vita__)
    SceCtrlData pad;
    init_video();
    console_init();
    sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);
    sqlite3_rw_init();
#endif

    printf("Vitali v" VERSION " - Vita License database updater\n");
    printf("Copyright (c) 2017-2018 VitaSmith (GPLv3)\n\n");

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "--version") == 0)) {
            printf("\nVisit https://github.com/VitaSmith/vitali for the source\n");
            goto out;
        }
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            printf("\nUsage: vitali [ZRIF_URI] [DB_FILE]\n");
            goto out;
        }
        if (i == 1)
            zrif_uri = argv[i];
        else if (i == 2)
            db_path = argv[i];
    }


retry:
    is_url = (strncmp(zrif_uri, "http", 4) == 0);
    if (is_url) {
        if (download_file(zrif_uri, zrif_tmp))
            zrif_uri = zrif_tmp;
        else
            goto out;
    }

    fd = _open(zrif_uri, _O_RDONLY | _O_BINARY);
    if (fd <= 0) {
        perr("Cannot open file '%s'\n", zrif_uri);
        goto out;
    }

    size = _lseek(fd, 0, SEEK_END);
    if (size < 16) {
        perr("Size of '%s' is too small\n", zrif_uri);
        goto out;
    }

    _lseek(fd, 0, SEEK_SET);

    free(buf);
    /* Allow some extra space in case the redirect URL is at the very end of our buffer */
    buf = malloc(size + 16);
    if (buf == NULL) {
        perr("Cannot allocate buffer\n");
        goto out;
    }
    rsize = _read(fd, buf, size);
    if (rsize != size) {
        perr("Cannot read from '%s'\n", zrif_uri);
        goto out;
    }
    buf[size] = 0;
    buf[size + 1] = 0;

    if ((buf[0] == 'P') && (buf[1] == 'K')) {
        /* Assume that we are dealing with a .xlsx file */
        printf("Parsing XLSX file...\n");
        char* new_buf = unzip_xlsx(buf, size, &size);
        if (new_buf == NULL)
            goto out;
        free(buf);
        buf = new_buf;
    } else if (strstr(buf, "<title>Too Many Requests</title>") != NULL) {
        /* Google spreadsheet may return a "Too Many Requests page */
        safe_close(fd);
        remove(zrif_tmp);
        printf("Too many requests - Retrying in 5 seconds...\n");
        msleep(5000);
        goto retry;
    } else {
        char* p = strstr(buf, "https://docs.google.com/spreadsheets");
        if (p != NULL) {
            zrif_uri = p;
            p = strstr(p, "/edit'");
        }
        if (p != NULL) {
            safe_close(fd);
            remove(zrif_tmp);
            strcpy(p, "/export?format=xlsx");
            goto retry;
        }
    }
    safe_close(fd);

    fd = _open(db_path, _O_RDONLY);
    if (fd > 0) {
        if (_lseek(fd, 0, SEEK_END) == 0) {
            printf("Removing stale database '%s'...\n", db_path);
            safe_close(fd);
            remove(db_path);
            initialize_db = true;
        } else {
            safe_close(fd);
        }
    } else {
        initialize_db = true;
    }

    rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        perr("Cannot open database '%s'\n", db_path);
        goto out;
    }

    if (initialize_db) {
        rc = sqlite3_exec(db, schema, NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            perr("Cannot set database schema\n");
            goto out;
        }
    }

    rc = sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        perr("Cannot create transaction: %s\n", errmsg);
        goto out;
    }

    zrif = buf;
    while ((zrif = memchr(zrif, 'K', size - ((intptr_t)zrif - (intptr_t)buf) + 2)) != NULL) {
        if ((zrif[1] != 'O') || (zrif[2] != '5') || (zrif[3] != 'i')) {
            zrif++;
            continue;
        }
        processed++;
        cur_tick = utime();
        if (cur_tick - last_tick >= REFRESH_STEP) {
            last_tick = cur_tick;
            printf("\rProcessed %d licenses", processed);
        }
        zrif_len = strspn(zrif, zrif_charset);
        zrif[zrif_len] = 0;
        rif_len = decode_zrif(zrif, rif, sizeof(rif));
        if (rif_len != 0) {
            /* PSM and regular RIFs have CONTENT_ID at different offsets */
            content_id = (char*)&rif[(((uint64_t*)rif)[0] == 0ULL) ? 0x50 : 0x10];
            snprintf(query, sizeof(query), "INSERT INTO Licenses VALUES('%s', ?)", content_id);
            if (((rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL)) != SQLITE_OK)
                || ((rc = sqlite3_bind_blob(stmt, 1, rif, (int)rif_len, SQLITE_STATIC)) != SQLITE_OK)
                || ((rc = sqlite3_step(stmt)) != SQLITE_DONE)
                || ((rc = sqlite3_finalize(stmt)) != SQLITE_OK)) {
                if (rc == SQLITE_CONSTRAINT) {
                    duplicate++;
                } else {
                    perr("\nCannot add %s from zRIF %s: %s\n", content_id, zrif, sqlite3_errmsg(db));
                    failed++;
                }
            } else {
                added++;
            }
        } else {
#if !defined(__vita__)
            perr("\nCannot decode zRIF: %s\n", zrif);
#endif
            failed++;
        }
        zrif += zrif_len + 1;
    }

    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        perr("\nCannot commit transaction: %s\n", errmsg);
        goto out;
    }

    printf("\rProcessed %d licenses:\n %d added, %d duplicate(s), %d failed.\n", processed, added, duplicate, failed);
    printf("Database '%s' was successfully %s.\n", db_path, initialize_db ? "created" : "updated");
    ret = 0;

out:
    remove(zrif_tmp);
    if (errmsg != NULL)
        sqlite3_free(errmsg);
    sqlite3_close(db);
    free(buf);
    safe_close(fd);

#if defined(__vita__)
    sqlite3_rw_exit();
    console_set_color(CYAN);
    if (needs_keypress) {
        printf("\nPress X to exit.\n");
        do {
            sceCtrlPeekBufferPositive(0, &pad, 1);
        } while (!(pad.buttons & SCE_CTRL_CROSS));
    }
    console_exit();
    end_video();
    sceKernelExitProcess(0);
#else
    if (needs_keypress) {
        printf("\nPress any key to exit...\n");
        fflush(stdout);
        getchar();
    }
#endif
    return ret;
}
