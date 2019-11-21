BINS = jigdump jigit-mkimage jigsum jigsum-sha256 rsyncsum lib extract-data parallel-sums
CFLAGS += -g -Wall -I libjte -pthread -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
#CC = gcc

CKSUM_OBJS = libjte/libjte_libjte_la-checksum.o libjte/libjte_libjte_la-md5.o libjte/libjte_libjte_la-sha1.o libjte/libjte_libjte_la-sha256.o libjte/libjte_libjte_la-sha512.o

all: $(BINS)

jigit-mkimage: mkimage.o endian.o md5.o libjte/libjte_libjte_la-sha256.o uncompress.o jig-base64.o
	$(CC) $(LDFLAGS) -o $@ $+ -lz -lbz2

extract-data: extract-data.o endian.o uncompress.o
	$(CC) $(LDFLAGS) -o $@ $+ -lz -lbz2

jigsum: jigsum.o md5.o jig-base64.o
	$(CC) $(LDFLAGS) -o $@ $+

jigsum-sha256: jigsum-sha256.o libjte/libjte_libjte_la-sha256.o jig-base64.o
	$(CC) $(LDFLAGS) -o $@ $+

rsyncsum: rsync.o md5.o jig-base64.o
	$(CC) $(LDFLAGS) -o $@ $+

jigdump: jigdump.o md5.o jig-base64.o
	$(CC) $(LDFLAGS) -o $@ $+

lib: libjte/Makefile
	make -C libjte

libjte/Makefile:
	cd libjte && ./configure

$(CKSUM_OBJS): lib

parallel-sums: parallel-sums.o $(CKSUM_OBJS)
	$(CC) -pthread $(LDFLAGS) -o $@ $+ -lpthread

clean:
	rm -f *.o $(BINS) *~ build-stamp
	-make -C libjte clean

distclean: clean
	-make -C libjte distclean

# Create source tarball from git. Complicated some - do autoconf dance
# in there too
gitdist:	Makefile
		@VERSION=$$(git describe | awk '{gsub("^.*/","");gsub("^v","");print $$0}'); \
		echo "VERSION is $$VERSION"; \
		OUTPUT="jigit-$$VERSION"; \
		WD=$$(pwd); \
		if [ -e "../$$OUTPUT.tar.xz" ]; then \
		 echo "../$$OUTPUT.tar.xz exists - delete it first"; \
		 exit 1; \
		fi ; \
		if [ -d "../$$OUTPUT" ]; then \
		 echo "../$$OUTPUT exists - delete it first"; \
		 exit 1; \
		fi ; \
		echo "Creating working dir in ../$$OUTPUT"; \
		git archive --format=tar --prefix="$$OUTPUT/" HEAD | tar -C .. -xf - ; \
		echo "Running autoconf then cleanup in ../$$OUTPUT"; \
		cd ../$$OUTPUT/libjte && ./bootstrap && rm -rf autom4te.cache && cd $$WD; \
		echo "Creating dist tarball in ../$$OUTPUT.tar.xz"; \
		tar -C .. -c --xz -f ../$$OUTPUT.tar.xz $$OUTPUT; rm -rf ../$$OUTPUT

