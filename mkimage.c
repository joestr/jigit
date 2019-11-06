/*
 * mkimage
 *
 * Tool to create an ISO image from jigdo files
 *
 * Copyright (c) 2004-2019 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <zlib.h>
#include <bzlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include "endian.h"
#include "jig-base64.h"
#include "md5.h"
#include "sha256.h"
#include "jigdo.h"

#define MISSING -1
#define UNKNOWN -2

static FILE *logfile = NULL;
static FILE *outfile = NULL;
static FILE *missing_file = NULL;
static long long start_offset = 0;
static long long end_offset = 0;
static int quick = 0;
static int verbose = 0;
static int image_md5_valid = 0;
static int image_sha256_valid = 0;
static int check_jigdo_header = 1;
static UINT64 out_size = 0;
static char *missing_filename = NULL;

#define ROUND_UP(N, S)      ((((N) + (S) - 1) / (S)) * (S))

#define MD5_BITS            128
#define MD5_BYTES           (MD5_BITS / 8)
#define HEX_MD5_BYTES       (MD5_BITS / 4)
#define BASE64_MD5_BYTES    ((ROUND_UP (MD5_BITS, 6)) / 6)

#define SHA256_BITS         256
#define SHA256_BYTES        (SHA256_BITS / 8)
#define HEX_SHA256_BYTES    (SHA256_BITS / 4)
#define BASE64_SHA256_BYTES ((ROUND_UP (SHA256_BITS, 6)) / 6)

/* number of chars used to print a file size in our input checksum
 * file */
#define SIZE_BYTES          12

typedef struct match_list_
{
    struct match_list_ *next;
    char *match;
    char *mirror_path;
} match_list_t;

static match_list_t *match_list_head = NULL;
static match_list_t *match_list_tail = NULL;

typedef struct md5_list_
{
    struct md5_list_ *next;
    INT64 file_size;
    char *md5;
    char *full_path;
} md5_list_t;

static md5_list_t *md5_list_head = NULL;
static md5_list_t *md5_list_tail = NULL;

typedef struct sha256_list_
{
    struct sha256_list_ *next;
    INT64 file_size;
    char *sha256;
    char *full_path;
} sha256_list_t;

static sha256_list_t *sha256_list_head = NULL;
static sha256_list_t *sha256_list_tail = NULL;
static zip_state_t zip_state;

/* Grab the file component from a full path */
static char *file_base_name(char *path)
{
    char *endptr = path;
    char *ptr = path;
    
    while (*ptr != '\0')
    {
        if ('/' == *ptr)
            endptr = ++ptr;
        else
            ++ptr;
    }
    return endptr;
}

static void write_missing_entry(char *missing, char *filename)
{
    if (!missing_file)
    {
        missing_file = fopen(missing, "wb");
        if (!missing_file)
        {
            fprintf(logfile, "write_missing_entry: Unable to open missing log %s; error %d\n", missing, errno);
            exit(1);
        }
    }
    fprintf(missing_file, "%s\n", filename);
}

static INT64 get_file_size(char *filename)
{
    struct stat sb;
    int error = 0;
    
    error = stat(filename, &sb);
    if (error)
        return MISSING;
    else
        return sb.st_size;
}

static void display_progress(FILE *file, char *text)
{
    INT64 written = ftello(file);
    if (out_size > 0)
        fprintf(logfile, "\r %5.2f%%  %-60.60s",
                100.0 * written / out_size, text);
}

static int add_match_entry(char *match)
{
    match_list_t *entry = NULL;
    char *mirror_path = NULL;
    char *ptr = match;

    /* Split "Foo=/mirror/foo" into its components */
    while (*ptr)
    {
        if ('=' == *ptr)
        {
            *ptr = 0;
            ptr++;
            mirror_path = ptr;
            break;
        }
        ptr++;
    }

    if (!mirror_path)
    {
        fprintf(logfile, "Could not parse malformed match entry \"%s\"\n", match);
        return EINVAL;
    }        
    
    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return ENOMEM;

    if (verbose)
        fprintf(logfile, "Adding match entry %s:%s\n", match, mirror_path);

    entry->match = match;
    entry->mirror_path = mirror_path;
    
    if (!match_list_head)
    {
        match_list_head = entry;
        match_list_tail = entry;
    }
    else
    {
        match_list_tail->next = entry;
        match_list_tail = entry;
    }
    
    return 0;
}

static int file_exists(char *path, INT64 *size)
{
    struct stat sb;
    int error = 0;
    
    error = stat(path, &sb);
    if (!error && S_ISREG(sb.st_mode))
    {
        *size = sb.st_size;
        return 1;
    }
    
    /* else */
    return 0;
}

static md5_list_t *find_file_in_md5_list(unsigned char *base64_md5, int need_size)
{
    md5_list_t *md5_list_entry = md5_list_head;
    
    while (md5_list_entry)
    {        
        if (verbose > 2)
            fprintf(logfile, "find_file_in_md5_list: looking for %s, looking at %s (%s)\n", 
                    base64_md5, md5_list_entry->md5, md5_list_entry->full_path);
        
        if (!memcmp(md5_list_entry->md5, base64_md5, BASE64_MD5_BYTES)) {
            if (need_size &&
                md5_list_entry->file_size == UNKNOWN)
                md5_list_entry->file_size =
                    get_file_size(md5_list_entry->full_path);

            return md5_list_entry;
        }
        /* else */
        md5_list_entry = md5_list_entry->next;
    }
    return NULL; /* Not found */
}

static sha256_list_t *find_file_in_sha256_list(unsigned char *base64_sha256, int need_size)
{
    sha256_list_t *sha256_list_entry = sha256_list_head;

    while (sha256_list_entry)
    {
        if (verbose > 2)
            fprintf(logfile, "find_file_in_sha256_list: looking for %s, looking at %s (%s)\n", 
                    base64_sha256, sha256_list_entry->sha256, sha256_list_entry->full_path);

        if (!memcmp(sha256_list_entry->sha256, base64_sha256, BASE64_SHA256_BYTES)) {
            if (need_size &&
                sha256_list_entry->file_size == UNKNOWN)
                sha256_list_entry->file_size =
                    get_file_size(sha256_list_entry->full_path);

            return sha256_list_entry;
        }
        /* else */
        sha256_list_entry = sha256_list_entry->next;
    }
    return NULL; /* Not found */
}

static int find_file_in_mirror(char *jigdo_match, char *jigdo_name,
                               char *match, INT64 *file_size, char **mirror_path)
{
    match_list_t *entry = match_list_head;
    char *path = NULL;
    int path_size = 0;
    int jigdo_name_size = strlen(jigdo_name);

    while (entry)
    {
        if (!strcmp(entry->match, match))
        {
            int mirror_path_size = strlen(entry->mirror_path);
            if ((jigdo_name_size + 2 + mirror_path_size) > path_size)
            {
                free(path);
                /* grow by 100 characters more than we need, to reduce
                 * time taken in malloc if we work through lengthening
                 * paths. */
                path_size = jigdo_name_size + mirror_path_size + 100;
                path = malloc(path_size);
                if (!path)
                    return ENOMEM;
            }
            
            sprintf(path, "%s/%s", entry->mirror_path, jigdo_name);
            if (file_exists(path, file_size))
            {
                *mirror_path = path;
                return 0;
            }
        }
        entry = entry->next;
    }
    
    free(path);
    return ENOENT;
}

static int hex_to_nibble(char hex)
{
    if (hex >= '0' && hex <= '9')
        return hex - '0';
    else if (hex >= 'A' && hex <= 'F')
        return 10 + hex - 'A';
    else if (hex >= 'a' && hex <= 'f')
        return 10 + hex - 'a';
    return 0;
}

static int add_md5_entry(INT64 size, char *md5, char *path)
{
    md5_list_t *new = NULL;    
    new = calloc(1, sizeof(*new));
    if (!new)
        return ENOMEM;

    new->md5 = md5;
    new->full_path = path;
    new->file_size = size;
    
    if (!md5_list_head)
    {
        md5_list_head = new;
        md5_list_tail = new;
    }
    else
    {
        md5_list_tail->next = new;
        md5_list_tail = new;
    }
    
    return 0;
}

/* Parse an incoming MD5 file entry, working in place in the
 * (strduped) buffer we've been passed */
static int parse_md5_entry(char *md5_entry)
{
    int error = 0;
    char *file_name = NULL;
    char *md5 = NULL;
    unsigned char bin_md5[MD5_BYTES];
    int i;

    md5_entry[HEX_MD5_BYTES] = 0;
    md5_entry[HEX_MD5_BYTES + 1] = 0;

    /* Re-encode hex as base64 and overwrite in place; safe, as the
     * md5 will be shorter than the hex. */
    for (i = 0; i < MD5_BYTES; i++)
        bin_md5[i] = (hex_to_nibble(md5_entry[2 * i]) << 4) |
                      hex_to_nibble(md5_entry[2 * i + 1]);
    strncpy(md5_entry, base64_dump(bin_md5, MD5_BYTES), BASE64_MD5_BYTES);

    md5_entry[BASE64_MD5_BYTES] = 0;
    md5 = md5_entry;
    file_name = &md5_entry[HEX_MD5_BYTES + 2 + SIZE_BYTES + 2];

    if ('\n' == file_name[strlen(file_name) -1])
        file_name[strlen(file_name) - 1] = 0;
    
    error = add_md5_entry(UNKNOWN, md5, file_name);
    return error;
}

static int parse_md5_file(char *filename)
{
    char buf[2048];
    FILE *file = NULL;
    char *ret = NULL;
    int error = 0;

    file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(logfile, "Failed to open MD5 file %s, error %d!\n", filename, errno);
        return errno;
    }
    
    while(1)
    {
        ret = fgets(buf, sizeof(buf), file);
        if (NULL == ret)
            break;
        error = parse_md5_entry(strdup(buf));
        if (error)
            return error;
    }
    return 0;
}

static int add_sha256_entry(INT64 size, char *sha256, char *path)
{
    sha256_list_t *new = NULL;
    new = calloc(1, sizeof(*new));
    if (!new)
        return ENOMEM;

    new->sha256 = sha256;
    new->full_path = path;
    new->file_size = size;

    if (!sha256_list_head)
    {
        sha256_list_head = new;
        sha256_list_tail = new;
    }
    else
    {
        sha256_list_tail->next = new;
        sha256_list_tail = new;
    }

    return 0;
}

/* Parse an incoming SHA256 file entry, working in place in the
 * (strduped) buffer we've been passed */
static int parse_sha256_entry(char *sha256_entry)
{
    int error = 0;
    char *file_name = NULL;
    char *sha256 = NULL;
    unsigned char bin_sha256[SHA256_BYTES];
    int i;

    sha256_entry[HEX_SHA256_BYTES] = 0;
    sha256_entry[HEX_SHA256_BYTES + 1] = 0;

    /* Re-encode hex as base64 and overwrite in place; safe, as the
     * sha256 will be shorter than the hex. */
    for (i = 0; i < SHA256_BYTES; i++)
        bin_sha256[i] = (hex_to_nibble(sha256_entry[2 * i]) << 4) |
                      hex_to_nibble(sha256_entry[2 * i + 1]);
    strncpy(sha256_entry, base64_dump(bin_sha256, SHA256_BYTES), BASE64_SHA256_BYTES);

    sha256_entry[BASE64_SHA256_BYTES] = 0;
    sha256 = sha256_entry;
    file_name = &sha256_entry[HEX_SHA256_BYTES + 2 + SIZE_BYTES + 2];

    if ('\n' == file_name[strlen(file_name) -1])
        file_name[strlen(file_name) - 1] = 0;

    error = add_sha256_entry(UNKNOWN, sha256, file_name);
    return error;
}

static int parse_sha256_file(char *filename)
{
    char buf[2048];
    FILE *file = NULL;
    char *ret = NULL;
    int error = 0;

    file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(logfile, "Failed to open SHA256 file %s, error %d!\n", filename, errno);
        return errno;
    }

    while(1)
    {
        ret = fgets(buf, sizeof(buf), file);
        if (NULL == ret)
            break;
        error = parse_sha256_entry(strdup(buf));
        if (error)
            return error;
    }
    return 0;
}

/* DELIBERATELY do not sort these, or do anything clever with
   insertion. The entries in the jigdo file should be in the same
   order as the ones we'll want from the template. Simply add to the
   end of the singly-linked list each time! */
static int add_file_entry(char *jigdo_entry)
{
    int error = 0;
    char *file_name = NULL;
    INT64 file_size = 0;
    char *ptr = jigdo_entry;
    char *base64_checksum = NULL;
    int csum_length;
    char *match = NULL;
    char *jigdo_name = NULL;
    
    /* Grab out the component strings from the entry in the jigdo file */
    base64_checksum = jigdo_entry;
    while (0 != *ptr)
    {
        if ('=' == *ptr)
        {
            *ptr = 0;
            ptr++;
            match = ptr;
        }
        else if (':' == *ptr)
        {
            *ptr = 0;
            ptr++;
            jigdo_name = ptr;
        }
        else if ('\n' == *ptr)
            *ptr = 0;
        else
            ptr++;
    }

    csum_length = strlen(base64_checksum);
    if (csum_length == BASE64_SHA256_BYTES)
    {
        if (find_file_in_sha256_list((unsigned char *)base64_checksum, 0))
        {
            free(jigdo_entry);
            return 0; /* We already have an entry for this file; don't
                       * waste any more time on it */
	}
    }
    else if (csum_length == BASE64_MD5_BYTES)
    {
        if (find_file_in_md5_list((unsigned char *)base64_checksum, 0))
        {
            free(jigdo_entry);
            return 0; /* We already have an entry for this file; don't
                       * waste any more time on it */
	}
    }
    else
    {
        csum_length = -1; /* flag error */
    }

    /* else look for the file in the filesystem */
    if (-1 == csum_length || NULL == match || NULL == jigdo_name)
    {
        fprintf(logfile, "Could not parse malformed jigdo entry \"%s\"\n", jigdo_entry);
        free(jigdo_entry);
        return EINVAL;
    }

    error = find_file_in_mirror(match, jigdo_name, match, &file_size, &file_name);
    switch (error)
    {
        case 0:
            base64_checksum = strdup(jigdo_entry);
            if (base64_checksum)
            {
                if (csum_length == BASE64_MD5_BYTES)
                    add_md5_entry(file_size, base64_checksum, file_name);
                else
                    add_sha256_entry(file_size, base64_checksum, file_name);
                free(jigdo_entry);
                break;
            }
            /* else, fall through... */

        case ENOMEM:
            fprintf(logfile, "Unable to allocate memory looking for %s\n", jigdo_name);
            fprintf(logfile, "Abort!\n");
            exit (ENOMEM);
            break;

        default: /* ENOENT */
            if (missing_filename)
            {
                if (csum_length == BASE64_MD5_BYTES)
                    add_md5_entry(MISSING, base64_checksum, jigdo_name);
                else
                    add_sha256_entry(MISSING, base64_checksum, jigdo_name);
            }
            else
            {
                fprintf(logfile, "Unable to find a file to match %s\n", jigdo_name);
                fprintf(logfile, "Abort!\n");
                exit (ENOENT);
            }
            break;
    }

    return 0;
}

static int parse_jigdo_file(char *filename)
{
    char buf[2048];
    gzFile file = NULL;
    char *ret = NULL;
    int error = 0;
    int num_files = 0;
    
    file = gzopen(filename, "rb");
    if (!file)
    {
        fprintf(logfile, "Failed to open jigdo file %s, error %d!\n", filename, errno);
        return errno;
    }

    /* Validate that we have a jigdo file */
    if (check_jigdo_header)
    {
        ret = gzgets(file, buf, sizeof(buf));
        if (NULL == ret)
        {
            gzclose(file);
            fprintf(logfile, "Unable to read from jigdo file %s\n", filename);
            return EIO;
        }
        if (strncmp(buf, "# JigsawDownload", 16))
        {
            gzclose(file);
            fprintf(logfile, "Not a valid jigdo file: %s\n", filename);
            return EINVAL;
        }
    }
    
    /* Find the [Parts] section of the jigdo file */
    while (1)
    {
        ret = gzgets(file, buf, sizeof(buf));
        if (NULL == ret)
            break;
        if (!strncmp(buf, "[Parts]", 7))
            break;
    }

    /* Now grab the individual file entries and build a list */
    while (1)
    {
        ret = gzgets(file, buf, sizeof(buf));
        if (NULL == ret || !strcmp(buf, "\n"))
            break;
        if (!strcmp(buf, "[") || !strcmp(buf, "#"))
            continue;
        error = add_file_entry(strdup(buf));
        num_files++;
        if (error)
            break;
    }
    if (verbose)
        fprintf(logfile, "Found entries for %d files in jigdo file %s\n", num_files, filename);
    
    gzclose(file);
    return error;
}

static int skip_data_block(INT64 data_size, FILE *template_file)
{
    int error = 0;
    INT64 remaining = data_size;
    INT64 size = 0;

    /* If we're coming in in the middle of the image, we'll need to
       skip through some compressed data */
    while (remaining)
    {
        if (!zip_state.data_buf)
        {
            error = read_data_block(template_file, logfile, &zip_state);
            if (error)
            {
                fprintf(logfile, "Unable to decompress template data, error %d\n",
                        error);
                return error;
            }
        }
        size = MIN((zip_state.buf_size - zip_state.offset_in_curr_buf), remaining);
        zip_state.offset_in_curr_buf += size;
        remaining -= size;
        
        if (zip_state.offset_in_curr_buf == zip_state.buf_size)
        {
            free(zip_state.data_buf);
            zip_state.data_buf = NULL;
        }
    }
    
    fprintf(logfile, "skip_data_block: skipped %lld bytes of unmatched data\n", data_size);
    return error;
}

/* Trivial helper - update all valid checksums */
static void update_checksum_context(struct mk_MD5Context *md5_context,
				    struct sha256_ctx *sha256_context,
				    const void *buffer,
				    size_t len)
{
    if (md5_context && image_md5_valid)
        mk_MD5Update(md5_context, buffer, len);
    if (sha256_context && image_sha256_valid)
        sha256_process_bytes(buffer, len, sha256_context);
}

static int parse_data_block(INT64 data_size, FILE *template_file,
                            struct mk_MD5Context *md5_context,
			    struct sha256_ctx *sha256_context)
{
    int error = 0;
    INT64 remaining = data_size;
    INT64 size = 0;
    int out_size = 0;

    while (remaining)
    {
        if (!zip_state.data_buf)
        {
            error = read_data_block(template_file, logfile, &zip_state);
            if (error)
            {
                fprintf(logfile, "Unable to decompress template data, error %d\n",
                        error);
                return error;
            }
        }
        size = MIN((zip_state.buf_size - zip_state.offset_in_curr_buf), remaining);
        out_size = fwrite(&zip_state.data_buf[zip_state.offset_in_curr_buf], size, 1, outfile);
        if (!out_size)
        {
            fprintf(logfile, "parse_data_block: fwrite %lld failed with error %d; aborting\n", size, ferror(outfile));
            return ferror(outfile);
        }

        if (verbose)
            display_progress(outfile, "template data");

        if (!quick)
            update_checksum_context(md5_context, sha256_context,
				    (unsigned char *)&zip_state.data_buf[zip_state.offset_in_curr_buf],
				    size);
        zip_state.offset_in_curr_buf += size;
        remaining -= size;
        
        if (zip_state.offset_in_curr_buf == zip_state.buf_size)
        {
            free(zip_state.data_buf);
            zip_state.data_buf = NULL;
        }
    }
    if (verbose > 1)
        fprintf(logfile, "parse_data_block: wrote %lld bytes of unmatched data\n", data_size);
    return error;
}

static int parse_file_block_md5(INT64 offset, INT64 data_size, INT64 file_size, 
				unsigned char *md5, struct mk_MD5Context *md5_context,
				struct sha256_ctx *sha256_context, char *missing)
{
    char *base64_md5 = base64_dump(md5, MD5_BYTES);
    FILE *input_file = NULL;
    char buf[BUF_SIZE];
    INT64 remaining = data_size;
    int num_read = 0;
    struct mk_MD5Context file_context;
    unsigned char file_md5[MD5_BYTES];
    int out_size = 0;
    md5_list_t *md5_list_entry = NULL;
    
    if (!quick)
        mk_MD5Init(&file_context);

    md5_list_entry = find_file_in_md5_list((unsigned char *)base64_md5, 1);
    if (md5_list_entry && file_size == md5_list_entry->file_size)
    {
        if (verbose > 1)
            fprintf(logfile, "Reading %s\n", md5_list_entry->full_path);
        
        input_file = fopen(md5_list_entry->full_path, "rb");
        if (!input_file)
        {
            fprintf(logfile, "Unable to open mirror file %s, error %d\n",
                    md5_list_entry->full_path, errno);
            return errno;
        }
        
        if (missing)
        {
            fclose(input_file);
            return 0;
        }
        
        fseek(input_file, offset, SEEK_SET);
        while (remaining)
        {
            int size = MIN(BUF_SIZE, remaining);
            memset(buf, 0, BUF_SIZE);
            
            num_read = fread(buf, size, 1, input_file);
            if (!num_read)
            {
                fprintf(logfile, "Unable to read from mirror file %s, error %d (offset %ld, length %d)\n",
                        md5_list_entry->full_path, errno, ftell(input_file), size);
                fclose(input_file);
                return errno;
            }
            if (!quick)
            {
                update_checksum_context(md5_context, sha256_context, (unsigned char *)buf, size);
                mk_MD5Update(&file_context, (unsigned char *)buf, size);
            }
            
            out_size = fwrite(buf, size, 1, outfile);
            if (!out_size)
            {
                fprintf(logfile, "parse_file_block: fwrite %d failed with error %d; aborting\n", size, ferror(outfile));
                return ferror(outfile);
            }
            
            if (verbose)
                display_progress(outfile, file_base_name(md5_list_entry->full_path));
            
            remaining -= size;
        }
        if (verbose > 1)
            fprintf(logfile, "parse_file_block: wrote %lld bytes of data from %s\n",
                    file_size, md5_list_entry->full_path);
        fclose(input_file);
        
        if (!quick)
        {
            mk_MD5Final(file_md5, &file_context);
        
            if (memcmp(file_md5, md5, MD5_BYTES))
            {
                fprintf(logfile, "MD5 MISMATCH for file %s\n", md5_list_entry->full_path);
                fprintf(logfile, "    template looking for %s\n", md5);
                fprintf(logfile, "    file in mirror is    %s\n", file_md5);
                return EINVAL;
            }
        }
        return 0;
    }
    if ( missing &&
         (MISSING == md5_list_entry->file_size) &&
         (!memcmp(md5_list_entry->md5, base64_md5, MD5_BYTES) ) )
    {
        write_missing_entry(missing, md5_list_entry->full_path);
        return 0;
    }
    /* else */
    if (verbose)
    {
        char hex_md5[HEX_MD5_BYTES + 1];
        int i;

        for (i = 0; i < MD5_BYTES; i++)
            sprintf(hex_md5 + 2 * i, "%2.2x", (unsigned int) md5[i]);

        fprintf(logfile, "Unable to find a file for block with md5 %s (%s)\n", hex_md5, base64_md5);
    }
    return ENOENT;
}

static int parse_file_block_sha256(INT64 offset, INT64 data_size, INT64 file_size,
				   unsigned char *sha256, struct mk_MD5Context *md5_context,
				   struct sha256_ctx *sha256_context, char *missing)
{
    char *base64_sha256 = base64_dump(sha256, SHA256_BYTES);
    FILE *input_file = NULL;
    char buf[BUF_SIZE];
    INT64 remaining = data_size;
    int num_read = 0;
    struct sha256_ctx file_context;
    unsigned char file_sha256[SHA256_BYTES];
    int out_size = 0;
    sha256_list_t *sha256_list_entry = NULL;

    if (!quick)
        sha256_init_ctx(&file_context);

    sha256_list_entry = find_file_in_sha256_list((unsigned char *)base64_sha256, 1);
    if (sha256_list_entry && file_size == sha256_list_entry->file_size)
    {
        if (verbose > 1)
            fprintf(logfile, "Reading %s\n", sha256_list_entry->full_path);

        input_file = fopen(sha256_list_entry->full_path, "rb");
        if (!input_file)
        {
            fprintf(logfile, "Unable to open mirror file %s, error %d\n",
                    sha256_list_entry->full_path, errno);
            return errno;
        }

        if (missing)
        {
            fclose(input_file);
            return 0;
        }

        fseek(input_file, offset, SEEK_SET);
        while (remaining)
        {
            int size = MIN(BUF_SIZE, remaining);
            memset(buf, 0, BUF_SIZE);

            num_read = fread(buf, size, 1, input_file);
            if (!num_read)
            {
                fprintf(logfile, "Unable to read from mirror file %s, error %d (offset %ld, length %d)\n",
                        sha256_list_entry->full_path, errno, ftell(input_file), size);
                fclose(input_file);
                return errno;
            }
            if (!quick)
            {
                update_checksum_context(md5_context, sha256_context, (unsigned char *)buf, size);
                sha256_process_bytes((unsigned char *)buf, size, &file_context);
            }

            out_size = fwrite(buf, size, 1, outfile);
            if (!out_size)
            {
                fprintf(logfile, "parse_file_block: fwrite %d failed with error %d; aborting\n", size, ferror(outfile));
                return ferror(outfile);
            }

            if (verbose)
                display_progress(outfile, file_base_name(sha256_list_entry->full_path));

            remaining -= size;
        }
        if (verbose > 1)
            fprintf(logfile, "parse_file_block: wrote %lld bytes of data from %s\n",
                    file_size, sha256_list_entry->full_path);
        fclose(input_file);

        if (!quick)
        {
            sha256_finish_ctx(&file_context, file_sha256);

            if (memcmp(file_sha256, sha256, SHA256_BYTES))
            {
                fprintf(logfile, "SHA256 MISMATCH for file %s\n", sha256_list_entry->full_path);
                fprintf(logfile, "    template looking for %s\n", sha256);
                fprintf(logfile, "    file in mirror is    %s\n", file_sha256);
                return EINVAL;
            }
        }
        return 0;
    }
    if ( missing &&
         (MISSING == sha256_list_entry->file_size) &&
         (!memcmp(sha256_list_entry->sha256, base64_sha256, SHA256_BYTES) ) )
    {
        write_missing_entry(missing, sha256_list_entry->full_path);
        return 0;
    }
    /* else */
    if (verbose)
    {
        char hex_sha256[HEX_SHA256_BYTES + 1];
        int i;

        for (i = 0; i < SHA256_BYTES; i++)
            sprintf(hex_sha256 + 2 * i, "%2.2x", (unsigned int) sha256[i]);

        fprintf(logfile, "Unable to find a file for block with sha256 %s (%s)\n", hex_sha256, base64_sha256);
    }
    return ENOENT;
}

static int parse_template_file(char *filename, int sizeonly, char *missing, char *output_name)
{
    INT64 template_offset = 0;
    char *buf = NULL;
    FILE *file = NULL;
    INT64 file_size = 0;
    INT64 desc_start = 0;
    INT64 written_length = 0;
    INT64 output_offset = 0;
    INT64 desc_size = 0;
    size_t bytes_read = 0;
    size_t total_read = 0;
    int i = 0;
    int error = 0;
    char *bufptr;
    struct mk_MD5Context template_md5_context;
    struct sha256_ctx template_sha256_context;
    unsigned char image_md5sum[MD5_BYTES];
    unsigned char image_sha256sum[SHA256_BYTES];
    unsigned char image_md5sum_from_tmpl[MD5_BYTES];
    unsigned char image_sha256sum_from_tmpl[SHA256_BYTES];

    zip_state.total_offset = 0;
    
    file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(logfile, "Failed to open template file %s, error %d!\n", filename, errno);
        return errno;
    }

    buf = malloc(BUF_SIZE);
    if (!buf)
    {
        fprintf(logfile, "Failed to malloc %d bytes. Abort!\n", BUF_SIZE);
        fclose(file);
        return ENOMEM;
    }

    /* Find the beginning of the desc block */
    file_size = get_file_size(filename);
    fseek(file, file_size - 6, SEEK_SET);
    fread(buf, 6, 1, file);
    desc_size = read_le48((unsigned char *)buf);
    desc_start = file_size - desc_size;

    /* Load the DESC block in from the template file in and find the
     * final descriptor that describes the image
     * itself. Unfortunately, only way to do this is by scanning
     * through the whole set of descriptors in the template. */
    fseek(file, desc_start, SEEK_SET);
    buf = realloc(buf, desc_size);
    if (!buf)
    {
        fprintf(logfile, "Failed to malloc %lld bytes. Abort!\n", desc_size);
        fclose(file);
        return ENOMEM;
    }
    while (total_read < desc_size)
    {
        bytes_read = fread(buf, 1, desc_size, file);
        if (ferror(file))
        {
            fprintf(logfile, "Failed to read to the end of the template file, error %d\n", ferror(file));
            fclose(file);
            free(buf);
            return EIO;
        }
        total_read += bytes_read;
    }

    /* Now start parsing the DESC block */
    bufptr = buf;
    if (strncmp(bufptr, "DESC", 4))
    {
        fprintf(logfile, "Failed to find desc start in the template file\n");
        fclose(file);
        free(buf);
        return EINVAL;
    }
    bufptr += 4;

    if ((file_size - desc_start) != read_le48((unsigned char *)bufptr))
    {
        fprintf(logfile, "Inconsistent desc length in the template file!\n");
        fprintf(logfile, "Final chunk says %lld, first chunk says %lld\n",
                file_size - desc_start, read_le48((unsigned char *)bufptr));
        fclose(file);
        free(buf);
        return EINVAL;
    }
    bufptr += 6;

    while (bufptr < (buf + desc_size - 6))
    {
        switch (bufptr[0]) {
            case BLOCK_DATA:
                bufptr += 7;
                break;
            case BLOCK_MATCH_MD5:
                bufptr += 31;
                break;
            case BLOCK_MATCH_SHA256:
                bufptr += 47;
                break;
            case BLOCK_IMAGE_MD5:
                out_size = read_le48((unsigned char *)&bufptr[1]);
                memcpy(image_md5sum_from_tmpl, (unsigned char*)&bufptr[7], MD5_BYTES);
		image_md5_valid = 1;
                bufptr += 27;
                break;
            case BLOCK_IMAGE_SHA256:
                out_size = read_le48((unsigned char *)&bufptr[1]);
                memcpy(image_sha256sum_from_tmpl, (unsigned char*)&bufptr[7], SHA256_BYTES);
		image_sha256_valid = 1;
                bufptr += 43;
                break;
            default:
                fprintf(logfile, "Unknown block type %d, offset %ld\n", bufptr[0], bufptr - buf);
                fclose(file);
                free(buf);
                return EINVAL;
        }
    }

    if (!image_md5_valid && !image_sha256_valid)
    {
        fprintf(logfile, "Failed to find a valid image information block in the template file\n");
        fclose(file);
	free(buf);
        return EINVAL;
    }

    if (sizeonly)
    {
        fclose(file);
	free(buf);
        printf("%lld\n", out_size);
        return 0;
    }

    if (verbose)
    {
        if (image_md5_valid)
        {
            fprintf(logfile, "Image MD5 should be    ");
            for (i = 0; i < MD5_BYTES; i++)
                fprintf(logfile, "%2.2x", image_md5sum_from_tmpl[i]);
            fprintf(logfile, "\n");
        }
        if (image_sha256_valid)
        {
            fprintf(logfile, "Image SHA256 should be ");
            for (i = 0; i < SHA256_BYTES; i++)
                fprintf(logfile, "%2.2x", image_sha256sum_from_tmpl[i]);
            fprintf(logfile, "\n");
        }
        fprintf(logfile, "Image size should be   %lld bytes\n", out_size);
    }

    if (!quick)
    {
        if (image_md5_valid)
            mk_MD5Init(&template_md5_context);
        if (image_sha256_valid)
            sha256_init_ctx(&template_sha256_context);
    }

    if (verbose)
        fprintf(logfile, "Creating ISO image %s\n", output_name);

    template_offset = 10;

    /* Main loop - back to the start of the DESC block and now walk
     * through and expand each entry we find */
    while (1)
    {
        INT64 extent_size;
        INT64 skip = 0;
        INT64 read_length = 0;

        bufptr = &buf[template_offset];

        if (template_offset >= (desc_size - 6))
        {
            if (verbose > 1)
                fprintf(logfile, "Reached end of template file\n");
            break; /* Finished! */
        }
        
        if (output_offset > end_offset) /* Past the range we were asked for */
        {
            fprintf(logfile, "Reached end of range requested\n");            
            break;
        }

        extent_size = read_le48((unsigned char *)&bufptr[1]);
        read_length = extent_size;

        switch (bufptr[0])
        {
            case BLOCK_DATA: /* unmatched data */
                template_offset += 7;
                if (missing)
                    break;
                if ((output_offset + extent_size) >= start_offset)
                {
                    if (skip)
                        error = skip_data_block(skip, file);
                    if (error)
                    {
                        fprintf(logfile, "Unable to read data block to skip, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    error = parse_data_block(read_length, file, &template_md5_context, &template_sha256_context);
                    if (error)
                    {
                        fprintf(logfile, "Unable to read data block, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    written_length += read_length;
                }
                else
                    error = skip_data_block(extent_size, file);
                break;
            case BLOCK_MATCH_MD5:
                template_offset += 31;
                if ((output_offset + extent_size) >= start_offset)
                {
                    error = parse_file_block_md5(skip, read_length, extent_size, (unsigned char *)&bufptr[15],
						 &template_md5_context, &template_sha256_context, missing);
                    if (error)
                    {
                        fprintf(logfile, "Unable to read file block, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    written_length += read_length;
                }
                break;
            case BLOCK_MATCH_SHA256:
                template_offset += 47;
                if ((output_offset + extent_size) >= start_offset)
                {
                    error = parse_file_block_sha256(skip, read_length, extent_size, (unsigned char *)&bufptr[15],
						    &template_md5_context, &template_sha256_context, missing);
                    if (error)
                    {
                        fprintf(logfile, "Unable to read file block, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    written_length += read_length;
                }
                break;
            case BLOCK_IMAGE_MD5:
                template_offset += 27;
                break;
            case BLOCK_IMAGE_SHA256:
                template_offset += 43;
                break;
            default:
                fprintf(logfile, "Unknown block type %d, offset %ld\n", bufptr[0], bufptr - buf);
                fclose(file);
                return EINVAL;
        }
        output_offset += extent_size;
    }

    if (missing && missing_file)
        return ENOENT;
    
    fclose(file);
    if (verbose)
    {
        fprintf(logfile, "\n");
        if (!quick)
        {
            if (image_md5_valid)
            {
                mk_MD5Final (image_md5sum, &template_md5_context);
		fprintf(logfile, "Output image MD5 is    ");
		for (i = 0; i < MD5_BYTES; i++)
                    fprintf(logfile, "%2.2x", image_md5sum[i]);
		fprintf(logfile, "\n");
		if (0 == memcmp(image_md5sum, image_md5sum_from_tmpl, MD5_BYTES))
		{
                    fprintf(logfile, "OK: MD5 checksums match, image is good!\n");
                    fprintf(logfile, "WARNING: MD5 is not considered a secure hash!\n");
                    fprintf(logfile, "WARNING: It is recommended to verify your image in other ways too!\n");
		}
		else
		{
                    fprintf(logfile, "CHECKSUMS DO NOT MATCH - PROBLEM DETECTED\n");
		    fclose(file);
		    free(buf);
		    return EIO;
		}
	    }
            if (image_sha256_valid)
            {
                sha256_finish_ctx(&template_sha256_context, image_sha256sum);
		fprintf(logfile, "Output image SHA256 is ");
		for (i = 0; i < SHA256_BYTES; i++)
                    fprintf(logfile, "%2.2x", image_sha256sum[i]);
		fprintf(logfile, "\n");
		if (0 == memcmp(image_sha256sum, image_sha256sum_from_tmpl, SHA256_BYTES))
		{
                    fprintf(logfile, "OK: SHA256 checksums match, image is good!\n");
		}
		else
		{
                    fprintf(logfile, "CHECKSUMS DO NOT MATCH - PROBLEM DETECTED\n");
		    fclose(file);
		    free(buf);
		    return EIO;
		}
	    }
        }
        fprintf(logfile, "Output image length is %lld bytes\n", written_length);
    }
    
    return 0;
}

static void usage(char *progname)
{
    printf("%s [OPTIONS]\n\n", progname);
    printf(" Options:\n");
    printf(" -f <MD5 name>       Specify an input MD5 file. MD5s must be in jigdo's\n");
    printf("                     pseudo-base64 format\n");
    printf(" -F <SHA256 name>    Specify an input SHA256 file. SHA256s must be in jigdo's\n");
    printf("                     pseudo-base64 format\n");
    printf(" -j <jigdo name>     Specify the input jigdo file\n");
    printf(" -t <template name>  Specify the input template file\n");
    printf(" -m <item=path>      Map <item> to <path> to find the files in the mirror\n");
    printf(" -M <missing name>   Rather than try to build the image, just check that\n");
    printf("                     all the needed files are available. If any are missing,\n");
    printf("                     list them in this file.\n");
    printf(" -v                  Make the output logging more verbose; may be added\n");
    printf("                     multiple times\n");
    printf(" -l <logfile>        Specify a logfile to append to.\n");
    printf("                     If not specified, will log to stderr\n");
    printf(" -o <outfile>        Specify a file to write the ISO image to.\n");
    printf("                     If not specified, will write to stdout\n");
    printf(" -q                  Quick mode. Don't check MD5sums. Dangerous!\n");
    printf(" -s <bytenum>        Start byte number; will start at 0 if not specified\n");
    printf(" -e <bytenum>        End byte number; will end at EOF if not specified\n");    
    printf(" -z                  Don't attempt to rebuild the image; simply print its\n");
    printf("                     size in bytes\n");
    printf(" -O                  Support Old-format .jigdo files without the JigsawDownload\n");
    printf("                     header\n");
}

int main(int argc, char **argv)
{
    char *template_filename = NULL;
    char *jigdo_filename = NULL;
    char *md5_filename = NULL;
    char *sha256_filename = NULL;
    char *output_name = NULL;
    int c = -1;
    int error = 0;
    int sizeonly = 0;

    logfile = stderr;
    outfile = stdout;

    memset(&zip_state, 0, sizeof(zip_state));

    while(1)
    {
        c = getopt(argc, argv, ":ql:o:j:t:f:F:m:M:h?s:e:zvO");
        if (-1 == c)
            break;
        
        switch(c)
        {
            case 'v':
                verbose++;
                break;
            case 'q':
                quick = 1;
                break;
            case 'l':
                logfile = fopen(optarg, "ab");
                if (!logfile)
                {
                    fprintf(stderr, "Unable to open log file %s\n", optarg);
                    return errno;
                }
                setlinebuf(logfile);
                break;
            case 'o':
                output_name = optarg;
                outfile = fopen(output_name, "wb");
                if (!outfile)
                {
                    fprintf(logfile, "Unable to open output file %s\n", optarg);
                    return errno;
                }
                break;
            case 'j':
                if (jigdo_filename)
                {
                    fprintf(logfile, "Can only specify one jigdo file!\n");
                    return EINVAL;
                }
                /* else */
                jigdo_filename = optarg;
                break;
            case 't':
                if (template_filename)
                {
                    fprintf(logfile, "Can only specify one template file!\n");
                    return EINVAL;
                }
                /* else */
                template_filename = optarg;
                break;
            case 'f':
                if (md5_filename)
                {
                    fprintf(logfile, "Can only specify one MD5 file!\n");
                    return EINVAL;
                }
                /* else */
                md5_filename = optarg;
                break;                
            case 'F':
                if (sha256_filename)
                {
                    fprintf(logfile, "Can only specify one SHA256 file!\n");
                    return EINVAL;
                }
                /* else */
                sha256_filename = optarg;
                break;
            case 'm':
                error = add_match_entry(strdup(optarg));
                if (error)
                    return error;
                break;
            case 'M':
                missing_filename = optarg;
                break;
            case ':':
                fprintf(logfile, "Missing argument!\n");
                return EINVAL;
                break;
            case 'h':
            case '?':
                usage(argv[0]);
            return 0;
            break;
            case 's':
                start_offset = strtoull(optarg, NULL, 10);
                if (start_offset != 0)
                    quick = 1;
                break;
            case 'e':
                end_offset = strtoull(optarg, NULL, 10);
                if (end_offset != 0)
                    quick = 1;
                break;
            case 'z':
                sizeonly = 1;
                break;
            case 'O':
                check_jigdo_header = 0;
                break;
            default:
                fprintf(logfile, "Unknown option!\n");
                return EINVAL;
        }
    }

    if (0 == end_offset)
        end_offset = LLONG_MAX;

    if ((NULL == jigdo_filename) &&
        (NULL == md5_filename) && 
	(NULL == sha256_filename) &&
        !sizeonly)
    {
        fprintf(logfile, "No jigdo file or MD5/SHA256 file specified!\n");
        usage(argv[0]);
        return EINVAL;
    }
    
    if (NULL == template_filename)
    {
        fprintf(logfile, "No template file specified!\n");
        usage(argv[0]);
        return EINVAL;
    }    

    if (md5_filename)
    {
        /* Build up a list of the files we've been fed */
        error = parse_md5_file(md5_filename);
        if (error)
        {
            fprintf(logfile, "Unable to parse the MD5 file %s, error %d\n", md5_filename, error);
            return error;
        }
    }

    if (sha256_filename)
    {
        /* Build up a list of the files we've been fed */
        error = parse_sha256_file(sha256_filename);
        if (error)
        {
            fprintf(logfile, "Unable to parse the SHA256 file %s, error %d\n", sha256_filename, error);
            return error;
        }
    }

    if (jigdo_filename)
    {
        /* Build up a list of file mappings */
        error = parse_jigdo_file(jigdo_filename);
        if (error)
        {
            fprintf(logfile, "Unable to parse the jigdo file %s\n", jigdo_filename);
            return error;
        }
    }

    if (!output_name)
        output_name = "to stdout";
    /* Read the template file and actually build the image to <outfile> */
    error = parse_template_file(template_filename, sizeonly, missing_filename, output_name);
    if (error)
    {
        fprintf(logfile, "Unable to recreate image from template file %s\n", template_filename);
        if (missing_filename)
            fprintf(logfile, "%s contains the list of missing files\n", missing_filename);
        return error;
    }        

    fclose(logfile);
    return 0;
}

