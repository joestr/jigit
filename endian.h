/*
 * endian.h
 *
 * Simple helper functions for reading and writing BE and LE numbers
 *
 * Copyright (c) 2004 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

void                  write_be64  (unsigned long long in, unsigned char *out);
unsigned long long    read_be64   (unsigned char *in);
void                  write_le64  (unsigned long long in, unsigned char *out);
unsigned long long    read_le64   (unsigned char *in);

void                  write_le48  (unsigned long long in, unsigned char *out);
unsigned long long    read_le48   (unsigned char *in);

void                  write_be32  (unsigned long in, unsigned char *out);
unsigned long         read_be32   (unsigned char *in);
void                  write_le32  (unsigned long in, unsigned char *out);
unsigned long         read_le32   (unsigned char *in);

void                  write_be16  (unsigned short in, unsigned char *out);
unsigned short        read_be16   (unsigned char *in);
void                  write_le16  (unsigned short in, unsigned char *out);
unsigned short        read_le16   (unsigned char *in);
