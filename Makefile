BINS = jigdump mkimage jigsum rsyncsum jigdoofus
CFLAGS = -g -pg -Wall -Werror -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
CFLAGS += $(shell pkg-config --cflags fuse)
CC = gcc

all: $(BINS)

DB_OBJ=jigdb-sql.o
DBLIB=-lsqlite3

mkimage: mkimage.o endian.o md5.o parse_jigdo.o parse_template.o decompress.o jd_interface.o $(DB_OBJ)
	$(CC) -o $@ $+ -lz -lbz2 -pg $(DBLIB)

jigdoofus: jigdoofus.o endian.o md5.o parse_jigdo.o parse_template.o decompress.o jd_interface.o $(DB_OBJ)
	$(CC) -o $@ $+ -lz -lbz2 -pg $(DBLIB) -lfuse

jigsum: jigsum.o md5.o $(DB_OBJ)
	$(CC) -o $@ $+ $(DBLIB)

rsyncsum: rsync.o md5.o
	$(CC) -o $@ $+

jigdump: jigdump.o md5.o
	$(CC) -o $@ $+

clean:
	rm -f *.o $(BINS) *~

distclean: clean
