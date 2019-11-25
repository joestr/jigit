/*
 * jigsum
 *
 * Tool to calculate and print MD5 checksums in jigdo's awkward
 * base64-ish encoding.
 *
 * Copyright (c) 2004-2019 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "jig-base64.h"
#include "md5.h"

#define BUF_SIZE 65536

/* MD5 is 128-bit! */
#define CKSUM_BITS 128
#define CKSUM_BYTES (CKSUM_BITS / 8)
#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))
#define BASE64_CKSUM_BYTES ((ROUND_UP (CKSUM_BITS, 6)) / 6)

#ifndef MIN
#define MIN(x,y)        ( ((x) < (y)) ? (x) : (y))
#endif

enum mode_e
{
    MODE_CHECK,
    MODE_CALC
};

static int md5_file(char *filename, char *md5, int verbose)
{
    FILE *file = NULL;
    char buf[BUF_SIZE];
    unsigned char file_md5[CKSUM_BYTES] = {0};
    char *base64_md5 = NULL;
    struct mk_MD5Context file_context;
    int done = 0;
    int bytes_read = 0;
    int error = 0;
    off_t file_size = -1;

    mk_MD5Init(&file_context);

    /* Check if we're reading from stdin */
    if (!strcmp("-", filename))
        file = stdin;
    else
    {
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

    if (verbose == 3)
    {
        fprintf(stderr, "Checking %s:\r", filename);
        fflush(stdout);
    }

    if (verbose == 4)
    {
        struct stat st;
        if (file != stdin)
        {
            stat(filename, &st);
            file_size = st.st_size;
            fprintf(stderr, "Checking %s: 0 / %lld bytes\r", filename, (long long)file_size);
        }
        else
        {
            fprintf(stderr, "Checking stdin: 0 bytes\r");
        }
        fflush(stdout);
    }
    
    while (!done)
    {
        int used = 0;
        memset(buf, 0, BUF_SIZE);

        used = fread(buf, 1, BUF_SIZE, file);
        bytes_read += used;
        if (used)
            mk_MD5Update(&file_context, (unsigned char *)buf, used);
        else
        {
            if (ferror(file) && (EISDIR != errno))
            {
                fprintf(stderr, "Unable to read from file %s; error %d\n",
                        filename, errno);
                error = errno;
            }
            break;
        }
        if (verbose == 4)
        {
            if (file != stdin)
                fprintf(stderr, "Checking %s: %lld / %lld bytes\r",
                        filename, (long long) bytes_read, (long long)file_size);
            else
                fprintf(stderr, "Checking stdin: %lld bytes\r", (long long) bytes_read);
            fflush(stdout);
        }
    }
    if (verbose == 4)
        fprintf(stderr, "\n");
    
    mk_MD5Final(file_md5, &file_context);
    base64_md5 = base64_dump(file_md5, CKSUM_BYTES);
    memcpy(md5, base64_md5, BASE64_CKSUM_BYTES + 1);
    fflush(stdout);
    free(base64_md5);

    if (file != stdin)
        fclose(file);
    
    return error;
}

static int md5_check(char *filename, int verbose)
{
    FILE *file = NULL;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    char base64_md5[BASE64_CKSUM_BYTES + 1] = {0};
    int error = 0;

    /* Check if we're reading from stdin */
    if (!strcmp("-", filename))
        file = stdin;
    else
    {
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

    while ((read = getline(&line, &len, file)) != -1) {
        /* Check the format of the line we've read. Should be:

           <N chars of sum><SPACE><SPACE><filename>

	   where N == BASE64_CKSUM_BYTES

           Look for the spaces and length at least. Use the strings
           directly in the buffer, add pointers to them in place.
        */
        char *this_md5;
        char *this_filename;
        int this_error = 0;

        if (read > (BASE64_CKSUM_BYTES + 2)
	    && line[BASE64_CKSUM_BYTES] == ' '
	    && line[BASE64_CKSUM_BYTES + 1] == ' ')
        {
            line[BASE64_CKSUM_BYTES] = 0;
            this_md5 = line;
            this_filename = &line[BASE64_CKSUM_BYTES + 2];
            if (line[read - 1] == '\n')
                line[--read] = '\0';
            
            this_error = md5_file(this_filename, base64_md5, verbose);
            if (this_error)
            {
                fprintf(stderr, "Failed to read %s, error %d (%s)\n",
                        this_filename, this_error, strerror(errno));
            }
            else
            {
                if (strcmp(base64_md5, this_md5))
                {
                    if (verbose > 0)
                        fprintf(stderr, "FAILED: %s\n", this_filename);
                    this_error++;
                }
                else
                {
                    if (verbose > 1)
                        fprintf(stderr, "OK: %s\n", this_filename);
                }
            }                
        }
        else
        {
            printf("ignoring malformed line %s\n", line);
        }
        error += this_error;
    }

    if (file != stdin)
        fclose(file);

    return error;
}

int main(int argc, char **argv)
{
    int i = 0;
    char base64_md5[BASE64_CKSUM_BYTES];
    enum mode_e mode = MODE_CALC;
    int verbose = 1;
    int c = -1;
    int error = 0;

    while(1)
    {
        c = getopt(argc, argv, "cv");
        if (-1 == c)
            break;

        switch(c)
        {
            case 'v':
                verbose++;
                break;
            case 'c':
                mode = MODE_CHECK;
                break;
        }
    }

    if (mode == MODE_CALC)
    {
        if (argc == optind)
        {
            if (!md5_file("-", base64_md5, verbose))
                printf("%s  %s\n", base64_md5, "-");
        }
        else
        {
            for (i = optind; i < argc; i++)
            {
                if (!md5_file(argv[i], base64_md5, verbose))
                    printf("%s  %s\n", base64_md5, argv[i]);
                fflush(stdout);
            }
        }
    }
    else
    {
        if (argc == optind)
        {
            error += md5_check("-", verbose);
        }
        else
        {
            for (i = optind; i < argc; i++)
                error += md5_check(argv[i], verbose);
        }
    }
    
    return error;
}

