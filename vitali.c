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
#include <io.h>
#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#endif
#else
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
#include "console.h"
#endif

#include "sqlite3.h"
#include "zrif.h"
#include "puff.h"

#define VERSION "1.3"
#define MAX_QUERY_LENGTH 128
#if defined(__vita__)
#define ZRIF_URI "ux0:data/sharedStrings.xml"
#else
#define ZRIF_URI "https://nopaystation.com/database"
#endif

#if defined(__vita__)
#define ZRIF_TMP_PATH       "ux0:data/zrif.tmp"
#define LICENSE_DB_PATH     "ux0:data/license.db"
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
#define ZRIF_TMP_PATH       "zrif.tmp"
#define LICENSE_DB_PATH     "license.db"
#define perr(...)           fprintf(stderr, __VA_ARGS__)
#if defined(_WIN32) || defined(__CYGWIN__)
#define USE_VBSCRIPT_DOWNLOAD true
#else
#define USE_VBSCRIPT_DOWNLOAD false
#endif
#endif

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

static char* unzip_xlsx(const char* in_buf, long in_size, long* out_size)
{
    const char* shared_strings = "xl/sharedStrings.xml";
    size_t shared_strings_len = strlen(shared_strings);
    char *pos, *out_buf = NULL;
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
        compressed_size = (size_t) ((uint32_t*)pos)[5];
        uncompressed_size = (size_t) ((uint32_t*)pos)[6];
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
void http_init()
{
    const int size = 1 * 1024 * 1024;
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam netInitParam;
    netInitParam.memory = malloc(size);
    netInitParam.size = size;
    netInitParam.flags = 0;
    sceNetInit(&netInitParam);
    sceNetCtlInit();
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
    sceHttpInit(size);
}

void http_exit()
{
    sceHttpTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTP);
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

static bool download_file(const char *url, const char *dest)
{
    bool ret = false;
    unsigned char buffer[16 * 1024];
    int template = 0, connection = 0, request = 0, handle = 0, fd = 0, read, written;

    printf("Downloading '%s'...\n", url);
    http_init();

    printf("sceHttpCreateTemplate()...\n");
    template = sceHttpCreateTemplate("PS Vita Vitali App", 1, 1);
    if (template < 0) {
        perr("sceHttpCreateTemplate() error: 0x%08X\n", (uint32_t)template);
        goto out;
    }

    printf("sceHttpCreateConnectionWithURL()...\n");
    connection = sceHttpCreateConnectionWithURL(template, url, 0);
    if (connection < 0) {
        perr("sceHttpCreateConnectionWithURL() error: 0x%08X\n", (uint32_t)connection);
        goto out;
    }

    printf("sceHttpCreateRequestWithURL()...\n");
    request = sceHttpCreateRequestWithURL(connection, SCE_HTTP_METHOD_GET, url, 0);
    if (request < 0) {
        perr("sceHttpCreateRequestWithURL() error: 0x%08X\n", (uint32_t)request);
        goto out;
    }

    printf("sceHttpSendRequest()...\n");
    handle = sceHttpSendRequest(request, NULL, 0);
    if (handle < 0) {
        perr("sceHttpSendRequest() error: 0x%08X\n", (uint32_t)handle);
        goto out;
    }

    fd = sceIoOpen(dest, SCE_O_WRONLY | SCE_O_CREAT, 0777);
    if (fd < 0) {
        perr("Could not open file '%s'\n", dest);
        goto out;
    }

    printf("sceHttpReadData()...\n");
    while ((read = sceHttpReadData(request, &buffer, sizeof(buffer))) > 0) {
        written = sceIoWrite(fd, buffer, read);
        if (written <= 0) {
            perr("Error writing file: 0x%08X\n", (uint32_t)written);
            goto out;
        }
    }
    if (read < 0) {
        perr("Error downloading file: 0x%08X\n", (uint32_t)read);
        goto out;
    }
    ret = true;

out:
    if (request > 0)
        sceHttpDeleteRequest(request);
    if (connection > 0)
        sceHttpDeleteConnection(connection);
    if (template > 0)
        sceHttpDeleteTemplate(template);
    if (fd >0)
        sceIoClose(fd);
    http_exit();
    return ret;
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
    printf("Downloading '%s'...\n", url);
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

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "--version") == 0)) {
            printf("Vitali - Vita License database updater, v" VERSION "\n");
            printf("Copyright(c) 2017-2018 - VitaSmith\n");
            printf("Visit https://github.com/VitaSmith/vitali for license details and source\n");
            goto out;
        }
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            printf("Usage: vitali [ZRIF_URI] [DB_FILE]\n");
            goto out;
        }
        if (i == 1)
            zrif_uri = argv[i];
        else if (i == 2)
            db_path = argv[i];
    }

redirect:
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
    // Allow some extra space in case the redirect URL is at the very end of our buffer
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
        char* new_buf = unzip_xlsx(buf, size, &size);
        if (new_buf == NULL)
            goto out;
        free(buf);
        buf = new_buf;
    } else {
        char* p = strstr(buf, "https://docs.google.com/spreadsheets");
        if (p != NULL) {
            zrif_uri = p;
            p = strstr(p, "/edit'");
        }
        if (p != NULL) {
            _close(fd);
            remove(zrif_tmp);
            strcpy(p, "/export?format=xlsx");
            goto redirect;
        }
    }
    _close(fd);
    if (is_url)
        printf("Download complete.\n");

    fd = _open(db_path, _O_RDONLY);
    if (fd > 0) {
        if (_lseek(fd, 0, SEEK_END) == 0) {
            printf("Removing stale database '%s'...\n", db_path);
            _close(fd);
            remove(db_path);
            initialize_db = true;
        } else {
            _close(fd);
        }
        fd = 0;
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
                    perr("Cannot add %s from zRIF %s: %s\n", content_id, zrif, sqlite3_errmsg(db));
                    failed++;
                }
            } else {
                added++;
            }
        } else {
            perr("Cannot decode zRIF: %s\n", zrif);
            failed++;
        }
        zrif += zrif_len + 1;
    }

    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        perr("Cannot commit transaction: %s\n", errmsg);
        goto out;
    }

    printf("\nProcessed %d license(s): %d added, %d duplicate(s), %d failed.\n", processed, added, duplicate, failed);
    printf("Database '%s' was successfully %s.\n", db_path, initialize_db ? "created" : "updated");
    ret = 0;

out:
    remove(zrif_tmp);
    if (errmsg != NULL)
        sqlite3_free(errmsg);
    sqlite3_close(db);
    free(buf);
    if (fd > 0)
        _close(fd);

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
