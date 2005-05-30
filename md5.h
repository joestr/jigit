/* See md5.c for explanation and copyright information.  */

#ifndef MD5_H
#define MD5_H

/* Unlike previous versions of this code, uint32 need not be exactly
   32 bits, merely 32 bits or more.  Choosing a data type which is 32
   bits instead of 64 is not important; speed is considerably more
   important.  ANSI guarantees that "unsigned long" will be big enough,
   and always using it seems to have few disadvantages.  */
typedef unsigned long mk_uint32;

struct mk_MD5Context {
	mk_uint32 buf[4];
	mk_uint32 bits[2];
	unsigned char in[64];
};

void mk_MD5Init (struct mk_MD5Context *context);
void mk_MD5Update (struct mk_MD5Context *context,
			   unsigned char const *buf, unsigned len);
void mk_MD5Final (unsigned char digest[16],
			  struct mk_MD5Context *context);
void mk_MD5Transform (mk_uint32 buf[4], const unsigned char in[64]);
int  mk_MD5File(char *filename, unsigned char digest[16]);

char *base64_dump(unsigned char *buf, size_t buf_size);
unsigned char *base64_parse(unsigned char *buf, size_t buf_size);
char *hex_dump(unsigned char *buf, size_t buf_size);
unsigned char *hex_parse(unsigned char *buf, size_t buf_size);

#endif /* !MD5_H */
