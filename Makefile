BINS = jigdump mkimage jigsum
CFLAGS = -g -Wall -Werror -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
CC = gcc

all: $(BINS)

mkimage: mkimage.o endian.o md5.o
	$(CC) -o $@ $+ -lz # -lbz2

jigsum: jigsum.o md5.o
	$(CC) -o $@ $+

clean:
	rm -f *.o $(BINS) *~

distclean: clean
