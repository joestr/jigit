#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sendfile.h>

/* Possible commands to run. Or them together... */
#define CMD_LIST        0x0001
#define CMD_BUILD_IMAGE 0x0002

typedef unsigned long long UINT64;
typedef unsigned long      UINT32;

#define BUF_SIZE 65536

/* Different types used in the extent_type below */
#define JTET_HEADER     0
#define JTET_FOOTER     1
#define JTET_FILE_MATCH 2
#define JTET_NOMATCH    3

#define JTE_ID_STRING     "JTE"
#define JTE_HEADER_STRING "MKJ IMAGE START"
#define JTE_FOOTER_STRING "*MKJ IMAGE END*"
#define JTE_VER_MAJOR     0x0001
#define JTE_VER_MINOR     0x0000

struct jt_extent_data
{
    unsigned char id[4];                       /* "JTE" plus NULL terminator */
    unsigned char extent_type;                 /* The type of this extent in the jigdo template file */
    unsigned char extent_length[8];            /* The length in bytes of this extent, including all
                                                  the metadata. 64-bit, big endian */
    unsigned char start_sector[4];             /* The start sector of this extent within the output image;
                                                  32-bit BE. Header and footer use 0xFFFFFFFF */
    union
    {
        struct
        {
            unsigned char header_string[16];   /* Recognition string. Should contain "MKJ IMAGE START",
                                                  including NULL terminator */
            unsigned char version[4];          /* Version number, encoded MMmm */
            unsigned char sector_size[4];      /* Sector size used in this image.
                                                  _Always_ expected to be 2KB. Stored as 32-bit BE */
            unsigned char pad[16];
        } header;
        struct
        {
            unsigned char footer_string[16];   /* Recognition string. Should contain "*MKJ IMAGE END*",
                                                  including NULL terminator */
            unsigned char image_size[8];       /* Size of image, in bytes. 64-bit BE. */
            unsigned char md5[16];             /* MD5SUM of the entire image */
        } footer;
        struct
        {
            unsigned char file_length[8];      /* The actual length of the file stored in this extent.
                                                  Will be <= extent_length; also 64-bit BE */
            unsigned char filename_length[4];  /* The length of the following filename entry */
            unsigned char md5[16];             /* MD5SUM of the _file_ data in this lump, without padding */
            unsigned char pad[12];
        } file_match;
        struct
        {
            unsigned char unmatched_length[8]; /* The length of the data in this extent. Will be == 
                                                  extent_length - sizeof(struct jt_extent_data) ; also 64-bit BE */
            unsigned char md5[16];             /* MD5SUM of this lump of unmatched data */
            unsigned char pad[16];
        } nomatch;
    } data;
};

int verbose = 0;
int cmd = 0;
UINT32 sector_size = 0;
int jte_fd = -1;
int out_fd = -1;

static char *print_md5(unsigned char *buf)
{
    static char outbuf[33];

    bzero(outbuf, sizeof(outbuf));
    sprintf(outbuf, "%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x",
            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
            buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
    return outbuf;
}

static int my_sendfile(int src_fd, int dst_fd, off_t length)
{
    char buf[BUF_SIZE];
    off_t bytes = 0;
    off_t bytes_read = 0;
    off_t bytes_written = 0;
    off_t size = 0;

    while (bytes < length)
    {
        size = length - bytes;
        if (size > BUF_SIZE)
            size = BUF_SIZE;
        
        bytes_read = read(src_fd, buf, size);
        if (size != bytes_read)
        {
            printf("my_sendfile: FAILED to read %llu bytes from input file; error %d\n", size, errno);
            return errno;
        }
        bytes_written = write(dst_fd, buf, bytes_read);
        if (bytes_read != bytes_written)
        {
            printf("my_sendfile: FAILED to write %llu bytes to output file; error %d\n", bytes_read, errno);
            return errno;
        }
        bytes += bytes_read;
    }
    
    /* Now pad if necessary */
    if (length % sector_size)
    {
        bzero (buf, BUF_SIZE);
        off_t pad_size = sector_size - (length % sector_size);
        
        bytes = write(dst_fd, buf, pad_size);
        if (-1 == bytes)
        {
            printf("my_sendfile: FAILED to pad with %llu bytes; error %d\n", pad_size, errno);
            return errno;
        }
    }
    
    return 0;
}

UINT64 handle_jtet_header(UINT64 extent_length,
                          UINT32 start_sector, unsigned char *buf)
{
    struct jt_extent_data *extent = (struct jt_extent_data *)buf;
    
    sector_size = extent->data.header.sector_size[0] << 24;
    sector_size |= extent->data.header.sector_size[1] << 16;
    sector_size |= extent->data.header.sector_size[2] << 8;
    sector_size |= extent->data.header.sector_size[3];
    
    if (cmd & CMD_LIST)
    {
        printf("  Header string: %s\n", extent->data.header.header_string);
        printf("  JTE version: %d.%d\n",
               (extent->data.header.version[0] << 8) | extent->data.header.version[1],
               (extent->data.header.version[2] << 8) | extent->data.header.version[3]);
        printf("  Sector size: %ld bytes\n", sector_size);
    }
    return extent_length;
}

UINT64 handle_jtet_footer(UINT64 extent_length,
                          UINT32 start_sector, unsigned char *buf)
{
    struct jt_extent_data *extent = (struct jt_extent_data *)buf;
    UINT64 image_size = (UINT64)extent->data.footer.image_size[0] << 56;
    image_size |= (UINT64)extent->data.footer.image_size[1] << 48;
    image_size |= (UINT64)extent->data.footer.image_size[2] << 40;
    image_size |= (UINT64)extent->data.footer.image_size[3] << 32;
    image_size |= (UINT64)extent->data.footer.image_size[4] << 24;
    image_size |= (UINT64)extent->data.footer.image_size[5] << 16;
    image_size |= (UINT64)extent->data.footer.image_size[6] << 8;
    image_size |= (UINT64)extent->data.footer.image_size[7];

    if (cmd & CMD_LIST)
    {
        printf("  Footer string: %s\n", extent->data.footer.footer_string);
        printf("  ISO image size: %llu\n", image_size);
        printf("  ISO image MD5sum: %s\n", print_md5(extent->data.footer.md5));
    }
    return 0;
}

UINT64 handle_jtet_file_match(UINT64 extent_length,
                              UINT32 start_sector, unsigned char *buf)
{
    int error = 0;
    int chunk_fd = -1;
    struct jt_extent_data *extent = (struct jt_extent_data *)buf;
    char *filename = &buf[57];
    UINT64 length = 0;
    int filename_length = 0;

    length |= (UINT64)extent->data.file_match.file_length[0] << 56;
    length |= (UINT64)extent->data.file_match.file_length[1] << 48;
    length |= (UINT64)extent->data.file_match.file_length[2] << 40;
    length |= (UINT64)extent->data.file_match.file_length[3] << 32;
    length |= (UINT64)extent->data.file_match.file_length[4] << 24;
    length |= (UINT64)extent->data.file_match.file_length[5] << 16;
    length |= (UINT64)extent->data.file_match.file_length[6] << 8;
    length |= (UINT64)extent->data.file_match.file_length[7];

    filename_length |= extent->data.file_match.filename_length[0] << 24;
    filename_length |= extent->data.file_match.filename_length[1] << 16;
    filename_length |= extent->data.file_match.filename_length[2] << 8;
    filename_length |= extent->data.file_match.filename_length[3];

    if (cmd & CMD_LIST)
    {
        printf("  File length: %llu\n", length);
        printf("  Filename len: %d\n", filename_length);
        printf("  Filename: %s\n", filename);
        printf("  File MD5sum: %s\n", print_md5(extent->data.file_match.md5));
    }
    
    if (cmd & CMD_BUILD_IMAGE)
    {
        chunk_fd = open(filename, O_RDONLY|O_LARGEFILE);
        if (-1 == chunk_fd)
        {
            printf("FAILED to open filename %s, error %d. Aborting\n", filename, errno);
            return 0;
        }
        else
        {
            if (verbose)
                printf("Writing %7llu bytes of %s to output file\n", length, filename);
            error = my_sendfile(chunk_fd, out_fd, length);
            close(chunk_fd);
            if (error)
            {
                printf("FAILED to copy contents of %s into output file; error %d. Aborting\n", filename, error);
                return 0;
            }
        }
    }
    
    return extent_length;
}

UINT64 handle_jtet_no_match(UINT64 extent_length,
                            UINT32 start_sector, unsigned char *buf)
{
    int error = 0;
    UINT64 length = 0;
    struct jt_extent_data *extent = (struct jt_extent_data *)buf;

    length |= (UINT64)extent->data.nomatch.unmatched_length[0] << 56;
    length |= (UINT64)extent->data.nomatch.unmatched_length[1] << 48;
    length |= (UINT64)extent->data.nomatch.unmatched_length[2] << 40;
    length |= (UINT64)extent->data.nomatch.unmatched_length[3] << 32;
    length |= (UINT64)extent->data.nomatch.unmatched_length[4] << 24;
    length |= (UINT64)extent->data.nomatch.unmatched_length[5] << 16;
    length |= (UINT64)extent->data.nomatch.unmatched_length[6] << 8;
    length |= (UINT64)extent->data.nomatch.unmatched_length[7];

    if (cmd & CMD_LIST)
    {
        printf("  Unmatched data, length %llu\n", length);
        printf("  Chunk MD5sum: %s\n", print_md5(extent->data.nomatch.md5));
    }
    
    if (cmd & CMD_BUILD_IMAGE)
    {
        /* Seek past the header of this block */
        lseek(jte_fd, sizeof(struct jt_extent_data), SEEK_CUR);
        if (verbose)
            printf("Writing %7llu bytes of unmatched image data to output file\n", length);
        error = my_sendfile(jte_fd, out_fd, length);
        if (error)
        {
            printf("FAILED to copy unmatched data chunk into output file; error %d. Aborting\n", error);
            return 0;
        }
    }
    
    return extent_length;
}

UINT64 parse_jte_block(UINT64 offset, unsigned char *buf, size_t buf_size)
{
    int    extent_type = 0;
    UINT64 extent_length = 0;
    UINT32 start_sector = 0;
    struct jt_extent_data *extent = (struct jt_extent_data *)buf;
    
    if (strncmp(extent->id, JTE_ID_STRING, 4))
    {
        printf("Error! Didn't find expected JTE block at offset %lld\n", offset);
        return 0;
    }
    if (cmd & CMD_LIST)
        printf("\nJTE block found at offset %lld\n", offset);

    extent_type = extent->extent_type;

    extent_length |= (UINT64)extent->extent_length[0] << 56;
    extent_length |= (UINT64)extent->extent_length[1] << 48;
    extent_length |= (UINT64)extent->extent_length[2] << 40;
    extent_length |= (UINT64)extent->extent_length[3] << 32;
    extent_length |= (UINT64)extent->extent_length[4] << 24;
    extent_length |= (UINT64)extent->extent_length[5] << 16;
    extent_length |= (UINT64)extent->extent_length[6] << 8;
    extent_length |= (UINT64)extent->extent_length[7];

    start_sector |= extent->start_sector[0] << 24;
    start_sector |= extent->start_sector[1] << 16;
    start_sector |= extent->start_sector[2] << 8;
    start_sector |= extent->start_sector[3];
    
    if (cmd & CMD_LIST)
        printf("  extent type %d, length %llu, start_sector %ld (out offset %llu)\n",
               extent_type, extent_length, start_sector, (off_t)start_sector * sector_size);

    switch(extent_type)
    {
        case JTET_HEADER:
            return handle_jtet_header(extent_length, start_sector, buf);
        case JTET_FOOTER:
            return handle_jtet_footer(extent_length, start_sector, buf);
        case JTET_FILE_MATCH:
            return handle_jtet_file_match(extent_length, start_sector, buf);
        case JTET_NOMATCH:
            return handle_jtet_no_match(extent_length, start_sector, buf);
        default:
            printf("Awooga! Invalid extent type!\n");
            return 0;
    }
}

int main(int argc, char **argv)
{
    char *filename = NULL;
    char *outfile = NULL;
    unsigned char *buf = NULL;
    UINT64 offset = 0;
    UINT64 bytes = 0;
    int done = 0;
    int i = 0;
    
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-l"))
            cmd |= CMD_LIST;
        else if (!strcmp(argv[i], "-b"))
            cmd |= CMD_BUILD_IMAGE;
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-o") && i < argc - 1)
            outfile = argv[++i];
        else if (!strcmp(argv[i], "-f") && i < argc - 1)
            filename = argv[++i];
    }

    if (0 == cmd)
        cmd = CMD_LIST;

    if (NULL == filename)
    {
        printf("No filename specified! Try again...\n");
        return EINVAL;
    }

    if ( (cmd & CMD_BUILD_IMAGE) && !outfile)
    {
        printf("No output filename given; aborting...\n");
        return EINVAL;
    }
    
    jte_fd = open(filename, O_RDONLY|O_LARGEFILE);
    if (-1 == jte_fd)
    {
        printf("Failed to open input file %s, error %d!. Try again...\n", filename, errno);
        return errno;
    }

    if (outfile)
    {
        out_fd = open(outfile, O_RDWR|O_CREAT|O_TRUNC|O_LARGEFILE, 0644);
        if (-1 == out_fd)
        {
            printf("Failed to open output file %s, error %d!\n", outfile, errno);
            return errno;
        }
    }

    buf = malloc(BUF_SIZE);
    if (!buf)
    {
        printf("Failed to malloc %d bytes. Abort!\n", BUF_SIZE);
        return ENOMEM;
    }

    while (!done)
    {
        UINT64 start_offset = -1;
        lseek(jte_fd, offset, SEEK_SET);
        bytes = read(jte_fd, buf, BUF_SIZE);
        if (0 >= bytes)
        {
            printf("Failed to read! error %d\n", errno);
            done = 1;
            break;
        }
        lseek(jte_fd, offset, SEEK_SET);
        start_offset = parse_jte_block(offset, buf, bytes);
        if (!start_offset)
            break; /* We're finished! */
        offset += start_offset;
    }        
    
    close(jte_fd);
    if (-1 != out_fd)
        close(out_fd);

    return 0;
}
