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
#include "endian.h"
#include "md5.h"
#include "jigdb.h"
#include "jte.h"

FILE *logfile = NULL;
FILE *outfile = NULL;
FILE *missing_file = NULL;
long long start_offset = 0;
long long end_offset = 0;
int quick = 0;
int verbose = 0;
UINT64 out_size = 0;
char *missing_filename = NULL;

typedef enum state_
{
    STARTING,
    IN_DATA,
    IN_DESC,
    DUMP_DESC,
    DONE,
    ERROR
} e_state;

match_list_t *match_list_head = NULL;
match_list_t *match_list_tail = NULL;

md5_list_t *md5_list_head = NULL;
md5_list_t *md5_list_tail = NULL;

typedef enum
{
    CT_GZIP,
    CT_BZIP2
} ctype_e;

struct
{
    char   *data_buf;
    ctype_e algorithm;
    INT64   buf_size;
    INT64   offset_in_curr_buf;
    INT64   total_offset;
} zip_state;

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

static int read_data_block(FILE *template_file)
{
    char inbuf[1024];
    INT64 i = 0;
    static INT64 template_offset = -1;
    INT64 compressed_len = 0;
    INT64 uncompressed_len = 0;
    char *comp_buf = NULL;
    int read_num = 0;
    int error = 0;

    if (-1 == template_offset)
    {
        fseek(template_file, 0, SEEK_SET);
        fread(inbuf, sizeof(inbuf), 1, template_file);
        for (i = 0; i < sizeof(inbuf); i++)
        {
            if (!strncmp(&inbuf[i], "DATA", 4))
            {
                zip_state.algorithm = CT_GZIP;
                template_offset = i;
                break;
            }
            if (!strncmp(&inbuf[i], "BZIP", 4))
            {
                zip_state.algorithm = CT_BZIP2;
                template_offset = i;
                break;
            }
        }
        if (-1 == template_offset)
        {
            fprintf(logfile, "Unable to locate DATA block in template (offset %lld)\n",
                    template_offset);
            return EINVAL;
        }    
    }
    
    fseek(template_file, template_offset, SEEK_SET);
    fread(inbuf, 16, 1, template_file);
    if (strncmp(inbuf, "DATA", 4) && strncmp(inbuf, "BZIP", 4))
    {
        fprintf(logfile, "Unable to locate DATA block in template (offset %lld)\n",
                template_offset);
        return EINVAL;
    }    
    
    compressed_len = read_le48((unsigned char *)&inbuf[4]);
    uncompressed_len = read_le48((unsigned char *)&inbuf[10]);

    comp_buf = calloc(1, compressed_len);
    if (!comp_buf)
    {
        fprintf(logfile, "Unable to locate DATA block in template (offset %lld)\n",
                template_offset);
        return ENOMEM;
    }
    
    zip_state.data_buf = calloc(1, uncompressed_len);
    if (!zip_state.data_buf)
    {
        fprintf(logfile, "Unable to allocate %lld bytes for decompression\n",
                uncompressed_len);
        return ENOMEM;
    }

    read_num = fread(comp_buf, compressed_len, 1, template_file);
    if (0 == read_num)
    {
        fprintf(logfile, "Unable to read %lld bytes for decompression\n",
                uncompressed_len);
        return EIO;
    }

    error = decompress_data_block(comp_buf, compressed_len,
                                  zip_state.data_buf, uncompressed_len, zip_state.algorithm);
    if (error)
    {
        fprintf(logfile, "Unable to decompress data block, error %d\n", error);
        return error;
    }
        
    template_offset += compressed_len;
    zip_state.buf_size = uncompressed_len;
    zip_state.offset_in_curr_buf = 0;
    free (comp_buf);
    return 0;
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
            error = read_data_block(template_file);
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

static int parse_data_block(INT64 data_size, FILE *template_file, struct mk_MD5Context *context)
{
    int error = 0;
    INT64 remaining = data_size;
    INT64 size = 0;
    int out_size = 0;

    while (remaining)
    {
        if (!zip_state.data_buf)
        {
            error = read_data_block(template_file);
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
            mk_MD5Update(context,
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

static int read_file_data(char *filename, char *missing, INT64 offset, INT64 data_size,
                          struct mk_MD5Context *file_context, struct mk_MD5Context *image_context)
{
    FILE *input_file = NULL;
    INT64 remaining = data_size;
    char buf[BUF_SIZE];
    int num_read = 0;
    int out_size = 0;

    input_file = fopen(filename, "rb");
    if (!input_file)
    {
        fprintf(logfile, "Unable to open mirror file %s, error %d\n",
                filename, errno);
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
                    filename, errno, ftell(input_file), size);
            fclose(input_file);
            return errno;
        }
        if (file_context)
        {
            mk_MD5Update(image_context, (unsigned char *)buf, size);
            mk_MD5Update(file_context, (unsigned char *)buf, size);
        }
        
        out_size = fwrite(buf, size, 1, outfile);
        if (!out_size)
        {
            fprintf(logfile, "read_file_data: fwrite %d failed with error %d; aborting\n", size, ferror(outfile));
            return ferror(outfile);
        }
        
        if (verbose)
            display_progress(outfile, file_base_name(filename));
        
        remaining -= size;
    }
    if (verbose > 1)
        fprintf(logfile, "read_file_data: wrote %lld bytes of data from %s\n",
                data_size, filename);
    fclose(input_file);
    return 0;
}

static int parse_file_block(INT64 offset, INT64 data_size, INT64 file_size, JIGDB *dbp,
                            unsigned char *md5, struct mk_MD5Context *image_context,
                            char *missing)
{
    char *base64_md5 = base64_dump(md5, 16);
    struct mk_MD5Context file_context;
    struct mk_MD5Context *use_context = NULL;
    unsigned char file_md5[16];
    md5_list_t *md5_list_entry = NULL;
    db_file_entry_t *db_entry = NULL;
    int error = 0;
    char *filename = NULL;

    if (!quick)
    {
        use_context = &file_context;
        mk_MD5Init(use_context);
    }

    /* Try the DB first if we have one */
    if (dbp)
    {
        error = db_lookup_file_by_md5(dbp, base64_md5, &db_entry);
        if (!error)
            filename = db_entry->filename;
    }

    /* No joy; fall back to the MD5 list */
    if (!filename)
    {
        md5_list_entry = find_file_in_md5_list(base64_md5);
        if (md5_list_entry && file_size == md5_list_entry->file_size)
            filename = md5_list_entry->full_path;
    }

    if (filename)
    {
        error = read_file_data(filename, missing, offset, data_size,
                               use_context, image_context);
        
        if (error && (ENOENT != error))
        {
            fprintf(logfile, "Failed to read file %s, error %d\n", filename, error);
            return error;
        }
        
        if (!quick)
        {
            mk_MD5Final(file_md5, &file_context);
            
            if (memcmp(file_md5, md5, 16))
            {
                fprintf(logfile, "MD5 MISMATCH for file %s\n", filename);
                fprintf(logfile, "    template looking for %s\n", md5);
                fprintf(logfile, "    file %s is    %s\n", filename, file_md5);
                return EINVAL;
            }
        }
        return 0;
    }
    
    /* No file found. Add it to the list of missing files, or complain */
    if ( missing &&
         (MISSING == md5_list_entry->file_size) &&
         (!memcmp(md5_list_entry->md5, base64_md5, 16) ) )
    {
        write_missing_entry(missing, md5_list_entry->full_path);
        return 0;
    }
    /* else */
    return ENOENT;
}

static int parse_template_file(char *filename, int sizeonly,
                               char *missing, char *output_name, char *db_filename)
{
    INT64 template_offset = 0;
    INT64 bytes = 0;
    char *buf = NULL;
    FILE *file = NULL;
    INT64 file_size = 0;
    INT64 desc_start = 0;
    INT64 written_length = 0;
    INT64 output_offset = 0;
    int i = 0;
    int error = 0;
    struct mk_MD5Context template_context;
    unsigned char image_md5sum[16];
    JIGDB *dbp = NULL;

    zip_state.total_offset = 0;
    
    file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(logfile, "Failed to open template file %s, error %d!\n", filename, errno);
        return errno;
    }

    if (db_filename)
    {
        dbp = db_open(db_filename);
        if (!dbp)
        {
            fprintf(logfile, "Failed to open DB file %s, error %d\n", db_filename, errno);
            return errno;
        }
        /* If we have a DB, then we should cache the template
         * information in it too. Check and see if there is
         * information about this template file in the database
         * already. */
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
    desc_start = file_size - read_le48((unsigned char *)buf);

    /* Now seek back to the beginning image desc block to grab the MD5
       and image length */
    fseek(file, file_size - 33, SEEK_SET);
    fread(buf, BUF_SIZE, 1, file);
    if (buf[0] != 5) /* image data */
    {
        fprintf(logfile, "Failed to find image desc in the template file\n");
        fclose(file);
        return EINVAL;
    }

    if (sizeonly)
    {
        fclose(file);
        printf("%lld\n", read_le48((unsigned char *)&buf[1]));
        return 0;
    }

    if (verbose)
    {
        fprintf(logfile, "Image MD5 should be    ");
        for (i = 0; i < 16; i++)
            fprintf(logfile, "%2.2x", (unsigned char)buf[i+7]);
        fprintf(logfile, "\n");
        fprintf(logfile, "Image size should be   %lld bytes\n", read_le48((unsigned char *)&buf[1]));
    }

    out_size = read_le48((unsigned char *)&buf[1]);
    
    /* Now seek back to the start of the desc block */
    fseek(file, desc_start, SEEK_SET);
    fread(buf, 10, 1, file);
    if (strncmp(buf, "DESC", 4))
    {
        fprintf(logfile, "Failed to find desc start in the template file\n");
        fclose(file);
        return EINVAL;
    }
    if ((file_size - desc_start) != read_le48((unsigned char *)&buf[4]))
    {
        fprintf(logfile, "Inconsistent desc length in the template file!\n");
        fprintf(logfile, "Final chunk says %lld, first chunk says %lld\n",
                file_size - desc_start, read_le48((unsigned char *)&buf[4]));
        fclose(file);
        return EINVAL;
    }

    if (!quick)
        mk_MD5Init(&template_context);
    template_offset = desc_start + 10;

    if (1 == verbose)
        fprintf(logfile, "Creating ISO image %s\n", output_name);

    /* Main loop - walk through the template file and expand each entry we find */
    while (1)
    {
        INT64 extent_size;
        INT64 skip = 0;
        INT64 read_length = 0;

        if (template_offset >= (file_size - 33))
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
        
        fseek(file, template_offset, SEEK_SET);
        bytes = fread(buf, (MIN (BUF_SIZE, file_size - template_offset)), 1, file);
        if (1 != bytes)
        {
            fprintf(logfile, "Failed to read template file!\n");
            fclose(file);
            return EINVAL;
        }
        
        extent_size = read_le48((unsigned char *)&buf[1]);
        read_length = extent_size;
        
        if (start_offset > output_offset)
            skip = start_offset - output_offset;
        if ((output_offset + extent_size) > end_offset)
            read_length -= (output_offset + extent_size - end_offset - 1);
        read_length -= skip;
        
        switch (buf[0])
        {
            
            case 2: /* unmatched data */
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
                    error = parse_data_block(read_length, file, &template_context);
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
            case 6:
                template_offset += 31;
                if ((output_offset + extent_size) >= start_offset)
                {
                    error = parse_file_block(skip, read_length, extent_size, dbp,
                                             (unsigned char *)&buf[15], &template_context, missing);
                    if (error)
                    {
                        fprintf(logfile, "Unable to read file block, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    written_length += read_length;
                }
                break;
            default:
                fprintf(logfile, "Unknown block type %d!\n", buf[0]);
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
            mk_MD5Final (image_md5sum, &template_context);
            fprintf(logfile, "Output image MD5 is    ");
            for (i = 0; i < 16; i++)
                fprintf(logfile, "%2.2x", image_md5sum[i]);
            fprintf(logfile, "\n");
        }
        fprintf(logfile, "Output image length is %lld bytes\n", written_length);
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

    logfile = stderr;
    outfile = stdout;

    bzero(&zip_state, sizeof(zip_state));

    while(1)
    {
        c = getopt(argc, argv, ":?M:d:e:f:h:j:l:m:o:qs:t:vz");
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
                    fprintf(stderr, "Unable to open output file %s\n", optarg);
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
            case 'd':
                if (db_filename)
                {
                    fprintf(logfile, "Can only specify one db file!\n");
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
            default:
                fprintf(logfile, "Unknown option!\n");
                return EINVAL;
        }
    }

    if (0 == end_offset)
        end_offset = LLONG_MAX;

    if ((NULL == jigdo_filename) &&
        (NULL == md5_filename) && 
        (NULL == db_filename) && 
        !sizeonly)
    {
        fprintf(logfile, "No jigdo file, DB file or MD5 file specified!\n");
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
            fprintf(logfile, "Unable to parse the MD5 file %s\n", md5_filename);
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
    error = parse_template_file(template_filename, sizeonly,
                                missing_filename, output_name, db_filename);
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

