/*
 * jigdo.h
 *
 * Common prototypes/macros/definitions
 *
 * Copyright (c) 2007 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

/* Compression algorithms supported in the compressed data blocks
 * inside a template file */
#define COMP_GZIP  1
#define COMP_BZIP2 2

/* Block types within the template file */
#define BLOCK_DATA    2
#define BLOCK_IMAGE   5
#define BLOCK_MATCH   6

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
    DONE,
    ERROR
} e_state;


