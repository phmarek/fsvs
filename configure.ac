#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.71])
AC_INIT([fsvs],[esyscmd(make --quiet --no-print-directory -f Makefile.in version-nnl 2>/dev/null)],[https://github.com/phmarek/fsvs])
AC_USE_SYSTEM_EXTENSIONS

# if [[ "x$cache_file" == /dev/null ]]
# then
# 	cache_file=config.cache
# fi
# AC_CACHE_LOAD

AC_CONFIG_SRCDIR([src/actions.c])
AC_CONFIG_HEADERS([src/config.h])

AC_MSG_NOTICE([***  Now configuring FSVS ]AC_PACKAGE_VERSION[  ***])

# Checks for programs.
AC_PROG_CC
AC_PROG_CPP

AC_DEFINE(APR_PATH)
AC_SUBST(APR_PATH)

##################################### Header files
INCDIRS="/usr/local/include /usr/include /openpkg/include "
# The subversion headers do a #include <apr_pools.h>, so the APR libraries 
# *have* to be directly specified.
# Furthermore there's apr-1/ as directory name, depending on apr version.
# Is there something like this available for subversion?
AC_ARG_WITH(aprinc,
		AS_HELP_STRING([--with-aprinc=PATH],[Specify an include directory for the APR headers.]),
		[
			INCDIRS="$INCDIRS $withval"
		], [
			if APR_PATH=`apr-1-config --includedir || apr-config --includedir`
			then
				INCDIRS="$INCDIRS $APR_PATH"
			fi
		])

AC_ARG_WITH(svninc,
		AS_HELP_STRING([--with-svninc=PATH],[Specify an include directory for the subversion headers.]),
		[
			INCDIRS="$INCDIRS $withval"
		])

AC_ARG_WITH(svninc,
		AS_HELP_STRING([--with-svninc=PATH],[Specify an include directory for the subversion headers.]),
		[
			INCDIRS="$INCDIRS $withval"
		])

AC_ARG_WITH(waa_md5,
		AS_HELP_STRING([--with-waa_md5=NUMBER],[Specifies how many hex characters of the MD5 of the working copy root
			 should be used to address the data in the WAA.
			 This may be increased if you have a lot of different working 
			 copies on a single machine.
			 The default is 0; useful values are 0, and from 6 to 32.]),
		[
		 # The shell gives an error on numeric comparision with a non-numeric 
		 # value.
		 # We allow from 3 characters on, although it might not make much 
		 # sense.
		 WAA_WC_MD5_CHARS=`perl -e '$_=0+shift; print $_+0 if $_==0 || ($_>3 && $_<=16)' "$withval"`
		 if [[ "$WAA_WC_MD5_CHARS" = "" ]]
		 then
			 AC_MSG_ERROR([[The given value for --with-waa_md5 is invalid.]])
		 fi
		], [ WAA_WC_MD5_CHARS=0 ])
AC_DEFINE_UNQUOTED(WAA_WC_MD5_CHARS, $WAA_WC_MD5_CHARS,
			 [Number of bytes for WAA addressing is $WAA_WC_MD5_CHARS.])
AC_SUBST(WAA_WC_MD5_CHARS)


CFLAGS="$CFLAGS -D_GNU_SOURCE=1 -D_FILE_OFFSET_BITS=64 -DPCRE2_CODE_UNIT_WIDTH=8"
for dir in $INCDIRS
do
# using -I would result in the files being _non_ system include 
# directories, ie. they'd clutter the dependency files.
# That's why -idirafter is used.
	CFLAGS="$CFLAGS -idirafter $dir"
done
AC_DEFINE_UNQUOTED(CFLAGS, [$CFLAGS])
AC_SUBST(CFLAGS)
AC_MSG_NOTICE(["CFLAGS=$CFLAGS"])

##################################### Linker
LIBDIRS="/usr/local/lib /openpkg/lib"

AC_ARG_WITH(aprlib,
		AS_HELP_STRING([--with-aprlib=PATH],[Specify a directory containing APR libraries.]),
		[
			LIBDIRS="$LIBDIRS $withval"
		])

AC_ARG_WITH(svnlib,
		AS_HELP_STRING([--with-svnlib=PATH],[Specify a directory containing subversion libraries.]),
		[
			LIBDIRS="$LIBDIRS $withval"
		])

for dir in $LIBDIRS
do
	LDFLAGS="$LDFLAGS -L$dir"
done
AC_DEFINE_UNQUOTED(LDFLAGS, [$LDFLAGS])
AC_SUBST(LDFLAGS)

AC_MSG_NOTICE(["LDFLAGS=$LDFLAGS"])


EXTRALIBS="-laprutil-1 -lapr-1"
if [[ `uname -s` = "SunOS" ]]
then
	# Solaris 10, thanks Peter.
	EXTRALIBS="-lsocket -lnsl $EXTRALIBS"
fi
if [[ `uname -s` = "Darwin" ]]
then
	# OSX 10.6 - thanks, Florian.
	EXTRALIBS="-liconv $EXTRALIBS"
	have_fmemopen=no
fi


AC_DEFINE_UNQUOTED(EXTRALIBS, [$EXTRALIBS])
AC_SUBST(EXTRALIBS)


##################################### Checks
# Checks for libraries.
AC_CHECK_LIB([pcre2-8], [pcre2_compile_8],
	[],
	[AC_MSG_FAILURE([Sorry, can't find PCRE2-8.])])
AC_CHECK_LIB([aprutil-1], [apr_md5_init], [],
	[AC_MSG_FAILURE([Sorry, can't find APR.])])
AC_CHECK_LIB([svn_delta-1], [svn_txdelta_apply], [],
	[AC_MSG_FAILURE([Sorry, can't find subversion.])])
AC_CHECK_LIB([svn_ra-1], [svn_ra_initialize], [],
	[AC_MSG_FAILURE([Sorry, can't find subversion.])])
AC_CHECK_LIB([gdbm], [gdbm_firstkey], [],
	[AC_MSG_FAILURE([Sorry, can't find gdbm.])])

# Checks for header files.
# Autoupdate added the next two lines to ensure that your configure
# script's behavior did not change.  They are probably safe to remove.
AC_CHECK_INCLUDES_DEFAULT
AC_PROG_EGREP

AC_CHECK_HEADERS([fcntl.h stddef.h stdlib.h string.h sys/time.h unistd.h pcre2.h ],
		[],
		[AC_MSG_FAILURE([Needed header file not found.])])

#apr_file_io.h subversion-1/svn_md5.h])
AC_HEADER_DIRENT

AC_CHECK_MEMBERS([struct stat.st_mtim])

AC_COMPILE_IFELSE(
	[AC_LANG_PROGRAM(
		[[ #include <valgrind/memcheck.h> ]], 
		[[ VALGRIND_MAKE_MEM_DEFINED(0, 2); ]] )],
		[have_valgrind=yes], [have_valgrind=no])
if test x$have_valgrind = xyes ; then
	AC_DEFINE(HAVE_VALGRIND, 1, compatible valgrind version found)
else
  AC_MSG_NOTICE([No compatible valgrind version.])
fi


# Check whether S_IFMT is dense, ie. a single block of binary ones.
# If it isn't, the bitcount wouldn't tell the needed bits to represent the 
# data.
# If S_IFMT is dense, the increment results in a single carry bit.
# Checked via changing /usr/include/bits/stat.h.
AC_RUN_IFELSE([AC_LANG_SOURCE([
  #include "src/preproc.h"
  int main(int argc, char **args)
  {
	  if (_BITCOUNT( (S_IFMT >> MODE_T_SHIFT_BITS) + 1) == 1)
	  return 0;
  else
	  return 1;
  }
  ])],
  [AC_MSG_NOTICE([S_IFMT is ok.])],
  [AC_MSG_FAILURE([You have a sparse S_IFMT. Please create a github issue.])])


AC_CHECK_HEADERS([linux/kdev_t.h])

AC_ARG_ENABLE(dev-fake,
	AS_HELP_STRING([--enable-dev-fake],[Include fake definitions for MAJOR(), MINOR() and MKDEV(). Needed if none found.]),
	[AC_DEFINE([ENABLE_DEV_FAKE])
	 ENABLE_DEV_FAKE=1],
	[])
AC_SUBST(ENABLE_DEV_FAKE)


AC_ARG_ENABLE(debug,
	AS_HELP_STRING([--enable-debug],[compile some extra debug checks in (valgrind, gdb) (default is no)]),
	[AC_DEFINE([ENABLE_DEBUG])
	 ENABLE_DEBUG=1],
	[])
AC_SUBST(ENABLE_DEBUG)


AC_ARG_ENABLE(gcov,
	AS_HELP_STRING([--enable-gcov],[whether to compile with instrumentation for gcov (default is no) (needs --enable-debug)]),
	[AC_DEFINE([ENABLE_GCOV])
	 ENABLE_GCOV=1],
	[])
AC_DEFINE([ENABLE_GCOV])
AC_SUBST(ENABLE_GCOV)

AC_COMPILE_IFELSE(
	[AC_LANG_PROGRAM(
		[[ #include <fcntl.h> ]], 
		[[ int i=O_DIRECTORY; ]] )],
		[have_o_directory=yes], [have_o_directory=no])
if test x$have_o_directory = xyes ; then
	AC_DEFINE(HAVE_O_DIRECTORY, 1, O_DIRECTORY found)
fi
AC_SUBST(HAVE_O_DIRECTORY)


AC_LINK_IFELSE(
	[AC_LANG_PROGRAM(
	 [[ #include <stdio.h> ]], 
	 [[ char **environ; int main(void) { return environ == NULL; } ]] )],
		 [need_environ_extern=no], [need_environ_extern=yes])
if test x$need_environ_extern = xyes ; then
	AC_DEFINE(NEED_ENVIRON_EXTERN, 1, "char **environ" needs "extern")
fi
AC_SUBST(NEED_ENVIRON_EXTERN)

if test x$have_fmemopen = x
then
	AC_LINK_IFELSE(
		[AC_LANG_PROGRAM(
			[[ #include <stdio.h> ]], 
			[[ int main(int argc, char *args[])
			{ return fmemopen(args[0], 2, args[1]) == NULL; }	  ]] )],
			[have_fmemopen=yes], [have_fmemopen=no])
fi
if test x$have_fmemopen = xyes
then
	AC_DEFINE(HAVE_FMEMOPEN, 1, [fmemopen() found])
else
	AC_MSG_WARN([fmemopen() not found. debug_buffer option not available.])
fi
AC_SUBST(HAVE_FMEMOPEN)


if locale -a > /dev/null 2>&1
then
	AC_DEFINE([HAVE_LOCALES],[1])
fi
AC_SUBST(HAVE_LOCALES)


AC_ARG_WITH(chroot,
	AS_HELP_STRING([--with-chroot=PATH],[Specify a chroot environment for the fsvs-chrooter 
		helper.]),
	[
		if test "$withval" = "yes" ; then
			AC_MSG_ERROR([--with-chroot requires an argument.])
		else
			CHROOTER_JAIL=$withval
			AC_DEFINE_UNQUOTED(CHROOTER_JAIL, "$CHROOTER_JAIL",
				[The path of a chroot jail.])
		fi
	])
AC_SUBST(CHROOTER_JAIL)


AC_ARG_ENABLE(release,
	AS_HELP_STRING([--enable-release],[whether to compile without debug messages. Makes image smaller (to about half size), but makes -d and -D inoperative. (Default is no)]),
	[AC_DEFINE([ENABLE_RELEASE])
	 ENABLE_RELEASE=1],
	[])
AC_SUBST(ENABLE_RELEASE)

if [[ "$ENABLE_RELEASE$ENABLE_DEBUG" = "11" ]]
then
	AC_MSG_ERROR([[--enable-debug and --enable-release are incompatibel.
	Use one or the other.]])
fi

AC_CHECK_FUNCS([getdents64])
AC_CHECK_HEADERS([linux/types.h])
AC_CHECK_HEADERS([linux/unistd.h])
AC_CHECK_TYPES([comparison_fn_t])

AC_SYS_LARGEFILE

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_C_INLINE
AC_CHECK_MEMBERS([struct stat.st_rdev])
AC_CHECK_HEADERS_ONCE([sys/time.h])

AC_STRUCT_TM

# Checks for library functions.
AC_FUNC_CHOWN
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_MMAP
AC_FUNC_REALLOC

AC_FUNC_VPRINTF
AC_CHECK_FUNCS([fchdir getcwd gettimeofday memmove memset mkdir munmap rmdir strchr strdup strerror strrchr strtoul strtoull alphasort dirfd lchown lutimes strsep])

# AC_CACHE_SAVE

AC_CONFIG_FILES([src/Makefile tests/Makefile src/.ycm_extra_conf.py])
AC_OUTPUT

# Cause a recompile
touch src/config.h

if [ [ "$ac_cv_header_linux_kdev_t_h" = "no" -a "x$ENABLE_DEV_FAKE" = "x" ] ]
then
	AC_MSG_WARN([
  * MAJOR(), MINOR() and MAKEDEV() definitions not found.
  * Fake a definition, but that could make problems for ignore patterns
  * and commits/updates of device nodes, so these will be disabled.
  * Please use a github issue for help or, if you know your 
  * systems' way, to report the correct header name.
  *
  * If you *really* need to use device compares, and have *no* other way, 
  * you could try using the --enable-dev-fake option on ./configure.])
fi

# vi: ts=3 sw=3
