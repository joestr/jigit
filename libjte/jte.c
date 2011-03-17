/*
 * jte.c
 *
 * 
 * Copyright (c) 2004-2006 Steve McIntyre <steve@einval.com>
 * Copyright (c) 2010-2011 Thomas Schmitt <scdbackup@gmx.net>
 * Copyright (c) 2010-2011 George Danchev <danchev@spnet.net>
 * 
 * These routines were originally implemented by 
 * Steve McIntyre <steve@einval.com>. 
 * More recently few tweaks and additions were applied by 
 * Thomas Schmitt <scdbackup@gmx.net> and
 * George Danchev <danchev@spnet.net>
 * 
 * Implementation of the Jigdo Template Engine - make jigdo files
 * directly when making ISO images
 *
 * GNU GPL v2+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif
#endif

#include <string.h>
#include <stdlib.h>

#ifdef LIBJTE_WITH_ZLIB
#include <zlib.h>
#endif

#ifdef LIBJTE_WITH_LIBBZ2
#include <bzlib.h>
#endif

#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include "jte.h"
#include "checksum.h"
#include "endianconv.h"
#include "rsync.h"
#include "md5.h"
#include "libjte_private.h"
#include "libjte.h"

/* #include "version.h" */                 /* generated by autotools */

/* Different types used in building our state list below */
#define JTET_FILE_MATCH 1
#define JTET_NOMATCH    2

#define JTE_VER_MAJOR     0x0001
#define JTE_VER_MINOR     0x0013
#define JTE_NAME          "JTE"
#define JTE_COMMENT       "JTE at http://www.einval.com/~steve/software/JTE/ ; jigdo at http://atterer.org/jigdo/"

#define JIGDO_TEMPLATE_VERSION "1.1"

#define JTE_MAX_ERROR_LIST_LENGTH 20


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

static void exit_if_enabled(struct libjte_env *o, int value)
{
    if (!(o->error_behavior & 2))
        return;
    libjte_clear_msg_list(0, 1);
    exit(value);
}

int libjte_report_no_mem(struct libjte_env *o, size_t size, int flag)
{
    sprintf(o->message_buffer, "Out of memory for %.f new bytes",
            (double) size);
    libjte_add_msg_entry(o, o->message_buffer, 0);
    return -1;
}

/* @param flag bit0-7 = mode
                        0= include_list
                        1= exclude_list
*/
static int jte_add_path_match(struct libjte_env *o, char *pattern, int flag)
{
    struct path_match *new = NULL, **old;
    int mode;

    mode = flag & 255;
    if (mode == 0)
        old = &(o->include_list);
    else
        old = &(o->exclude_list);

    new = malloc(sizeof *new);
    if (new == NULL)
        return libjte_report_no_mem(o, sizeof *new, 0);
    
    regcomp(&new->match_pattern, pattern, REG_NEWLINE);
    new->match_rule = strdup(pattern);
    if (new->match_rule == NULL)
        return libjte_report_no_mem(o, sizeof *new, 0);

    /* Order on the list doesn't matter! */
    new->next = *old;

    *old = new;
    return 0;
}

/* @param flag bit0-7 = mode
                        0= include_list
                        1= exclude_list
*/
int libjte_destroy_path_match_list(struct libjte_env *o, int flag)
{
    struct path_match **old, *s, *s_next;
    int mode;

    mode = flag & 255;
    if (mode == 0)
        old = &(o->include_list);
    else
        old = &(o->exclude_list);
    for (s = *old; s != NULL; s = s_next) {
        s_next = s->next;
        regfree(&(s->match_pattern));
        free(s->match_rule);
        free(s);
    }
    *old = NULL;
    return 1;
}

/* Build the list of exclusion regexps */
extern int jte_add_exclude(struct libjte_env *o, char *pattern)
{
    int ret;
    
    ret = jte_add_path_match(o, pattern, 1);
    return ret;
}

/* Check if the file should be excluded because of a filename match. 1
   means exclude, 0 means not */
static int check_exclude_by_name(struct libjte_env *o,
                                 char *filename, char **matched)
{
    struct path_match *ptr = o->exclude_list;
    regmatch_t pmatch[1];

    while (ptr)
    {
        if (!regexec(&ptr->match_pattern, filename, 1, pmatch, 0))
        {
            *matched = ptr->match_rule;
            return 1;
        }
        ptr = ptr->next;
    }
    
    /* Not matched, so return 0 */
    return 0;
}

/* Build the list of required inclusion regexps */
extern int jte_add_include(struct libjte_env *o, char *pattern)
{
    int ret;
    
    ret = jte_add_path_match(o, pattern, 0);
    return ret;
}

int libjte_destroy_entry_list(struct libjte_env *o, int flag)
{
    entry_t *s, *s_next;

    for (s = o->entry_list ; s != NULL; s = s_next) {
        s_next = s->next;
        if (s->entry_type == JTET_FILE_MATCH) {
            if (s->data.file.filename != NULL)
                free(s->data.file.filename);
        }
        free(s);
    }
    o->entry_list = o->entry_last = NULL;
    return 1;
}

/* Check if a file has to be MD5-matched to be valid. If we get called
   here, we've failed to match any of the MD5 entries we were
   given. If the path to the filename matches one of the paths in our
   list, clearly it must have been corrupted. Abort with an error. */
static int check_md5_file_match(struct libjte_env *o, char *filename)
{
    struct path_match *ptr = o->include_list;
    regmatch_t pmatch[1];

    while (ptr)
    {
        if (!regexec(&ptr->match_pattern, filename, 1, pmatch, 0))
        {
			sprintf(o->message_buffer,
				"File %1.1024s should have matched an MD5 entry, but didn't! (Rule '%1.1024s')",
				filename, ptr->match_rule);
			libjte_add_msg_entry(o, o->message_buffer, 0);
			exit_if_enabled(o, 1);
			return -1;
	}
        ptr = ptr->next;
    }
    return 0;
}    

/* Should we list a file separately in the jigdo output, or should we
   just dump it into the template file as binary data? Three things
   cases to look for here:

   1. Small files are better simply folded in, as they take less space that way.

   2. Files in /doc (for example) may change in the archive all the
      time and it's better to not have to fetch snapshot copies if we
      can avoid it.      

   3. Files living in specified paths *must* match an entry in the
      md5-list, or they must have been corrupted. If we find a corrupt
      file, bail out with an error.

*/
extern int list_file_in_jigdo(struct libjte_env *o,
            char *filename, off_t size, char **realname, unsigned char md5[16])
{
    char *matched_rule;
    md5_list_entry_t *entry = o->md5_list;
    int md5sum_done = 0, ret;
    
    if (o->jtemplate_out == NULL)
        return 0;

    memset(md5, 0, sizeof(md5));

    /* Cheaper to check file size first */
    if (size < o->jte_min_size)
    {
        if (o->verbose > 1) {
            sprintf(o->message_buffer,
                   "Jigdo-ignoring file %1.1024s; it's too small", filename);
            libjte_add_msg_entry(o, o->message_buffer, 0);
        }
        return 0;
    }
    
    /* Now check the excluded list by name */
    if (check_exclude_by_name(o, filename, &matched_rule))
    {
        if (o->verbose > 1) {
            sprintf(o->message_buffer,
                    "Jigdo-ignoring file %1.1024s; it's covered in the exclude list by \"%1.1024s\"",
                    filename, matched_rule);
            libjte_add_msg_entry(o, o->message_buffer, 0);
        }
        return 0;
    }

    /* Check to see if the file is in our md5 list. Check three things:
       
       1. the size
       2. the filename
       3. (only if the first 2 match) the md5sum

       If we get a match for all three, include the file and return
       the full path to the file that we have gleaned from the mirror.
    */

    while (entry)
    {
        if (size == entry->size)
        {
            if (!strcmp(file_base_name(filename), file_base_name(entry->filename)))
            {
                if (!md5sum_done)
                {
                    ret = calculate_md5sum(filename, size, md5);
                    if (ret < 0) { /* (0 is success) */
                        sprintf(o->message_buffer,
                                "Error with file '%1.1024s' : errno=%d",
                                filename, errno);
                        libjte_add_msg_entry(o, o->message_buffer, 0);
                        return -1;
                    }
                    md5sum_done = 1;
                }
                if (!memcmp(md5, entry->MD5, sizeof(entry->MD5)))
                {
                    *realname = entry->filename;
                    return 1;
                }
            }
        }
        entry = entry->next;
    }

    /* We haven't found an entry in our MD5 list to match this
     * file. If we should have done, complain and bail out. */
    ret = check_md5_file_match(o, filename);
    return ret;
}

/* Add a mapping of pathnames (e.g. Debian=/mirror/debian). We should
   be passed TO=FROM here */
extern int jte_add_mapping(struct libjte_env *o, char *arg)
{
    struct path_mapping *new = NULL;
    struct path_mapping *entry = NULL;
    char *p = arg;
    char *from = NULL;
    char *to = NULL;
    char *eqpt = NULL;

    /* Find the "=" in the string passed. */
    while (*p)
    {
        if ('=' == *p)
        {
            if (eqpt == NULL)
                eqpt = p;
            p++;
            from = p;
            break;
        }
        p++;
    }
    if (from == NULL || !strlen(from) || eqpt == arg)
        return EINVAL;
    from = strdup(from);
    if (from == NULL)
        return ENOMEM;
    to = calloc(1, (eqpt - arg) + 1);
    if (to == NULL) {
        free(from);
        return ENOMEM;
    }
    strncpy(to, arg, eqpt - arg);
    to[eqpt - arg] = 0;
    
    new = malloc(sizeof(*new));
    if (!new) {
        free(to);
        free(from);
        return ENOMEM;
    }
    new->from = from;
    new->to = to;
    new->next = NULL;

    if (o->verbose > 0) {
        sprintf(o->message_buffer,
                "Adding mapping from %1.1024s to %1.1024s for the jigdo file",
                from, to);
        libjte_add_msg_entry(o, o->message_buffer, 0);
    }
    if (o->map_list == NULL)
        o->map_list = new;
    else
    {
        /* Order is important; add to the end of the list */
        entry = o->map_list;
        while (NULL != entry->next)
            entry = entry->next;
        entry->next = new;
    }
    return 0;
}


int libjte_destroy_path_mapping(struct libjte_env *o, int flag)
{
    struct path_mapping *s, *s_next;

    for (s = o->map_list; s != NULL; s = s_next) {
        s_next = s->next;
        free(s->from);
        free(s->to);
        free(s);
    }
    o->map_list = NULL;
    return 1;
}


/* Check if the filename should be remapped; if so map it, otherwise
   return the original name. */
static char *remap_filename(struct libjte_env *o, char *filename)
{
    char *new_name = filename;
    struct path_mapping *entry = o->map_list;
    
    while (entry)
    {
        if (!strncmp(filename, entry->from, strlen(entry->from)))
        {
            new_name = calloc(1, 2 + strlen(filename) + strlen(entry->to) - strlen(entry->from));
            if (!new_name)
            {
                sprintf(o->message_buffer,
                        "Failed to malloc new filename; abort!");
                libjte_add_msg_entry(o, o->message_buffer, 0);
                exit_if_enabled(o, 1);
                return NULL;
            }
            sprintf(new_name, "%s:%s", entry->to, &filename[strlen(entry->from)]);
            return new_name;
        }
        entry = entry->next;
    }

    /* No mapping in effect */
    return strdup(filename);
}    

/* Write data to the template file and update the MD5 sum */
static int template_fwrite(struct libjte_env *o,
                      const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t written;

    checksum_update(o->template_context, ptr, size * nmemb);
    written = fwrite(ptr, size, nmemb, stream);
    o->template_size += written * size;
    if (written != nmemb)
        return 0;
    return 1;
}

/* Create a new template file and initialise it */
static int write_template_header(struct libjte_env *o)
{
    char buf[2048];
    int i = 0;
    char *p = buf;

#ifndef LIBJTE_WITH_LIBBZ2

    if (o->jte_template_compression == JTE_TEMP_BZIP2) {
        sprintf(o->message_buffer,
            "libjte: Compression algorithm BZIP2 not enabled at compile time");
        libjte_add_msg_entry(o, o->message_buffer, 0);
        exit_if_enabled(o, 1);
        return -1;
    }

#endif /* LIBJTE_WITH_LIBBZ2 */

    memset(buf, 0, sizeof(buf));

    if (o->template_context != NULL)
        checksum_free_context(o->template_context);
    o->template_context = checksum_init_context(o->checksum_algo_tmpl,
                                                "template");
    if (o->template_context == NULL)
    {
        sprintf(o->message_buffer,
                "cannot allocate template checksum contexts");
        libjte_add_msg_entry(o, o->message_buffer, 0);
        exit_if_enabled(o, 1);
        return -1;
    }
    
    i += sprintf(p, "JigsawDownload template %s libjte-%d.%d.%d \r\n",
             JIGDO_TEMPLATE_VERSION, 
             LIBJTE_VERSION_MAJOR, LIBJTE_VERSION_MINOR, LIBJTE_VERSION_MICRO);
    p = &buf[i];

    i += sprintf(p, "%s \r\n", JTE_COMMENT);
    p = &buf[i];

    i += sprintf(p, "\r\n");
    if (template_fwrite(o, buf, i, 1, o->t_file) <= 0)
        return 0;
    return 1;
}

/* Read the MD5 list and build a list in memory for us to use later */
static void add_md5_entry(struct libjte_env *o,
                   unsigned char *md5, uint64_t size, char *filename)
{
    md5_list_entry_t *new = NULL;
    
    new = calloc(1, sizeof(md5_list_entry_t));
    memcpy(new->MD5, md5, sizeof(new->MD5));
    new->size = size;
    new->filename = strdup(filename);
    
    /* Add to the end of the list */
    if (NULL == o->md5_last)
    {
        o->md5_last = new;
        o->md5_list = new;
    }
    else
    {
        o->md5_last->next = new;
        o->md5_last = new;
    }
}

int libjte_destroy_md5_list(struct libjte_env *o, int flag)
{
    md5_list_entry_t *s, *s_next;

    for (s = o->md5_list; s != NULL; s = s_next) {
        s_next = s->next;
        free(s->filename);
        free(s);
    }
    o->md5_list = o->md5_last = NULL;
    return 1;
}

/* Parse a 12-digit decimal number */
static uint64_t parse_number(unsigned char in[12])
{
    uint64_t size = 0;
    int i = 0;
    
    for (i = 0; i < 12; i++)
    {
        size *= 10;
        if (isdigit(in[i]))
            size += (in[i] - '0');
    }

    return size;
}
    
/* Read the MD5 list and build a list in memory for us to use later
   MD5 list format:

   <---MD5--->  <--Size-->  <--Filename-->
       32          12          remaining
*/
static int parse_md5_list(struct libjte_env *o)
{
    FILE *md5_file = NULL;
    unsigned char buf[1024];
    unsigned char md5[16];
    char *filename = NULL;
    unsigned char *numbuf = NULL;
    int num_files = 0;
    uint64_t size = 0;

    md5_file = fopen(o->jmd5_list, "rb");
    if (!md5_file)
    {
        sprintf(o->message_buffer, "cannot read from MD5 list file '%1.1024s'",
                o->jmd5_list);
        libjte_add_msg_entry(o, o->message_buffer, 0);
        exit_if_enabled(o, 1);
        return -1;
    }

    memset(buf, 0, sizeof(buf));
    while (fgets((char *)buf, sizeof(buf), md5_file))
    {
        numbuf = &buf[34];
        filename = (char *)&buf[48];
        /* Lose the trailing \n from the fgets() call */
        if (buf[strlen((char *)buf)-1] == '\n')
	  buf[strlen((char *)buf)-1] = 0;

        if (mk_MD5Parse(buf, md5))
        {
            sprintf(o->message_buffer, "cannot parse MD5 file '%1.1024s'",
                    o->jmd5_list);
            libjte_add_msg_entry(o, o->message_buffer, 0);
            exit_if_enabled(o, 1);
            return -1;
        }
        size = parse_number(numbuf);
        add_md5_entry(o, md5, size, filename);
        memset(buf, 0, sizeof(buf));
        num_files++;
    }
    if (o->verbose > 0) {
        sprintf(o->message_buffer,
              "parse_md5_list: added MD5 checksums for %d files", num_files);
        libjte_add_msg_entry(o, o->message_buffer, 0);
    }
    fclose(md5_file);
    return 1;
}

/* Initialise state and start the jigdo template file */
int write_jt_header(struct libjte_env *o,
                     FILE *template_file, FILE *jigdo_file)
{
    int ret;

    o->t_file = template_file;
    o->j_file = jigdo_file;

    /* Start checksum work for the image */
    if (o->iso_context != NULL)
        checksum_free_context(o->iso_context);
    o->iso_context = checksum_init_context(o->checksum_algo_iso, "iso");
    if (o->iso_context == NULL)
    {
        sprintf(o->message_buffer, "cannot allocate iso checksum contexts");
        libjte_add_msg_entry(o, o->message_buffer, 0);
        exit_if_enabled(o, 1);
        return -1;
    }

    /* Start the template file */
    ret = write_template_header(o);
    if (ret <= 0)
        return ret;

    /* Load up the MD5 list if we've been given one */
    if (o->jmd5_list) {
        ret = parse_md5_list(o);
        if (ret <= 0)
            return ret;
    }
    return 1;
}

/* Compress and flush out a buffer full of template data */
static int flush_gzip_chunk(struct libjte_env *o, void *buffer, off_t size)
{

#ifdef LIBJTE_WITH_ZLIB

    z_stream c_stream; /* compression stream */
    unsigned char comp_size_out[6];
    unsigned char uncomp_size_out[6];
    off_t compressed_size_out = 0;
    int err = 0;
    unsigned char *comp_buf = NULL;

    c_stream.zalloc = NULL;
    c_stream.zfree = NULL;
    c_stream.opaque = NULL;

    err = deflateInit(&c_stream, Z_BEST_COMPRESSION);
    if (NULL == (comp_buf = malloc(2 * size))) /* Worst case */
        return 0;
    c_stream.next_out = comp_buf;
    c_stream.avail_out = 2 * size;
    c_stream.next_in = buffer;
    c_stream.avail_in = size;
    
    err = deflate(&c_stream, Z_NO_FLUSH);
    err = deflate(&c_stream, Z_FINISH);
    
    compressed_size_out = c_stream.total_out + 16;
    err = deflateEnd(&c_stream);

    if (template_fwrite(o, "DATA", 4, 1, o->t_file) <= 0)
        return 0;

    write_le48(compressed_size_out, &comp_size_out[0]);
    if (template_fwrite(o, comp_size_out, sizeof(comp_size_out), 1, o->t_file)
        <= 0)
        return 0;

    write_le48(size, &uncomp_size_out[0]);
    if (template_fwrite(o, uncomp_size_out, sizeof(uncomp_size_out), 1, 
                        o->t_file) <= 0)
        return 0;
    
    if (template_fwrite(o, comp_buf, c_stream.total_out, 1, o->t_file) <= 0)
        return 0;
    free(comp_buf);
    return 1;

#else /* LIBJTE_WITH_ZLIB */

    static int complaints = 0;

    if (complaints >= 3)
        return 0;
    complaints++;
    fprintf(stderr,
            "\nlibjte: Configuration error. Use without enabled zlib\n\n");
    return 0;

#endif /* ! LIBJTE_WITH_ZLIB */

}


#ifdef LIBJTE_WITH_LIBBZ2

/* Compress and flush out a buffer full of template data */
static int flush_bz2_chunk(struct libjte_env *o, void *buffer, off_t size)
{
    bz_stream c_stream; /* compression stream */
    unsigned char comp_size_out[6];
    unsigned char uncomp_size_out[6];
    off_t compressed_size_out = 0;
    int err = 0;
    char *comp_buf = NULL;

    c_stream.bzalloc = NULL;
    c_stream.bzfree = NULL;
    c_stream.opaque = NULL;

    err = BZ2_bzCompressInit(&c_stream, 9, 0, 0);
    if (NULL == (comp_buf = (char*) malloc(2 * size))) /* Worst case */
        return 0;
    c_stream.next_out = comp_buf;
    c_stream.avail_out = 2 * size;
    c_stream.next_in = buffer;
    c_stream.avail_in = size;
    
    err = BZ2_bzCompress(&c_stream, BZ_FINISH);
    
    compressed_size_out = c_stream.total_out_lo32 + 16;
    err = BZ2_bzCompressEnd(&c_stream);

    if (template_fwrite(o, "BZIP", 4, 1, o->t_file) <= 0)
        return 0;

    write_le48(compressed_size_out, &comp_size_out[0]);
    if (template_fwrite(o, comp_size_out, sizeof(comp_size_out), 1, 
                        o->t_file) <= 0)
        return 0;

    write_le48(size, &uncomp_size_out[0]);
    if (template_fwrite(o, uncomp_size_out, sizeof(uncomp_size_out), 1,
                        o->t_file) <= 0)
        return 0;
    
    if (template_fwrite(o, comp_buf, c_stream.total_out_lo32, 1,
                        o->t_file) <= 0)
        return 0;
    free(comp_buf);
    return 1;
}

#else /* LIBJTE_WITH_LIBBZ2 */

/* Compress and flush out a buffer full of template data */
static int flush_bz2_chunk(struct libjte_env *o, void *buffer, off_t size)
{
    return 0;
}

#endif /* ! LIBJTE_WITH_LIBBZ2 */


static int flush_compressed_chunk(struct libjte_env *o,
                                  void *buffer, off_t size)
{
    int ret;

    if (size <= 0)
        return 1;

    if (o->jte_template_compression == JTE_TEMP_BZIP2)
        ret = flush_bz2_chunk(o, buffer, size);
    else
        ret = flush_gzip_chunk(o, buffer, size);
    return ret;
}

/* Append to an existing data buffer, and compress/flush it if
   necessary */
static int write_compressed_chunk(struct libjte_env *o,
                                   unsigned char *buffer, size_t size)
{
    int ret;

	if (o->uncomp_buf == NULL)
	{
		if (o->jte_template_compression == JTE_TEMP_BZIP2)
			o->uncomp_size = 900 * 1024;
		else
			o->uncomp_size = 1024 * 1024;
		o->uncomp_buf = malloc(o->uncomp_size);
		if (o->uncomp_buf == NULL)
		{
                   sprintf(o->message_buffer,
                "failed to allocate %lu bytes for template compression buffer",
                           (unsigned long) o->uncomp_size);
                   libjte_add_msg_entry(o, o->message_buffer, 0);
                   exit_if_enabled(o, 1);
                   return -1;
		}
	}

    if ((o->uncomp_buf_used + size) > o->uncomp_size)
    {
        ret = flush_compressed_chunk(o, o->uncomp_buf, o->uncomp_buf_used);
        if (ret <= 0)
            return ret;
        o->uncomp_buf_used = 0;
    }

    if (!size) /* Signal a flush before we start writing the DESC entry */
    {
        ret = flush_compressed_chunk(o, o->uncomp_buf, o->uncomp_buf_used);
        if (ret <= 0)
            return ret;
        return 1;
    }
    
    if (!o->uncomp_buf_used)
        memset(o->uncomp_buf, 0, o->uncomp_size);

    while (size > o->uncomp_size)
    {
        ret = flush_compressed_chunk(o, buffer, o->uncomp_size);
        if (ret <= 0)
            return ret;
        buffer += o->uncomp_size;
        size -= o->uncomp_size;
    }
    memcpy(&(o->uncomp_buf[o->uncomp_buf_used]), buffer, size);
    o->uncomp_buf_used += size;
    return 1;
}

/* Loop through the list of DESC entries that we've built up and
   append them to the template file */
static int write_template_desc_entries(struct libjte_env *o, off_t image_len)
{
    entry_t *entry = o->entry_list;
    off_t desc_len = 0;
    unsigned char out_len[6];
    jigdo_image_entry_t jimage;
    int ret;

    desc_len = 16 /* DESC + length twice */
        + (sizeof(jigdo_file_entry_t) * o->num_matches)
        + (sizeof(jigdo_chunk_entry_t) * o->num_chunks)
        + sizeof(jigdo_image_entry_t);

    write_le48(desc_len, &out_len[0]);
    ret = write_compressed_chunk(o, NULL, 0);
    if (ret <= 0)
        return ret;
    if (template_fwrite(o, "DESC", 4, 1, o->t_file) <= 0)
        return 0;
    if (template_fwrite(o, out_len, sizeof(out_len), 1, o->t_file) <= 0)
        return 0;
    
    while (entry)
    {
        switch (entry->entry_type)
        {
            case JTET_FILE_MATCH:
            {
                jigdo_file_entry_t jfile;
                jfile.type = 6; /* Matched file */
                write_le48(entry->data.file.file_length, &jfile.fileLen[0]);
                write_le64(entry->data.file.rsyncsum, &jfile.fileRsync[0]);
                memcpy(jfile.fileMD5, entry->data.file.md5, sizeof(jfile.fileMD5));
                if (template_fwrite(o, &jfile, sizeof(jfile), 1,
                                    o->t_file) <= 0)
                    return 0;
                break;
            }
            case JTET_NOMATCH:
            {
                jigdo_chunk_entry_t jchunk;
				jchunk.type = 2; /* Raw data, compressed */
                write_le48(entry->data.chunk.uncompressed_length, &jchunk.skipLen[0]);
                if (template_fwrite(o, &jchunk, sizeof(jchunk), 1,
                                    o->t_file) <= 0)
                    return 0;
                break;
            }
        }
        entry = entry->next;
    }

    jimage.type = 5;
    write_le48(image_len, &jimage.imageLen[0]);
    checksum_copy(o->iso_context, CHECK_MD5, &jimage.imageMD5[0]);
    write_le32(MIN_JIGDO_FILE_SIZE, &jimage.blockLen[0]);
    if (template_fwrite(o, &jimage, sizeof(jimage), 1, o->t_file) <= 0)
        return 0;
    if(template_fwrite(o, out_len, sizeof(out_len), 1, o->t_file) <= 0)
        return 0;
    return 1;
}

/* Dump a buffer in jigdo-style "base64" */
static char *base64_dump(struct libjte_env *o,
                         unsigned char *buf, size_t buf_size)
{
    const char *b64_enc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int value = 0;
    unsigned int i;
    int bits = 0;
    char *output_buffer = NULL;
    int output_buffer_size;
    char *p = NULL;

    output_buffer_size = buf_size * 8 / 6 + 1 + 1; /* round up , care for 0 */
    p = output_buffer = calloc(1, output_buffer_size);
    if (output_buffer == NULL)
    {
        sprintf(o->message_buffer,
                "base64_dump: Out of memory for buffer size %d",
                output_buffer_size);
        libjte_add_msg_entry(o, o->message_buffer, 0);
        exit_if_enabled(o, 1);
        return NULL;
    }
    memset(output_buffer, 0, output_buffer_size);

    for (i = 0; i < buf_size ; i++)
    {
        value = (value << 8) | buf[i];
        bits += 2;
        p += sprintf(p, "%c", b64_enc[(value >> bits) & 63U]);
        if (bits >= 6) {
            bits -= 6;
            p += sprintf(p, "%c", b64_enc[(value >> bits) & 63U]);
        }
    }
    if (bits > 0)
    {
        value <<= 6 - bits;
        p += sprintf(p, "%c", b64_enc[value & 63U]);
    }
    return output_buffer;
}


static char *uint64_to_dec(uint64_t num, char dec[40])
{
    int i, l = 0, tr;

    dec[0] = 0;
    while (num > 0 && l < 39) {
        dec[l++] = '0' + num % 10;
        num /= 10;
    }
    dec[l] = 0;

    /* Revert sequence of digits to Big Endian Decimal */
    for (i = 0;i < l / 2; i++) {
        tr = dec[i];
        dec[i] = dec[l - i - 1];
        dec[l - i - 1] = tr;
    }
    return dec;
}


/* Write the .jigdo file to match the .template we've just finished. */
static int write_jigdo_file(struct libjte_env *o)
{
    unsigned char template_md5sum[16];
    entry_t *entry = o->entry_list;
    int i = 0;
    struct checksum_info *info = NULL;
    FILE *j_file = o->j_file;
    char *b64, dec[40];

    checksum_final(o->template_context);
    checksum_copy(o->template_context, CHECK_MD5, &template_md5sum[0]);

    fprintf(j_file, "# JigsawDownload\n");
    fprintf(j_file, "# See <http://atterer.org/jigdo/> for details about jigdo\n");
    fprintf(j_file, "# See <http://www.einval.com/~steve/software/CD/JTE/> for details about JTE\n\n");
    
    fprintf(j_file, "[Jigdo]\n");
    fprintf(j_file, "Version=%s\n", JIGDO_TEMPLATE_VERSION);
    fprintf(j_file, "Generator=libjte-%d.%d.%d\n\n",
            LIBJTE_VERSION_MAJOR, LIBJTE_VERSION_MINOR, LIBJTE_VERSION_MICRO);

    fprintf(j_file, "[Image]\n");
    fprintf(j_file, "Filename=%s\n", file_base_name(o->outfile));
    fprintf(j_file, "Template=http://localhost/%s\n", o->jtemplate_out);

    b64 = base64_dump(o, &template_md5sum[0], sizeof(template_md5sum));
    if (b64 == NULL)
        return -1;
    fprintf(j_file, "Template-MD5Sum=%s \n", b64);
    free(b64);

    for (i = 0; i < NUM_CHECKSUMS; i++)
    {
        if (o->checksum_algo_tmpl & (1 << i))
        {
            info = checksum_information(i);
            fprintf(j_file, "# Template Hex %sSum %s\n", info->name,
                    checksum_hex(o->template_context, i));
        }
    }
    fprintf(j_file, "# Template size %s bytes\n",
            uint64_to_dec(o->template_size, dec));

    for (i = 0; i < NUM_CHECKSUMS; i++)
    {
        if (o->checksum_algo_iso & (1 << i))
        {
            info = checksum_information(i);
            fprintf(j_file, "# Image Hex %sSum %s\n",
                    info->name, checksum_hex(o->iso_context, i));
        }
    }

    fprintf(j_file, "# Image size %s bytes\n\n",
            uint64_to_dec(o->image_size, dec));

    fprintf(j_file, "[Parts]\n");
    while (entry)
    {
        if (JTET_FILE_MATCH == entry->entry_type)
        {
            char *new_name = remap_filename(o, entry->data.file.filename);

            if (new_name == NULL)
                return -1;
            b64 = base64_dump(o, &entry->data.file.md5[0],
                              sizeof(entry->data.file.md5));
            if (b64 == NULL)
                return -1;
            fprintf(j_file, "%s=%s\n", b64, new_name);
            free(b64);
            free(new_name);
        }
        entry = entry->next;
    }

    fprintf(j_file, "\n[Servers]\n");
    fflush(j_file);
    return 1;
}

/* Finish and flush state; for now:
   
   1. Dump the DESC blocks and the footer information in the jigdo template file
   2. Write the jigdo .jigdo file containing file pointers
*/
int write_jt_footer(struct libjte_env *o)
{
    int ret;

    /* Finish calculating the image's checksum */
    checksum_final(o->iso_context);

    ret = write_template_desc_entries(o, o->image_size);
    if (ret <= 0)
        return ret;

    ret = write_jigdo_file(o);
    return ret;
}

/* Add a raw data entry to the list of extents; no file to match */
static void add_unmatched_entry(struct libjte_env *o, int uncompressed_length)
{
    entry_t *new_entry = NULL;

    /* Can we extend a previous non-match entry? */
    if (o->entry_last && (JTET_NOMATCH == o->entry_last->entry_type))
    {
        o->entry_last->data.chunk.uncompressed_length += uncompressed_length;
        return;
    }

    new_entry = calloc(1, sizeof(entry_t));
    new_entry->entry_type = JTET_NOMATCH;
    new_entry->next = NULL;
    new_entry->data.chunk.uncompressed_length = uncompressed_length;

    /* Add to the end of the list */
    if (NULL == o->entry_last)
    {
        o->entry_last = new_entry;
        o->entry_list = new_entry;
    }
    else
    {
        o->entry_last->next = new_entry;
        o->entry_last = new_entry;
    }
    o->num_chunks++;
}

/* Add a file match entry to the list of extents */
static void add_file_entry(struct libjte_env *o,
                           char *filename, off_t size, unsigned char *md5,
                           uint64_t rsyncsum)
{
    entry_t *new_entry = NULL;

    new_entry = calloc(1, sizeof(entry_t));
    new_entry->entry_type = JTET_FILE_MATCH;
    new_entry->next = NULL;
    memcpy(new_entry->data.file.md5, md5, sizeof(new_entry->data.file.md5));
    new_entry->data.file.file_length = size;
    new_entry->data.file.rsyncsum = rsyncsum;
    new_entry->data.file.filename = strdup(filename);

    /* Add to the end of the list */
    if (NULL == o->entry_last)
    {
        o->entry_last = new_entry;
        o->entry_list = new_entry;
    }
    else
    {
        o->entry_last->next = new_entry;
        o->entry_last = new_entry;
    }
    o->num_matches++;
}    

/* Cope with an unmatched block in the .iso file:

   1. Write a compressed data chunk in the jigdo template file
   2. Add an entry in our list of unmatched chunks for later */
int jtwrite(struct libjte_env *o, void *buffer, int size, int count)
{
    int ret;

    if (o->jtemplate_out == NULL)
        return 0;

    /* Update the global image checksum */
    checksum_update(o->iso_context, buffer, size * count);

    /* Write a compressed version of the data to the template file,
       and add a reference on the state list so we can write that
       later. */
    ret = write_compressed_chunk(o, buffer, size*count);
    if (ret <= 0)
        return ret;
    add_unmatched_entry(o, size*count);
    return 1;
}

/* Cope with a file entry in the .iso file:

   1. Read the file for the image's md5 checksum
   2. Add an entry in our list of files to be written into the .jigdo later
*/
int write_jt_match_record(struct libjte_env *o,
                           char *filename, char *mirror_name, int sector_size,
                           off_t size, unsigned char md5[16])
{
    char                buf[32768];
    off_t               remain = size;
	FILE               *infile = NULL;
	int	                use = 0;
    uint64_t  rsync64_sum = 0;
    int first_block = 1;

    memset(buf, 0, sizeof(buf));

    if ((infile = fopen(filename, "rb")) == NULL) {
#ifndef	HAVE_STRERROR
		sprintf(o->message_buffer, "cannot open '%s': (%d)",
				filename, errno);
#else
		sprintf(o->message_buffer, "cannot open '%s': %s",
				filename, strerror(errno));
#endif
                libjte_add_msg_entry(o, o->message_buffer, 0);
		exit_if_enabled(o, 1);
		return -1;
	}

    while (remain > 0)
    {
        use = remain;
        if (remain > sizeof(buf))
            use = sizeof(buf);
		if (fread(buf, 1, use, infile) == 0)
        {
			sprintf(o->message_buffer,
                                "cannot read from '%s'", filename);
                        libjte_add_msg_entry(o, o->message_buffer, 0);
			exit_if_enabled(o, 1);
			return -1;
	}
        if (first_block)
            rsync64_sum = rsync64((unsigned char *) buf, MIN_JIGDO_FILE_SIZE);
        checksum_update(o->iso_context, (unsigned char *) buf, use);
        remain -= use;
        first_block = 0;
    }

    fclose(infile);
    
    /* Update the image checksum with any necessary padding data */
    if (size % sector_size)
    {
        int pad_size = sector_size - (size % sector_size);
        memset(buf, 0, pad_size);
        checksum_update(o->iso_context, (unsigned char *) buf, pad_size);
    }

    add_file_entry(o, mirror_name, size, &md5[0], rsync64_sum);
    if (size % sector_size)
    {
        int pad_size = sector_size - (size % sector_size);
        write_compressed_chunk(o, (unsigned char *) buf, pad_size);
        add_unmatched_entry(o, pad_size);
    }        
    return 1;
}

int libjte_add_msg_entry(struct libjte_env *o, char *message, int flag)
{
    jigdo_msg_entry_t *new_entry = NULL, *s = NULL;
    int list_length = 0;

    if (o->error_behavior & 1) {
         fprintf(stderr, "libjte: %s\n", message);
         return 1;
    }
    if (o->msg_list != NULL) {
        /* Find end of list and eventually do an emergency message dump */
        for (s = o->msg_list; s->next != NULL; s = s->next)
            list_length++;
        if (list_length >= JTE_MAX_ERROR_LIST_LENGTH) {
            libjte_clear_msg_list(o, 1 | 2); /* dump to stderr */
            o->msg_list = s = NULL;
        }
    }

    new_entry = calloc(1, sizeof(jigdo_msg_entry_t));
    if (new_entry == NULL) {
no_mem:;
        fprintf(stderr, "libjte: %s\n", message);
        fprintf(stderr, "libjte: OUT OF MEMORY\n");
        return -1;
    }
    new_entry->next = NULL;
    new_entry->message = strdup(message);
    if (new_entry->message == NULL) {
        free(new_entry);
        goto no_mem;
    }
    if (o->msg_list == NULL)
        o->msg_list = new_entry;
    else
        s->next = new_entry;
    return 1;
}

