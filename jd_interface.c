/*
 * jd_interface.c
 *
 * Interfaces for building an ISO image, using the data stored in the DB
 *
 * Copyright (c) 2005 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "endian.h"
#include "jigdb.h"
#include "jte.h"
#include "md5.h"

typedef struct _jd_state
{
    char  *template_filename;
    FILE  *template;
    char   last_filename[PATH_MAX];
    JIGDB *dbp;
    db_template_entry_t db_info;
} jd_state_t;

typedef struct _jd_compressed_cache
{
    unsigned long long uncomp_offset;
    unsigned char *buffer;
    unsigned char template_id[33];
} jd_compressed_cache_t;

int M_jd_cache_size = 0;
int M_jd_cache_used = 0;
jd_compressed_cache_t *M_compressed_cache = NULL;

int jd_timer_init(jd_time_t *timer)
{
    int error = 0;
    error = gettimeofday(&timer->tv, &timer->tz);
    return error;
}

INT64 jd_timer_count(jd_time_t *timer)
{
    int error = 0;
    struct timeval new_tv;
    struct timezone new_tz;
    INT64 count;
    
    error = gettimeofday(&new_tv, &new_tz);
    if (error)
        return -1;
    
    count = (new_tv.tv_sec * 1000000) + new_tv.tv_usec;
    count -= (timer->tv.tv_sec * 1000000);
    count -= timer->tv.tv_usec;
    
    return count;
}

int jd_init(int cache_size)
{
    if (M_jd_cache_size)
        return EINVAL; /* Already initialised! */

    M_jd_cache_size = cache_size;    
    M_compressed_cache = calloc(sizeof(jd_compressed_cache_t), M_jd_cache_size);
    if (!M_compressed_cache)
        return ENOMEM;

    return 0;
}

/* Look in the cache for the appropriate buffer */
static int jd_lookup_compressed_cache(db_compressed_entry_t *comp_entry, unsigned char **buffer)
{
    int i = 0;
    int j = 0;
    jd_compressed_cache_t tmp;

#ifdef DEBUG
    fprintf(stderr, "jd_lookup_compressed_cache: Looking up template %s, offset %lld\n",
            comp_entry->template_id, comp_entry->uncomp_offset);    
#endif
    for (i = 0; i < M_jd_cache_used; i++)
    {
        if ( (comp_entry->uncomp_offset == M_compressed_cache[i].uncomp_offset) &&
             (!strcmp(comp_entry->template_id, M_compressed_cache[i].template_id)) )
        {
#ifdef DEBUG
            fprintf(stderr, "jd_lookup_compressed_cache: (%d) Found template %s, offset %lld\n",
                    i, comp_entry->template_id, comp_entry->uncomp_offset);    
#endif
            /* Found the block. Put it first in the array, for LRU
             * behaviour */
            memcpy(&tmp, &M_compressed_cache[i], sizeof(tmp));
            for (j = i; j ; j--)
                memcpy(&M_compressed_cache[j], &M_compressed_cache[j-1], sizeof(tmp));
            memcpy(&M_compressed_cache[0], &tmp, sizeof(tmp));                    
            *buffer = tmp.buffer;
            return 0;
        }
    }    
    return ENOENT;
}

/* Add the uncompressed buffer to our cache */
static int jd_add_to_compressed_cache(db_compressed_entry_t *comp_entry, unsigned char *buffer)
{
    int i = 0;

    /* If we don't have a cache configured, return immediately */
    if (!M_jd_cache_size)
        return 0;

#ifdef DEBUG
    fprintf(stderr, "jd_add_to_compressed_cache: M_jd_cache_size %d, M_jd_cache_used %d\n",
            M_jd_cache_size, M_jd_cache_used);
    fprintf(stderr, "Adding entry template_id %s, offset %lld, buffer %p\n",
            comp_entry->template_id, comp_entry->uncomp_offset, buffer);
#endif
    if (M_compressed_cache[M_jd_cache_size-1].buffer)
        free(M_compressed_cache[M_jd_cache_size-1].buffer);

    if (M_jd_cache_size > 1)
        for (i = M_jd_cache_size - 1; i ; i--)
            memcpy(&M_compressed_cache[i], &M_compressed_cache[i-1], sizeof(jd_compressed_cache_t));
        
    strcpy(M_compressed_cache[0].template_id, comp_entry->template_id);
    M_compressed_cache[0].uncomp_offset = comp_entry->uncomp_offset;
    M_compressed_cache[0].buffer = buffer;

    if (M_jd_cache_size > M_jd_cache_used)
        M_jd_cache_used++;
    
    return 0;
}
    
JD *jd_open(JIGDB *dbp, char *template_file)
{
    jd_state_t *jd = NULL;
    
    jd = calloc(1, sizeof(jd_state_t));
    if (!jd)
    {
        errno = ENOMEM;
        return NULL;
    }
    
    jd->template_filename = template_file;
    jd->template = fopen(template_file, "rb");
    if (!jd->template)
    {
        free(jd);
        return NULL;
    }
    jd->dbp = dbp;
    if (db_lookup_template_by_path(dbp, template_file, &jd->db_info))
    {
        fclose(jd->template);
        free(jd);
        return NULL;
    }

    return jd;
}

int jd_close(JD *state)
{
    jd_state_t *jd = state;
    
    if (jd)
    {
        if (jd->template)
            fclose(jd->template);
        free(jd);
    }
    return 0;
}

int jd_md5(JD *state, unsigned char md5[16])
{
    jd_state_t *jd = state;
    unsigned char *md5_out = NULL;
    if (!jd)
        return EBADF;

    md5_out = hex_parse(jd->db_info.image_md5, sizeof(md5));
    memcpy(md5, md5_out, sizeof(md5));
    free(md5_out);

    return 0;
}    

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

static int jd_read_file(jd_state_t *state, unsigned char *md5, INT64 file_offset,
                        INT64 length, unsigned char *buffer, INT64 *bytes_read)
{
    int error = 0;
    FILE *file = NULL;
    db_file_entry_t file_entry;
    size_t num_read = 0;
    
    error = db_lookup_file_by_md5(state->dbp, md5, &file_entry);
    if (error)
        return error;

    file = fopen(file_entry.filename, "rb");
    if (!file)
        return errno;

#ifdef DEBUG
    fprintf(stderr, "jd_read_file: reading file %s, offset %lld\n", file_entry.filename, file_offset);    
#endif

    error = fseek(file, file_offset, SEEK_SET);
    if (error)
        return error;
    
    num_read = fread(buffer, 1, length, file);
    if (num_read < 0)
    {
        fclose(file);
        return errno;
    }
    *bytes_read = num_read;
    fclose(file);
    strncpy(state->last_filename, file_base_name(file_entry.filename), sizeof(state->last_filename));
    
    return error;
}

static int jd_read_compressed(jd_state_t *state, INT64 uncomp_offset,
                              INT64 length, unsigned char *buffer, INT64 *bytes_read)
{
    int error = 0;
    db_compressed_entry_t comp_entry;
    size_t num_read = 0;
    unsigned char *comp_buf = NULL;
    unsigned char *uncomp_buf = NULL;
    size_t bytes_copied = 0;

    error = db_lookup_compressed_by_offset(state->dbp, uncomp_offset,
                                           state->db_info.template_md5, &comp_entry);
    if (error)
    {
        fprintf(stderr, "jd_read_compressed: unable to find a comp match for offset %lld, error %d\n",
                uncomp_offset, error);
        return error;
    }

#ifdef DEBUG
    fprintf(stderr, "jd_read_compressed: reading compressed entry @%lld, (uncomp %lld, length %lld), offset %lld\n", comp_entry.comp_offset, comp_entry.uncomp_offset, comp_entry.uncomp_size, uncomp_offset);
#endif

    /* Check the uncompressed cache first */
    error = jd_lookup_compressed_cache(&comp_entry, &uncomp_buf);
    if (error)
    {
        fprintf(stderr, "CACHE MISS!\n");

        error = fseek(state->template, comp_entry.comp_offset, SEEK_SET);
        if (error)
        {
            fprintf(stderr, "jd_read_compressed: unable to seek! error %d\n", error);
            return error;
        }
    
        comp_buf = malloc(comp_entry.comp_size);
        if (!comp_buf)
            return ENOMEM;
        uncomp_buf = malloc(comp_entry.uncomp_size);
        if (!uncomp_buf)
        {
            free(comp_buf);
            return ENOMEM;
        }

        num_read = fread(comp_buf, 1, comp_entry.comp_size, state->template);
        if (num_read != comp_entry.comp_size)
        {
            fprintf(stderr, "jd_read_compressed: asked to read %lld bytes, got %d\n",
                    comp_entry.comp_size, num_read);
            free(comp_buf);
            free(uncomp_buf);
            return EIO;
        }
        error = decompress_data_block(comp_buf, comp_entry.comp_size, uncomp_buf,
                                      comp_entry.uncomp_size, comp_entry.comp_type);
        if (error)
        {
            fprintf(stderr, "jd_read_compressed: failed to decompress, error %d\n",
                    error);
            free(comp_buf);
            free(uncomp_buf);
            return error;
        }

        if (M_jd_cache_size)
        {
            error = jd_add_to_compressed_cache(&comp_entry, uncomp_buf);
            if (error)
            {
                fprintf(stderr, "jd_read_compressed: failed to add to the compressed cache, error %d\n",
                        error);
                free(comp_buf);
                free(uncomp_buf);
                return error;
            }        
        }
        free(comp_buf);
    }
    
    uncomp_offset -= comp_entry.uncomp_offset;

    strcpy(state->last_filename, "template data");
    bytes_copied = length;
    if (length > (comp_entry.uncomp_size - uncomp_offset))
        bytes_copied = comp_entry.uncomp_size - uncomp_offset;
    memcpy(buffer, uncomp_buf + uncomp_offset, bytes_copied);
    *bytes_read = bytes_copied;

    if (!M_jd_cache_size)
        free(uncomp_buf);

    return 0;
}
    
int jd_read(JD *state, INT64 start_offset, INT64 length, unsigned char *buffer, INT64 *bytes_read)
{
    jd_state_t *jd = state;
    db_block_entry_t block;
    int error = 0;
    INT64 block_offset = 0;
    jd_time_t jd_time;

    if (!jd)
        return EBADF;

    jd_timer_init(&jd_time);
    /* Lookup the appropriate block */
    error = db_lookup_block_by_offset(jd->dbp, start_offset,
                                      jd->db_info.template_md5, &block);
    if (error)
        return error;    

#ifdef DEBUG
    fprintf(stderr, "jd_read: image offset %lld, block details:\n", start_offset);
    fprintf(stderr, "         block start offset %lld, type %d\n", block.image_offset, block.type);
    fprintf(stderr, "         took %lld usec\n", jd_timer_count(&jd_time));
#endif

    block_offset = start_offset - block.image_offset;
    
    if (length > (block.size - block_offset))
        length = block.size - block_offset;

    jd_timer_init(&jd_time);
    switch (block.type)
    {
        case BT_FILE:
            error = jd_read_file(jd, block.md5, block_offset, length, buffer, bytes_read);
            break;
        case BT_BLOCK:
            error = jd_read_compressed(jd, block.uncomp_offset + block_offset, length,
                                       buffer, bytes_read);
            break;
        default:
            fprintf(stderr, "Unknown block type %d!\n", block.type);
            return EINVAL;
    }

#ifdef DEBUG
    fprintf(stderr, "jd_read: got %lld bytes\n", *bytes_read);
    fprintf(stderr, "         took %lld usec\n", jd_timer_count(&jd_time));
#endif
    return error;
}

int jd_size(JD *state, INT64 *size)
{
    jd_state_t *jd = state;
    if (!jd)
        return EBADF;

    *size = jd->db_info.image_size;

    return 0;
}    

int jd_last_filename(JD *state, char **name)
{
    jd_state_t *jd = state;
    if (!jd)
        return EBADF;

    *name = jd->last_filename;

    return 0;    
}

