#ifndef JIGDB_H
#define JIGDB_H

#include <time.h>
#include <limits.h>

#define JIGDB void

enum filetype
{
    FT_LOCAL = 0,  /* We have the file directly-accessible in a local filesystem */
    FT_ISO,        /* We have the file in an ISO image locally */
    FT_REMOTE,     /* File is on a specified mirror */
    FT_JIGDO       /* We need to go to the upstream jigdo mirror site */
};

typedef struct
{
    unsigned char      md5[32];
    enum filetype      type;
    time_t             mtime;
    time_t             age;  /* UINT_MAX - time when added */
    unsigned long long file_size;
    char               filename[PATH_MAX];
    char               extra[PATH_MAX];    /* empty for local files;
                                            * path to ISO for local ISO loopbacks;
                                            * base URL for remote files*/
} db_file_entry_t;    

JIGDB *db_open(char *db_name);
int db_close(JIGDB *dbp);
int db_store_file(JIGDB *dbp, db_file_entry_t *entry);
int db_lookup_file_by_age(JIGDB *dbp, time_t added, db_file_entry_t **out);
int db_lookup_file_older(JIGDB *dbp, db_file_entry_t **out);
int db_lookup_file_by_md5(JIGDB *dbp, char *md5, db_file_entry_t **out);
int db_lookup_file_by_name(JIGDB *dbp, char *filename, db_file_entry_t **out);
int db_delete_file(JIGDB *dbp, char *md5, enum filetype type, char *filename);
int db_dump(JIGDB *dbp);

#endif /* JIGDB_H */



