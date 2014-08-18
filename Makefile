BINS = jigdump jigit-mkimage jigsum rsyncsum lib extract-data
CFLAGS += -g -Wall -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
#CC = gcc

all: $(BINS)

jigit-mkimage: mkimage.o endian.o md5.o uncompress.o
	$(CC) $(LDFLAGS) -o $@ $+ -lz -lbz2

extract-data: extract-data.o endian.o uncompress.o
	$(CC) $(LDFLAGS) -o $@ $+ -lz -lbz2

jigsum: jigsum.o md5.o
	$(CC) $(LDFLAGS) -o $@ $+

rsyncsum: rsync.o md5.o
	$(CC) $(LDFLAGS) -o $@ $+

jigdump: jigdump.o md5.o
	$(CC) $(LDFLAGS) -o $@ $+

lib: libjte/Makefile
	make -C libjte

libjte/Makefile:
	cd libjte && ./configure

clean:
	rm -f *.o $(BINS) *~ build-stamp
	-make -C libjte clean

distclean: clean
	-make -C libjte distclean