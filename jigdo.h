/*
 * jigdo.h
 *
 * Common prototypes/macros/definitions
 *
 * Copyright (c) 2007-2019 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

/* Compression algorithms supported in the compressed data blocks
 * inside a template file */
#define COMP_GZIP  1
#define COMP_BZIP2 2

/* Block types within the template file */
#define BLOCK_DATA            2
#define BLOCK_IMAGE_MD5       5
#define BLOCK_MATCH_MD5       6
#define BLOCK_WRITTEN_MD5     7
#define BLOCK_IMAGE_SHA256    8
#define BLOCK_MATCH_SHA256    9
#define BLOCK_WRITTEN_SHA256 10

/* Useful types and macros */
typedef long long INT64;
typedef unsigned long long UINT64;
typedef unsigned long      UINT32;

#ifndef LLONG_MAX
#   define LLONG_MAX (INT64)INT_MAX * INT_MAX
#endif

#ifndef MIN
#define MIN(x,y)        ( ((x) < (y)) ? (x) : (y))
#endif

#define BUF_SIZE 65536

typedef enum state_
{
    STARTING,
    IN_DATA,
    IN_DESC,
    DUMP_DESC,
    DUMP_DESC_PTR,
    DONE,
    ERROR
} e_state;

typedef struct zs_
{
    char   *data_buf;
    INT64   buf_size;
    INT64   offset_in_curr_buf;
    INT64   total_offset;
} zip_state_t;

int read_data_block(FILE *template_file, FILE *logfile, zip_state_t *zip_state);

