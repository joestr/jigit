#ifndef JIGDB_H
#define JIGDB_H

#include <time.h>
#include <limits.h>

typedef enum
{
    CT_GZIP,
    CT_BZIP2
} ctype_e;

#define JIGDB void

/* Top-level details about a template file */
typedef struct
{
    unsigned long long template_size;           /* The size of the template file */
    unsigned long long image_size;              /* The size of the output image */
    time_t             template_mtime;          /* The mtime of the template file */
    unsigned char      template_name[PATH_MAX]; /* The template file name */
    unsigned char      template_md5[33];        /* MD5 of the template file */
    unsigned char      image_md5[33];           /* MD5 of the image file */
} db_template_entry_t;

/* Details about the blocks in the template file; each points to a
 * matched file, or a compressed lump of unmatched data in the
 * template file */
enum blocktype
{
    BT_FILE = 6,   /* md5 pointer to a file */
    BT_BLOCK = 2   /* pointer to a block of data from the template file */
};

typedef struct
{
    unsigned long long image_offset;            /* Start offset within the image */
    unsigned long long size;                    /* Size of the lump */
    unsigned long long uncomp_offset;           /* Offset within the compressed template
                                                   data iff type is BT_BLOCK */
    unsigned char      template_id[33];         /* MD5 of the template */
    enum blocktype     type;
    unsigned char      md5[33];                 /* MD5 iff type is BT_FILE */
} db_block_entry_t;

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
    time_t             time_added;         /* time when this record was added */
    enum filetype      type;
    unsigned char      md5[33];            /* md5sum of the file */
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
    unsigned long long comp_offset;             /* Start offset of this compressed block,
                                                   measured from the beginning of the
                                                   template file. NOTE: This is NOT the
                                                   start of the compressed data itself,
                                                   but the start of the header! */
    unsigned long long uncomp_offset;           /* Start offset when uncompressed */
    unsigned long long comp_size;               /* Size of the compressed block */
    unsigned long long uncomp_size;             /* Size of uncompressed block */
    ctype_e            comp_type;               /* CT_GZIP or CT_BZIP2 */
    unsigned char      template_id[33];         /* MD5 of the template */
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

/* Store a template entry */
int db_store_template(JIGDB *dbp, db_template_entry_t *entry);

/* Lookup a template entry. */
int db_lookup_template_by_path(JIGDB *dbp, char *template_name, db_template_entry_t *out);

/********************
 *
 * Block functions
 *
 *******************/

/* Store a template match / non-match block entry */
int db_store_block(JIGDB *dbp, db_block_entry_t *entry);

/* Lookup a block by output offset. The specified offset will be
 * within the range covered by the returned entry, or ENOENT. */
int db_lookup_block_by_offset(JIGDB *dbp, unsigned long long image_offset,
                              unsigned char *template_id, db_block_entry_t *out);

/****************
 *
 * File functions
 *
 ***************/

/* Store details of a file */
int db_store_file(JIGDB *dbp, db_file_entry_t *entry);

/* Lookup a file by md5 */
int db_lookup_file_by_md5(JIGDB *dbp, unsigned char *md5, db_file_entry_t *out);

/* Lookup a file by name */
int db_lookup_file_by_name(JIGDB *dbp, char *filename, db_file_entry_t *out);

/* Delete a file by name */
int db_delete_file_by_name(JIGDB *dbp, unsigned char *md5, enum filetype type, char *filename);

/* Delete files added previous to a specified date */
int db_delete_files_by_age(JIGDB *dbp, time_t date);

/* Delete a file entry */
int db_delete_file(JIGDB *dbp, unsigned char *md5, enum filetype type, char *filename);

/****************************
 *
 * Compressed block functions
 *
 ***************************/

/* Store details of a block */
int db_store_compressed(JIGDB *dbp, db_compressed_entry_t *entry);

/* Lookup a block by its UNCOMPRESSED offset */
int db_lookup_compressed_by_offset(JIGDB *dbp, unsigned long uncomp_offset,
                                   unsigned char *template_id, db_compressed_entry_t *out);

#endif /* JIGDB_H */
