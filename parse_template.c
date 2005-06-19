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
            jd_log(0, "Unable to locate DATA block in template (offset %lld)\n",
                   template_offset);
            return EINVAL;
        }    
    }
    
    fseek(template_file, template_offset, SEEK_SET);
    fread(inbuf, 16, 1, template_file);
    if (strncmp(inbuf, "DATA", 4) && strncmp(inbuf, "BZIP", 4))
    {
        jd_log(0, "Unable to locate DATA block in template (offset %lld)\n",
               template_offset);
        return EINVAL;
    }    
    
    compressed_len = read_le48((unsigned char *)&inbuf[4]);
    uncompressed_len = read_le48((unsigned char *)&inbuf[10]);

    comp_buf = calloc(1, compressed_len);
    if (!comp_buf)
    {
        jd_log(0, "Unable to locate DATA block in template (offset %lld)\n",
               template_offset);
        return ENOMEM;
    }
    
    zip_state.data_buf = calloc(1, uncompressed_len);
    if (!zip_state.data_buf)
    {
        jd_log(0, "Unable to allocate %lld bytes for decompression\n",
               uncompressed_len);
        return ENOMEM;
    }

    read_num = fread(comp_buf, compressed_len, 1, template_file);
    if (0 == read_num)
    {
        jd_log(0, "Unable to read %lld bytes for decompression\n",
               uncompressed_len);
        return EIO;
    }

    error = decompress_data_block(comp_buf, compressed_len,
                                  zip_state.data_buf, uncompressed_len, zip_state.algorithm);
    if (error)
    {
        jd_log(0, "Unable to decompress data block, error %d\n", error);
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
                jd_log(0, "Unable to decompress template data, error %d\n",
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
    
    jd_log(2, "skip_data_block: skipped %lld bytes of unmatched data\n", data_size);
    return error;
}

static int parse_data_block(INT64 data_size, FILE *template_file, INT64 image_size, INT64 current_offset,
                            struct mk_MD5Context *context, FILE *outfile, int no_md5)
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
                jd_log(0, "Unable to decompress template data, error %d\n",
                       error);
                return error;
            }
        }
        size = MIN((zip_state.buf_size - zip_state.offset_in_curr_buf), remaining);
        write_size = fwrite(&zip_state.data_buf[zip_state.offset_in_curr_buf], size, 1, outfile);
        if (!write_size)
        {
            jd_log(0, "parse_data_block: fwrite %lld failed with error %d; aborting\n", size, ferror(outfile));
            return ferror(outfile);
        }

        display_progress(1, image_size, current_offset, "template data");
        if (!no_md5)
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
    jd_log(2, "parse_data_block: wrote %lld bytes of unmatched data\n", data_size);
    return error;
}

static int read_file_data(char *filename, char *missing, INT64 image_size, INT64 current_offset,
                          INT64 offset, INT64 data_size, 
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
        jd_log(0, "Unable to open mirror file %s, error %d\n",
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
            jd_log(0, "Unable to read from mirror file %s, error %d (offset %ld, length %d)\n",
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
            jd_log(0, "read_file_data: fwrite %d failed with error %d; aborting\n", size, ferror(outfile));
            return ferror(outfile);
        }
        
        display_progress(1, image_size, current_offset, file_base_name(filename));        
        remaining -= size;
    }
    jd_log(2, "read_file_data: wrote %lld bytes of data from %s\n", data_size, filename);
    fclose(input_file);
    return 0;
}

static int parse_file_block(INT64 offset, INT64 data_size, INT64 image_size, INT64 current_offset,
                            INT64 file_size, FILE *outfile,
                            JIGDB *dbp, unsigned char *md5, struct mk_MD5Context *image_context,
                            char *missing, int no_md5, md5_list_t **md5_list_head)
{
    char *base64_md5 = base64_dump(md5, 16);
    struct mk_MD5Context file_context;
    struct mk_MD5Context *use_context = NULL;
    unsigned char file_md5[16];
    md5_list_t *md5_list_entry = NULL;
    db_file_entry_t db_entry;
    int error = 0;
    char *filename = NULL;

    if (!no_md5)
    {
        use_context = &file_context;
        mk_MD5Init(use_context);
    }

    /* Try the DB first if we have one */
    if (dbp)
    {
        error = db_lookup_file_by_md5(dbp, base64_md5, &db_entry);
        if (!error)
            filename = db_entry.filename;
    }

    /* No joy; fall back to the MD5 list */
    if (!filename)
    {
        md5_list_entry = find_file_in_md5_list(base64_md5, md5_list_head);
        if (md5_list_entry && file_size == md5_list_entry->file_size)
            filename = md5_list_entry->full_path;
    }

    if (filename)
    {
        error = read_file_data(filename, missing, image_size, current_offset, offset, data_size,
                               use_context, image_context, outfile);
        
        if (error && (ENOENT != error))
        {
            jd_log(0, "Failed to read file %s, error %d\n", filename, error);
            free(base64_md5);
            return error;
        }
        
        if (!no_md5)
        {
            mk_MD5Final(file_md5, &file_context);
            
            if (memcmp(file_md5, md5, 16))
            {
                char *tmp_md5 = NULL;

                jd_log(0, "MD5 MISMATCH for file %s\n", filename);

                tmp_md5 = base64_dump(md5, 16);
                jd_log(0, "    template looking for %s\n", tmp_md5);
                free(tmp_md5);
                
                tmp_md5 = base64_dump(file_md5, 16);
                jd_log(0, "    file %s is    %s\n", filename, tmp_md5);
                free(tmp_md5);
                free(base64_md5);
                return EINVAL;
            }
        }
        free(base64_md5);
        return 0;
    }
    
    /* No file found. Add it to the list of missing files, or complain */
    if ( missing &&
         md5_list_entry &&
         (MISSING == md5_list_entry->file_size) &&
         (!memcmp(md5_list_entry->md5, base64_md5, 16) ) )
    {
        file_missing(missing, md5_list_entry->full_path);
        free(base64_md5);
        return 0;
    }
    /* else */
    free(base64_md5);
    return ENOENT;
}

int parse_template_file(char *filename, int sizeonly, int no_md5, char *missing,
                        FILE *outfile, char *output_name, JIGDB *dbp,
                        md5_list_t **md5_list_head, FILE *missing_file,
                        UINT64 start_offset, UINT64 end_offset)
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
        jd_log(0, "Failed to open template file %s, error %d!\n", filename, errno);
        return errno;
    }

    buf = malloc(BUF_SIZE);
    if (!buf)
    {
        jd_log(0, "Failed to malloc %d bytes. Abort!\n", BUF_SIZE);
        fclose(file);
        return ENOMEM;
    }

    /* Find the beginning of the desc block */
    file_size = get_file_size(filename);
    fseek(file, file_size - 6, SEEK_SET);
    fread(buf, 6, 1, file);
    desc_start = file_size - read_le48((unsigned char *)buf);

    /* Now seek back to the beginning of the image desc block to grab
       the MD5 and image length */
    fseek(file, file_size - 33, SEEK_SET);
    fread(buf, BUF_SIZE, 1, file);
    if (buf[0] != 5) /* image data */
    {
        jd_log(0, "Failed to find image desc in the template file\n");
        fclose(file);
        return EINVAL;
    }

    memcpy(image_md5sum, &buf[7], 16);

    /* Now seek back to the start of the desc block */
    fseek(file, desc_start, SEEK_SET);
    fread(buf, 10, 1, file);
    if (strncmp(buf, "DESC", 4))
    {
        jd_log(0, "Failed to find desc start in the template file\n");
        fclose(file);
        return EINVAL;
    }
    if ((file_size - desc_start) != read_le48((unsigned char *)&buf[4]))
    {
        jd_log(0, "Inconsistent desc length in the template file!\n");
        jd_log(0, "Final chunk says %lld, first chunk says %lld\n",
               file_size - desc_start, read_le48((unsigned char *)&buf[4]));
        fclose(file);
        return EINVAL;
    }

    if (!no_md5)
        mk_MD5Init(&template_context);
    template_offset = desc_start + 10;

    jd_log(1, "Creating ISO image %s\n", output_name);

    /* Main loop - walk through the template file and expand each entry we find */
    while (1)
    {
        INT64 extent_size;
        INT64 skip = 0;
        INT64 read_length = 0;

        if (template_offset >= (file_size - 33))
        {
            jd_log(2, "Reached end of template file\n");
            break; /* Finished! */
        }
        
        if (output_offset > end_offset) /* Past the range we were asked for */
        {
            jd_log(2, "Reached end of range requested\n");            
            break;
        }
        
        fseek(file, template_offset, SEEK_SET);
        bytes = fread(buf, (MIN (BUF_SIZE, file_size - template_offset)), 1, file);
        if (1 != bytes)
        {
            jd_log(0, "Failed to read template file!\n");
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
                        jd_log(0, "Unable to read data block to skip, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    error = parse_data_block(read_length, file, end_offset - start_offset, output_offset,
                                             &template_context, outfile, no_md5);
                    if (error)
                    {
                        jd_log(0, "Unable to read data block, error %d\n", error);
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
                    error = parse_file_block(skip, read_length, end_offset - start_offset, output_offset,
                                             extent_size, outfile, dbp,
                                             (unsigned char *)&buf[15], &template_context, missing,
                                             no_md5, md5_list_head);
                    if (error)
                    {
                        jd_log(0, "Unable to read file block, error %d\n", error);
                        fclose(file);
                        return error;
                    }
                    written_length += read_length;
                }
                break;
            default:
                jd_log(0, "Unknown block type %d!\n", buf[0]);
                fclose(file);
                return EINVAL;
        }
        output_offset += extent_size;
    }

    if (missing && missing_file)
        return ENOENT;
    
    fclose(file);
    jd_log(1, "\n");
    if (!no_md5)
    {
        mk_MD5Final (image_md5sum, &template_context);
        jd_log(1, "Output image MD5 is    ");
        for (i = 0; i < 16; i++)
            jd_log(1, "%2.2x", image_md5sum[i]);
        jd_log(1, "\n");
        jd_log(1, "Output image length is %lld bytes\n", written_length);
    }

    free(buf);
    return 0;
}

int add_new_template_file(JIGDB *dbp, char *filename)
{
    int error = 0;
    INT64 template_offset = 0;
    INT64 bytes = 0;
    char *buf = NULL;
    FILE *file = NULL;
    INT64 file_size = 0;
    INT64 desc_start = 0;
    INT64 image_offset = 0;
    INT64 comp_offset = 0;
    INT64 uncomp_offset = 0;
    unsigned char tmp_md5sum[16];
    char *md5_out = NULL;
    db_template_entry_t template;
    db_block_entry_t block;
    db_compressed_entry_t compressed;
    int num_compressed_blocks = 0;
    int num_data_blocks = 0;
    int num_file_blocks = 0;
    
    file = fopen(filename, "rb");
    if (!file)
    {
        jd_log(0, "add_new_template_file: Failed to open template file %s, error %d!\n", filename, errno);
        return errno;
    }

    buf = malloc(BUF_SIZE);
    if (!buf)
    {
        jd_log(0, "add_new_template_file: Failed to malloc %d bytes. Abort!\n", BUF_SIZE);
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
        jd_log(0, "add_new_template_file: Failed to find image desc in the template file\n");
        fclose(file);
        return EINVAL;
    }

    /* Set up an entry in the template table for this template */
    template.template_size = get_file_size(filename);
    template.image_size = read_le48((unsigned char *)&buf[1]);
    template.template_mtime = get_file_mtime(filename);
    strncpy(template.template_name, filename, sizeof(template.template_name));
    
    error = mk_MD5File(filename, tmp_md5sum);
    if (error)
    {
        jd_log(0, "add_new_template_file: failed to get md5sum of template file %s, error %d\n", filename, error);
        return error;
    }
    md5_out = hex_dump(tmp_md5sum, 16);
    strncpy(template.template_md5, md5_out, sizeof(template.template_md5));
    free(md5_out);

    md5_out = hex_dump(&buf[7], 16);
    strncpy(template.image_md5, md5_out, sizeof(template.image_md5));
    free(md5_out);
    
    error = db_store_template(dbp, &template);
    if (error)
    {
        jd_log(0, "add_new_template_file: failed to store template entry for %s in the DB, error %d\n", filename, error);
        return error;
    }
    
    /* Now seek back to the start of the desc block and start parsing
     * the file/data entries to feed into the block table. */
    fseek(file, desc_start, SEEK_SET);
    fread(buf, 10, 1, file);
    if (strncmp(buf, "DESC", 4))
    {
        jd_log(0, "Failed to find desc start in template file %s\n", filename);
        fclose(file);
        return EINVAL;
    }
    if ((file_size - desc_start) != read_le48((unsigned char *)&buf[4]))
    {
        jd_log(0, "Inconsistent desc length in template file %s!\n", filename);
        jd_log(0, "Final chunk says %lld, first chunk says %lld\n",
               file_size - desc_start, read_le48((unsigned char *)&buf[4]));
        fclose(file);
        return EINVAL;
    }

    template_offset = desc_start + 10;

    strncpy(block.template_id, template.template_md5, sizeof(block.template_id));

    /* Main loop - walk through the template file and dump each entry into the DB */
    while (1)
    {
        INT64 extent_size;
        INT64 read_length = 0;

        if (template_offset >= (file_size - 33))
        {
            jd_log(2, "Reached end of template file\n");
            break; /* Finished! */
        }
        
        fseek(file, template_offset, SEEK_SET);
        bytes = fread(buf, (MIN (BUF_SIZE, file_size - template_offset)), 1, file);
        if (1 != bytes)
        {
            jd_log(0, "Failed to read template file %s!\n", filename);
            fclose(file);
            return EINVAL;
        }
        
        extent_size = read_le48((unsigned char *)&buf[1]);
        read_length = extent_size;
        
        switch (buf[0])
        {            
            case 2: /* unmatched data */
                template_offset += 7;
                block.image_offset = image_offset;
                block.size = extent_size;
                block.uncomp_offset = uncomp_offset;
                strncpy(block.template_id, template.template_md5, sizeof(block.template_id));
                block.type = 2;                
                bzero(block.md5, sizeof(block.md5));
                error = db_store_block(dbp, &block);
                if (error)
                {
                    jd_log(0, "Failed to store unmatched data block at offset %lld in template file %s!\n", template_offset, filename);
                    fclose(file);
                    return error;
                }
                num_data_blocks++;
                uncomp_offset += extent_size;
                break;

            case 6:
                template_offset += 31;
                block.image_offset = image_offset;
                block.size = extent_size;
                block.uncomp_offset = 0;
                block.type = 6;
                md5_out = hex_dump(&buf[15], 16);
                strncpy(block.md5, md5_out, sizeof(block.md5));
                free(md5_out);
                error = db_store_block(dbp, &block);
                if (error)
                {
                    jd_log(0, "Failed to store file block at offset %lld in template file %s!\n", template_offset, filename);
                    fclose(file);
                    return error;
                }
                num_file_blocks++;
                break;

            default:
                jd_log(0, "Unknown block type %d in template file %s\n", buf[0], filename);
                fclose(file);
                return EINVAL;
        }
        image_offset += extent_size;
    }

    jd_log(1, "Template file %s contains %d data blocks and %d file blocks\n",
           filename, num_data_blocks, num_file_blocks);

    /* Now go back to the start of the template file. Look at all the
     * compressed blocks and add those to the "compressed" table */
    /* Find the first compressed block */
    fseek(file, 0, SEEK_SET);
    fread(buf, BUF_SIZE, 1, file);
    for (template_offset = 0; template_offset < BUF_SIZE; template_offset++)
    {
        if (!strncmp(&buf[template_offset], "DATA", 4))
            break;
        if (!strncmp(&buf[template_offset], "BZIP", 4))
            break;
    }

    strncpy(compressed.template_id, template.template_md5, sizeof(compressed.template_id));

    comp_offset = template_offset;
    uncomp_offset = 0;
    while (1)
    {
        /* Now walk through the compressed blocks */
        fseek(file, template_offset, SEEK_SET);
        fread(buf, 16, 1, file);

        if (!strncmp(buf, "DATA", 4))
            compressed.comp_type = CT_GZIP;
        else if (!strncmp(buf, "BZIP", 4))
            compressed.comp_type = CT_BZIP2;
        else if (!strncmp(buf, "DESC", 4))
            break;
        else
        {
            jd_log(0, "Failed to find compressed block at offset %lld in template file %s!\n", template_offset, filename);
            fclose(file);
            return EINVAL;
        }

        num_compressed_blocks++;
        compressed.comp_offset = comp_offset + 16;
        compressed.comp_size = read_le48((unsigned char *)&buf[4]) - 16;
        compressed.uncomp_offset = uncomp_offset;
        compressed.uncomp_size = read_le48((unsigned char *)&buf[10]);

        uncomp_offset += compressed.uncomp_size;
        comp_offset += read_le48((unsigned char *)&buf[4]);
        template_offset += read_le48((unsigned char *)&buf[4]);
        
        error = db_store_compressed(dbp, &compressed);
        if (error)
        {
            jd_log(0, "Failed to store file block at offset %lld in template file %s!\n", template_offset, filename);
            fclose(file);
            return error;
        }
    }

    jd_log(1, "Template file %s contains %d compressed blocks\n",
           filename, num_compressed_blocks);

    fclose(file);
    free(buf);
    return 0;
}
