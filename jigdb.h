#ifndef JIGDB_H
#define JIGDB_H

#include <time.h>
#include <limits.h>

#define JIGDB void

/* Details about the blocks in the template file; each points to a
 * matched file, or a compressed lump of unmatched data in the
 * template file */
enum blocktype
{
    BT_FILE = 0,   /* md5 pointer to a file */
    BT_BLOCK       /* pointer to a block of data from the template file */
};

typedef struct
{
    unsigned long long image_offset;            /* Start offset within the image */
    unsigned long long size;                    /* Size of the lump */
    unsigned long long uncomp_offset;           /* Offset within the compressed template
                                                   data iff type is BT_BLOCK */
    enum blocktype     type;
    unsigned char      template_name[PATH_MAX]; /* The template we're caching */
    unsigned char      md5[32];                 /* MD5 iff type is BT_FILE */
} db_template_entry_t;

/* Details about files we know about: local mirror, files in a local
 * ISO image, files listed in the jigdo, etc. */
enum filetype
{
    FT_LOCAL = 0,  /* We have the file directly-accessible in a local filesystem */
    FT_ISO,        /* We have the file in an ISO image locally */
    FT_REMOTE,     /* File is on a specified mirror */
    FT_JIGDO       /* We need to go to the upstream jigdo mirror site */
};

typedef struct
{
    unsigned long long file_size;          /* size of the file in bytes */
    time_t             mtime;              /* mtime of the file when we saw it */
    time_t             age;                /* UINT_MAX - time when added */
    enum filetype      type;
    unsigned char      md5[32];            /* md5sum of the file */
    char               filename[PATH_MAX]; /* path to the file */
    char               extra[PATH_MAX];    /* empty for local files;
                                            * path to ISO for local ISO loopbacks;
                                            * base URL for remote files*/
} db_file_entry_t;

/* Details about the compressed blocks of unmatched data within the
 * template file. The data is stored within the template file as a
 * series of compressed blocks, but we have no way to seek to the
 * right compressed block; if we want to read from the middle of the
 * image we have to decompress each in turn. Cache the details of each
 * block so we can work from an uncompressed offset directly to the
 * right compressed block. */
typedef struct
{
    unsigned long      comp_offset;             /* Start offset of this compressed block,
                                                   measured from the beginning of the
                                                   template file */
    unsigned long      uncomp_offset;           /* Start offset when uncompressed */
    unsigned long      uncomp_size;             /* Size of uncompressed block */
    unsigned char      template_name[PATH_MAX]; /* The template we're caching */
} db_compressed_entry_t;

/*******************
 *
 * Common interfaces
 *
 ******************/

/* Open/create the database */
JIGDB *db_open(char *db_name);

/* Close the database */
int db_close(JIGDB *dbp);

/* Delete ALL the template and compressed block entries for a
 * specified template file */
int db_delete_template_cache(JIGDB *dbp, char *template_name);

/* Dump the contents of the DB for debug */
int db_dump(JIGDB *dbp);

/********************
 *
 * Template functions
 *
 *******************/

/* Store a template match / non-match block entry */
int db_store_template(JIGDB *dbp, db_template_entry_t *entry);

/* Lookup a template entry by output offset. The specified offset will
 * be within the range covered by the returned entry, or ENOENT. */
int db_lookup_template_by_offset(JIGDB *dbp, char *template_name,
                                 unsigned long long image_offset, db_template_entry_t **out);

/****************
 *
 * File functions
 *
 ***************/

/* Store details of a file */
int db_store_file(JIGDB *dbp, db_file_entry_t *entry);

/* Lookup files added older than a specified age */
int db_lookup_file_by_age(JIGDB *dbp, time_t added, db_file_entry_t **out);

/* Lookup the next older file */
int db_lookup_file_older(JIGDB *dbp, db_file_entry_t **out);

/* Lookup a file by md5 */
int db_lookup_file_by_md5(JIGDB *dbp, char *md5, db_file_entry_t **out);

/* Lookup a file by name */
int db_lookup_file_by_name(JIGDB *dbp, char *filename, db_file_entry_t **out);

/* Delete a file entry */
int db_delete_file(JIGDB *dbp, char *md5, enum filetype type, char *filename);

/****************************
 *
 * Compressed block functions
 *
 ***************************/

/* Store details of a block */
int db_store_block(JIGDB *dbp, db_compressed_entry_t *entry);

/* Lookup a block by its UNCOMPRESSED offset */
int db_lookup_block_by_offset(JIGDB *dbp, char *template_name,
                              unsigned long uncomp_offset, db_compressed_entry_t **out);

#endif /* JIGDB_H */
