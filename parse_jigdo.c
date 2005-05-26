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

INT64 get_file_size(char *filename)
{
    struct stat sb;
    int error = 0;
    
    error = stat(filename, &sb);
    if (error)
        return MISSING;
    else
        return sb.st_size;
}

time_t get_file_mtime(char *filename)
{
    struct stat sb;
    int error = 0;
    
    error = stat(filename, &sb);
    if (error)
        return MISSING;
    else
        return sb.st_mtime;
}

md5_list_t *find_file_in_md5_list(unsigned char *base64_md5)
{
    md5_list_t *md5_list_entry = G_md5_list_head;
    
    while (md5_list_entry)
    {        
        if (!memcmp(md5_list_entry->md5, base64_md5, 16))
            return md5_list_entry;
        /* else */
        md5_list_entry = md5_list_entry->next;
    }
    return NULL; /* Not found */
}

static int add_md5_entry(INT64 size, char *md5, char *path)
{
    md5_list_t *new = NULL;    
    new = calloc(1, sizeof(*new));
    if (!new)
        return ENOMEM;

    new->md5 = md5;
    new->full_path = path;
    new->file_size = size;
    
    if (!G_md5_list_head)
    {
        G_md5_list_head = new;
        G_md5_list_tail = new;
    }
    else
    {
        G_md5_list_tail->next = new;
        G_md5_list_tail = new;
    }
    
    return 0;
}

static int parse_md5_entry(char *md5_entry)
{
    int error = 0;
    char *file_name = NULL;
    char *md5 = NULL;
    INT64 file_size = 0;

    md5_entry[22] = 0;
    md5_entry[23] = 0;

    md5 = md5_entry;
    file_name = &md5_entry[24];

    if ('\n' == file_name[strlen(file_name) -1])
        file_name[strlen(file_name) - 1] = 0;
    
    file_size = get_file_size(file_name);

    error = add_md5_entry(file_size, md5, file_name);
    return 0;
}

int parse_md5_file(char *filename)
{
    char buf[2048];
    FILE *file = NULL;
    char *ret = NULL;
    int error = 0;

    file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(G_logfile, "Failed to open MD5 file %s, error %d!\n", filename, errno);
        return errno;
    }
    
    while(1)
    {
        ret = fgets(buf, sizeof(buf), file);
        if (NULL == ret)
            break;
        error = parse_md5_entry(strdup(buf));
    }
    return 0;
}

static int file_exists(char *path, INT64 *size)
{
    struct stat sb;
    int error = 0;
    
    error = stat(path, &sb);
    if (!error && S_ISREG(sb.st_mode))
    {
        *size = sb.st_size;
        return 1;
    }
    
    /* else */
    return 0;
}

static int find_file_in_mirror(char *jigdo_match, char *jigdo_name,
                               char *match, INT64 *file_size, char **mirror_path)
{
    match_list_t *entry = G_match_list_head;
    char path[PATH_MAX];

    while (entry)
    {
        if (!strcmp(entry->match, match))
        {
            sprintf(path, "%s/%s", entry->mirror_path, jigdo_name);
            if (file_exists(path, file_size))
            {
                *mirror_path = strdup(path);
                return 0;
            }
        }
        entry = entry->next;
    }
    
    *mirror_path = jigdo_name;
    return ENOENT;
}


/* DELIBERATELY do not sort these, or do anything clever with
   insertion. The entries in the jigdo file should be in the same
   order as the ones we'll want from the template. Simply add to the
   end of the singly-linked list each time! */
static int add_file_entry(char *jigdo_entry)
{
    int error = 0;
    char *file_name = NULL;
    INT64 file_size = 0;
    char *ptr = jigdo_entry;
    char *base64_md5 = NULL;
    char *match = NULL;
    char *jigdo_name = NULL;
    
    /* Grab out the component strings from the entry in the jigdo file */
    base64_md5 = jigdo_entry;
    while (0 != *ptr)
    {
        if ('=' == *ptr)
        {
            *ptr = 0;
            ptr++;
            match = ptr;
        }
        else if (':' == *ptr)
        {
            *ptr = 0;
            ptr++;
            jigdo_name = ptr;
        }
        else if ('\n' == *ptr)
            *ptr = 0;
        else
            ptr++;
    }

    if (find_file_in_md5_list(base64_md5))
        return 0; /* We already have an entry for this file; don't
                   * waste any more time on it */

    /* else look for the file in the filesystem */
    if (NULL == match || NULL == jigdo_name)
    {
        fprintf(G_logfile, "Could not parse malformed jigdo entry \"%s\"\n", jigdo_entry);
        return EINVAL;
    }
    error = find_file_in_mirror(match, jigdo_name, match, &file_size, &file_name);

    if (error)
	{
		if (G_missing_filename)
			add_md5_entry(MISSING, base64_md5, file_name);
		else
		{
			fprintf(G_logfile, "Unable to find a file to match %s\n", file_name);
			fprintf(G_logfile, "Abort!\n");
			exit (ENOENT);
		}
	}
    else
        add_md5_entry(file_size, base64_md5, file_name);
    return 0;
}



int parse_jigdo_file(char *filename)
{
    char buf[2048];
    gzFile *file = NULL;
    char *ret = NULL;
    int error = 0;
    
    file = gzopen(filename, "rb");
    if (!file)
    {
        fprintf(G_logfile, "Failed to open jigdo file %s, error %d!\n", filename, errno);
        return errno;
    }

    /* Find the [Parts] section of the jigdo file */
    while (1)
    {
        ret = gzgets(file, buf, sizeof(buf));
        if (NULL == ret)
            break;
        if (!strncmp(buf, "[Parts]", 7))
            break;
    }

    /* Now grab the individual file entries and build a list */
    while (1)
    {
        ret = gzgets(file, buf, sizeof(buf));
        if (NULL == ret || !strcmp(buf, "\n"))
            break;
        if (!strcmp(buf, "[") || !strcmp(buf, "#"))
            continue;
        error = add_file_entry(strdup(buf));
        if (error)
            break;
    }

    gzclose(file);
    return error;
}
