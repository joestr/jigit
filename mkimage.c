/*
 * mkimage
 *
 * Tool to create an ISO image from jigdo files
 *
 * Copyright (c) 2004 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

#define BZ2_SUPPORT

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
#include <stdarg.h>
#include "endian.h"
#include "jigdb.h"
#include "jte.h"
#include "md5.h"

FILE *G_logfile = NULL;
FILE *G_outfile = NULL;
FILE *G_missing_file = NULL;
long long G_start_offset = 0;
long long G_end_offset = 0;
int G_quick = 0;
int G_verbose = 0;
UINT64 G_out_size = 0;
char *G_missing_filename = NULL;

typedef enum state_
{
    STARTING,
    IN_DATA,
    IN_DESC,
    DUMP_DESC,
    DONE,
    ERROR
} e_state;

match_list_t *G_match_list_head = NULL;
match_list_t *G_match_list_tail = NULL;

md5_list_t *G_md5_list_head = NULL;
md5_list_t *G_md5_list_tail = NULL;

extern void file_missing(char *missing, char *filename)
{
    if (!G_missing_file)
    {
        G_missing_file = fopen(missing, "wb");
        if (!G_missing_file)
        {
            jd_log(0, "file_missing: Unable to open missing log %s; error %d\n", missing, errno);
            exit(1);
        }
    }
    fprintf(G_missing_file, "%s\n", filename);
}

void display_progress(int verbose_level, INT64 image_size, INT64 current_offset, char *text)
{
    if ((verbose_level <= G_verbose) && (image_size > 0))
        jd_log(1, "\r %5.2f%%  %-60.60s",
               100.0 * current_offset / image_size, text);
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
        jd_log(0, "Could not parse malformed match entry \"%s\"\n", match);
        return EINVAL;
    }        
    
    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return ENOMEM;

    jd_log(1, "Adding match entry %s:%s\n", match, mirror_path);

    entry->match = match;
    entry->mirror_path = mirror_path;
    
    if (!G_match_list_head)
    {
        G_match_list_head = entry;
        G_match_list_tail = entry;
    }
    else
    {
        G_match_list_tail->next = entry;
        G_match_list_tail = entry;
    }
    
    return 0;
}

static void usage(char *progname)
{
    printf("%s [OPTIONS]\n\n", progname);
    printf(" Options:\n");
	printf(" -M <missing name>   Rather than try to build the image, just check that\n");
	printf("                     all the needed files are available. If any are missing,\n");
	printf("                     list them in this file.\n");
    printf(" -d <DB name>        Specify an input MD5 database file, as created by jigsum\n");
    printf(" -e <bytenum>        End byte number; will end at EOF if not specified\n");    
    printf(" -f <MD5 name>       Specify an input MD5 file. MD5s must be in jigdo's\n");
	printf("                     pseudo-base64 format\n");
    printf(" -j <jigdo name>     Specify the input jigdo file\n");
    printf(" -l <logfile>        Specify a logfile to append to.\n");
    printf("                     If not specified, will log to stderr\n");
    printf(" -m <item=path>      Map <item> to <path> to find the files in the mirror\n");
    printf(" -o <outfile>        Specify a file to write the ISO image to.\n");
    printf("                     If not specified, will write to stdout\n");
    printf(" -q                  Quick mode. Don't check MD5sums. Dangerous!\n");
    printf(" -s <bytenum>        Start byte number; will start at 0 if not specified\n");
    printf(" -t <template name>  Specify the input template file\n");
	printf(" -v                  Make the output logging more verbose\n");
    printf(" -z                  Don't attempt to rebuild the image; simply print its\n");
    printf("                     size in bytes\n");
}

int main(int argc, char **argv)
{
    char *template_filename = NULL;
    char *jigdo_filename = NULL;
    char *md5_filename = NULL;
    char *output_name = NULL;
    char *db_filename = NULL;
    int c = -1;
    int error = 0;
    int sizeonly = 0;
    JIGDB *dbp = NULL;
    db_template_entry_t template;
    JD *jdp = NULL;
    INT64 current_offset = 0;
    unsigned char data_buf[BUF_SIZE];
    INT64 bytes_read = 0;
    INT64 bytes_written = 0;
    struct mk_MD5Context image_context;
    unsigned char image_md5[16];

    G_logfile = stderr;
    G_outfile = stdout;

    while(1)
    {
        c = getopt(argc, argv, ":?M:d:e:f:h:j:l:m:o:qs:t:vz");
        if (-1 == c)
            break;
        
        switch(c)
        {
            case 'v':
                G_verbose++;
                break;
            case 'q':
                G_quick = 1;
                break;
            case 'l':
                G_logfile = fopen(optarg, "ab");
                if (!G_logfile)
                {
                    fprintf(stderr, "Unable to open log file %s\n", optarg);
                    return errno;
                }
                setlinebuf(G_logfile);
                break;
            case 'o':
                output_name = optarg;
                G_outfile = fopen(output_name, "wb");
                if (!G_outfile)
                {
                    jd_log(0, "Unable to open output file %s\n", optarg);
                    return errno;
                }
                break;
            case 'j':
                if (jigdo_filename)
                {
                    jd_log(0, "Can only specify one jigdo file!\n");
                    return EINVAL;
                }
                /* else */
                jigdo_filename = optarg;
                break;
            case 't':
                if (template_filename)
                {
                    jd_log(0, "Can only specify one template file!\n");
                    return EINVAL;
                }
                /* else */
                template_filename = optarg;
                break;
            case 'f':
                if (md5_filename)
                {
                    jd_log(0, "Can only specify one MD5 file!\n");
                    return EINVAL;
                }
                /* else */
                md5_filename = optarg;
                break;                
            case 'd':
                if (db_filename)
                {
                    jd_log(0, "Can only specify one db file!\n");
                    return EINVAL;
                }
                /* else */
                db_filename = optarg;
                break;                
            case 'm':
                error = add_match_entry(strdup(optarg));
                if (error)
                    return error;
                break;
            case 'M':
                G_missing_filename = optarg;
                break;
            case ':':
                jd_log(0, "Missing argument!\n");
                return EINVAL;
                break;
            case 'h':
            case '?':
                usage(argv[0]);
                return 0;
                break;
            case 's':
                G_start_offset = strtoull(optarg, NULL, 10);
                if (G_start_offset != 0)
                    G_quick = 1;
                break;
            case 'e':
                G_end_offset = strtoull(optarg, NULL, 10);
                if (G_end_offset != 0)
                    G_quick = 1;
                break;
            case 'z':
                sizeonly = 1;
                break;
            default:
                jd_log(0, "Unknown option!\n");
                return EINVAL;
        }
    }

    if ((NULL == jigdo_filename) &&
        (NULL == md5_filename) && 
        (NULL == db_filename) && 
        !sizeonly)
    {
        jd_log(0, "No jigdo file, DB file or MD5 file specified!\n");
        usage(argv[0]);
        return EINVAL;
    }
    
    if (NULL == template_filename)
    {
        jd_log(0, "No template file specified!\n");
        usage(argv[0]);
        return EINVAL;
    }    

    if (md5_filename)
    {
        /* Build up a list of the files we've been fed */
        error = parse_md5_file(md5_filename, &G_md5_list_head);
        if (error)
        {
            jd_log(0, "Unable to parse the MD5 file %s\n", md5_filename);
            return error;
        }
    }

    if (jigdo_filename)
    {
        /* Build up a list of file mappings */
        error = parse_jigdo_file(jigdo_filename, &G_md5_list_head, G_match_list_head, G_missing_filename);
        if (error)
        {
            jd_log(0, "Unable to parse the jigdo file %s\n", jigdo_filename);
            return error;
        }
    }

    if (!output_name)
        output_name = "to stdout";

    if (db_filename)
    {
        dbp = db_open(db_filename);
        if (!dbp)
        {
            jd_log(0, "Failed to open DB file %s, error %d\n", db_filename, errno);
            return errno;
        }
        /* If we have a DB, then we should cache the template
         * information in it too. Check and see if there is
         * information about this template file in the database
         * already. */
    }

    /* See if we know about this template file */
    error = db_lookup_template_by_path(dbp, template_filename, &template);
    if (error)
    {
        /* Not found. Parse it and put the details in the database */
        error = add_new_template_file(dbp, template_filename);
        if (error)
        {
            jd_log(0, "Unable to add template file %s to database, error %d\n",
                   template_filename, error);
            return error;
        }
        error = db_lookup_template_by_path(dbp, template_filename, &template);
        if (error)
        {
            jd_log(0, "Unable to re-read newly-added template file %s, error %d!\n",
                   template_filename, error);
            return error;
        }
    }

    /* Allocate enough space to cache details about 1 compressed lump,
     * that's all we need here */
    error = jd_init(1);
    if (error)
    {
        jd_log(0, "Unable to init JD cache interface, error %d\n", error);
        return error;
    }

    jdp = jd_open(dbp, template_filename);
    if (!jdp)
    {
        error = errno;
        jd_log(0, "Unable to open JD interface for template file %s (error %d)\n",
               template_filename, error);
        return error;
    }

    if (!G_quick)
        mk_MD5Init(&image_context);

    error = jd_size(jdp, &G_out_size);
    if (error)
    {
        jd_log(0, "Unable to read image size from the template information. Error %d\n", error);
        return error;
    }
    
    if (0 == G_end_offset)
        G_end_offset = G_out_size;

    /* Now the main loop - iterate in read/write/md5sum */
    current_offset = G_start_offset;
    while (!error && (current_offset < G_end_offset))
    {
        error = jd_read(jdp, current_offset, sizeof(data_buf), data_buf, &bytes_read);
        if (error)
        {
            jd_log(0, "Failed to read %d bytes at offset %lld, error %d\n",
                   sizeof(data_buf), current_offset, error);
            break;
        }
        if (0 == bytes_read)
            break;
        
        bytes_written = 0;
        while (bytes_written < bytes_read)
        {
            size_t this_write = fwrite(data_buf, 1, bytes_read, G_outfile);
            if (-1 == this_write)
            {
                jd_log(0, "Failed to write %lld bytes at offset %lld, error %d\n",
                       bytes_read, current_offset, error);
                break;
            }
            bytes_written += this_write;
        }
        
        if (!G_quick)
            mk_MD5Update(&image_context, data_buf, bytes_read);

        current_offset += bytes_read;

        if (G_verbose)
        {
            char *last_file = NULL;                
            error = jd_last_filename(jdp, &last_file);
            if (!error && (G_end_offset > G_start_offset))
                jd_log(1, "\r %5.2f%%  %-60.60s",
                       100.0 * (current_offset - G_start_offset) / (G_end_offset - G_start_offset), last_file);
        }
    }
    if (error)
    {
        jd_log(0, "Failed to create image, error %d\n", error);
        return error;
    }

    if (G_verbose)
    {
        jd_log(1, "\n");
        if (!G_quick)
        {
            mk_MD5Final (image_md5, &image_context);
            char *out_md5 = hex_dump(image_md5, 16);
            jd_log(1, "Output image MD5 is    %s\n", out_md5);
            free(out_md5);
        }
        jd_log(1, "Output image length written is %lld bytes\n", G_end_offset - G_start_offset);
    }

#if 0
    /* Read the template file and actually build the image to <outfile> */
    error = parse_template_file(template_filename, sizeonly, G_missing_filename,
                                G_outfile, output_name, dbp);
    if (error)
    {
        jd_log(0, "Unable to recreate image from template file %s\n", template_filename);
        if (G_missing_filename)
            jd_log(0, "%s contains the list of missing files\n", G_missing_filename);
        return error;
    }        
#endif

    fclose(G_logfile);
    return 0;
}

int jd_log(int level, char *fmt, ...)
{
    int error = 0;    
    va_list ap;

    if (level <= G_verbose)
    {
        va_start(ap, fmt);
        error = vfprintf(G_logfile, fmt, ap);
        va_end(ap);
    }
    return error;
}
