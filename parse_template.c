#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <zlib.h>
#ifdef BZ2_SUPPORT
#   include <bzlib.h>
#endif
#include "md5.h"
#include "endian.h"
#include "jigdb.h"
#include "jte.h"

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
            fprintf(G_logfile, "Unable to locate DATA block in template (offset %lld)\n",
                    template_offset);
            return EINVAL;
        }    
    }
    
    fseek(template_file, template_offset, SEEK_SET);
    fread(inbuf, 16, 1, template_file);
    if (strncmp(inbuf, "DATA", 4) && strncmp(inbuf, "BZIP", 4))
    {
        fprintf(G_logfile, "Unable to locate DATA block in template (offset %lld)\n",
                template_offset);
        return EINVAL;
    }    
    
    compressed_len = read_le48((unsigned char *)&inbuf[4]);
    uncompressed_len = read_le48((unsigned char *)&inbuf[10]);

    comp_buf = calloc(1, compressed_len);
    if (!comp_buf)
    {
        fprintf(G_logfile, "Unable to locate DATA block in template (offset %lld)\n",
                template_offset);
        return ENOMEM;
    }
    
    zip_state.data_buf = calloc(1, uncompressed_len);
    if (!zip_state.data_buf)
    {
        fprintf(G_logfile, "Unable to allocate %lld bytes for decompression\n",
                uncompressed_len);
        return ENOMEM;
    }

    read_num = fread(comp_buf, compressed_len, 1, template_file);
    if (0 == read_num)
    {
        fprintf(G_logfile, "Unable to read %lld bytes for decompression\n",
                uncompressed_len);
        return EIO;
    }

    error = decompress_data_block(comp_buf, compressed_len,
                                  zip_state.data_buf, uncompressed_len, zip_state.algorithm);
    if (error)
    {
        fprintf(G_logfile, "Unable to decompress data block, error %d\n", error);
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
                fprintf(G_logfile, "Unable to decompress template data, error %d\n",
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
    
    fprintf(G_logfile, "skip_data_block: skipped %lld bytes of unmatched data\n", data_size);
    return error;
}

static int parse_data_block(INT64 data_size, FILE *template_file, struct mk_MD5Context *context, FILE *outfile)
{
    int error = 0;
    INT64 remaining = data_size;
    INT64 size = 0;
    int write_size = 0;

    while (remaining)
    {
        if (!zip_state.data_buf)
        {
            error = read_data_block(template_file);
            if (error)
            {
                fprintf(G_logfile, "Unable to decompress template data, error %d\n",
                        error);
                return error;
            }
        }
        size = MIN((zip_state.buf_size - zip_state.offset_in_curr_buf), remaining);
        write_size = fwrite(&zip_state.data_buf[zip_state.offset_in_curr_buf], size, 1, outfile);
        if (!write_size)
        {
            fprintf(G_logfile, "parse_data_block: fwrite %lld failed with error %d; aborting\n", size, ferror(outfile));
            return ferror(outfile);
        }

        if (G_verbose)
            display_progress(outfile, "template data");

        if (!G_quick)
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
    if (G_verbose > 1)
        fprintf(G_logfile, "parse_data_block: wrote %lld bytes of unmatched data\n", data_size);
    return error;
}

static int read_file_data(char *filename, char *missing, INT64 offset, INT64 data_size,
                          struct mk_MD5Context *file_context, struct mk_MD5Context *image_context,
                          FILE *outfile)
{
    FILE *input_file = NULL;
    INT64 remaining = data_size;
    char buf[BUF_SIZE];
    int num_read = 0;
    int write_size = 0;

    input_file = fopen(filename, "rb");
    if (!input_file)
    {
        fprintf(G_logfile, "Unable to open mirror file %s, error %d\n",
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
            fprintf(G_logfile, "Unable to read from mirror file %s, error %d (offset %ld, length %d)\n",
                    filename, errno, ftell(input_file), size);
            fclose(input_file);
            return errno;
        }
        if (file_context)
        {
            mk_MD5Update(image_context, (unsigned char *)buf, size);
            mk_MD5Update(file_context, (unsigned char *)buf, size);
        }
        
        write_size = fwrite(buf, size, 1, outfile);
        if (!write_size)
        {
            fprintf(G_logfile, "read_file_data: fwrite %d failed with error %d; aborting\n", size, ferror(outfile));
            return ferror(outfile);
        }
        
        if (G_verbose)
            display_progress(outfile, file_base_name(filename));
        
        remaining -= size;
    }
    if (G_verbose > 1)
        fprintf(G_logfile, "read_file_data: wrote %lld bytes of data from %s\n",
                data_size, filename);
    fclose(input_file);
    return 0;
}

static int parse_file_block(INT64 offset, INT64 data_size, INT64 file_size, FILE *outfile,
                            JIGDB *dbp, unsigned char *md5, struct mk_MD5Context *image_context,
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

    if (!G_quick)
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
                               use_context, image_context, outfile);
        
        if (error && (ENOENT != error))
        {
            fprintf(G_logfile, "Failed to read file %s, error %d\n", filename, error);
            return error;
        }
        
        if (!G_quick)
        {
            mk_MD5Final(file_md5, &file_context);
            
            if (memcmp(file_md5, md5, 16))
            {
                fprintf(G_logfile, "MD5 MISMATCH for file %s\n", filename);
                fprintf(G_logfile, "    template looking for %s\n", base64_dump(md5, 16));
                fprintf(G_logfile, "    file %s is    %s\n", filename, base64_dump(file_md5, 16));
                return EINVAL;
            }
        }
        return 0;
    }
    
    /* No file found. Add it to the list of missing files, or complain */
    if ( missing &&
         md5_list_entry &&
         (MISSING == md5_list_entry->file_size) &&
         (!memcmp(md5_list_entry->md5, base64_md5, 16) ) )
    {
        file_missing(missing, md5_list_entry->full_path);
        return 0;
    }
    /* else */
    return ENOENT;
}

int parse_template_file(char *filename, int sizeonly, char *missing,
                        FILE *outfile, char *output_name, JIGDB *dbp)
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

    bzero(&zip_state, sizeof(zip_state));
    
    file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(G_logfile, "Failed to open template file %s, error %d!\n", filename, errno);
        return errno;
    }

    buf = malloc(BUF_SIZE);
    if (!buf)
    {
        fprintf(G_logfile, "Failed to malloc %d bytes. Abort!\n", BUF_SIZE);
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
        fprintf(G_logfile, "Failed to find image desc in the template file\n");
        fclose(file);
        return EINVAL;
    }

    if (sizeonly)
    {
        fclose(file);
        printf("%lld\n", read_le48((unsigned char *)&buf[1]));
        return 0;
    }

    if (G_verbose)
    {
        fprintf(G_logfile, "Image MD5 should be    ");
        for (i = 0; i < 16; i++)
            fprintf(G_logfile, "%2.2x", (unsigned char)buf[i+7]);
        fprintf(G_logfile, "\n");
        fprintf(G_logfile, "Image size should be   %lld bytes\n", read_le48((unsigned char *)&buf[1]));
    }

    G_out_size = read_le48((unsigned char *)&buf[1]);
    
    /* Now seek back to the start of the desc block */
    fseek(file, desc_start, SEEK_SET);
    fread(buf, 10, 1, file);
    if (strncmp(buf, "DESC", 4))
    {
        fprintf(G_logfile, "Failed to find desc start in the template file\n");
        fclose(file);
        return EINVAL;
    }
    if ((file_size - desc_start) != read_le48((unsigned char *)&buf[4]))
    {
        fprintf(G_logfile, "Inconsistent desc length in the template file!\n");
        fprintf(G_logfile, "Final chunk says %lld, first chunk says %lld\n",
                file_size - desc_start, read_le48((unsigned char *)&buf[4]));
        fclose(file);
        return EINVAL;
    }

    if (!G_quick)
        mk_MD5Init(&template_context);
    template_offset = desc_start + 10;

    if (1 == G_verbose)
        fprintf(G_logfile, "Creating ISO image %s\n", output_name);

    /* Main loop - walk through the template file and expand each entry we find */
    while (1)
    {
        INT64 extent_size;
        INT64 skip = 0;
        INT64 read_length = 0;

        if (template_offset >= (file_size - 33))
        {
            if (G_verbose > 1)
                fprintf(G_logfile, "Reached end of template file\n");
            break; /* Finished! */
        }
        
        if (output_offset > G_end_offset) /* Past the range we were asked for */
        {
            fprintf(G_logfile, "Reached end of range requested\n");            
            break;
        }
        
        fseek(file, template_offset, SEEK_SET);
        bytes = fread(buf, (MIN (BUF_SIZE, file_size - template_offset)), 1, file);
        if (1 != bytes)
        {
            fprintf(G_logfile, "Failed to read template file!\n");
            fclose(file);
            return EINVAL;
        }
        
        extent_size = read_le48((unsigned char *)&buf[1]);
        read_length = extent_size;
        
        if (G_start_offset > output_offset)
            skip = G_start_offset - output_offset;
        if ((output_offset + extent_size) > G_end_offset)
            read_length -= (output_offset + extent_size - G_end_offset - 1);
        read_length -= skip;
        
        switch (buf[0])
        {
            
            case 2: /* unmatched data */
                template_offset += 7;
                if (missing)
                    break;
                if ((output_offset + extent_size) >= G_start_offset)
                {
                    if (skip)
                        error = skip_data_block(skip, file);
                    if (error)
                    {
                        fprintf(G_logfile, "Unable to read data block to skip, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    error = parse_data_block(read_length, file, &template_context, outfile);
                    if (error)
                    {
                        fprintf(G_logfile, "Unable to read data block, error %d\n", error);
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
                if ((output_offset + extent_size) >= G_start_offset)
                {
                    error = parse_file_block(skip, read_length, extent_size, outfile, dbp,
                                             (unsigned char *)&buf[15], &template_context, missing);
                    if (error)
                    {
                        fprintf(G_logfile, "Unable to read file block, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    written_length += read_length;
                }
                break;
            default:
                fprintf(G_logfile, "Unknown block type %d!\n", buf[0]);
                fclose(file);
                return EINVAL;
        }
        output_offset += extent_size;
    }

    if (missing && G_missing_file)
        return ENOENT;
    
    fclose(file);
    if (G_verbose)
    {
        fprintf(G_logfile, "\n");
        if (!G_quick)
        {
            mk_MD5Final (image_md5sum, &template_context);
            fprintf(G_logfile, "Output image MD5 is    ");
            for (i = 0; i < 16; i++)
                fprintf(G_logfile, "%2.2x", image_md5sum[i]);
            fprintf(G_logfile, "\n");
        }
        fprintf(G_logfile, "Output image length is %lld bytes\n", written_length);
    }
    
    return 0;
}

