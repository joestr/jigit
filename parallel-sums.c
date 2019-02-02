/*
 * parallel-sums
 *
 * Calculate and print various checksums in parallel
 *
 * Copyright (c) 2015 - 2019 Steve McIntyre <steve@einval.com>
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
#include <getopt.h>
#include <pthread.h>
#include <sys/time.h>

static char *progname = "parallel-sums";

#define THREADED_CHECKSUMS
#include "libjte/checksum.h"

#ifndef MIN
#define MIN(x,y)        ( ((x) < (y)) ? (x) : (y))
#endif

struct state
{
    int desired;
    char *out_filename;
    FILE *out_stream;
};

/* Simple circular buffer implementation */
#define QSIZE 4
#define BUF_SIZE (1024*1024)
struct buf 
{
    unsigned long long bytes;
    unsigned char      d[BUF_SIZE];
};
#define NUM_BUF (QSIZE + 2)
struct buf data[NUM_BUF];

struct {
    pthread_mutex_t mutex;
    int read_idx; /* index in the data[]array for the reader to point at */
    int put_idx;
    int get_idx;
    int num_full;
    void *data[QSIZE];
    pthread_t reader;
    pthread_t processor;	
    pthread_cond_t notfull;
    pthread_cond_t notempty;
    FILE *infile;
} queue;

#define TIME long long
TIME total_read = 0;
TIME total_proc = 0;
unsigned long long bytes_read;
unsigned long long bytes_proc;
static struct state state[NUM_CHECKSUMS];
static struct option opts[NUM_CHECKSUMS + 2];
int checksums_mask = 0;

static void get_time(TIME *newtime)
{
    struct timeval tv;
    struct timezone tz;

    tz.tz_minuteswest = 0;
    tz.tz_dsttime = 0;    

    gettimeofday(&tv, &tz);
    *newtime = ((TIME)tv.tv_sec * 1000000) + tv.tv_usec;
}

void put_cb_data(void *data)
{
    pthread_mutex_lock(&queue.mutex);
    /* wait while the buffer is full */
    while (queue.num_full == QSIZE)
        pthread_cond_wait(&queue.notfull, &queue.mutex);
    queue.data[queue.put_idx] = data;
    queue.put_idx++;
    if (queue.put_idx >= QSIZE)
        queue.put_idx = 0;
    queue.num_full++;
    /* let a waiting consumer know there is data */
    pthread_cond_signal(&queue.notempty);
    pthread_mutex_unlock(&queue.mutex);
}

void *get_cb_data(void)
{
    void *data;

    pthread_mutex_lock(&queue.mutex);
    /* wait while there is nothing in the buffer */
    while (queue.num_full == 0)
        pthread_cond_wait(&queue.notempty, &queue.mutex);
    data = queue.data[queue.get_idx];
    queue.get_idx++;
    if (queue.get_idx >= QSIZE)
        queue.get_idx = 0;
    queue.num_full--;
    /* let a waiting producer know there is room */
    pthread_cond_signal(&queue.notfull);
    pthread_mutex_unlock(&queue.mutex);
    return data;
}

void *reader(void * junk)
{
    TIME start,stop,diff;
    while (1)
    {
        struct buf *in_ptr = &data[queue.read_idx];
        get_time(&start);
        in_ptr->bytes = fread(in_ptr->d, 1, BUF_SIZE, queue.infile);
        get_time(&stop);
        bytes_read += in_ptr->bytes;
        diff = stop - start;
	total_read += diff;
        put_cb_data(in_ptr);
        queue.read_idx++;
	if (queue.read_idx >= NUM_BUF)
            queue.read_idx = 0;

        if (in_ptr->bytes == 0)
            return NULL;
    }
}

void *processor(checksum_context_t *ctx)
{
    TIME start,stop,diff;
    while (1)
    {
        struct buf *ptr = get_cb_data();
        if (ptr && ptr->bytes)
        {
            get_time(&start);
            checksum_update(ctx, ptr->d, ptr->bytes);
            get_time(&stop);
            diff = stop - start;
            total_proc += diff;
            bytes_proc += ptr->bytes;
        }
        else
            return NULL;
    }
}

void usage()
{
    int i = 0;

    printf("%s\n\n", progname);
    printf("Usage: parallel-sums <--algo1 ALGO1FILE> [--algo2 ALGO2FILE ... -algo2N algoNfile] \\\n");
    printf("       <file1> [file2 ... fileN]\n\n");

    printf("Calculate checksums for a set of files (e.g. md5sum, sha1sum) in parallel,\n");
    printf("reducing repeated file I/O.\n");
    printf("This version of %s is compiled with support for the following checksum algorithms:\n", progname);
    for (i = 0; i < NUM_CHECKSUMS; i++)
    {
        struct checksum_info *info = checksum_information(i);
        printf("  %s (--%s)\n", info->name, info->prog);
    }
    exit (0);
}

int main(int argc, char **argv)
{
    int i = 0;
    int c = 0;
    int num_desired = 0;

    struct checksum_info *info= NULL;
    
    for (i = 0; i < NUM_CHECKSUMS; i++)
    {
        info = checksum_information(i);
        opts[i].name = strdup(info->prog);
	opts[i].has_arg = required_argument;
	opts[i].flag = NULL;
	opts[i].val = i;
    }

    opts[i].name = "help";
    opts[i].has_arg = no_argument;
    opts[i].flag = NULL;
    opts[i].val = i;
    i++;

    opts[i].name = NULL;
    opts[i].has_arg = 0;
    opts[i].flag = NULL;
    opts[i].val = 0;
    
    while (1)
    {
        int option_index = 0;
        c = getopt_long(argc, argv, "", opts, &option_index);
        if (c == -1 || c == '?')
        {
            break;
        }
	if (c == 4) {
            usage();
	}
	state[c].desired = 1;
	state[c].out_filename = strdup(optarg);
	state[c].out_stream = NULL;
	num_desired++;
	checksums_mask |= (1 << c);
    }

    if (0 == num_desired)
    {
        fprintf(stderr, "No checksum algorithms requested, bailing...\n");
	return 0;
    }

    if (optind >= argc)
    {
        /* nothing to do! */
        fprintf(stderr, "No files listed, nothing to do\n");
        return 0;
    }

    /* else - we have to work to do! */

    /* Open all the output files we want */
    for (i = 0; i < NUM_CHECKSUMS; i++)
    {
        if (state[i].desired)
	{
            state[i].out_stream = fopen(state[i].out_filename, "wb");
            if (NULL == state[i].out_stream)
	    {
                fprintf(stderr, "Can't open checksum file %s for writing, %s\n",
		        state[i].out_filename, strerror(errno));
	        return errno;
	    }
        }
    }

    /* Now work through the list of input files. */
    while (optind < argc)
    {
        checksum_context_t *ctx = checksum_init_context(checksums_mask, argv[optind]);
        if (NULL == ctx)
        {
            fprintf(stderr, "Failed to allocate memory for checksum context. ABORT\n");
            return ENOMEM;
        }

        queue.infile = fopen(argv[optind], "rb");
        if (queue.infile)
        {
            /* Main loop: read data, update checksums */
            queue.read_idx = 0;
            queue.put_idx = 0;
            queue.get_idx = 0;
            queue.num_full = 0;
            pthread_mutex_init(&queue.mutex, NULL);
	    pthread_cond_init(&queue.notfull, NULL);
	    pthread_cond_init(&queue.notempty, NULL);
            pthread_create(&queue.reader, NULL, reader, NULL);
            pthread_create(&queue.processor, NULL, processor, ctx);

            /* stuff happens in the threads */
            pthread_join (queue.reader, NULL);
            pthread_join (queue.processor, ctx);
            fclose(queue.infile);
            checksum_final(ctx);

            for (i = 0; i < NUM_CHECKSUMS; i++)
            {
                if (state[i].desired)
	        {
                    fprintf(state[i].out_stream, "%s  %s\n",
                            checksum_hex(ctx, i),
                            argv[optind]);
                    fflush(state[i].out_stream);
                }
            }
            checksum_free_context(ctx);
            optind++;
        }
    }

    /* Done with all the input files. Flush and close all the output
     * checksum files. */
    for (i = 0; i < NUM_CHECKSUMS; i++)
    {
        if (state[i].desired)
	{
            fflush(state[i].out_stream);
            fclose(state[i].out_stream);
        }
    }

    printf("bytes_read: %lld, processed: %lld\n", bytes_read, bytes_proc);
    printf("Time taken: %3f seconds reading, %3f seconds processing\n",
           ((double)total_read / 1000000), ((double)total_proc / 1000000));

    /* All done */
    return 0;
}
