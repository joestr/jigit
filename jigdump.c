/*
 * jigdump
 *
 * Tool to dump the contents of a jigdo template file
 *
 * Copyright (c) 2004 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

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
#include "jig-base64.h"
#include "md5.h"
#include "jigdo.h"

#define HEADER_STRING "JigsawDownload template"

static INT64 find_string(unsigned char *buf, size_t buf_size, char *search)
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

static INT64 parse_data_block(INT64 offset, unsigned char *buf, size_t buf_size)
{
    /* Parse the contents of this data block... */
    UINT64 dataLen = 0;
    UINT64 dataUnc = 0;

    if (!strncmp((char *)buf, "DATA", 4))
        printf("\ngzip data block found at offset %lld\n", offset);
    else
        printf("\nbzip2 data block found at offset %lld\n", offset);

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

static INT64 parse_desc_block(INT64 offset, unsigned char *buf, size_t buf_size)
{
    /* Parse the contents of this data block... */
    UINT64 descLen = 0;
    
    printf("\nDESC block found at offset %lld (%llx)\n", offset, offset);
    descLen = (UINT64)buf[4];
    descLen |= (UINT64)buf[5] << 8;
    descLen |= (UINT64)buf[6] << 16;
    descLen |= (UINT64)buf[7] << 24;
    descLen |= (UINT64)buf[8] << 32;
    descLen |= (UINT64)buf[9] << 40;
    printf("  DESC block size is %llu bytes\n", descLen);
    
    return 10;
}

static INT64 parse_desc_data(INT64 offset, unsigned char *buf, size_t buf_size)
{
    int type = buf[0];
    printf("  DESC entry: block type %d at offset %lld (0x%llx)\n",
	   type, offset, offset);
    
    switch (type)
    {
        case BLOCK_DATA:
        {
            UINT64 skipLen = 0;
            skipLen = (UINT64)buf[1];
            skipLen |= (UINT64)buf[2] << 8;
            skipLen |= (UINT64)buf[3] << 16;
            skipLen |= (UINT64)buf[4] << 24;
            skipLen |= (UINT64)buf[5] << 32;
            skipLen |= (UINT64)buf[6] << 40;
            printf("    7 byte entry:\n");
            printf("      Template data, %llu bytes of data\n", skipLen);
            return 7;
        }
        case BLOCK_IMAGE_MD5:
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

	    printf("    27 byte entry:\n");
	    printf("      Image info block v1 using MD5\n");
            printf("      Original image length %llu bytes\n", imglen);
            printf("      Image MD5: ");
            for (i = 7; i < 23; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[7], 16));
            printf("      Rsync block length %lu bytes\n", blocklen);
            return 27;
        }
        case BLOCK_IMAGE_SHA256:
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

            blocklen = (UINT32)buf[39];
            blocklen |= (UINT32)buf[40] << 8;
            blocklen |= (UINT32)buf[41] << 16;
            blocklen |= (UINT32)buf[42] << 24;

	    printf("    43 byte entry:\n");
	    printf("      Image info block v2 using SHA256\n");
            printf("      Original image length %llu bytes\n", imglen);
            printf("      Image SHA256: ");
            for (i = 7; i < 39; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[7], 32));
            printf("      Rsync block length %lu bytes\n", blocklen);
            return 43;
        }
        case BLOCK_MATCH_MD5:
        case BLOCK_WRITTEN_MD5:
        {
            UINT64 fileLen = 0;
            int i = 0;

            fileLen = (UINT64)buf[1];
            fileLen |= (UINT64)buf[2] << 8;
            fileLen |= (UINT64)buf[3] << 16;
            fileLen |= (UINT64)buf[4] << 24;
            fileLen |= (UINT64)buf[5] << 32;
            fileLen |= (UINT64)buf[6] << 40;
            
	    printf("    31 byte entry:\n");
            printf("      File %s block v1 using MD5\n",
                   (type == BLOCK_MATCH_MD5 ? "match" : "written"));
            printf("      length %llu bytes\n", fileLen);
            printf("      file rsyncsum: ");
            for (i = 7; i < 15; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[7], 8));
            printf("      file md5: ");
            for (i = 15; i < 31; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[15], 16));
            return 31;
        }
        case BLOCK_MATCH_SHA256:
        case BLOCK_WRITTEN_SHA256:
        {
            UINT64 fileLen = 0;
            int i = 0;

            fileLen = (UINT64)buf[1];
            fileLen |= (UINT64)buf[2] << 8;
            fileLen |= (UINT64)buf[3] << 16;
            fileLen |= (UINT64)buf[4] << 24;
            fileLen |= (UINT64)buf[5] << 32;
            fileLen |= (UINT64)buf[6] << 40;

	    printf("    47 byte entry:\n");
            printf("      File %s block v2 using SHA256\n",
                   (type == BLOCK_MATCH_SHA256 ? "match" : "written"));
            printf("      length %llu bytes\n", fileLen);
            printf("      file rsyncsum: ");
            for (i = 7; i < 15; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[7], 8));
            printf("      file sha256: ");
            for (i = 15; i < 47; i++)
                printf("%2.2x", buf[i]);
            printf(" (%s)\n", base64_dump(&buf[15], 32));
            return 47;
        }
        default:
	    printf("    Unrecognised block type %d, skipping forwards in hope\n", type);
	    return 1;
            break;
    }

    return 0;
}

static INT64 parse_desc_pointer(INT64 offset, unsigned char *buf, size_t buf_size, INT64 template_size, INT64 desc_offset)
{
    /* Parse the contents of this final block... */
    UINT64 ptr = 0;

    ptr = (UINT64)buf[0];
    ptr |= (UINT64)buf[1] << 8;
    ptr |= (UINT64)buf[2] << 16;
    ptr |= (UINT64)buf[3] << 24;
    ptr |= (UINT64)buf[4] << 32;
    ptr |= (UINT64)buf[5] << 48;
    printf("  DESC pointer offset %llu\n", ptr);
    printf("  points to DESC at offset %llu\n", template_size - ptr);
    printf("  actual DESC section starts at %llu\n", desc_offset);
    if (template_size - ptr != desc_offset)
	printf("ERROR: pointers don't match up\n");

    return 6;
}

static INT64 parse_tmp_desc_pointer(unsigned char *buf)
{
    /* Parse the contents of this final block... */
    UINT64 ptr = 0;

    ptr = (UINT64)buf[0];
    ptr |= (UINT64)buf[1] << 8;
    ptr |= (UINT64)buf[2] << 16;
    ptr |= (UINT64)buf[3] << 24;
    ptr |= (UINT64)buf[4] << 32;
    ptr |= (UINT64)buf[5] << 48;

    return ptr;
}

static INT64 parse_header_block(INT64 offset, unsigned char *buf, size_t buf_size)
{
    /* Massive overkill for sizes, but safe! */
    char format_version[BUF_SIZE] = {0};
    char generator[BUF_SIZE] = {0};
    int ret = 0;

    ret = sscanf((char *)buf, HEADER_STRING" %s %s", format_version, generator);
    if (ret != 2)
    {
        printf("Failed to parse header, ret %d\n", ret);
	return -1;
    }
    printf("Found header: \"%s\"\n", HEADER_STRING);
    printf("Format version: %s\n", format_version);
    printf("Generator: %s\n", generator);

    return 0;
}

/* return 1 if haystack ends with the text contained in needle, 0
 * otherwise */
static int string_ends(char *haystack, char *needle)
{
    int hlen;
    int nlen;

    if (NULL == haystack || NULL == needle)
        return 0;

    hlen = strlen(haystack);
    nlen = strlen(needle);
    if (nlen > hlen)
        return 0;
    if (!strcmp((haystack + hlen - nlen), needle))
        return 1;

    return 0;
}

int main(int argc, char **argv)
{
    char *filename = NULL;
    int fd = -1;
    unsigned char *buf = NULL;
    INT64 offset = 0;
    INT64 bytes = 0;
    INT64 template_size = 0;
    INT64 desc_offset = 0;
    struct stat sb;
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

    if (-1 == fstat(fd, &sb))
    {
        printf("Failed to stat file %s, error %d!. Try again...\n", filename, errno);
        return errno;
    }
    template_size = sb.st_size;

    buf = malloc(BUF_SIZE);
    if (!buf)
    {
        printf("Failed to malloc %d bytes. Abort!\n", BUF_SIZE);
        return ENOMEM;
    }

    /* If we're looking at a template file, find the beginning of the
     * data - read the first chunk, including the header. Only do this
     * if the filename ends in ".template" */
    if (string_ends(filename, ".template"))
    {
        printf("Filename %s ends in .template.\n", filename);
        printf("Assuming this is meant to be a template file.\n");
	while (STARTING == state)
	{
	    INT64 start_offset = -1;

	    bytes = read(fd, buf, BUF_SIZE);
	    if (0 >= bytes)
	    {
		state = DONE;
		break;
	    }

	    start_offset = find_string(buf, bytes, HEADER_STRING);
	    if (0 != start_offset)
	    {
		printf("Can't find header - this is not a template file\n");
		return EINVAL;
	    }
	    if (0 != parse_header_block(start_offset, buf, bytes))
		return EINVAL;

	    start_offset = find_string(buf, bytes, "DATA");
	    if (-1 == start_offset)
		start_offset = find_string(buf, bytes, "BZIP");
	    if (start_offset >= 0)
	    {
		offset += start_offset;
		state = IN_DATA;
		break;
	    }
	    offset += bytes;
	}
    }
    else
    {
        printf("Filename %s does not end in .template.\n", filename);
        printf("Assuming this is NOT meant to be a template file.\n");
        /* We're looking at a tmp file? No header available. Seek to
        the end for the final DESC * pointer */
        lseek(fd, template_size - 6, SEEK_SET);
        bytes = read(fd, buf, BUF_SIZE);
        if (bytes != 6)
        {
            state = DONE;
        }
        else
        {
            INT64 start_offset = parse_tmp_desc_pointer(buf);
            offset = template_size - start_offset;
            state = IN_DESC;
        }
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
            if (!find_string(buf, bytes, "DATA") || !find_string(buf, bytes, "BZIP"))
                state = IN_DATA;
            if (!find_string(buf, bytes, "DESC"))
	    {
                state = IN_DESC;
		desc_offset = offset;
	    }
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
		if (offset >= (template_size - 6))
			state = DUMP_DESC_PTR;;
                break;
	    case DUMP_DESC_PTR:
		state = DONE;
		start_offset = parse_desc_pointer(offset, buf, bytes, template_size, desc_offset);
		offset += start_offset;
		break;
            default:
                break;
        }
	if (offset == template_size)
	    state = DONE;
    }        
    
    close(fd);

    return 0;
}
