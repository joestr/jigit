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
#include "md5.h"

typedef unsigned long long UINT64;
typedef long long INT64;
typedef unsigned long      UINT32;

#define BUF_SIZE 65536

typedef enum state_
{
    STARTING,
    IN_DATA,
    IN_DESC,
    DUMP_DESC,
    DONE,
    ERROR
} e_state;

INT64 find_string(unsigned char *buf, size_t buf_size, char *search)
{
    size_t length = strlen(search);
    INT64 result;
    
    for (result = 0; result < (buf_size - length); result++)
    {
        if (!memcmp(&buf[result], search, length))
            return result;
    }
    return -1;
}

INT64 parse_data_block(INT64 offset, unsigned char *buf, size_t buf_size)
{
    /* Parse the contents of this data block... */
    UINT64 dataLen = 0;
    UINT64 dataUnc = 0;
    
    printf("\nDATA block found at offset %lld\n", offset);
    dataLen = (UINT64)buf[4];
    dataLen |= (UINT64)buf[5] << 8;
    dataLen |= (UINT64)buf[6] << 16;
    dataLen |= (UINT64)buf[7] << 24;
    dataLen |= (UINT64)buf[8] << 32;
    dataLen |= (UINT64)buf[9] << 40;
    printf("  compressed block size %llu bytes\n", dataLen);

    dataUnc = (UINT64)buf[10];
    dataUnc |= (UINT64)buf[11] << 8;
    dataUnc |= (UINT64)buf[12] << 16;
    dataUnc |= (UINT64)buf[13] << 24;
    dataUnc |= (UINT64)buf[14] << 32;
    dataUnc |= (UINT64)buf[15] << 40;
    printf("  uncompressed block size %llu bytes\n", dataUnc);

    return dataLen;
}

INT64 parse_desc_block(INT64 offset, unsigned char *buf, size_t buf_size)
{
    /* Parse the contents of this data block... */
    UINT64 descLen = 0;
    
    printf("\nDESC block found at offset %lld\n", offset);
    descLen = (UINT64)buf[4];
    descLen |= (UINT64)buf[5] << 8;
    descLen |= (UINT64)buf[6] << 16;
    descLen |= (UINT64)buf[7] << 24;
    descLen |= (UINT64)buf[8] << 32;
    descLen |= (UINT64)buf[9] << 40;
    printf("  DESC block size is %llu bytes\n", descLen);
    
    return 10;
}

INT64 parse_desc_data(INT64 offset, unsigned char *buf, size_t buf_size)
{
    int type = buf[0];
    printf("  DESC entry: block type %d\n", type);
    
    switch (type)
    {
        case 2:
        {
            UINT64 skipLen = 0;
            skipLen = (UINT64)buf[1];
            skipLen |= (UINT64)buf[2] << 8;
            skipLen |= (UINT64)buf[3] << 16;
            skipLen |= (UINT64)buf[4] << 24;
            skipLen |= (UINT64)buf[5] << 32;
            skipLen |= (UINT64)buf[6] << 40;
            printf("    Unmatched data, %llu bytes\n", skipLen);
            return 7;
        }
        case 5:
        {
            UINT64 imglen = 0;
            UINT32 blocklen = 0;
            int i = 0;

            imglen = (UINT64)buf[1];
            imglen |= (UINT64)buf[2] << 8;
            imglen |= (UINT64)buf[3] << 16;
            imglen |= (UINT64)buf[4] << 24;
            imglen |= (UINT64)buf[5] << 32;
            imglen |= (UINT64)buf[6] << 40;

            blocklen = (UINT32)buf[23];
            blocklen |= (UINT32)buf[24] << 8;
            blocklen |= (UINT32)buf[25] << 16;
            blocklen |= (UINT32)buf[26] << 24;

            printf("    Original image length %llu bytes\n", imglen);
            printf("    Image MD5: ");
            for (i = 7; i < 23; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[7], 16));
            printf("    Rsync block length %lu bytes\n", blocklen);
            return 0; /* i.e. we're finished! */
        }
        case 6:
        {
            UINT64 fileLen = 0;
            int i = 0;

            fileLen = (UINT64)buf[1];
            fileLen |= (UINT64)buf[2] << 8;
            fileLen |= (UINT64)buf[3] << 16;
            fileLen |= (UINT64)buf[4] << 24;
            fileLen |= (UINT64)buf[5] << 32;
            fileLen |= (UINT64)buf[6] << 40;
            
            printf("    File, length %llu bytes\n", fileLen);
            printf("    file rsyncsum: ");
            for (i = 7; i < 15; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[7], 8));
            printf("    file md5: ");
            for (i = 15; i < 31; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[15], 16));
            return 31;
        }
        default:
            break;
    }

    return 0;
}

int main(int argc, char **argv)
{
    char *filename = NULL;
    int fd = -1;
    unsigned char *buf = NULL;
    INT64 offset = 0;
    INT64 bytes = 0;
    e_state state = STARTING;
    
    if (argc != 2)
    {
        printf("No filename specified! Try again...\n");
        return EINVAL;
    }
    
    filename = argv[1];
    
    fd = open(filename, O_RDONLY);
    if (-1 == fd)
    {
        printf("Failed to open file %s, error %d!. Try again...\n", filename, errno);
        return errno;
    }

    buf = malloc(BUF_SIZE);
    if (!buf)
    {
        printf("Failed to malloc %d bytes. Abort!\n", BUF_SIZE);
        return ENOMEM;
    }

    /* Find the beginning of the data - read the first chunk, including the header */
    while (STARTING == state)
    {
        INT64 start_offset = -1;

        bytes = read(fd, buf, BUF_SIZE);
        if (0 >= bytes)
        {
            state = DONE;
            break;
        }
        start_offset = find_string(buf, bytes, "DATA");
        if (start_offset >= 0)
        {
            offset += start_offset;
            state = IN_DATA;
            break;
        }
        offset += bytes;
    }

    while (DONE != state && ERROR != state)
    {
        INT64 start_offset = -1;
        lseek(fd, offset, SEEK_SET);
        bytes = read(fd, buf, BUF_SIZE);
        if (0 >= bytes)
        {
            state = ERROR;
            break;
        }
        if (IN_DATA == state)
        {
            if (!find_string(buf, bytes, "DATA"))
                state = IN_DATA;
            if (!find_string(buf, bytes, "DESC"))
                state = IN_DESC;
        }
        switch (state)
        {
            case IN_DATA:
                start_offset = parse_data_block(offset, buf, bytes);
                offset += start_offset;
                break;
            case IN_DESC:
                start_offset = parse_desc_block(offset, buf, bytes);
                offset += start_offset;
                state = DUMP_DESC;
                break;
            case DUMP_DESC:
                start_offset = parse_desc_data(offset, buf, bytes);
                offset += start_offset;
                if (0 == start_offset)
                    state = DONE;
                break;
            default:
                break;
        }
    }        
    
    close(fd);

    return 0;
}
