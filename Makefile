BINS = jigdump mkimage jigsum rsyncsum
CFLAGS = -g -Wall -Werror -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
#CC = gcc

all: $(BINS)

#DB_OBJ=jigdb-db.o
#DBLIB=-ldb

DB_OBJ=jigdb-sql.o
DBLIB=-lsqlite3

mkimage: mkimage.o endian.o md5.o $(DB_OBJ)
	$(CC) -o $@ $+ -lz $(DBLIB)

jigsum: jigsum.o md5.o $(DB_OBJ)
	$(CC) -o $@ $+ $(DBLIB)

rsyncsum: rsync.o md5.o
	$(CC) -o $@ $+

jigdump: jigdump.o md5.o
	$(CC) -o $@ $+

clean:
	rm -f *.o $(BINS) *~

distclean: clean
