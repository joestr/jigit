
# LIBBURNIA_PKGCONFDIR is defined OS specific in acinclude.m4
# was: pkgconfigdir=$(libdir)/pkgconfig
pkgconfigdir=$(LIBBURNIA_PKGCONFDIR)

libincludedir=$(includedir)/libjte

lib_LTLIBRARIES = libjte/libjte.la
ACLOCAL_AMFLAGS = -I ./
## ========================================================================= ##

# Build libraries

libjte_libjte_la_LDFLAGS = \
	-version-info $(LT_CURRENT):$(LT_REVISION):$(LT_AGE)

# Eventually enabling system adapters for ACL and EA.
libjte_libjte_la_CFLAGS = $(ZLIB_DEF) $(BZIP2_DEF) $(PTHREADS_DEF)

libjte_libjte_la_SOURCES = \
	checksum.c \
	checksum.h \
	endian.c \
	endianconv.h \
	jte.c \
	jte.h \
	libjte.c \
	libjte.h \
	libjte_private.h \
	md5.c \
	md5.h \
	rsync.c \
	rsync.h \
	sha1.c \
	sha1.h \
	sha256.c \
	sha256.h \
	sha512.c \
	sha512.h

libjte_libjte_la_LIBADD= \
	-lz

libinclude_HEADERS = \
	libjte.h

install-exec-hook:
	$(LIBBURNIA_LDCONFIG_CMD) "$(DESTDIR)$(libdir)" || echo 'NOTE: Explicite dynamic library configuration failed. If needed, configure manually for:' "$(DESTDIR)$(libdir)"

## ========================================================================= ##

## Build demo applications (if there any)
noinst_PROGRAMS = \
	test/demo

# test/demo

test_demo_CPPFLAGS = -Ilibjte
test_demo_LDADD = $(libjte_libjte_la_OBJECTS) $(libjte_libjte_la_LIBADD)
test_demo_SOURCES = test/demo.c


## ========================================================================= ##

# Extra things
nodist_pkgconfig_DATA = \
	libjte-2.pc


EXTRA_DIST = \
	libjte-2.pc.in \
	bin/jigdo-gen-checksum-list \
	doc/jigdo-gen-checksum-list.1 \
	doc/API \
	doc/TODO \
	doc/NOTES \
	COPYRIGHT \
	libjte.ver

