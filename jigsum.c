#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "md5.h"

#define BUF_SIZE 65536

#ifndef MIN
#define MIN(x,y)        ( ((x) < (y)) ? (x) : (y))
#endif

static int md5_file(char *filename)
{
    FILE *file = NULL;
    char buf[BUF_SIZE];
    unsigned char file_md5[16] = {0};
    char *base64_md5 = NULL;
    struct mk_MD5Context file_context;
    int done = 0;
    int bytes_read = 0;

    mk_MD5Init(&file_context);

    /* Check if we're reading from stdin */
    if (!strcmp("-", filename))
        file = stdin;
    else
    {
        fprintf(stderr, "\r %-75.75s", filename);
        file = fopen(filename, "rb");
        if (!file)
        {
            switch (errno)
            {
                case EACCES:
                case EISDIR:
                    break;
                default:
                    fprintf(stderr, "Unable to open file %s; error %d\n", filename, errno);
                    break;
            }
            return errno;
        }
    }
    
    while (!done)
    {
        int used = 0;
        memset(buf, 0, BUF_SIZE);

        used = fread(buf, 1, BUF_SIZE, file);
        bytes_read += used;
        if (used)
            mk_MD5Update(&file_context, buf, used);
        else
        {
            if (ferror(file) && (EISDIR != errno))
                fprintf(stderr, "Unable to read from file %s; error %d\n",
                        filename, errno);
            break;
        }
    }
    
    mk_MD5Final(file_md5, &file_context);
    base64_md5 = base64_dump(file_md5, 16);
    if (file != stdin)
    {
        fclose(file);
        if (bytes_read)
            printf("%s  %s\n", base64_md5, filename);
    }
    else
        if (bytes_read)
            printf("%s\n", base64_md5);
    fflush(stdout);
    
    return 0;
}

int main(int argc, char **argv)
{
    int i = 0;
    
    for (i = 1; i < argc; i++)
        (void) md5_file(argv[i]);

    return 0;
}

