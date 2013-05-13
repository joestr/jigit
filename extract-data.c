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
#include "endian.h"
#include "md5.h"
#include "jigdo.h"

static FILE *logfile;
static zip_state_t zip_state;

int main(int argc, char **argv)
{
    FILE *in_file, *out_file;
    char *in_filename = argv[1];
    char *out_filename = argv[2];

    logfile = stderr;
    
    if (argc < 2)
    {
        fprintf(stderr, "ERROR: Need input file and output filename\n");
        return 1;
    }
    
    in_file = fopen(in_filename, "rb");
    if (!in_file)
    {
        fprintf(stderr, "ERROR: Failed to open input file %s for reading, error %d\n", in_filename, errno);
        return 1;
    }
        
    out_file = fopen(out_filename, "wb");
    if (!out_file)
    {
        fprintf(stderr, "ERROR: Failed to open output file %s for writing, error %d\n", out_filename, errno);
        return 1;
    }
        
    zip_state.total_offset = 0;
    while (1)
    {
        if (read_data_block(in_file, logfile, &zip_state))
            break;
        fwrite(zip_state.data_buf, zip_state.buf_size, 1, out_file);
        fprintf(stderr, "Wrote %lld bytes\n", zip_state.buf_size);
    }

    fclose(in_file);
    fclose(out_file);

    return 0;
}
