/*
 * test/demo.c
 *
 * Copyright (c) 2011 Thomas Schmitt <scdbackup@gmx.net>
 *
 * Licensed under "revised" FreeBSD license.
 * I.e. use as you like but do not claim to be the author.
 *
 */

/*
  This program demonstrates usage of libjte.

  It concatenates one or more data files to a single output file, as example
  of a payload image.
  While doing this it lets libjte create a template file and a jigdo file
  which together describe the content of the payload image. 

  The main program checks for compatibility and calls two functions:
    libjte_demo_setup()  creates a libjte_env object and sets its parameters
                         from program arguments. It also collects the list of
                         input files.
    libjte_demo_run()    produces a simple payload image format, the template
                         file, and the jigdo file. For that it reads the md5
                         file.
  Finally the main program disposes the libjte_env object.

  Function libjte_demo_write() takes care of writing a data chunk to the
  payload image as well as showing it to libjte.
 

  Applications must use 64 bit off_t. E.g. by defining
    #define _LARGEFILE_SOURCE
    #define _FILE_OFFSET_BITS 64
  or take special precautions to interface with the library by 64 bit integers
  where libjte/libjte.h prescribes off_t.
  This program gets fed with appropriate settings externally by libjte's
  autotools generated build system.


  Usage example:

    # Produce an .md5 file

    find . -name '*.c' -exec bin/jigdo-gen-md5-list '{}' ';' >test/file.md5

    # Produce payload image "test/file.outfile", the template file and
    # the jigdo file. Input is test/file.md5 and the data files *.c
   
    test/demo -outfile test/file.outfile \
              -template test/file.template \
              -jigdo test/file.jigdo \
              -md5 test/file.md5 \
              -mapping Debian=/home/ \
              *.c

    # Have a look at the human readable results

    view test/file.jigdo
    view test/file.outfile

    # Produce payload image from template and jigdo file by tool from
    # package "jigit".

    jigit-mkimage -t test/file.template \
                  -j test/file.jigdo \
                  -m Debian=/home/ \
                  -o test/file.rebuilt

    diff -q test/file.outfile test/file.rebuilt

*/


#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif
#endif

#include <errno.h>
#include <sys/stat.h>


/* In real applications use #include <libjte/libjte.h>
*/
#include "../libjte.h"


/* For simplicity, the program state is represented by global variables
   and the content of the program arguments.
*/

#define LIBJTE_DEMO_MAX_FILES 1000
#define LIBJTE_DEMO_BUFSIZE   4096

static struct libjte_env *handle = NULL;
static char *outfile = NULL;
static int filec = 0;
static char *filev[LIBJTE_DEMO_MAX_FILES + 1];


static int libjte_demo_setup(int argc, char **argv, int flag)
{
    int i, ret;
    int have_outfile = 0, have_template = 0, have_jigdo = 0, have_md5 = 0;

    ret = libjte_new(&handle, 0);
    if (ret <= 0) {
        fprintf(stderr, "Failed to create libjte_env object.\n");
        return -1;
    }

    for (i = 1; i < argc; i++) {
         if (argv[i][0] == '-')
             if (i + 1 >= argc)
                 goto usage;
         if (strcmp(argv[i], "-outfile") == 0) {
             i++;
             ret = libjte_set_outfile(handle, argv[i]);
             if (ret <= 0)
                 return ret;
             outfile = argv[i];
             have_outfile = 1;
         } else if (strcmp(argv[i], "-template") == 0) {
             i++;
             ret = libjte_set_template_path(handle, argv[i]);
             if (ret <= 0)
                 return ret;
             have_template = 1;
         } else if (strcmp(argv[i], "-jigdo") == 0) {
             i++;
             ret = libjte_set_jigdo_path(handle, argv[i]);
             if (ret <= 0)
                 return ret;
             have_jigdo = 1;
         } else if (strcmp(argv[i], "-md5") == 0) {
             i++;
             ret = libjte_set_md5_path(handle, argv[i]);
             if (ret <= 0)
                 return ret;
             have_md5 = 1;
         } else if (strcmp(argv[i], "-mapping") == 0) {
             i++;
             ret = libjte_add_mapping(handle, argv[i]);
             if (ret <= 0)
                 return ret;
         } else if (argv[i][0] == '-') {
             fprintf(stderr, "%s : Unknown option '%s'\n", argv[0], argv[i]);
             goto usage;
         } else {
             if (filec + 1 > LIBJTE_DEMO_MAX_FILES) {
                 fprintf(stderr, "%s : Too many file arguments. (> %d)\n",
                         argv[0], LIBJTE_DEMO_MAX_FILES);
                 return 0;
             }
             filev[filec++] = argv[i];
         }
    }
    if (have_outfile && have_template && have_jigdo && have_md5 && filec > 0)
         return 1;
usage:;
    fprintf(stderr, "Usage: %s -outfile path -template path -jigdo path -md5 path [-mapping To=From] file [file ...]\n", argv[0]);
    return 0;
}


static int libjte_demo_write(char *buf, int size, FILE *fp_out)
{
    int ret;

    ret = fwrite(buf, size, 1, fp_out);
    if (ret != 1) {
        fprintf(stderr, "Failed to fwrite(). errno= %d\n", errno);
        return 0;
    }
    ret= libjte_show_data_chunk(handle, buf, 1, size);
    if (ret <= 0) {
        fprintf(stderr, "libjte_show_data_chunk() failed with ret = %d\n",
                ret);
        return 0;
    }
    return 1;
}


static int libjte_demo_run(int flag)
{
    int ret, i, major, minor, micro, buf_fill;
    FILE *fp_out, *fp_in;
    char buf[LIBJTE_DEMO_BUFSIZE];
    struct stat stbuf;
    off_t todo;

    fp_out = fopen(outfile, "wb");
    if (fp_out == NULL) {
        fprintf(stderr, "Failed to open outfile for writing. errno= %d\n",
                errno);
        return 0;
    }

    ret = libjte_write_header(handle);
    if (ret <= 0)
        return ret;

    libjte__version(&major, &minor, &micro);
    sprintf(buf, "test/demo of libjte-%d.%d.%d\n", major, minor, micro);
    strcat(buf, 
           "==============================================================\n");
    ret = libjte_demo_write(buf, strlen(buf), fp_out);
    if (ret <= 0)
        return 0;

    for (i = 0; i < filec; i++) {
        ret = stat(filev[i], &stbuf);
        if (ret == -1) {
            fprintf(stderr, "Failed to stat() input file. errno= %d\n", errno);
    continue;
        }
        if (!S_ISREG(stbuf.st_mode)) {
            fprintf(stderr, "Not a data file: '%s'\n", filev[i]);
    continue;
        }
        sprintf(buf, "name=%1.1024s\nsize=%.f\n",
                     filev[i], (double) stbuf.st_size);
        strcat(buf, 
           "--------------------------------------------------------------\n");
        ret = libjte_demo_write(buf, strlen(buf), fp_out);
        if (ret <= 0)
            return 0;

        fp_in = fopen(filev[i], "rb");
        if (fp_in == NULL) {
            fprintf(stderr,
                    "Failed to open data file for reading. errno= %d\n",
                    errno);
    continue;
        }

        ret = libjte_begin_data_file(handle, filev[i], 1, stbuf.st_size);
        if (ret <= 0)
            return 0;
         
        todo = stbuf.st_size;
        while (todo > 0) {
            if (todo > LIBJTE_DEMO_BUFSIZE)
                buf_fill = LIBJTE_DEMO_BUFSIZE;
            else
                buf_fill = todo;
            ret = fread(buf, buf_fill, 1, fp_in);
            if (ret != 1) {
                fprintf(stderr, "Failed to fread() from '%s'. errno= %d\n",
                                 filev[i], errno);
                fclose(fp_in);
                return 0;
            }
            ret = libjte_demo_write(buf, buf_fill, fp_out);
            if (ret <= 0) {
                fclose(fp_in);
                return 0;
            }

            todo -= buf_fill;
        }
        fclose(fp_in);

        ret = libjte_end_data_file(handle);
        if (ret <= 0)
            return 0;

        strcpy(buf, 
         "\n--------------------------------------------------------------\n");
        ret = libjte_demo_write(buf, strlen(buf), fp_out);
        if (ret <= 0)
            return 0;
    }

    strcpy(buf, "test/demo end\n");
    ret = libjte_demo_write(buf, strlen(buf), fp_out);
    if (ret <= 0)
        return 0;

    ret = libjte_write_footer(handle);
    if (ret <= 0)
        return ret;

    fclose(fp_out);
    fp_out = NULL;
    return 1;
}


int main(int argc, char **argv)
{
    int ret, major, minor, micro;

    /* A warning to programmers who start their own projekt from here. */
    if (sizeof(off_t) != 8) {
        fprintf(stderr,
               "\n%s: Compile time misconfiguration. off_t is not 64 bit.\n\n",
               argv[0]);
        exit(1);
    }

    /* Applications should as first check the dynamically loaded library.
    */
    if (! libjte__is_compatible(LIBJTE_VERSION_MAJOR, LIBJTE_VERSION_MINOR,
                                LIBJTE_VERSION_MICRO, 0)) {
        libjte__version(&major, &minor, &micro);
        fprintf(stderr, "%s : Found libjte-%d.%d.%d , need libjte-%d.%d.%d \n",
                         argv[0], major, minor, micro,
                         LIBJTE_VERSION_MAJOR, LIBJTE_VERSION_MINOR,
                         LIBJTE_VERSION_MICRO);
        exit(2);
    }

    ret = libjte_demo_setup(argc, argv, 0);
    if (ret <= 0) {
        libjte_destroy(&handle);    
        exit(3);
    }

    ret = libjte_demo_run(0);
    if (ret <= 0) {
        libjte_destroy(&handle);    
        exit(4);
    }

    libjte_destroy(&handle);    
    exit(0);
}

