BINS = dump dump-jte mkimage 
CFLAGS = -g -Wall -Werror -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
CC = gcc

all: $(BINS)

mkimage: mkimage.o endian.o md5.o
	$(CC) -o $@ $+ -lz

clean:
	rm -f *.o $(BINS) *~
