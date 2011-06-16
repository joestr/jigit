BINS = jigdump jigit-mkimage jigsum rsyncsum lib
CFLAGS = -g -Wall -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
#CC = gcc

all: $(BINS)

jigit-mkimage: mkimage.o endian.o md5.o
	$(CC) -o $@ $+ -lz -lbz2

jigsum: jigsum.o md5.o
	$(CC) -o $@ $+

rsyncsum: rsync.o md5.o
	$(CC) -o $@ $+

jigdump: jigdump.o md5.o
	$(CC) -o $@ $+

lib: libjte/Makefile
	make -C libjte

libjte/Makefile:
	cd libjte && ./configure

clean:
	rm -f *.o $(BINS) *~ build-stamp
	-make -C libjte clean

distclean: clean
	-make -C libjte distclean