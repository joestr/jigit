/*
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.  This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 */

/* This code was modified in 1997 by Jim Kingdon of Cyclic Software to
   not require an integer type which is exactly 32 bits.  This work
   draws on the changes for the same purpose by Tatu Ylonen
   <ylo@cs.hut.fi> as part of SSH, but since I didn't actually use
   that code, there is no copyright issue.  I hereby disclaim
   copyright in any changes I have made; this code remains in the
   public domain.  */

/* Note regarding cvs_* namespace: this avoids potential conflicts
   with libraries such as some versions of Kerberos.  No particular
   need to worry about whether the system supplies an MD5 library, as
   this file is only about 3k of object code.  */

/* Steve McIntyre, 2004/05/31: borrowed this code from the CVS
   library. s/cvs_/mk_/ across the source */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>	/* for memcpy() and memset() */
#include <stdlib.h> /* for calloc() */
#include <stdio.h>
#include <errno.h>

#define BUF_SIZE 65536

#include "md5.h"

/* Little-endian byte-swapping routines.  Note that these do not
   depend on the size of datatypes such as mk_uint32, nor do they require
   us to detect the endianness of the machine we are running on.  It
   is possible they should be macros for speed, but I would be
   surprised if they were a performance bottleneck for MD5.  */

static mk_uint32
getu32 (addr)
     const unsigned char *addr;
{
	return (((((unsigned long)addr[3] << 8) | addr[2]) << 8)
		| addr[1]) << 8 | addr[0];
}

static void
putu32 (data, addr)
     mk_uint32 data;
     unsigned char *addr;
{
	addr[0] = (unsigned char)data;
	addr[1] = (unsigned char)(data >> 8);
	addr[2] = (unsigned char)(data >> 16);
	addr[3] = (unsigned char)(data >> 24);
}

/*
 * Start MD5 accumulation.  Set bit count to 0 and buffer to mysterious
 * initialization constants.
 */
void
mk_MD5Init (ctx)
     struct mk_MD5Context *ctx;
{
	ctx->buf[0] = 0x67452301;
	ctx->buf[1] = 0xefcdab89;
	ctx->buf[2] = 0x98badcfe;
	ctx->buf[3] = 0x10325476;

	ctx->bits[0] = 0;
	ctx->bits[1] = 0;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
void
mk_MD5Update (ctx, buf, len)
     struct mk_MD5Context *ctx;
     unsigned char const *buf;
     unsigned len;
{
	mk_uint32 t;

	/* Update bitcount */

	t = ctx->bits[0];
	if ((ctx->bits[0] = (t + ((mk_uint32)len << 3)) & 0xffffffff) < t)
		ctx->bits[1]++;	/* Carry from low to high */
	ctx->bits[1] += len >> 29;

	t = (t >> 3) & 0x3f;	/* Bytes already in shsInfo->data */

	/* Handle any leading odd-sized chunks */

	if ( t ) {
		unsigned char *p = ctx->in + t;

		t = 64-t;
		if (len < t) {
			memcpy(p, buf, len);
			return;
		}
		memcpy(p, buf, t);
		mk_MD5Transform (ctx->buf, ctx->in);
		buf += t;
		len -= t;
	}

	/* Process data in 64-byte chunks */

	while (len >= 64) {
		memcpy(ctx->in, buf, 64);
		mk_MD5Transform (ctx->buf, ctx->in);
		buf += 64;
		len -= 64;
	}

	/* Handle any remaining bytes of data. */

	memcpy(ctx->in, buf, len);
}

/*
 * Final wrapup - pad to 64-byte boundary with the bit pattern 
 * 1 0* (64-bit count of bits processed, MSB-first)
 */
void
mk_MD5Final (digest, ctx)
     unsigned char digest[16];
     struct mk_MD5Context *ctx;
{
	unsigned count;
	unsigned char *p;

	/* Compute number of bytes mod 64 */
	count = (ctx->bits[0] >> 3) & 0x3F;

	/* Set the first char of padding to 0x80.  This is safe since there is
	   always at least one byte free */
	p = ctx->in + count;
	*p++ = 0x80;

	/* Bytes of padding needed to make 64 bytes */
	count = 64 - 1 - count;

	/* Pad out to 56 mod 64 */
	if (count < 8) {
		/* Two lots of padding:  Pad the first block to 64 bytes */
		memset(p, 0, count);
		mk_MD5Transform (ctx->buf, ctx->in);

		/* Now fill the next block with 56 bytes */
		memset(ctx->in, 0, 56);
	} else {
		/* Pad block to 56 bytes */
		memset(p, 0, count-8);
	}

	/* Append length in bits and transform */
	putu32(ctx->bits[0], ctx->in + 56);
	putu32(ctx->bits[1], ctx->in + 60);

	mk_MD5Transform (ctx->buf, ctx->in);
	putu32(ctx->buf[0], digest);
	putu32(ctx->buf[1], digest + 4);
	putu32(ctx->buf[2], digest + 8);
	putu32(ctx->buf[3], digest + 12);
	memset(ctx, 0, sizeof(ctx));	/* In case it's sensitive */
}

#ifndef ASM_MD5

/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

/* This is the central step in the MD5 algorithm. */
#define MD5STEP(f, w, x, y, z, data, s) \
	( w += f(x, y, z) + data, w &= 0xffffffff, w = w<<s | w>>(32-s), w += x )

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to
 * reflect the addition of 16 longwords of new data.  MD5Update blocks
 * the data and converts bytes into longwords for this routine.
 */
void
mk_MD5Transform (buf, inraw)
     mk_uint32 buf[4];
     const unsigned char inraw[64];
{
	register mk_uint32 a, b, c, d;
	mk_uint32 in[16];
	int i;

	for (i = 0; i < 16; ++i)
		in[i] = getu32 (inraw + 4 * i);

	a = buf[0];
	b = buf[1];
	c = buf[2];
	d = buf[3];

	MD5STEP(F1, a, b, c, d, in[ 0]+0xd76aa478,  7);
	MD5STEP(F1, d, a, b, c, in[ 1]+0xe8c7b756, 12);
	MD5STEP(F1, c, d, a, b, in[ 2]+0x242070db, 17);
	MD5STEP(F1, b, c, d, a, in[ 3]+0xc1bdceee, 22);
	MD5STEP(F1, a, b, c, d, in[ 4]+0xf57c0faf,  7);
	MD5STEP(F1, d, a, b, c, in[ 5]+0x4787c62a, 12);
	MD5STEP(F1, c, d, a, b, in[ 6]+0xa8304613, 17);
	MD5STEP(F1, b, c, d, a, in[ 7]+0xfd469501, 22);
	MD5STEP(F1, a, b, c, d, in[ 8]+0x698098d8,  7);
	MD5STEP(F1, d, a, b, c, in[ 9]+0x8b44f7af, 12);
	MD5STEP(F1, c, d, a, b, in[10]+0xffff5bb1, 17);
	MD5STEP(F1, b, c, d, a, in[11]+0x895cd7be, 22);
	MD5STEP(F1, a, b, c, d, in[12]+0x6b901122,  7);
	MD5STEP(F1, d, a, b, c, in[13]+0xfd987193, 12);
	MD5STEP(F1, c, d, a, b, in[14]+0xa679438e, 17);
	MD5STEP(F1, b, c, d, a, in[15]+0x49b40821, 22);

	MD5STEP(F2, a, b, c, d, in[ 1]+0xf61e2562,  5);
	MD5STEP(F2, d, a, b, c, in[ 6]+0xc040b340,  9);
	MD5STEP(F2, c, d, a, b, in[11]+0x265e5a51, 14);
	MD5STEP(F2, b, c, d, a, in[ 0]+0xe9b6c7aa, 20);
	MD5STEP(F2, a, b, c, d, in[ 5]+0xd62f105d,  5);
	MD5STEP(F2, d, a, b, c, in[10]+0x02441453,  9);
	MD5STEP(F2, c, d, a, b, in[15]+0xd8a1e681, 14);
	MD5STEP(F2, b, c, d, a, in[ 4]+0xe7d3fbc8, 20);
	MD5STEP(F2, a, b, c, d, in[ 9]+0x21e1cde6,  5);
	MD5STEP(F2, d, a, b, c, in[14]+0xc33707d6,  9);
	MD5STEP(F2, c, d, a, b, in[ 3]+0xf4d50d87, 14);
	MD5STEP(F2, b, c, d, a, in[ 8]+0x455a14ed, 20);
	MD5STEP(F2, a, b, c, d, in[13]+0xa9e3e905,  5);
	MD5STEP(F2, d, a, b, c, in[ 2]+0xfcefa3f8,  9);
	MD5STEP(F2, c, d, a, b, in[ 7]+0x676f02d9, 14);
	MD5STEP(F2, b, c, d, a, in[12]+0x8d2a4c8a, 20);

	MD5STEP(F3, a, b, c, d, in[ 5]+0xfffa3942,  4);
	MD5STEP(F3, d, a, b, c, in[ 8]+0x8771f681, 11);
	MD5STEP(F3, c, d, a, b, in[11]+0x6d9d6122, 16);
	MD5STEP(F3, b, c, d, a, in[14]+0xfde5380c, 23);
	MD5STEP(F3, a, b, c, d, in[ 1]+0xa4beea44,  4);
	MD5STEP(F3, d, a, b, c, in[ 4]+0x4bdecfa9, 11);
	MD5STEP(F3, c, d, a, b, in[ 7]+0xf6bb4b60, 16);
	MD5STEP(F3, b, c, d, a, in[10]+0xbebfbc70, 23);
	MD5STEP(F3, a, b, c, d, in[13]+0x289b7ec6,  4);
	MD5STEP(F3, d, a, b, c, in[ 0]+0xeaa127fa, 11);
	MD5STEP(F3, c, d, a, b, in[ 3]+0xd4ef3085, 16);
	MD5STEP(F3, b, c, d, a, in[ 6]+0x04881d05, 23);
	MD5STEP(F3, a, b, c, d, in[ 9]+0xd9d4d039,  4);
	MD5STEP(F3, d, a, b, c, in[12]+0xe6db99e5, 11);
	MD5STEP(F3, c, d, a, b, in[15]+0x1fa27cf8, 16);
	MD5STEP(F3, b, c, d, a, in[ 2]+0xc4ac5665, 23);

	MD5STEP(F4, a, b, c, d, in[ 0]+0xf4292244,  6);
	MD5STEP(F4, d, a, b, c, in[ 7]+0x432aff97, 10);
	MD5STEP(F4, c, d, a, b, in[14]+0xab9423a7, 15);
	MD5STEP(F4, b, c, d, a, in[ 5]+0xfc93a039, 21);
	MD5STEP(F4, a, b, c, d, in[12]+0x655b59c3,  6);
	MD5STEP(F4, d, a, b, c, in[ 3]+0x8f0ccc92, 10);
	MD5STEP(F4, c, d, a, b, in[10]+0xffeff47d, 15);
	MD5STEP(F4, b, c, d, a, in[ 1]+0x85845dd1, 21);
	MD5STEP(F4, a, b, c, d, in[ 8]+0x6fa87e4f,  6);
	MD5STEP(F4, d, a, b, c, in[15]+0xfe2ce6e0, 10);
	MD5STEP(F4, c, d, a, b, in[ 6]+0xa3014314, 15);
	MD5STEP(F4, b, c, d, a, in[13]+0x4e0811a1, 21);
	MD5STEP(F4, a, b, c, d, in[ 4]+0xf7537e82,  6);
	MD5STEP(F4, d, a, b, c, in[11]+0xbd3af235, 10);
	MD5STEP(F4, c, d, a, b, in[ 2]+0x2ad7d2bb, 15);
	MD5STEP(F4, b, c, d, a, in[ 9]+0xeb86d391, 21);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}
#endif

int mk_MD5File(char *filename, unsigned char digest[16])
{
    int error = 0;
    FILE *input_file = NULL;
    unsigned char buf[BUF_SIZE];
    int num_read = 0;
    struct mk_MD5Context file_context;

    input_file = fopen(filename, "rb");
    if (!input_file)
        return errno;

    mk_MD5Init(&file_context);

    do
    {
        num_read = fread(buf, 1, BUF_SIZE, input_file);
        switch (num_read)
        {
            case -1:
                /* error */
                fclose(input_file);
                return errno;
            case 0:
                /* we're finished */
                break;
            default:
                mk_MD5Update(&file_context, buf, num_read);
                break;
        }
    } while (num_read);

    fclose(input_file);

    mk_MD5Final(digest, &file_context);
    
    return error;
}

char *base64_dump(unsigned char *buf, size_t buf_size)
{
    const char *b64_enc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int value = 0;
    unsigned int i;
    int bits = 0;
    int out_size = (8 * buf_size / 5) + 2;
    char *out = calloc(1, out_size);
    unsigned int out_pos = 0;

    if (!out)
        return NULL;

    for (i = 0; i < buf_size; i++)
    {
        value = (value << 8) | buf[i];
        bits += 2;
        out[out_pos++] = b64_enc[(value >> bits) & 63U];
        if (bits >= 6) {
            bits -= 6;
            out[out_pos++] = b64_enc[(value >> bits) & 63U];
        }
    }
    if (bits > 0)
    {
        value <<= 6 - bits;
        out[out_pos++] = b64_enc[value & 63U];
    }
    return out;
}

unsigned char *base64_parse(unsigned char *buf, size_t out_buf_size)
{
    unsigned int i;
    unsigned char *out = calloc(1, out_buf_size);
    unsigned int in_pos = 0;
    unsigned int out_bits_done = 0;
    unsigned int in_bits = 0;
    int in_value = 0;
    
    if (!out)
        return NULL;

    i = 0;
    while (i < out_buf_size)
    {
        while (out_bits_done < 8)
        {
            if (!in_bits)
            {
                if (buf[in_pos] >= 'A' && buf[in_pos] <= 'Z')
                    in_value = buf[in_pos] - 'A';
                else if (buf[in_pos] >= 'a' && buf[in_pos] <= 'z')
                    in_value = buf[in_pos] - 'a' + 26;
                else if (buf[in_pos] >= '0' && buf[in_pos] <= '9')
                    in_value = buf[in_pos] - '0' + 52;
                else if (buf[in_pos] == '-')
                    in_value = 62;
                else if (buf[in_pos] == '_')
                    in_value = 63;
                else
                    fprintf(stderr, "INVALID B64 DIGIT %c\n", buf[in_pos]);
                in_pos++;
                in_bits = 6;
            }
            out[i] <<= 1;
            out[i] |= (in_value & 0x20) >> 5;
            out_bits_done++;
            in_value = (in_value & 0x1F) << 1;
            in_bits--;
        }
        out_bits_done = 0;
        i++;
    }
    return out;
}

char *hex_dump(unsigned char *buf, size_t buf_size)
{
    int value = 0;
    unsigned int i;
    char *out = calloc(1, (buf_size * 2) + 1);
    unsigned int out_pos = 0;

    if (!out)
        return NULL;

    for (i = 0; i < buf_size; i++)
    {
        value = buf[i];
        if ( (value & 0xF0) >= 0xA0)
            out[out_pos++] = ( (value & 0xF0) >> 4 ) - 10 + 'a';
        else
            out[out_pos++] = ( (value & 0xF0) >> 4 ) + '0';

        if ( (value & 0x0F) >= 0x0A)
            out[out_pos++] = (value & 0x0F) - 10 + 'a';
        else
            out[out_pos++] = (value & 0x0F) + '0';
    }
    return out;
}

unsigned char *hex_parse(unsigned char *buf, size_t out_buf_size)
{
    int value_lo = 0;
    int value_hi = 0;
    unsigned int i;
    unsigned char *out = calloc(1, out_buf_size);
    
    if (!out)
        return NULL;

    for (i = 0; i < out_buf_size; i++)
    {
        if (buf[2*i] >= '0' && buf[2*i] <= '9')
            value_hi = buf[2*i] -  '0';
        else if (buf[2*i] >= 'a' && buf[2*i] <= 'f')
            value_hi = buf[2*i] -  'a' + 10;
        else if (buf[2*i] >= 'A' && buf[2*i] <= 'F')
            value_hi = buf[2*i] -  'A' + 10;
        else
            fprintf(stderr, "INVALID HEX DIGIT %c\n", buf[2*i]);

        if (buf[(2*i)+1] >= '0' && buf[(2*i)+1] <= '9')
            value_lo = buf[(2*i)+1] -  '0';
        else if (buf[(2*i)+1] >= 'a' && buf[(2*i)+1] <= 'f')
            value_lo = buf[(2*i)+1] -  'a' + 10;
        else if (buf[(2*i)+1] >= 'A' && buf[(2*i)+1] <= 'F')
            value_lo = buf[(2*i)+1] -  'A' + 10;
        else
            fprintf(stderr, "INVALID HEX DIGIT %c\n", buf[(2*i)+1]);
        
        out[i] = (value_hi << 4) | value_lo;
    }
    return out;
}

#ifdef TEST
/* Simple test program.  Can use it to manually run the tests from
   RFC1321 for example.  */
#include <stdio.h>

int
main (int argc, char **argv)
{
	struct mk_MD5Context context;
	unsigned char checksum[16];
	int i;
	int j;

	if (argc < 2)
	{
		fprintf (stderr, "usage: %s string-to-hash\n", argv[0]);
		exit (1);
	}
	for (j = 1; j < argc; ++j)
	{
		printf ("MD5 (\"%s\") = ", argv[j]);
		mk_MD5Init (&context);
		mk_MD5Update (&context, argv[j], strlen (argv[j]));
		mk_MD5Final (checksum, &context);
		for (i = 0; i < 16; i++)
		{
			printf ("%02x", (unsigned int) checksum[i]);
		}
		printf ("\n");
	}
	return 0;
}
#endif /* TEST */
