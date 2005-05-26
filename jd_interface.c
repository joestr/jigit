/*
 * jd_interface.c
 *
 * Interfaces for building an ISO image, using the data stored in the DB
 *
 * Copyright (c) 2005 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

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
#include "jigdb.h"
#include "jte.h"

typedef struct _jd_state
{
    char *template_filename;
    FILE *template;
} jd_state_t;

JD *jd_open(char *template_file)
{
    jd_state_t *jd = NULL;
    
    jd = calloc(1, sizeof(jd_state_t));
    if (!jd)
    {
        errno = ENOMEM;
        return NULL;
    }
    
    jd->template_filename = template_file;
    jd->template = fopen(template_file, "rb");
    if (!jd->template)
    {
        free(jd);
        return NULL;
    }

    return jd;
}

int jd_close(JD *state)
{
    jd_state_t *jd = state;
    
    if (jd)
    {
        if (jd->template)
            fclose(jd->template);
        free(jd);
    }
    return 0;
}

int jd_md5(JD *state, unsigned char md5[16]);

int jd_read(JD *state, INT64 start_offset, INT64 length, unsigned char *buffer, INT64 *bytes_read);
int jd_size(JD *state, INT64 *size);
int jd_last_filename(JD *state, char **name);
