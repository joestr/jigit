#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <zlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "endian.h"
#include "md5.h"

typedef unsigned long long UINT64;
typedef unsigned long      UINT32;

#define BUF_SIZE 65536

#ifndef MIN
#define MIN(x,y)        ( ((x) < (y)) ? (x) : (y))
#endif

FILE *logfile = NULL;
int quick = 0;

typedef enum state_
{
    STARTING,
    IN_DATA,
    IN_DESC,
    DUMP_DESC,
    DONE,
    ERROR
} e_state;

typedef struct match_list_
{
    struct match_list_ *next;
    char *match;
    char *mirror_path;
} match_list_t;

match_list_t *match_list_head = NULL;
match_list_t *match_list_tail = NULL;

typedef struct jigdo_list_
{
    struct jigdo_list_ *next;
    off_t file_size;
    char *md5;
    char *full_path;
} jigdo_list_t;

jigdo_list_t *jigdo_list_head = NULL;
jigdo_list_t *jigdo_list_tail = NULL;

struct
{
    char   *data_buf;
    size_t  buf_size;
    off_t   curr_offset;
} zip_state;

static int add_match_entry(char *match)
{
    match_list_t *entry = NULL;
    char *mirror_path = NULL;
    char *ptr = match;

    /* Split "Foo=/mirror/foo" into its components */
    while (*ptr)
    {
        if ('=' == *ptr)
        {
            *ptr = 0;
            ptr++;
            mirror_path = ptr;
            break;
        }
        ptr++;
    }

    if (!mirror_path)
    {
        fprintf(logfile, "Could not parse malformed match entry \"%s\"\n", match);
        return EINVAL;
    }        
    
    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return ENOMEM;

    fprintf(logfile, "Adding match entry %s:%s\n", match, mirror_path);

    entry->match = match;
    entry->mirror_path = mirror_path;
    
    if (!match_list_head)
    {
        match_list_head = entry;
        match_list_tail = entry;
    }
    else
    {
        match_list_tail->next = entry;
        match_list_tail = entry;
    }
    
    return 0;
}

static int file_exists(char *path, off_t *size)
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

static char *base64_dump(unsigned char *buf, size_t buf_size)
{
    const char *b64_enc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int value = 0;
    unsigned int i;
    int bits = 0;
    char *out = calloc(1, 30);
    unsigned int out_pos = 0;

    if (!out)
        return NULL;

    for (i = 0; i < buf_size ; i++)
    {
        value = (value << 8) | buf[i];
        bits += 2;
        out[out_pos++] = b64_enc[(value >> bits) & 63U];
        if (bits >= 8) {
            bits -= 6;
            out[out_pos++] = b64_enc[(value >> bits) & 63U];
        }
    }
    if (bits > 0)
    {
        value <<= 8 - bits;
        out[out_pos++] = b64_enc[(value >> bits) & 63U];
    }
    return out;
}

static int find_file_in_mirror(char *jigdo_entry, char **mirror_path, char **md5sum, off_t *file_size)
{
    match_list_t *entry = match_list_head;
    char path[PATH_MAX];
    char *jigdo_name = NULL;
    char *match = NULL;
    char *ptr = jigdo_entry;

    *md5sum = jigdo_entry;

    /* Grab out the component strings from the entry in the jigdo file */
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

    if (NULL == match || NULL == jigdo_name)
    {
        fprintf(logfile, "Could not parse malformed jigdo entry \"%s\"\n", jigdo_entry);
        return EINVAL;
    }

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
    
    fprintf(logfile, "Could not find file %s:%s in any path\n", match, jigdo_name);
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
    char *md5 = NULL;
    jigdo_list_t *new = NULL;
    off_t file_size = 0;
    
    error = find_file_in_mirror(jigdo_entry, &file_name, &md5, &file_size);
    if (error)
        return error;
    
    new = calloc(1, sizeof(*new));
    if (!new)
        return ENOMEM;

    new->md5 = md5;
    new->full_path = file_name;
    new->file_size = file_size;
    
    if (!jigdo_list_head)
    {
        jigdo_list_head = new;
        jigdo_list_tail = new;
    }
    else
    {
        jigdo_list_tail->next = new;
        jigdo_list_tail = new;
    }
    
    return 0;
}

static int parse_jigdo_file(char *filename)
{
    unsigned char buf[2048];
    FILE *file = NULL;
    char *ret = NULL;
    int error = 0;
    
    file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(logfile, "Failed to open jigdo file %s, error %d!\n", filename, errno);
        return errno;
    }

    /* Find the [Parts] section of the jigdo file */
    while (1)
    {
        ret = fgets(buf, sizeof(buf), file);
        if (NULL == ret)
            break;
        if (!strncmp(buf, "[Parts]", 7))
            break;
    }

    /* Now grab the individual file entries and build a list */
    while (1)
    {
        ret = fgets(buf, sizeof(buf), file);
        if (NULL == ret || !strcmp(buf, "\n"))
            break;
        if (!strcmp(buf, "[") || !strcmp(buf, "#"))
            continue;
        error = add_file_entry(strdup(buf));
        if (error)
            return error;
    }
    
    return 0;
}

static off_t get_file_size(char *filename)
{
    struct stat sb;
    int error = 0;
    
    error = stat(filename, &sb);
    if (error)
        return -1;
    else
        return sb.st_size;
}

static int decompress_data_block(char *in_buf, size_t in_len, char *out_buf, size_t out_len)
{
    int error = 0;
    z_stream uc_stream;
    
    uc_stream.zalloc = NULL;
    uc_stream.zfree = NULL;
    uc_stream.opaque = NULL;
    uc_stream.next_in = in_buf;
    uc_stream.avail_in = in_len;

    error = inflateInit(&uc_stream);
    if (Z_OK != error)
    {
        fprintf(logfile, "decompress_data_block: failed to init, error %d\n", error);
        return EIO;
    }
    
    uc_stream.next_out = out_buf;
    uc_stream.avail_out = out_len;

    error = inflate(&uc_stream, Z_FINISH);
    if (Z_OK != error && Z_STREAM_END != error)
    {
        fprintf(logfile, "decompress_data_block: failed to decompress, error %d\n", error);
        return EIO;
    }
    
    error = inflateEnd(&uc_stream);
    if (Z_OK != error)
    {
        fprintf(logfile, "decompress_data_block: failed to end, error %d\n", error);
        return EIO;
    }
    
    return 0;
}    

static int read_data_block(FILE *template_file)
{
    char inbuf[1024];
    off_t i = 0;
    static off_t template_offset = -1;
    off_t compressed_len = 0;
    off_t uncompressed_len = 0;
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
    
    fseek(template_file, template_offset, SEEK_SET);
    fread(inbuf, 16, 1, template_file);
    if (strncmp(inbuf, "DATA", 4))
    {
        fprintf(logfile, "Unable to locate DATA block in template (offset %lld)\n",
                template_offset);
        return EINVAL;
    }    
    
    compressed_len = read_le48(&inbuf[4]);
    uncompressed_len = read_le48(&inbuf[10]);

    comp_buf = calloc(1, compressed_len);
    if (!comp_buf)
    {
        fprintf(logfile, "Unable to locate DATA block in template (offset %lld)\n",
                template_offset);
        return ENOMEM;
    }
    
    zip_state.data_buf = calloc(1, uncompressed_len);
    if (!zip_state.data_buf)
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

    error = decompress_data_block(comp_buf, compressed_len,
                                  zip_state.data_buf, uncompressed_len);
    if (error)
    {
        fprintf(logfile, "Unable to decompress data block, error %d\n", error);
        return error;
    }
        
    template_offset += compressed_len;
    zip_state.buf_size = uncompressed_len;
    zip_state.curr_offset = 0;
    free (comp_buf);
    return 0;
}

static int parse_data_block(size_t data_size, FILE *template_file, struct mk_MD5Context *context)
{
    int error = 0;
    size_t remaining = data_size;
    size_t size = 0;

    while (remaining)
    {
        if (!zip_state.data_buf)
        {
            error = read_data_block(template_file);
            if (error)
            {
                fprintf(logfile, "Unable to decompress template data, error %d\n",
                        error);
                return error;
            }
        }
        size = MIN((zip_state.buf_size - zip_state.curr_offset), remaining);
        fwrite(&zip_state.data_buf[zip_state.curr_offset], 1, size, stdout);
        if (!quick)
            mk_MD5Update(context, &zip_state.data_buf[zip_state.curr_offset], size);
        zip_state.curr_offset += size;
        remaining -= size;
        
        if (zip_state.curr_offset == zip_state.buf_size)
        {
            free(zip_state.data_buf);
            zip_state.data_buf = NULL;
        }
    }
    
    fprintf(logfile, "parse_data_block: wrote %d bytes of unmatched data\n", data_size);
    return error;
}

static int parse_file_block(off_t file_size, char *md5, struct mk_MD5Context *image_context)
{
    char *base64_md5 = base64_dump(md5, 16);
    FILE *input_file = NULL;
    char buf[BUF_SIZE];
    size_t remaining = file_size;
    int num_read = 0;
    struct mk_MD5Context file_context;
    char file_md5[16];
    jigdo_list_t *jigdo_list_current = jigdo_list_head;
    
    if (!quick)
        mk_MD5Init(&file_context);

    while (jigdo_list_current)
    {        
        if ( (jigdo_list_current->file_size == file_size) &&
             (!memcmp(jigdo_list_current->md5, base64_md5, 16) ) )
        {
            input_file = fopen(jigdo_list_current->full_path, "rb");
            if (!input_file)
            {
                fprintf(logfile, "Unable to open mirror file %s, error %d\n",
                        jigdo_list_current->full_path, errno);
                return errno;
            }
            while (remaining)
            {
                int size = MIN(BUF_SIZE, remaining);
                memset(buf, 0, BUF_SIZE);

                num_read = fread(buf, size, 1, input_file);
                if (!num_read)
                {
                    fprintf(logfile, "Unable to open mirror file %s, error %d\n",
                            jigdo_list_current->full_path, errno);
                    fclose(input_file);
                    return errno;
                }
                if (!quick)
                {
                    mk_MD5Update(image_context, buf, size);
                    mk_MD5Update(&file_context, buf, size);
                }
            
                fwrite(buf, size, 1, stdout);
            
                remaining -= size;
            }
            fprintf(logfile, "parse_file_block: wrote %lld bytes of data from %s\n",
                    file_size, jigdo_list_current->full_path);
            fclose(input_file);

            if (!quick)
            {
                mk_MD5Final(file_md5, &file_context);
        
                if (memcmp(file_md5, md5, 16))
                {
                    fprintf(logfile, "MD5 MISMATCH for file %s\n", jigdo_list_current->full_path);
                    fprintf(logfile, "    template looking for %s\n", md5);
                    fprintf(logfile, "    file in mirror is    %s\n", file_md5);
                    return EINVAL;
                }
            }
            return 0;
        }
        jigdo_list_current = jigdo_list_current->next;
    }
    return ENOENT;
}

static int parse_template_file(char *filename)
{
    off_t offset = 0;
    off_t bytes = 0;
    unsigned char *buf = NULL;
    FILE *file = NULL;
    off_t file_size = 0;
    off_t desc_start = 0;
    off_t image_length = 0;
    off_t written_length = 0;
    int i = 0;
    int error = 0;
    struct mk_MD5Context template_context;
    unsigned char image_md5sum[16];
    
    file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(logfile, "Failed to open template file %s, error %d!\n", filename, errno);
        return errno;
    }

    buf = malloc(BUF_SIZE);
    if (!buf)
    {
        fprintf(logfile, "Failed to malloc %d bytes. Abort!\n", BUF_SIZE);
        fclose(file);
        return ENOMEM;
    }

    /* Find the beginning of the desc block */
    file_size = get_file_size(filename);
    fseek(file, file_size - 6, SEEK_SET);
    fread(buf, 6, 1, file);
    desc_start = file_size - read_le48(buf);

    /* Now seek back to the beginning image desc block to grab the MD5
       and image length */
    fseek(file, file_size - 33, SEEK_SET);
    fread(buf, BUF_SIZE, 1, file);
    if (buf[0] != 5) /* image data */
    {
        fprintf(logfile, "Failed to find image desc in the template file\n");
        fclose(file);
        return EINVAL;
    }
    image_length = read_le48(&buf[1]);
    fprintf(logfile, "Image is %lld bytes\n", image_length);
    fprintf(logfile, "Image MD5 is ");
    for (i = 0; i < 16; i++)
        fprintf(logfile, "%2.2x", buf[i+7]);
    fprintf(logfile, "\n");

    /* Now seek back to the start of the desc block and start
       assembling the image */
    fseek(file, desc_start, SEEK_SET);
    fread(buf, 10, 1, file);
    if (strncmp(buf, "DESC", 4))
    {
        fprintf(logfile, "Failed to find desc start in the template file\n");
        fclose(file);
        return EINVAL;
    }
    if ((file_size - desc_start) != read_le48(&buf[4]))
    {
        fprintf(logfile, "Inconsistent desc length in the template file!\n");
        fprintf(logfile, "Final chunk says %lld, first chunk says %lld\n",
                file_size - desc_start, read_le48(&buf[4]));
        fclose(file);
        return EINVAL;
    }

    if (!quick)
        mk_MD5Init(&template_context);
    offset = desc_start + 10;
    while (1)
    {
        if (offset >= (file_size - 33))
            break; /* Finished! */

        fseek(file, offset, SEEK_SET);
        bytes = fread(buf, (MIN (BUF_SIZE, file_size - offset)), 1, file);
        if (1 != bytes)
        {
            fprintf(logfile, "Failed to read template file!\n");
            fclose(file);
            return EINVAL;
        }
        
        switch (buf[0])
        {
            case 2: /* unmatched data */
                error = parse_data_block(read_le48(&buf[1]), file, &template_context);
                if (error)
                {
                    fprintf(logfile, "Unable to read data block, error %d\n", error);
                    fclose(file);
                    return error;
                }
                offset += 7;
                written_length += read_le48(&buf[1]);
                break;
            case 6:
                error = parse_file_block(read_le48(&buf[1]), &buf[15], &template_context);
                if (error)
                {
                    fprintf(logfile, "Unable to read file block, error %d\n", error);
                    fclose(file);
                    return error;
                }
                offset += 31;
                written_length += read_le48(&buf[1]);
                break;
            default:
                fprintf(logfile, "Unknown block type %d!\n", buf[0]);
                fclose(file);
                return EINVAL;
        }
    }
    
    fclose(file);
    if (!quick)
    {
        mk_MD5Final (image_md5sum, &template_context);
        fprintf(logfile, "Output image MD5 is ");
        for (i = 0; i < 16; i++)
            fprintf(logfile, "%2.2x", image_md5sum[i]);
        fprintf(logfile, "\n");
    }
    fprintf(logfile, "Output image length is %lld\n", written_length);

    return 0;
}

static void usage(char *progname)
{
    printf("%s [OPTIONS]\n\n", progname);
    printf(" Options:\n");
    printf(" -j <jigdo name>     Specify the input jigdo file\n");
    printf(" -t <template name>  Specify the input template file\n");
    printf(" -m <item=path>      Map <item> to <path> to find the files in the mirror\n");
    printf(" -l <logfile>        Specify a logfile to append to.\n");
    printf("                     If not specified, will log to stderr\n");
    printf(" -q                  Quick mode. Don't check MD5sums. Dangerous!\n");
}

int main(int argc, char **argv)
{
    char *template_filename = NULL;
    char *jigdo_filename = NULL;
    int c = -1;
    int error = 0;

    logfile = stderr;

    bzero(&zip_state, sizeof(zip_state));

    while(1)
    {
        c = getopt(argc, argv, ":ql:j:t:m:h?");
        if (-1 == c)
            break;
        
        switch(c)
        {
            case 'q':
                quick = 1;
                break;
            case 'l':
                logfile = fopen(optarg, "wb");
                if (!logfile)
                {
                    fprintf(logfile, "Unable to open log file %s\n", optarg);
                    return errno;
                }
                break;
            case 'j':
                if (jigdo_filename)
                {
                    fprintf(logfile, "Can only specify one jigdo file!\n");
                    return EINVAL;
                }
                /* else */
                jigdo_filename = optarg;
                break;
            case 't':
                if (template_filename)
                {
                    fprintf(logfile, "Can only specify one template file!\n");
                    return EINVAL;
                }
                /* else */
                template_filename = optarg;
                break;
            case 'm':
                error = add_match_entry(strdup(optarg));
                if (error)
                    return error;
                break;
            case ':':
                fprintf(logfile, "Missing argument!\n");
                return EINVAL;
                break;
            case 'h':
            case '?':
                usage(argv[0]);
                return 0;
                break;
            default:
                fprintf(logfile, "Unknown option!\n");
                return EINVAL;
        }
    }

    if (NULL == jigdo_filename)
    {
        fprintf(logfile, "No jigdo file specified!\n");
        usage(argv[0]);
        return EINVAL;
    }
    
    if (NULL == template_filename)
    {
        fprintf(logfile, "No template file specified!\n");
        usage(argv[0]);
        return EINVAL;
    }    

    /* Build up a list of file mappings */
    error = parse_jigdo_file(jigdo_filename);
    if (error)
    {
        fprintf(logfile, "Unable to parse the jigdo file %s\n", jigdo_filename);
        return error;
    }

    /* Read the template file and actually build the image to stdout */
    error = parse_template_file(template_filename);
    if (error)
    {
        fprintf(logfile, "Unable to recreate image from template file %s\n", template_filename);
        return error;
    }        

    return 0;
}
