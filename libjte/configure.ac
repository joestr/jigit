dnl IMPORTANT:
dnl When changing the library version, change also the macros LIBJTE_VERSION_*
dnl in libjte.h.
dnl You will probably want to increment LT_CURRENT and LT_AGE to increment
dnl the age counter X of the library name libjte.so.2.X.0
dnl
AC_INIT([libjte], [2.0.0], [https://www.einval.com/~steve/software/JTE/])

AC_PREREQ([2.50])

AC_CANONICAL_HOST
AC_CANONICAL_TARGET

LIBBURNIA_SET_FLAGS

AM_INIT_AUTOMAKE([subdir-objects])
AC_CONFIG_MACRO_DIR([./])

dnl
dnl if MAJOR or MINOR version changes, be sure to change AC_INIT above to match
dnl
dnl CURRENT and AGE describe the binary compatibility interval of a
dnl dynamic library.
dnl See also http://www.gnu.org/software/libtool/manual.html#Interfaces
dnl
dnl The name of the library will be libjte.so.$CURRENT-$AGE.$AGE.$REV
dnl In the terminology of this file:
dnl   CURRENT = LT_CURRENT
dnl   REV     = LT_REVISION
dnl   AGE     = LT_AGE
dnl
dnl SONAME of the emerging library is  LT_CURRENT - LT_AGE.
dnl

dnl 
dnl Library name = libjte.so.(LT_CURRENT-LT_AGE).(LT_AGE).(LT_REVISION)
dnl
dnl I.e. currently: libjte.so.2.0.0
dnl
LT_CURRENT=2
LT_AGE=0
LT_REVISION=0
LT_CURRENT_MINUS_AGE=`expr $LT_CURRENT - $LT_AGE`

AC_SUBST(LT_CURRENT)
AC_SUBST(LT_REVISION)
AC_SUBST(LT_AGE)
AC_SUBST(LT_CURRENT_MINUS_AGE)

AC_PREFIX_DEFAULT([/usr/local])
test "$prefix" = "NONE" && prefix=$ac_default_prefix

AM_MAINTAINER_MODE

AM_PROG_CC_C_O
AC_C_CONST
AC_C_INLINE
AC_C_BIGENDIAN

dnl Large file support
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO
AC_CHECK_FUNC([fseeko])
if test ! $ac_cv_func_fseeko; then
   AC_MSG_ERROR([Libjte requires largefile support.])
fi

AC_PROG_LIBTOOL
AC_SUBST(LIBTOOL_DEPS)
LIBTOOL="$LIBTOOL --silent"

AC_PROG_INSTALL

AC_CHECK_HEADERS([fcntl.h inttypes.h limits.h stddef.h stdint.h stdlib.h string.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT8_T
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_CHECK_FUNCS([memset regcomp strcasecmp strdup strerror strncasecmp strndup strstr])

dnl Use GNU extensions if available
AC_DEFINE(_GNU_SOURCE, 1)


TARGET_SHIZZLE
AC_SUBST(ARCH)


if test x$GCC = xyes; then
    CFLAGS="$CFLAGS -std=c99 -g -pedantic -Wall"
fi
CFLAGS="$CFLAGS -DDEBUG"


dnl Determine target directory for libjte-*.pc
dnl Important: Must be performed _after_ TARGET_SHIZZLE
dnl
LIBBURNIA_SET_PKGCONFIG


dnl Add compiler-specific flags

dnl >>> Shall we test for zlib or just fail to build ?
dnl
dnl The macro LIBJTE_WITH_ZLIB is needed in any case to make libjte code
dnl compilable without dependency on zlib. It will not be usable then.
dnl So for now in libjte itself, the macro gets set unconditionally.
dnl In GNU xorriso build system it will not be set if zlib is not enabled.
dnl
ZLIB_DEF="-DLIBJTE_WITH_ZLIB"
AC_SUBST(ZLIB_DEF)

AC_ARG_ENABLE(libbz2,
[  --enable-libbz2           Enable use of libbz2 by libjte, default=yes],
               , enable_libbz2=yes) 
if test "x$enable_libbz2" = xyes; then
dnl Check whether there is the header for libbz2.
    BZIP2_DEF="-DLIBJTE_WITH_LIBBZ2"
    AC_CHECK_HEADER(bzlib.h, AC_CHECK_LIB(bz2, BZ2_bzCompressInit, , BZIP2_DEF= ), BZIP2_DEF= )
else
    BZIP2_DEF=
fi
AC_SUBST(BZIP2_DEF)

AC_ARG_ENABLE(pthreads,
[  --enable-pthreads           Enable use of pthreads by libjte, default=yes],
               , enable_pthreads=yes) 
if test "x$enable_pthreads" = xyes; then
dnl Check whether there is the header for libpthread.
    PTHREADS_DEF="-DTHREADED_CHECKSUMS"
    AC_CHECK_HEADER(pthread.h, AC_CHECK_LIB(pthread, pthread_create, , PTHREADS_DEF= ), PTHREADS_DEF= )
else
    PTHREADS_DEF=
fi
AC_SUBST(PTHREADS_DEF)

# Library versioning normally serves a complex purpose.
# Since libjte obeys strict ABI backward compatibility, it needs only the
# simple feature to declare function names "global:" or "local:". Only the
# global ones are visible to applications at library load time.
AC_ARG_ENABLE(versioned-libs,
[  --enable-versioned-libs Enable strict symbol encapsulation , default=yes],
               , enable_versioned_libs=yes)
if test x$enable_versioned_libs = xyes; then
    vers_libs_test=no
    LIBJTE_ASSERT_VERS_LIBS
    if test x$vers_libs_test = xno
    then 
        echo "disabled strict symbol encapsulation (test failed)"
    else
        echo "enabled  strict symbol encapsulation"
    fi
else
    echo "disabled strict symbol encapsulation"
fi

AC_ARG_ENABLE(ldconfig-at-install,
[  --enable-ldconfig-at-install    On GNU/Linux run ldconfig, default=yes],
               , ldconfig_at_install=yes)
if test x$ldconfig_at_install = xyes; then 
    dummy=dummy
else
    LIBBURNIA_LDCONFIG_CMD="echo 'NOTE: ldconfig is disabled. If needed, configure manually for:'"
    echo "disabled run of ldconfig during installation on GNU/Linux"
fi  
AC_SUBST(LIBBURNIA_LDCONFIG_CMD)

AC_CONFIG_FILES([
	Makefile
	version.h
	libjte-2.pc
	])
AC_OUTPUT
