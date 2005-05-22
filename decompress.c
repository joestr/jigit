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
#include "jigdb.h"
#include "jte.h"

static int ungzip_data_block(char *in_buf, INT64 in_len, char *out_buf, INT64 out_len)
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
        return EIO;
    
    uc_stream.next_out = (unsigned char *)out_buf;
    uc_stream.avail_out = out_len;

    error = inflate(&uc_stream, Z_FINISH);
    if (Z_OK != error && Z_STREAM_END != error)
        return EIO;
    
    error = inflateEnd(&uc_stream);
    if (Z_OK != error)
        return EIO;
    
    return 0;
}    

#ifdef BZ2_SUPPORT
static int unbzip_data_block(char *in_buf, INT64 in_len, char *out_buf, INT64 out_len)
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
        return EIO;
    
    uc_stream.next_out = out_buf;
    uc_stream.avail_out = out_len;

    error = BZ2_bzDecompress(&uc_stream);
    if (BZ_OK != error && BZ_STREAM_END != error)
        return EIO;
    
    error = BZ2_bzDecompressEnd(&uc_stream);
    if (BZ_OK != error)
        return EIO;
    
    return 0;
}    
#endif

int decompress_data_block(char *in_buf, INT64 in_len, char *out_buf,
                                 INT64 out_len, int compress_type)
{
    switch (compress_type)
    {
#ifdef BZ2_SUPPORT
        case CT_BZIP2:
            return unbzip_data_block(in_buf, in_len, out_buf, out_len);
            break;
#endif
        case CT_GZIP:
            return ungzip_data_block(in_buf, in_len, out_buf, out_len);
            break;
        default:
            return EINVAL;
    }
}

