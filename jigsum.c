/*
 * jigsum
 *
 * Tool to calculate and print MD5 checksums in jigdo's awkward
 * base64-ish encoding.
 *
 * Copyright (c) 2004 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "md5.h"
#include "jigdb.h"

#define BUF_SIZE 65536
#define OLD_FILE_RECORD_AGE 86400
#define DB_NAME "jigit_db.sql"

#ifndef MIN
#define MIN(x,y)        ( ((x) < (y)) ? (x) : (y))
#endif

JIGDB *database = NULL;

static int check_cache(char *filename, struct stat *sb, char *base64_md5)
{
    int error = 0;
    db_file_entry_t *entry;

    error = db_lookup_file_by_name(database, filename, &entry);
    if (!error)
    {
        if ( (sb->st_mtime <= entry->mtime) &&
             (sb->st_size == entry->file_size) )
            /* We have a cache entry already; simply return
             * the cached sum */
        {
            strcpy(base64_md5, entry->md5);
            return 1;
        }
        else
        {
            /* We have an entry for this file, but the mtime or size
             * has changed. Delete the old entry and replace it later
             * on */
            error = db_delete_file_by_name(database, entry->md5, entry->type, entry->filename);
            if (error)
                printf("check_cache: unable to delete old entry for file %s\n", filename);
        }
    }
    return 0;
}

static unsigned long long calculate_md5(char *filename, FILE *file, char *base64_md5)
{
    char buf[BUF_SIZE];
    unsigned char file_md5[16] = {0};
    int done = 0;
    struct mk_MD5Context file_context;
    unsigned long long bytes_read = 0;
    char *tmp_md5 = NULL;
    
    mk_MD5Init(&file_context);
    while (!done)
    {
        int used = 0;
        memset(buf, 0, BUF_SIZE);

        used = fread(buf, 1, BUF_SIZE, file);
        bytes_read += used;
        if (used)
            mk_MD5Update(&file_context, (unsigned char *)buf, used);
        else
        {
            if (ferror(file) && (EISDIR != errno))
                fprintf(stderr, "Unable to read from file %s; error %d\n",
                        filename, errno);
            break;
        }
    }    
    mk_MD5Final(file_md5, &file_context);
    tmp_md5 = base64_dump(file_md5, 16);
    strcpy(base64_md5, tmp_md5);
    free(tmp_md5);
    return bytes_read;
}

static int md5_file(char *filename)
{
    FILE *file = NULL;
    char base64_md5[33] = {0};
    unsigned long long bytes_read = 0;
    db_file_entry_t entry;
    struct stat sb;
    int found_in_db = 0;
    int error = 0;
    char buf[PATH_MAX];
    char *fullpath = NULL;

    /* Check if we're reading from stdin */
    if (!strcmp("-", filename))
    {
        (void)calculate_md5("<STDIN>", stdin, base64_md5);
        printf("%s\n", base64_md5);
        fflush(stdout);
        return 0;
    }

    /* Make an absolute pathname if necessary */
    if (filename[0] == '/')
        fullpath = filename;
    else
    {
        size_t wdlen = 0;
        if (buf != getcwd(buf, sizeof(buf)))
        {
            fprintf(stderr, "md5_file: Unable to get CWD!; giving up on file %s, error %d\n",
                    filename, errno);
            return errno;
        }
        wdlen = strlen(buf);
        strcpy(buf + wdlen, "/");
        strcpy(buf + wdlen + 1, filename);
        fullpath = buf;
    }

    /* Check the DB to see if we already have a checksum for this file */
    error = stat(fullpath, &sb);
    if (error)
    {
        fprintf(stderr, "md5_file: Unable to stat file %s, error %d\n", fullpath, errno);
        return errno;
    }
    if (S_ISDIR(sb.st_mode))
        return EISDIR;
    found_in_db = check_cache(fullpath, &sb, base64_md5);
    if (!found_in_db)
    {
        file = fopen(fullpath, "rb");
        if (!file)
        {
            switch (errno)
            {
                case EACCES:
                case EISDIR:
                    break;
                default:
                    fprintf(stderr, "Unable to open file %s; error %d\n", fullpath, errno);
                    break;
            }
            return errno;
        }
        bytes_read = calculate_md5(fullpath, file, base64_md5);
        fclose(file);
        memset(&entry, 0, sizeof(entry));
        strncpy(&entry.md5[0], base64_md5, sizeof(entry.md5));
        entry.type = FT_LOCAL;
        entry.mtime = sb.st_mtime;
        entry.time_added = time(NULL);
        entry.file_size = bytes_read;
        strncpy(&entry.filename[0], fullpath, sizeof(entry.filename));
        /* "extra" blanked already */
        error = db_store_file(database, &entry);
        if (error)
            fprintf(stderr, "Unable to write database entry; error %d\n", error);
    }

    printf("%s  %s\n", base64_md5, fullpath);
    fflush(stdout);
    return 0;
}

/* Walk through the database deleting entries more than <n> seconds old */
static void jigsum_db_cleanup(int delay)
{
    (void)db_delete_files_by_age(database, time(NULL) - delay);
}

int main(int argc, char **argv)
{
    int i = 0;

    database = db_open(DB_NAME);
    if (!database)
    {
        fprintf(stderr, "Unable to open database, error %d\n", errno);
        return errno;
    }                

    /* Clear out old records */
    jigsum_db_cleanup(OLD_FILE_RECORD_AGE);
    
    for (i = 1; i < argc; i++)
        (void) md5_file(argv[i]);

    db_dump(database);

    db_close(database);

    return 0;
}

