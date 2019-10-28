#include <stdlib.h>     /* for calloc() */

char *base64_dump(unsigned char *buf, size_t buf_size)
{
    const char *b64_enc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int value = 0;
    unsigned int i;
    int bits = 0;
    char *out = calloc(1, buf_size * 2);
    unsigned int out_pos = 0;

    if (!out)
        return NULL;

    for (i = 0; i < buf_size ; i++)
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

