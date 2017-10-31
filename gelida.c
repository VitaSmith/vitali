/*
  Gelida - GEnerate LIcense DAtabase for PS Vita
  Copyright © 2017 - VitaSmith

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
#include <inttypes.h>
#include <string.h>

#include "sqlite3.h"
#include "zrif.h"
#include "puff.h"

#define VERSION "1.1"
#define MAX_QUERY_LENGTH 128

static const char* zrif_charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/+=";

static int create_db(const char* db_path)
{
    int rc;
    sqlite3 *db = NULL;
    const char* schema = "CREATE TABLE Licenses (" \
        "CONTENT_ID TEXT NOT NULL UNIQUE," \
        "RIF BLOB NOT NULL," \
        "PRIMARY KEY(CONTENT_ID)" \
        ")";

    rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK)
        goto out;
    rc = sqlite3_exec(db, schema, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        goto out;

out:
    sqlite3_close(db);
    return rc;
}

char* unzip_xlsx(const char* in_buf, size_t in_size, size_t* out_size)
{
    const char* shared_strings = "xl/sharedStrings.xml";
    size_t shared_strings_len = strlen(shared_strings);
    FILE *fd = NULL;
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
        fprintf(stderr, "Could not find '%s' in XLSX file\n", shared_strings);
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
            fprintf(stderr, "Could not allocate xlsx decompression buffer\n");
            goto out;
        }
        if (puff(0, (uint8_t*)out_buf, &uncompressed_size, (uint8_t*)pos, &compressed_size) != 0)
            fprintf(stderr, "Could not decompress '%s' in XLSX file\n", shared_strings);
        goto out;
    }

out:
    if (fd != NULL)
        fclose(fd);
    *out_size = (out_buf == NULL) ? 0 : uncompressed_size;
    return out_buf;
}

int main(int argc, char** argv)
{
    int ret = 1, rc, processed = 0, added = 0, duplicate = 0, failed = 0;
    char *db_path = "license.db";
    char *content_id, *errmsg = NULL;
    char *buf = NULL, *zrif = NULL;
    char query[MAX_QUERY_LENGTH];
    uint8_t rif[1024];
    size_t rif_len, zrif_len;
    FILE *fd = NULL;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt;

    if (argc < 2) {
        fprintf(stderr, "Usage: gelida ZRIF_FILE [DB_FILE]\n");
        goto out;
    }

    if ((strcmp(argv[1], "-v") == 0) || (strcmp(argv[1], "--version") == 0)) {
        printf("gelida v" VERSION "\n");
        printf("Copyright(c) 2017 - VitaSmith\n");
        printf("Visit https://github.com/VitaSmith/gelida for license details and source\n");
        goto out;
    }

    if (argc > 2)
        db_path = argv[2];

    fd = fopen(argv[1], "rb");
    if (fd == NULL) {
        fprintf(stderr, "Cannot open file '%s'\n", argv[2]);
        goto out;
    }

    fseek(fd, 0L, SEEK_END);
    size_t size = (size_t) ftell(fd);
    if (size < 16) {
        fprintf(stderr, "Size of '%s' is too small\n", argv[2]);
        goto out;
    }

    fseek(fd, 0L, SEEK_SET);

    buf = malloc(size + 2);
    if (buf == NULL) {
        fprintf(stderr, "Cannot allocate buffer\n");
        goto out;
    }
    size_t read = fread(buf, 1, size, fd);
    if (read != size) {
        fprintf(stderr, "Cannot read from '%s'\n", argv[2]);
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
    }

    fclose(fd);
    fd = fopen(db_path, "rb");
    if (fd != NULL) {
        fclose(fd);
        fd = NULL;
    } else if (create_db(db_path) != SQLITE_OK) {
        fprintf(stderr, "Could not create database '%s'\n", db_path);
        goto out;
    }

    rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL);
    if (fd != SQLITE_OK) {
        fprintf(stderr, "Could not open database '%s'\n", db_path);
        goto out;
    }
    rc = sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Could create transaction: %s\n", errmsg);
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
                    fprintf(stderr, "Could not add %s from zRIF %s: %s\n", content_id, zrif, sqlite3_errmsg(db));
                    failed++;
                }
            } else {
                added++;
            }
        } else {
            fprintf(stderr, "Could not decode zRIF: %s\n", zrif);
            failed++;
        }
        zrif += zrif_len + 1;
    }

    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Could commit transaction: %s\n", errmsg);
        goto out;
    }

    printf("Processed %d license(s): %d added, %d duplicate(s), %d failed\n", processed, added, duplicate, failed);
    ret = 0;

out:
    if (errmsg != NULL)
        sqlite3_free(errmsg);
    sqlite3_close(db);
    free(buf);
    if (fd != NULL)
        fclose(fd);
    return ret;
}
