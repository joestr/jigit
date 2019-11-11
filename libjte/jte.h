/*
 * jte.c
 *
 * Copyright (c) 2004-2006 Steve McIntyre <steve@einval.com>
 * Copyright (c) 2010 Thomas Schmitt <scdbackup@gmx.net>
 * Copyright (c) 2010 George Danchev <danchev@spnet.net>
 *
 * Prototypes and declarations for JTE
 *
 * GNU GPL v2
 */

#ifndef _JTE_JTE_H_
#define _JTE_JTE_H_

/* The API environment handle which replaces the old global variables */
struct libjte_env;

#include <stdio.h>
#include <unistd.h>
#include <regex.h>
#include "checksum.h"

typedef int BOOL;


extern int write_jt_header(struct libjte_env *o,
                            FILE *template_file, FILE *jigdo_file);
extern int write_jt_footer(struct libjte_env *o);
extern int jtwrite(struct libjte_env *o,
                            void *buffer, int size, int count);
extern int write_jt_match_record(struct libjte_env *o,
                            char *filename, char *mirror_name, int sector_size,
                            off_t size, unsigned char *checksum);
extern int  list_file_in_jigdo(struct libjte_env *o,
                            char *filename, off_t size, char **realname,
                            unsigned char *checksum);
extern int  jte_add_exclude(struct libjte_env *o, char *pattern);
extern int  jte_add_include(struct libjte_env *o, char *pattern);
extern int  jte_add_mapping(struct libjte_env *o, char *arg);

int libjte_destroy_path_match_list(struct libjte_env *o, int flag);
int libjte_destroy_path_mapping(struct libjte_env *o, int flag);
int libjte_destroy_entry_list(struct libjte_env *o, int flag);
int libjte_destroy_checksum_list(struct libjte_env *o, int flag);

int libjte_report_no_mem(struct libjte_env *o, size_t size, int flag);


typedef enum _jtc_e
{
    JTE_TEMP_GZIP = 0,
    JTE_TEMP_BZIP2
} jtc_t;

struct _checksum_data
{
    enum checksum_types type;
    char *name;      /* the name of the checksum algorithm */
    int   raw_bytes; /* how many bytes needed to store the raw
		      * checksum */
    int   hex_bytes; /* how many chars needed to store it as a hex
		      * string, *not including* the NULL terminator */
    int   b64_bytes; /* how many chars needed to store it as a base64
		      * string, *not including* the NULL terminator */
};

#define ROUND_UP(N, S)      ((((N) + (S) - 1) / (S)) * (S))

#define MD5_BITS            128
#define MD5_BYTES           (MD5_BITS / 8)
#define HEX_MD5_BYTES       (MD5_BITS / 4)
#define BASE64_MD5_BYTES    ((ROUND_UP (MD5_BITS, 6)) / 6)

#define SHA256_BITS         256
#define SHA256_BYTES        (SHA256_BITS / 8)
#define HEX_SHA256_BYTES    (SHA256_BITS / 4)
#define BASE64_SHA256_BYTES ((ROUND_UP (SHA256_BITS, 6)) / 6)

static const struct _checksum_data check_algos[] = 
{
    {
		CHECK_MD5,
		"md5",
		MD5_BYTES,
		HEX_MD5_BYTES,
		BASE64_MD5_BYTES
	},
    {
		CHECK_SHA256,
		"sha256",
		SHA256_BYTES,
		HEX_SHA256_BYTES,
		BASE64_SHA256_BYTES
	},
    {
		NUM_CHECKSUMS,
		NULL,
		0,
		0,
		0
	}
};

#define MIN_JIGDO_FILE_SIZE 1024

/*	
	Simple list to hold the results of -jigdo-exclude and
	-jigdo-force-match command line options. Seems easiest to do this
	using regexps.
*/
struct path_match
{
    regex_t  match_pattern;
    char    *match_rule;
    struct path_match *next;
};

/* List of mappings e.g. Debian=/mirror/debian */
struct path_mapping
{
    char                *from;
    char                *to;
    struct path_mapping *next;
};

/* List of files that we've seen, ready to write into the template and
   jigdo files */
typedef struct _file_entry
{
    unsigned char      *checksum;
    off_t               file_length;
    uint64_t            rsyncsum;
    char               *filename;
} file_entry_t;

typedef struct _unmatched_entry
{
    off_t uncompressed_length;
} unmatched_entry_t;    

typedef struct _entry
{
    int entry_type; /* JTET_TYPE as above */
    struct _entry *next;
    union
    {
        file_entry_t      file;
        unmatched_entry_t chunk;
    } data;
} entry_t;

typedef struct _jigdo_file_entry_md5
{
    unsigned char type;
    unsigned char fileLen[6];
    unsigned char fileRsync[8];
    unsigned char fileMD5[MD5_BYTES];
} jigdo_file_entry_md5_t;

typedef struct _jigdo_file_entry_sha256
{
    unsigned char type;
    unsigned char fileLen[6];
    unsigned char fileRsync[8];
    unsigned char fileSHA256[SHA256_BYTES];
} jigdo_file_entry_sha256_t;

typedef struct _jigdo_chunk_entry
{
    unsigned char type;
    unsigned char skipLen[6];
} jigdo_chunk_entry_t;

typedef struct _jigdo_image_entry_md5
{
    unsigned char type;
    unsigned char imageLen[6];
    unsigned char imageMD5[MD5_BYTES];
    unsigned char blockLen[4];
} jigdo_image_entry_md5_t;

typedef struct _jigdo_image_entry_sha256
{
    unsigned char type;
    unsigned char imageLen[6];
    unsigned char imageSHA256[SHA256_BYTES];
    unsigned char blockLen[4];
} jigdo_image_entry_sha256_t;

typedef struct _checksum_list_entry
{
    struct _checksum_list_entry *next;
    unsigned char      *checksum;
    uint64_t            size;
    char               *filename;
} checksum_list_entry_t;


typedef struct _jigdo_msg_entry
{
    struct _jigdo_msg_entry *next;
    char *message;
} jigdo_msg_entry_t;
int libjte_add_msg_entry(struct libjte_env *o, char *message, int flag);
/* Destructor is API call  libjte_clear_msg_list() */

#endif
/*_JTE_JTE_H_*/
