#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <bzlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "endian.h"
#include "md5.h"
#include "jigdo.h"

static int ungzip_data_block(char *in_buf, INT64 in_len, char *out_buf, INT64 out_len, FILE *logfile)
{
    int error = 0;
    z_stream uc_stream;
    
    uc_stream.zalloc = NULL;
    uc_stream.zfree = NULL;
    uc_stream.opaque = NULL;
    uc_stream.next_in = (unsigned char *)in_buf;
    uc_stream.avail_in = in_len;

    error = inflateInit(&uc_stream);
    if (Z_OK != error)
    {
        fprintf(logfile, "ungzip_data_block: failed to init, error %d\n", error);
        return EIO;
    }
    
    uc_stream.next_out = (unsigned char *)out_buf;
    uc_stream.avail_out = out_len;

    error = inflate(&uc_stream, Z_FINISH);
    if (Z_OK != error && Z_STREAM_END != error)
    {
        fprintf(logfile, "ungzip_data_block: failed to decompress, error %d\n", error);
        return EIO;
    }
    
    error = inflateEnd(&uc_stream);
    if (Z_OK != error)
    {
        fprintf(logfile, "ungzip_data_block: failed to end, error %d\n", error);
        return EIO;
    }
    
    return 0;
}    

static int unbzip2_data_block(char *in_buf, INT64 in_len, char *out_buf, INT64 out_len, FILE *logfile)
{
    int error = 0;
    bz_stream uc_stream;
    
    uc_stream.bzalloc = NULL;
    uc_stream.bzfree = NULL;
    uc_stream.opaque = NULL;
    uc_stream.next_in = in_buf;
    uc_stream.avail_in = in_len;

    error = BZ2_bzDecompressInit(&uc_stream, 0, 0);
    if (BZ_OK != error)
    {
        fprintf(logfile, "unbzip2_data_block: failed to init, error %d\n", error);
        return EIO;
    }
    
    uc_stream.next_out = out_buf;
    uc_stream.avail_out = out_len;

    error = BZ2_bzDecompress(&uc_stream);
    if (BZ_OK != error && BZ_STREAM_END != error)
    {
        fprintf(logfile, "unbzip2_data_block: failed to decompress, error %d\n", error);
        return EIO;
    }
    
    error = BZ2_bzDecompressEnd(&uc_stream);
    if (BZ_OK != error)
    {
        fprintf(logfile, "unbzip2_data_block: failed to end, error %d\n", error);
        return EIO;
    }
    
    return 0;
}    

int read_data_block(FILE *template_file, FILE *logfile, zip_state_t *zip_state)
{
    char inbuf[1024];
    INT64 i = 0;
    static INT64 template_offset = -1;
    INT64 compressed_len = 0;
    INT64 uncompressed_len = 0;
    char *comp_buf = NULL;
    int read_num = 0;
    int error = 0;
    int compress_type = COMP_GZIP; /* Sensible default */

    /* If we've just started on this template file, find the first
     * compressed data block */
    if (-1 == template_offset)
    {
        fseek(template_file, 0, SEEK_SET);
        fread(inbuf, sizeof(inbuf), 1, template_file);
        for (i = 0; i < sizeof(inbuf); i++)
        {
            if (!strncmp(&inbuf[i], "DATA", 4) || !strncmp(&inbuf[i], "BZIP", 4))
            {
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

    /* Seek to the beginning of the (first/next) compressed data
     * block, identify the compression type then decompress using the
     * appropriate algorithm */
    fseek(template_file, template_offset, SEEK_SET);
    fread(inbuf, 16, 1, template_file);
    if (!strncmp(inbuf, "DATA", 4))
        compress_type = COMP_GZIP;
    else if (!strncmp(inbuf, "BZIP", 4))
        compress_type = COMP_BZIP2;
    else
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
    
    zip_state->data_buf = calloc(1, uncompressed_len);
    if (!zip_state->data_buf)
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

    switch (compress_type)
    {
        case COMP_GZIP:
            error = ungzip_data_block(comp_buf, compressed_len,
                                      zip_state->data_buf, uncompressed_len, logfile);
            break;
        case COMP_BZIP2:
            error = unbzip2_data_block(comp_buf, compressed_len,
                                       zip_state->data_buf, uncompressed_len, logfile);
            break;
    }

    if (error)
    {
        fprintf(logfile, "Unable to decompress data block, error %d\n", error);
        return error;
    }
        
    template_offset += compressed_len;
    zip_state->buf_size = uncompressed_len;
    zip_state->offset_in_curr_buf = 0;
    free (comp_buf);
    return 0;
}
