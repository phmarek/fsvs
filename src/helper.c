/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <dlfcn.h>
#include <netdb.h>
#include <time.h>
#include <sys/types.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <apr_file_io.h>
#include <apr_md5.h>
#include <subversion-1/svn_config.h>

#include "global.h"
#include "waa.h"
#include "est_ops.h"
#include "options.h"
#include "interface.h"
#include "checksum.h"
#include "helper.h"
#include "cache.h"


/** \file
 * General helper functions. */

/* Must be behind global.h or at least config.h */
#ifdef HAVE_LOCALES
#include <iconv.h>
#endif


#ifdef HAVE_LOCALES
/** Initializer for i18n operations.
 * \param from_charset Source charset
 * \param to_charset Destination charset.
 * \param cd iconv-handle */
int hlp___get_conv_handle(const char* from_charset, 
		const char* to_charset,
		iconv_t* cd)
{
	int status;

	status=0;
	*cd = iconv_open(to_charset, from_charset);
	STOPIF_CODE_ERR( *cd == (iconv_t)-1, errno,
			"Conversion from %s to %s is not supported",
			from_charset, to_charset);

ex:
	return status;
}


/** Charset convert function.
 * Using a handle obtained with \a hlp___get_conv_handle() this function 
 * dynamically allocates some buffer space, and returns the converted 
 * data in it.
 *
 * \param cd A conversion handle
 * \param from The source string
 * \param to A pointer to the converted string.
 * \param len The maximum number of input characters to translate.
 *
 * If \a from is \a NULL, \a to is returned as \a NULL.
 *
 * A few buffers are used round-robin, so that the caller need not free
 * anything and the maximum memory usage is limited.
 * Normally only 1 or 2 buffers are "active", eg. file name for a symlink
 * and its destination, or source and destination for apply_textdelta. 
 *
 * The destination string is always terminated with a \\0.
 *
 * \note If there's an irreparable conversion error, we must not print 
 * (parts of) the strings. One or even both might not be suitable for 
 * printing on the current console - so we do not know what could happen. */
inline int hlp___do_convert(iconv_t cd, 
		const char* from, char** to, 
		int len)
{
	static struct cache_t *cache;
	int status;
	char* to_buf;
	const char* from_buf;
	size_t srclen_rem, dstlen_rem;
	int iconv_ret, i, done;
	struct cache_entry_t *ent;


	STOPIF( cch__new_cache(&cache, 8), NULL);

	/* Input = NULL ==> Output = NULL */
	if (!from)
	{
		*to = NULL;
		goto ex;
	}


	srclen_rem = len == -1 ? strlen(from)+1 : len;
	from_buf=from;

	STOPIF( cch__add(cache, 0, NULL, srclen_rem, &to_buf), NULL);
	ent=cache->entries[cache->lru];

	/* Do the conversion. */
	while(srclen_rem)
	{
		done=to_buf - ent->data;

		/* Check for buffer space; reallocate, if necessary. */
		if (ent->len - done < srclen_rem)
		{
			/* Calculate increased buffer size. */
			i=ent->len + 2*srclen_rem + 16;

			/* If we'd need less than 256 bytes, then get 256 bytes.
			 * There's a good chance that we'll get longer file names;
			 * we'll avoid a re-allocate, and it isn't that much memory. */
			i = i < 256 ? 256 : i;

			STOPIF( cch__entry_set(cache->entries + cache->lru,
						0, NULL, i, 1, &to_buf), NULL);
			ent=cache->entries[cache->lru];

			/* Due to the reallocate, the buffer address may have
			 * changed. Update our working pointer according to our
			 * remembered position. */
			to_buf=ent->data+done;
		}

		/* How much space is left? */
		dstlen_rem=ent->len - done;

		/* Field precision and length cannot be given as long; let's hope they 
		 * are never that long :-) */
		DEBUGP("before iconv from=%-*.*s", 
				(int)srclen_rem, (int)srclen_rem, from_buf);
		/* iconv should have a const in it! */
		iconv_ret = iconv(cd,
				(char**)&from_buf, &srclen_rem,
				&to_buf, &dstlen_rem);
		DEBUGP("after iconv to=%s ret=%d", 
				ent->data,
				iconv_ret);

		/* Only allowed error is E2BIG. */
		if (iconv_ret == -1)
		{
			/* We don't know which pointer has the local codeset, and even if we
			 * did, we don't know if it's safe to print it.
			 * After all, we got a conversion error - there may be invalid 
			 * characters in it.
			 * "Hier seyen Drachen" :-] */
			STOPIF_CODE_ERR( errno != E2BIG, errno,
					"Conversion of string failed. "
					"Next bytes are \\x%02X\\x%02X\\x%02X\\x%02X",
					srclen_rem>=1 ? from_buf[0] : 0,
					srclen_rem>=2 ? from_buf[1] : 0,
					srclen_rem>=3 ? from_buf[2] : 0,
					srclen_rem>=4 ? from_buf[3] : 0
					);

			/* We got E2BIG, so get more space. That should happen automatically 
			 * in the next round. */
		}
	}


	/* Terminate */
	*to_buf=0;

	/* Return */
	*to=ent->data;
	DEBUGP("converted %*.*s to %s", len, len, from, *to);

ex:
	/* reset the conversion. */
	iconv(cd, NULL, NULL, NULL, NULL);

	return status;
}


/** Dummy converter function.
 * We need to honor the length parameter; but this function XOR the 
 * hlp___do_convert() run, so only one cache is active.
 *
 * If the length is given as \c -1, the original string is returned. */
int hlp___dummy_convert(const char *input, char**output, int len)
{
	int status;
	static struct cache_t *cache;


	status=0;
	if (!input) 
		*output=NULL;
	else
	{
		if (len == -1)
			len=strlen(input)+1;

		STOPIF( cch__new_cache(&cache, 8), NULL);
		STOPIF( cch__add(cache, 0, input, len+1, output), NULL);
		(*output)[len]=0;
	}

ex:
	return status;
}


/** -.
 * If \a len \c ==-1, a \c strlen() is done.
 * */
int hlp__local2utf8(const char *local_string, char** utf8_string, int len)
{
	static iconv_t iconv_cd = NULL;
	int status;

	status=0;
	if (!local_codeset)
		STOPIF( hlp___dummy_convert(local_string, utf8_string, len), NULL);
	else
	{
		if (!iconv_cd)
		{
			STOPIF( hlp___get_conv_handle( local_codeset, "UTF-8", &iconv_cd),
					NULL);
		}

		STOPIF( hlp___do_convert(iconv_cd, local_string, utf8_string, len), 
				NULL);
	}

ex:
	return status;
}


/** -.
 * If \a len \c ==-1, a \c strlen() is done.
 * */
int hlp__utf82local(const char *utf8_string, char** local_string, int len)
{
	static iconv_t iconv_cd = NULL;
	int status;

	status=0;
	if (!local_codeset)
		STOPIF( hlp___dummy_convert(utf8_string, local_string, len), NULL);
	else
	{
		/* Get a conversion handle, if not already done. */
		if (!iconv_cd)
		{
			STOPIF( hlp___get_conv_handle( "UTF-8", local_codeset, &iconv_cd),
					NULL);
		}

		STOPIF( hlp___do_convert(iconv_cd, utf8_string, local_string, len),
				NULL);
	}

ex:
	return status;
}
#else
/* For safety return a copy. */
int hlp__local2utf8(const char *local_string, char** utf8_string, int len)
{
	static struct cache_entry_t *c=NULL;

	return cch__entry_set( &c, 0, local_string, len, 0, utf8_string);
}

int hlp__utf82local(const char *utf8_string, char** local_string, int len)
{
	return hlp__local2utf8(utf8_string, local_string, len);
}
#endif


/** Small utility function to copy a system-defined struct \a stat into our
 * own struct \a sstat_t - which is much smaller. */
	inline void __attribute__((always_inline)) 
hlp__copy_stats(struct stat *src, struct sstat_t *dest) 
{
	if (S_ISCHR(src->st_mode) || S_ISBLK(src->st_mode)) 
		dest->rdev=src->st_rdev;
	else
		dest->size=src->st_size;

	dest->mode=src->st_mode;

	dest->dev=src->st_dev;
	dest->ino=src->st_ino;

	dest->uid=src->st_uid;
	dest->gid=src->st_gid;

#ifdef HAVE_STRUCT_STAT_ST_MTIM
	dest->mtim=src->st_mtim;
	dest->ctim=src->st_ctim;
#else
	dest->mtim.tv_sec=src->st_mtime;
	dest->mtim.tv_nsec=0;
	dest->ctim.tv_sec=src->st_ctime;
	dest->ctim.tv_nsec=0;
#endif
}


/** \defgroup stat_wrap Stat()-Wrappers.
 * \ingroup perf
 *
 * These functions wrap the syscalls \a lstat() and \a fstat(), to return
 * the "normalized" 0 for success and an error number otherwise.
 * Furthermore they return their result in a struct \a sstat_t pointer.
 *
 * The copying done in these functions hurts a bit, but the space wasted
 * by the normal struct stat64 hurts much more.
 * GHz are cheap, memory doesn't scale as much. 
 *
 * \note As we want to store symlinks as such, we never use \a stat() calls -
 * these would follow the symlinks and return the wrong meta-data. 
 * */
/** @{ */
/** A wrapper for \a lstat(). */
int hlp__lstat(const char *fn, struct sstat_t *st)
{
	int status;
	struct stat st64;

	status=lstat(fn, &st64);
	if (status == 0) 
	{
		DEBUGP("%s: uid=%llu gid=%llu mode=0%llo dev=0x%llx "
				"ino=%llu rdev=0x%llx size=%llu",
				fn,
				(t_ull)st64.st_uid, (t_ull)st64.st_gid, (t_ull)st64.st_mode, 
				(t_ull)st64.st_dev, (t_ull)st64.st_ino, 
				(t_ull)st64.st_rdev, (t_ull)st64.st_size);

		/* FIFOs or sockets are never interesting; they get filtered out by 
		 * pretending that they don't exist. */
		/* We should return -ENOENT here, so that higher levels can give 
		 * different error messages ... it might be confusing if "fsvs info 
		 * socket" denies some existing entry. */
		if (S_ISFIFO(st64.st_mode) || S_ISSOCK(st64.st_mode) || S_ISDOOR(st64.st_mode))
		{
			st64.st_mode = (st64.st_mode & ~S_IFMT) | S_IFGARBAGE;
			status=-ENOENT;
		}

		if (st)
			hlp__copy_stats(&st64, st);
	}
	else
	{
		status=errno;
		DEBUGP("stat %s: errno=%d", fn, errno);
	}

	return status;
}


/** A wrapper for \a fstat(). */
int hlp__fstat(int fd, struct sstat_t *st)
{
	int status;
	struct stat st64;

	status=fstat(fd, &st64);
	if (status == 0) 
	{
		hlp__copy_stats(&st64, st);
		DEBUGP("fd %d: uid=%d gid=%d mode=%o dev=%llx ino=%llu rdev=%llx size=%llu",
				fd,
				st->uid, st->gid, st->mode, 
				(t_ull)st->dev, (t_ull)st->ino, 
				(t_ull)st->rdev, (t_ull)st->size);
	}
	else
	{
		status=errno;
		DEBUGP("stat #%d: errno=%d", fd, errno);
	}

	return status;
}
/** @} */


/** -.
 * \param dst A target buffer
 * \param len An optional length-saving field
 * \param ... A \c NULL -terminated list of pointers, which will be 
 * concatenated.
 * \return \a dst as target buffer.
 *
 * This works like \c strcpy, but removes sequences like <tt>/./</tt> and 
 * <tt>//</tt>.
 *
 * \warning The buffer \a dst will be \b overwritten and \b has to have 
 * enough space!
 *
 * If the first path has no PATH_SEPARATOR as first character, the 
 * start_path is used as beginning - so that always full paths are 
 * returned.
 *
 * Used for cases where we cannot run \a realpath() (as this follows 
 * symlinks, something we don't want), but need to take care of 
 * some strange pathnames. 
 *
 * \note Not as easy as it sounds - try to do cases like 
 * <tt>a//..//.///b/c//</tt>. 
 *
 * For people reading JoelOnSoftware and related pages - yes, this is
 * cheating, but it works and is fast :-).
 * */
/* As we need to compare up to 4 characters "/../", we use 4 pointers, which
 * usually will point to neighbor characters. 
 * That gives the compares easy access to characters splitted over several
 * arguments. */
char *hlp__pathcopy(char *dst, int *len, ...)
{
	static const char ps[]={ PATH_SEPARATOR, 0 };
	int had_path;
	char *dest;
	const char *src;
	const char *src_1, *src_2, *src_3;
	int status, eop;
	va_list va;
	static const char null=0;


	status=0;
	had_path=0;
	eop=0;
	va_start(va, len);
	/* dest is the working pointer, dst is needed to check for too many .. */
	dest=dst;
	src_1=src_2=src_3=&null;


	void Increment()
	{
		src  =src_1;
		src_1=src_2;
		src_2=src_3;

		/* Next character. */
		if (*src_3) src_3++;

		/* If just prepared character in \c *src_3 is a \c \\0 , 
		 * wrap to next argument. 
		 * (If there are still arguments.) */
		if (!eop)
			while (!*src_3)
			{
				src_3=va_arg(va, char*);
				if (src_3) 
					DEBUGP("adding %s", src_3);
				else
				{ 
					/* End of parameters. Let the pointers point to a valid character
					 * (instead of NULL), so that dereferencing works. */
					eop=1;
					src_3=&null;
					break;
				}
			}
	}


	/* Do first 4 characters */
	Increment();
	if (*src_3 != PATH_SEPARATOR)
	{
		strcpy(dest, start_path);
		dest+=start_path_len;

		/* If we have a PATH_SEPARATOR at the end (eg. we're in /), we have to 
		 * remove it. */
		while (start_path_len>0 && dest[-1]==PATH_SEPARATOR) dest--;

		/* We need to fake a PATH_SEPARATOR into the stream; this bubbles up 
		 * and will be done in the loop.  */
		src_2=ps;
	}
	else
	{
		Increment();
	}
	Increment();
	Increment();


	while (*src)
	{
		if (*src == PATH_SEPARATOR)
		{
			if (!had_path)
				*(dest++)=*src;
			Increment();


			had_path=1;
			/* The next few checks are duplicated in ops__traverse */
			if (*src == '.' && *src_1 == PATH_SEPARATOR)
			{
				/* Simply ignore the ".". The next PATH_SEPARATOR gets ignored by
				 * the next round. */
				Increment();
			}
			else if (*src == '.' && *src_1 == 0)
			{
				/* We've got a "." as last parameter. Remove the last 
				 * PATH_SEPARATOR, and ignore the "." to stop the loop. */
				/* But only if it's not something like "/.", ie. keep the first 
				 * PATH_SEPARATOR.  */
				if (dest-dst > 1) *(--dest)=0;
				Increment();
			}
			else
			{
				if (*src   == '.' && 
						*src_1 == '.' && 
						(*src_2 == PATH_SEPARATOR || *src_2 == 0) )
				{
					Increment();
					Increment();

					/* Remove just written PATH_SEPARATOR - in case of a/b/../ we 
					 * have to remove the b as well.*/
					dest[-1]=0;
					dest=strrchr(dst, PATH_SEPARATOR);
					/* Prevent too many "..". */
					if (!dest) dest=dst;
					/* We re-write the PATH_SEPARATOR, so that following /./ are 
					 * correctly done. */
					had_path=0;
				}
			}
		}
		else
		{
			*(dest++)=*src;
			Increment();
			had_path=0;
		}

#if 0
		/* For debugging: */
		dest[0]=0;
		DEBUGP("solution: %s", dst);
		DEBUGP("next: %s", src);
#endif
	}

	/* Terminate! */
	*dest=0;

	if (len) *len=dest-dst;

	DEBUGP("finished path is %s", dst);

	return dst;
}


/** -.
 *
 * Normally a number; currently a special case is recogniced, namely \c 
 * HEAD .
 *
 * If the parameter \c eos is not \c NULL, it gets set to the character 
 * behind the parsed part, which is ignored.
 * If it is \c NULL, the string must end here. */
int hlp__parse_rev(char *stg, char **eos, svn_revnum_t *rev)
{
	static const char head[]="HEAD";
	int status;
	int inval;
	char *end;

	status=0;
	if (strncasecmp(stg, head, strlen(head)) == 0)
	{
		*rev=SVN_INVALID_REVNUM;
		end=stg+strlen(head);
	}
	else
		*rev=strtoull(stg, &end, 10);

	inval = opt_target_revision == 0;
	if (eos) 
		*eos=end;
	else 
		inval |= (stg == end) || (*end != 0);

	STOPIF_CODE_ERR( inval, EINVAL,
			"The given revision argument '%s' is invalid",
			stg);

ex:
	return status;
}


/** -.
 * Has a few buffers for these operations.
 *
 * The cache is statically allocated, as we cannot return \c ENOMEM.
 *
 * If we cannot store a cache during querying, we'll return the value, but 
 * forget that we already know it.
 *
 * \todo Keep most-used? */
const char *hlp__get_grname(gid_t gid, char *not_found)
{
	struct group *gr;
	static struct cache_t cache = { .max=CACHE_DEFAULT };
	char *str;


	if (cch__find(&cache, gid, NULL, &str, NULL) == 0)
		return *str ? str : not_found;

	gr=getgrgid(gid);

	cch__add(&cache, gid, gr ? gr->gr_name : "", -1, &str);
	return *str ? str : not_found;
}


/** -.
 * Has a few buffers for these operations; the cache is statically 
 * allocated, as we cannot return \c ENOMEM.
 *
 * If we cannot store a cache during querying, we'll return the value, but 
 * forget that we already know it.
 * */
const char *hlp__get_uname(uid_t uid, char *not_found)
{
	struct passwd *pw;
	static struct cache_t cache = { .max=CACHE_DEFAULT };
	char *str;

	if (cch__find(&cache, uid, NULL, &str, NULL) == 0)
		return *str ? str : not_found;

	pw=getpwuid(uid);

	cch__add(&cache, uid, pw ? pw->pw_name : "", -1, &str);
	return *str ? str : not_found;
}


/** -.
 * Uses a simple hash function. */
int hlp__get_uid(char *user, uid_t *uid, apr_pool_t *pool)
{
	static struct cache_t *cache=NULL;
	int status;
	apr_gid_t a_gid;
	/* Needed for 64bit type-conversions. */
	cache_value_t cv;


	STOPIF( cch__new_cache(&cache, 64), NULL);

	if (cch__hash_find(cache, user, &cv) == ENOENT)
	{
		status=apr_uid_get(uid, &a_gid, user, pool);
		if (status)
			status=ENOENT;
		else
		{
			cv=*uid;
			STOPIF( cch__hash_add(cache, user, cv), NULL);
		}
	}
	else
		*uid=(uid_t)cv;

ex:
	return status;
}


/** -.
 * Uses a simple hash function. */
int hlp__get_gid(char *group, gid_t *gid, apr_pool_t *pool)
{
	static struct cache_t *cache=NULL;
	int status;
	/* Needed for 64bit type-conversions. */
	cache_value_t cv;


	STOPIF( cch__new_cache(&cache, 64), NULL);
	if (cch__hash_find(cache, group, &cv) == ENOENT)
	{
		status=apr_gid_get(gid, group, pool);
		if (status)
			status=ENOENT;
		else
		{
			cv=*gid;
			STOPIF( cch__hash_add(cache, group, cv), NULL);
		}
	}
	else
		*gid=(gid_t)cv;

ex:
	return status;
}


#define STRING_LENGTH (4096)
/** -.
 * Returns 0 for success, \c EOF for no more data.
 *
 * Empty lines (only whitespace) are ignored (but counted).
 *
 * If \a no_ws is set, the returned pointer has whitespace at beginning
 * and end removed; \c \\r and \c \\n at the end are always removed.
 *
 * Only a single statically allocated buffer is used, so the line has to be 
 * copied if needed over several invocations.
 *
 * \a eos is set to the last non-whitespace character in the line. */
int hlp__string_from_filep(FILE *input, 
		char **string, char **eos, int flags)
{
	int status;
	static char *buffer=NULL;
	static unsigned linenum;
	char *start;
	int i;


	status=0;

	if (flags & SFF_RESET_LINENUM)
		linenum=0;
	if (flags & SFF_GET_LINENUM)
		return linenum;
	if (!input) goto ex;

	/* We impose a hard limit for simplicities' sake. */
	if (!buffer)
		STOPIF( hlp__alloc( &buffer, STRING_LENGTH), NULL);

	while (1)
	{
		start=NULL;
		linenum++;
		if (!fgets(buffer, STRING_LENGTH, input))
		{
			/* fgets() returns NULL at EOF or on error; feof() 
			 * is (reliably) set only after \c fgets(). */
			if (feof(input)) 
			{
				status=EOF;
				goto ex;
			}

			status=errno;
			goto ex;
		}

		start=buffer;
		if (flags & SFF_WHITESPACE)
			start=hlp__skip_ws(start);

		if ((flags & SFF_COMMENT) && *start == '#') 
			continue;


		i=strlen(start)-1;

		/* Remove the \n at the end. For DOS-CRLF pairs we must use this test 
		 * order. */
		if (i > 0 && start[i] == '\n')
			i--;
		if (i > 0 && start[i] == '\r')
			i--;

		/* Always remove the \r|\n at the end; other whitespace is optionally 
		 * removed.  */
		start[i+1]=0;

		while (i>=0 && isspace(start[i])) i--;
		if (flags & SFF_WHITESPACE)
			/* i is now in [-1 ... ] */
			start[i+1]=0;
		if (eos) *eos=start+i+1;

		if (*start) break;
	}

	if (start)
		DEBUGP("read string %s", start);

	*string = start;

ex:
	return status;
}


/** -.
 * Some special values are recogniced - eg. \c \\r, \c \\n. 
 */
int hlp__safe_print(FILE *output, char *string, int maxlen)
{
	static const char to_encode[32]= {
		'0',   0,   0,   0,   0,   0,   0,   0,
		'b', 't', 'n',   0,   0, 'r',   0,   0,
		'f',   0,   0,   0,   0,   0,   0,   0,
		0,   0,   0,   0,   0,   0,   0,   0,
	};
	int cur;
	int status;

	status=0;
	/* No need to optimize here and change that into a single compare.
	 * After all, we're doing IO. */
	while (status>=0 && maxlen>0)
	{
		/* We could mark the string as "unsigned", but that would collide with many 
		 * other definitions. See linus' post Jan. 2007 on linux-kernel about the 
		 * "braindead compiler". */
		cur=(*string) & 0xff;
		string++;
		maxlen--;

		if (cur == 0x7f)
		{
			STOPIF_CODE_EPIPE( fputs("\\x7f", output), NULL);
			continue;
		}

		/* Thanks to UTF-8, we need to take care only for the control characters.
		 * The things above 0x80 are needed. */ 
		if (cur<sizeof(to_encode) || !isprint(cur))
		{
			STOPIF_CODE_EPIPE( 
					to_encode[cur] ? 
					fprintf(output, "\\%c", to_encode[cur]) :
					fprintf(output, "\\x%02x", cur), NULL);
			continue;
		}

		/* Normal (or at least unfiltered) character. */
		STOPIF_CODE_EPIPE( fputc(cur, output), NULL);
	}

	status=0;

ex:
	return status;
}


/** select() loop for the encoder pipes.
 *
 * As we can always get/put data from/to the associated stream, the only 
 * rate limiter is the child process - how fast it can take/give data.
 *
 * So we have to poll. */
int hlp___encoder_waiter(struct encoder_t *encoder)
{
	int status;
	/* The names look reversed, because the pipe_* name is how the child sees 
	 * it.  */
	struct pollfd poll_data;

	if (encoder->is_writer)
	{
		poll_data.events=POLLOUT;
		poll_data.fd=encoder->pipe_in;
	}
	else
	{
		poll_data.events=POLLIN;
		poll_data.fd=encoder->pipe_out;
	}

	status=0;
	/* We cannot wait indefinitely, because we don't get any close event yet.  
	 * */
	STOPIF_CODE_ERR( poll(&poll_data, 1, 100) == -1, errno,
			"Error polling for data");

ex:
	return status;
}


/** Writer function for an encoder.
 * We have to write the full buffer through the pipe before returning - 
 * possibly in some chunks.
 * */ 
svn_error_t *hlp___encode_write(void *baton,
		const char *data, apr_size_t *len)
{
	int status;
	svn_error_t *status_svn;
	int write_pos, bytes_left;
	struct encoder_t *encoder=baton;
	apr_size_t wlen;


	status=0;
	write_pos=0;
	bytes_left= len ? *len : 0;
	/* If we get data==NULL, we go on until we get an eof. */
	while (data ? (bytes_left || encoder->bytes_left) : !encoder->eof)
	{
		/* Try to give the child process some data. */
		if (bytes_left)
		{
			status=send(encoder->pipe_in, data+write_pos, 
					bytes_left,
					MSG_DONTWAIT);

			DEBUGP("sending %d bytes to child %d from %d: %d; %d", 
					bytes_left, encoder->child, write_pos, status, errno);

			if (status == -1)
			{
				status=errno;
				if (status == EAGAIN)
				{
					/* Just wait, we might be able to send that later. */
				}
				else
					STOPIF(status, "Error writing to child");
			}
			else
			{
				/* Data sent. */
				write_pos+=status;
				bytes_left-=status;
				status=0;
				DEBUGP("%d bytes left", bytes_left);
			}
		}

		/* --- Meanwhile the child process processes the data --- */

		if (encoder->pipe_out != -1)
		{
			/* Try to read some data. */
			status = recv(encoder->pipe_out, encoder->buffer,
					sizeof(encoder->buffer), MSG_DONTWAIT);
			DEBUGP("receiving bytes from child %d: %d; %d", 
					encoder->child, status, errno);
			if (status==0)
			{
				STOPIF_CODE_ERR( close(encoder->pipe_out) == -1, errno,
						"Cannot close connection to child");
				DEBUGP("child %d finished", encoder->child);
				encoder->pipe_out=EOF;
				encoder->eof=1;
			}
			else
			{
				if (status == -1)
				{
					status=errno;
					if (status == EAGAIN)
					{
						/* Just wait, we might get data later. */
						status=0;
					}
					else
						STOPIF(status, "Error reading from child");
				}
				else
				{
					apr_md5_update(&encoder->md5_ctx, encoder->buffer, status);
					encoder->bytes_left=status;
					encoder->data_pos=0;
					/* No error, got so many bytes ... Send 'em on. */
					status=0;
				}
			}
		}

		if (encoder->bytes_left)
		{
			wlen=encoder->bytes_left;
			STOPIF_SVNERR( svn_stream_write,
					(encoder->orig, encoder->buffer + encoder->data_pos, &wlen));

			encoder->data_pos+=wlen;
			encoder->bytes_left-=wlen;
		}

		STOPIF( hlp___encoder_waiter(encoder), NULL);
	}

	/* *len is not changed - we wrote the full data. */

ex:
	RETURN_SVNERR(status);
}


/** Reader function for an encoder.
 * A \c svn_stream_t reader gets as much data as it requested; a short read 
 * would be interpreted as EOF.
 * */ 
svn_error_t *hlp___encode_read(void *baton,
		char *data, apr_size_t *len)
{
	int status;
	svn_error_t *status_svn;
	int read_pos, bytes_left;
	struct encoder_t *encoder=baton;
	int ign_count;


	status=0;
	ign_count=1;
	read_pos=0;
	bytes_left=*len;
	while (bytes_left && !encoder->eof)
	{
		/* No more data buffered? */
		if (!encoder->bytes_left && encoder->orig)
		{
			encoder->data_pos=0;
			encoder->bytes_left=sizeof(encoder->buffer);

			STOPIF_SVNERR( svn_stream_read, 
					(encoder->orig, encoder->buffer, &(encoder->bytes_left)) );

			DEBUGP("read %llu bytes from stream", (t_ull)encoder->bytes_left);
			if (encoder->bytes_left < sizeof(encoder->buffer))
			{
				STOPIF_SVNERR( svn_stream_close, (encoder->orig) );
				encoder->orig=NULL;
			}
		}

		/* Try to give the child process some data. */
		if (encoder->bytes_left)
		{
			status=send(encoder->pipe_in, encoder->buffer+encoder->data_pos,
					encoder->bytes_left,
					MSG_DONTWAIT);

			DEBUGP("sending %llu bytes to child %d from %d: %d; %d", 
					(t_ull)encoder->bytes_left, 
					encoder->child, encoder->data_pos, status, errno);

			if (status == -1)
			{
				status=errno;
				if (status == EAGAIN)
				{
					/* Just wait, we might be able to send that later. */
				}
				else
					STOPIF(status, "Error writing to child");
			}
			else
			{
				/* Data sent. */
				apr_md5_update(&encoder->md5_ctx, 
						encoder->buffer+encoder->data_pos, 
						status);
				encoder->data_pos+=status;
				encoder->bytes_left-=status;
				status=0;
				DEBUGP("%llu bytes left", (t_ull)encoder->bytes_left);
			}
		}

		if (encoder->bytes_left == 0 && !encoder->orig && 
				encoder->pipe_in != -1)
		{
			DEBUGP("closing connection");
			STOPIF_CODE_ERR( close(encoder->pipe_in) == -1, errno,
					"Cannot close connection to child");
			encoder->pipe_in=-1;
		}

		/* --- Meanwhile the child process processes the data --- */

		/* Try to read some data. */
		status = recv(encoder->pipe_out, data+read_pos,
				bytes_left, MSG_DONTWAIT);
		if (status==-1 && errno==EAGAIN && ign_count>0)
			ign_count--;
		else
			DEBUGP("receiving %d bytes from child %d: errno=%d", 
					status, encoder->child, errno);
		if (status==0)
		{
			encoder->eof=1;
			STOPIF_CODE_ERR( close(encoder->pipe_out) == -1, errno,
					"Cannot close connection to child");
		}
		else
		{
			if (status == -1)
			{
				status=errno;
				if (status == EAGAIN)
				{
					if (ign_count == 0) ign_count=20;
					/* Just wait, we might get data later. */
				}
				else
					STOPIF(status, "Error reading from child");
			}
			else
			{
				/* No error, got so many bytes ... */
				read_pos+=status;
				bytes_left-=status;
				status=0;
			}
		}

		STOPIF( hlp___encoder_waiter(encoder), NULL);
	}

	*len=read_pos;

ex:
	RETURN_SVNERR(status);
}


svn_error_t *hlp___encode_close(void *baton)
{
	int status;
	svn_error_t *status_svn;
	int retval;
	struct encoder_t *encoder=baton;
	md5_digest_t md5;


	DEBUGP("closing connections for %d", encoder->child);

	if (encoder->is_writer && encoder->pipe_in!=EOF)
	{
		/* We close STDIN of the child, and wait until there's no more data 
		 * left. Then we close STDOUT. */
		STOPIF_CODE_ERR( close(encoder->pipe_in) == -1, errno,
				"Cannot close connection to child");
		encoder->pipe_in=EOF;

		STOPIF_SVNERR( hlp___encode_write, (baton, NULL, NULL));
		STOPIF_SVNERR( svn_stream_close, (encoder->orig) );
	}

	status=waitpid(encoder->child, &retval, 0);
	DEBUGP("child %d gave %d - %X", encoder->child, status, retval);
	STOPIF_CODE_ERR(status == -1, errno,
			"Waiting for child process failed");
	/* waitpid() returns the child pid */
	status=0;

	apr_md5_final(md5, &encoder->md5_ctx);
	if (encoder->output_md5)
		memcpy(encoder->output_md5, md5, sizeof(*encoder->output_md5));
	DEBUGP("encode end gives MD5 of %s", cs__md5tohex_buffered(md5));

	STOPIF_CODE_ERR(retval != 0, ECHILD, 
			"Child process returned 0x%X", retval);

ex:
	IF_FREE(encoder);

	RETURN_SVNERR(status);
}


/** Helper function.
 * Could be marked \c noreturn. */
void hlp___encode_filter_child(int pipe_in[2], int pipe_out[2],
	 const char *path, const char *command)
{
	int status, i;

	/* The symmetry of the sockets makes it a bit easier - it doesn't matter 
	 * which handle we take. */
	STOPIF_CODE_ERR( dup2(pipe_in[1], STDIN_FILENO) == -1 ||
			dup2(pipe_out[1], STDOUT_FILENO) == -1,
			errno, "Cannot dup2() the childhandles");

	/* Now we may not give any more debug information to STDOUT - it would 
	 * get read as data from the encoder! */

	/* Close known filehandles. */
	STOPIF_CODE_ERR( ( close(pipe_in[0]) |
				close(pipe_out[0]) | 
				close(pipe_in[1]) | 
				close(pipe_out[1]) ) == -1, 
			errno, "Cannot close the pipes");


	/* Try to close other filehandles, like a connection to the repository or 
	 * similar. They should not be kept for the exec()ed process, as 
	 * (hopefully) nobody did a fcntl(FD_CLOEXEC) on them, but better to be 
	 * sure. */
	/* There are other constants than FD_SETSIZE, but they should do the 
	 * same. */
	/* We start from 3 - STDIN, STDOUT and STDERR should be preserved. */
	for(i=3; i<FD_SETSIZE; i++)
		/* No error checking. */
		close(i);


	/* \todo: do we have to do an UTF8-to-local transformation?
	 * We normally have that as a property in the repository, so it's UTF8.
	 * But we use it locally with another encoding (perhaps?), so it should get 
	 * translated?
	 * If someone uses non-ASCII-characters in this command, he gets what he 
	 * deserves - a chance to write a patch. */

	/* \todo: possibly substitute some things in command, like filename or 
	 * similar. */

	if (path[0] == '.' && path[1] == PATH_SEPARATOR) 
		path+=2;
	setenv(FSVS_EXP_CURR_ENTRY, path, 1);

	/* We could do a system in the parent process, but then we'd have to 
	 * juggle the filedescriptors around. Better to use a childprocess, where 
	 * it soon doesn't matter. */
	/* Calling '/bin/sh -c "..."' makes a normal shell with a childprocess;
	 * we get the same with system(). Sadly it doesn't exec() over the shell 
	 * process itself. */
	/* We cannot use status, because that's overwritten by the macros 
	 * below. */
	i=system(command);

	STOPIF_CODE_ERR(i == -1, errno, 
			"Count not execute the command '%s'", command);
	STOPIF_CODE_ERR(WIFSIGNALED(i), ECANCELED,
			"The command '%s' got killed by signal %d",
			command, WTERMSIG(i));
	STOPIF_CODE_ERR(WEXITSTATUS(i), ECANCELED,
			"The command '%s' returned an errorcode %u",
			command, WEXITSTATUS(i));
	STOPIF_CODE_ERR(!WIFEXITED(i), ECANCELED,
			"The command '%s' didn't exit normally", command);

	/* Don't do parent cleanups, just quit. */
	_exit(0);
ex:
	_exit(1);
}


/** -.
 * For \a is_writer data gets written into \a s_stream, gets piped as \c 
 * STDIN into \a command, and the resulting \c STDOUT can be read back with 
 * \a output; if \a is_writer is zero, data must be read from \a output to  
 * get processed.
 *
 * After finishing the returned pointer \a *encoder_out can be used to 
 * retrieve the MD5. 
 * 
 * \a *encoder_out must not be \c free()d; delta editors that store data 
 * locally get driven by the subversion libraries, and the only place where 
 * we can be sure that it's no longer in use is when the streams get 
 * closed. Therefore the \c free()ing has to be done in 
 * hlp___encode_close(); callers that want to get the final MD5 can set the 
 * \c encoder->output_md5 pointer to the destination address.
 * */
int hlp__encode_filter(svn_stream_t *s_stream, const char *command, 
		int is_writer,
		char *path,
		svn_stream_t **output, 
		struct encoder_t **encoder_out,
		apr_pool_t *pool) 
{
	int status;
	svn_stream_t *new_str;
	struct encoder_t *encoder;
	int pipe_in[2];
	int pipe_out[2];


	DEBUGP("encode filter: %s", command);
	status=0;

	STOPIF( hlp__alloc( &encoder, sizeof(*encoder)), NULL);

	new_str=svn_stream_create(encoder, pool);
	STOPIF_ENOMEM( !new_str);

	svn_stream_set_read(new_str, hlp___encode_read);
	svn_stream_set_write(new_str, hlp___encode_write);
	svn_stream_set_close(new_str, hlp___encode_close);


	/* We use a socketpair and not a normal pipe because on a socket we can try 
	 * to change the in-kernel buffer, possibly up to some hundred MB - which 
	 * is not possible with a pipe (limited to 4kB). */
	STOPIF_CODE_ERR( 
			socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_in) == -1 ||
			socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_out) == -1, 
			errno, "Cannot create a socket pair");

	/* So that the child doesn't have any data cached: */
	fflush(NULL);

	encoder->child=fork();
	if (encoder->child == 0)
		hlp___encode_filter_child(pipe_in, pipe_out, path, command);


	/* Parent continues. */
	STOPIF_CODE_ERR(encoder->child == -1, errno, "Cannot fork()");

	STOPIF_CODE_ERR( ( close(pipe_in[1]) | close(pipe_out[1]) ) == -1, 
			errno, "Cannot close the pipes");

	encoder->pipe_in=pipe_in[0];
	encoder->pipe_out=pipe_out[0];

	encoder->orig=s_stream;
	encoder->bytes_left=0;
	encoder->eof=0;
	encoder->is_writer=is_writer;
	encoder->output_md5=NULL;
	apr_md5_init(& encoder->md5_ctx);

	*encoder_out = encoder;

	*output=new_str;
ex:
	return status;
}


/** Checks for the needed environment variables, and does the chroot()ing 
 * if necessary.
 * See \ref howto_chroot. */
int hlp__chrooter(void)
{
	int status;
	char *libs, *root, *cwd;
	int fd, len;
	static const char delim[]=" \r\n\t\f";
	static const char so_pre[]="lib";
	static const char so_post[]=".so";
	void *hdl;
	char filename[128];


	libs=getenv(CHROOTER_LIBS_ENV);
	DEBUGP("Libraries to load: %s", libs);

	root=getenv(CHROOTER_ROOT_ENV);
	DEBUGP("fd of old root: %s", root);

	cwd=getenv(CHROOTER_CWD_ENV);
	DEBUGP("fd of old cwd: %s", cwd);

	/* If none are set, there's no need for the chroot. Just return. */
	status = (libs ? 1 : 0) | (root ? 2 : 0) | (cwd ? 4 : 0);
	if (status == 0) 
	{
		DEBUGP("All are empty, just return.");
		goto ex;
	}

	/* Only one not set? */
	STOPIF_CODE_ERR(status != 7, EINVAL,
			"All of %s, %s and %s must be set!", 
			CHROOTER_LIBS_ENV, 
			CHROOTER_CWD_ENV, 
			CHROOTER_ROOT_ENV);
	status=0;

	strcpy(filename, so_pre);

	/* Load libraries */
	libs=strtok(libs, delim);
	while (libs && *libs)
	{
		DEBUGP("Trying library %s", libs);

		hdl=dlopen(libs, RTLD_NOW | RTLD_GLOBAL);
		if (hdl == NULL)
		{
			len=strlen(libs);
			if (sizeof(filename) < 
					(len + strlen(so_pre) + strlen(so_post) +2))
				DEBUGP("Library name %s too long for expansion", libs);
			else
			{
				strcpy(filename+strlen(so_pre), libs);
				strcpy(filename+strlen(so_pre)+len, so_post);
				/* 2nd try */
				hdl=dlopen(filename, RTLD_NOW | RTLD_GLOBAL);
			}
			/* We allow to specify only "m" for "libm.so". */
		}
		STOPIF_CODE_ERR( hdl == NULL, errno, 
				"Cannot load library %s", libs);

		libs=strtok(NULL, delim);
	}

	/* Load message lists */
	strerror(EINVAL);

	/* Locale data */
	iconv_open("437","850");

	/* Load DNS libraries. */
	gethostbyname("localhost");
	//gethostbyname("Does.surely.not.exist.invalid");


	/* Back to the root(s) :-) */
	fd=atoi(root);
	STOPIF_CODE_ERR( fchdir(fd) == -1, errno,
			"Cannot fchdir() on handle %d", fd);
	/* We ignore errors here, and on the close below. */
	close(fd);

	STOPIF_CODE_ERR( chroot(".") == -1, errno,
			"Cannot chroot() back");

	/* Go to the remembered wd */
	fd=atoi(cwd);
	STOPIF_CODE_ERR( fchdir(fd) == -1, errno,
			"Cannot fchdir() on handle %d", fd);
	close(fd);

ex:
	return status;
}


/** Is the given environment valid for substitution?
 * Returns a boolean - 0 means not valid, everything else ok.
 *
 * It is valid, if
 * - The name starts with \c WC
 * - The first \a p2c_len characters of \a path2cmp match with the value.
 *   If \c p2c_len equals \c -1 the length of the path in the environment 
 *   variable is taken.
 *
 * If \a value_len is not \c NULL, it get set to the length of the path in 
 * the environment variable.
 * */
inline int hlp___is_valid_env(char *env, 
		char *path2cmp, int p2c_len,
		char **value, int *value_len)
{
	char *cp;
	int x;


	*value=NULL;
	cp=strchr(env, '=');
	if (!cp) return 0;

	/* Skip '=' */
	cp++;
	*value=cp;

	x=strlen(cp);
	/* Remove PATH_SEPARATORs at the end */
	while (x>0 && cp[x-1]==PATH_SEPARATOR) x--;

	if (value_len) *value_len=x;
	if (p2c_len == -1) p2c_len=x;

	/* The name must start with "WC"; */
	return env[0]=='W' && env[1] == 'C' &&
		/* and the value must match the given path. */
		strncmp(cp, path2cmp, p2c_len) == 0;
}


/** Here we simply (try to) match the (few) environment variables against 
 * the entries, and set pointers for matching paths; so for displaying them  
 * it suffices to walk the tree up until an entry with \c arg set is found. 
 * */
int hlp__match_path_envs(struct estat *root)
{
	int status;
	char **env;
	char *cp;
	struct estat *sts;
	int len;


	status=0;
	for(env=environ; *env; env++)
	{
		DEBUGP("test env %s", *env);
		/* The path in the environment variable must have at least the working 
		 * copy path. */
		if (hlp___is_valid_env(*env, wc_path, wc_path_len, &cp, &len))
		{
			/* The length cannot be smaller; if it's equal, it's the root entry.  
			 * */
			if (len == wc_path_len)
				sts=root;
			else
			{
				/* Find entry. The +1 is the PATH_SEPARATOR. */
				status=ops__traverse(root, cp+wc_path_len+1, 0, 0, &sts);
				if (status)
				{
					DEBUGP("no match: %s", *env);
					continue;
				}
			}

			/* It matches. */
			len=(cp-*env)-1;

			/* We could use hlp__strnalloc() here; but then we'd have to copy 
			 * from *env-1, which *should* be save, as that is normally allocated 
			 * on the top of the (mostly downgrowing) stack.
			 * Be conservative. */
			STOPIF( hlp__alloc( &sts->arg, 1+len+1+3), NULL);

			/* \todo DOS-compatible as %env% ? */
			sts->arg[0]=ENVIRONMENT_START;
			memcpy(sts->arg+1, *env, len);
			sts->arg[1+len]=0;

			DEBUGP("match: %s gets %s", sts->name, sts->arg);
		}
	}

	/* Ignore previous ENOENT and similar. */
	status=0;
ex:
	return status;
}


/** Can be in several formats; see \ref o_opt_path.
 *
 * \todo Build the \c wc_relative_path only if necessary - remove the 
 * parameter from the caller chains. */
int hlp__format_path(struct estat *sts, char *wc_relative_path,
		char **output)
{
	int status;
	static struct cache_entry_t *cache=NULL;
	char *path, **env, *cp, *match;
	struct estat *parent_with_arg;
	static const char ps[2]= { PATH_SEPARATOR, 0};
	int len, sts_rel_len, max_len;


	status=0;
	switch (opt__get_int(OPT__PATH))
	{
		case PATH_WCRELATIVE:
			path=wc_relative_path;
			break;

		case PATH_CACHEDENVIRON:
		case PATH_PARMRELATIVE:
			parent_with_arg=sts;
			while (parent_with_arg->parent && !parent_with_arg->arg) 
			{
				//				DEBUGP("no arg: %s", parent_with_arg->name);
				parent_with_arg=parent_with_arg->parent;
			}

			/* If we got out of the loop, but there's no ->arg, we must be at the 
			 * root (because ! ->parent is the other condition).
			 * The root is always the wc_path, so set it as default ... */
			/** \todo We should set it beginning from a command line parameter, 
			 * if we have one. Preferably the nearest one ... */
			if (!parent_with_arg->arg) parent_with_arg->arg=wc_path;


			len=strlen(parent_with_arg->arg);
			sts_rel_len=sts->path_len - parent_with_arg->path_len;

			/* If there was no parameter, and we're standing at the WC root, we 
			 * would have no data to print.
			 * (We cannot set "." as argument for the root entry, because all 
			 * other entries would inherit that - we'd get "./file", just like 
			 * before. */
			if (len == 0 && sts_rel_len == 0) 
			{
				path=".";
				break;
			}


			DEBUGP("parent=%s, has %s; len=%d, rel_len=%d", 
					parent_with_arg->name, parent_with_arg->arg, len, sts_rel_len);
			/* Maybe we should cache the last \c parent_with_arg and \c pwa_len. */
			STOPIF( cch__entry_set(&cache, 0, NULL, 
						len + 1 + sts_rel_len + 3,
						0, &path), NULL);

			/* We cannot use hlp__pathcopy(), as that would remove things like 
			 * ./.././, which the user possibly wants.
			 * Use the parameter as given; only avoid putting a superfluous / at 
			 * the end.  */
			memcpy(path, parent_with_arg->arg, len);

			/* If we had an parameter (so the PATH_SEPARATOR won't be the first 
			 * character), and the last character of the parameter isn't already a 
			 * PATH_SEPARATOR, and we have to append some path (ie. our current 
			 * element isn't already finished [because it was directly given]), 
			 * we set a PATH_SEPARATOR. */
			if (len>0 && 
					path[len-1] != PATH_SEPARATOR &&
					parent_with_arg != sts) 
				path[len++]=PATH_SEPARATOR;

			memcpy(path+len, 
					wc_relative_path+parent_with_arg->path_len+1, 
					sts_rel_len);
			path[len+sts_rel_len]=0;
			break;

		case PATH_ABSOLUTE:
		case PATH_FULLENVIRON:
			STOPIF( cch__entry_set(&cache, 0, NULL, 
						wc_path_len + 1 + sts->path_len + 1, 0, &path), NULL);
			hlp__pathcopy(path, NULL, wc_path, ps, wc_relative_path, NULL);

			if (opt__get_int(OPT__PATH) == PATH_ABSOLUTE) break;

			/* Substitute some environment.
			 * \todo It would be better to cache already matched environment 
			 * variables in the corresponding struct estats (eg. in the arg 
			 * variable); then we could match like above in the case 
			 * PATH_PARMRELATIVE.
			 * The problem is that this cannot be done in a single point in time 
			 * (although just before waa__partial_update() looks promising) - 
			 * because a major directory tree might be new, and this will only be 
			 * found during processing the new items.
			 *
			 * So there are two ways:
			 * - Looping *once* through the environment, after waa__input_tree()
			 *   - Adv: Done once, is fast.
			 *   - Disadv: May miss some opportunities for matching.
			 * - Doing that for every path
			 *   - Disadv: Slow, because it has to be tried *every* time
			 *   - Adv: Matches as much as possible
			 *
			 * Maybe there should simply be a choise between PATH_ENVIRON_FAST 
			 * and PATH_ENVIRON_FULL. */
			match=NULL;
			/* We need at least a single character to substitute. */
			max_len=1;
			for(env=environ; *env; env++)
			{
				if (!hlp___is_valid_env(*env, path, -1, &cp, &len)) 
					continue;

				if (len > max_len && 
						path[len]==PATH_SEPARATOR)
				{
					match=*env;
					max_len=len;
				}
			}

			if (match)
			{
				DEBUGP("matched %s", match);
				cp=strchr(match, '=');
				/* If the environment variable has a longer name than the path, we 
				 * don't substitute. */
				if (max_len > cp-match+1)
				{
					/* length of environment variable name */
					len=cp-match;

					/* \todo DOS-compatible as %env% ? */
					*path=ENVIRONMENT_START;
					memcpy(path+1, match, len);
					path[1+ len]=PATH_SEPARATOR;

					/* debug: */
					path[1+ len+1]=0;
					DEBUGP("path=%s, rest=%s; have %d, sts has %d", 
							path, path+max_len+1,
							max_len, sts->path_len);

					memmove(path+1+len, 
							path+max_len,
							wc_path_len+sts->path_len - max_len);
				}
			}

			break;

		default:
			BUG_ON(1);
	}

	*output=path;
ex:
	return status;
}


/** -. 
 *
 * Can be non-numeric, like \c HEAD. */
char *hlp__rev_to_string(svn_revnum_t rev)
{
	static int last=0;
	/* Sadly GCC doesn't statically solve sizeof(rev)*log(10)/log(2) ... */
	static char buffers[2][(int)(sizeof(rev)*4)+3];

	last++;
	if (last>= sizeof(buffers)/sizeof(buffers[0])) last=0;

	if (rev == SVN_INVALID_REVNUM)
		strcpy(buffers[last], "HEAD");
	else 
	{
		BUG_ON(rev < 0);
		sprintf(buffers[last], "%llu", (t_ull)rev);
	}

	return buffers[last];
}


/** -.
 * If \a max<0, the comparision is done until the \c \\0.
 * \a max is the maximum number of characters to compare; the result is 
 * always \c equal (\c ==0), if \c max==0.
 * 
 * Not useable for lesser/greater compares. */
int hlp__strncmp_uline_eq_dash(char *always_ul, char *other, int max)
{
	while (max)
	{
		/* Nicer than the negation. */
		if (*always_ul == *other || 
				(*always_ul == '_' && *other == '-')) 
			;
		else
			/* Different. */
			return 1;

		/* We need not check for *other==0, because they must be equal 
		 * according the above comparision.
		 * We must check afterwards, because the \0 must be compared, too. */
		if (max<0 && *always_ul==0) break;

		if (max > 0) max--;
		always_ul++;
		other++;
	}

	return 0;
}


/** -.
 * */
int hlp__is_special_property_name(const char *name)
{
	static const char prop_pre_toignore[]="svn:entry";
	static const char prop_pre_toignore2[]="svn:wc:";

	if (strncmp(name, prop_pre_toignore, strlen(prop_pre_toignore)) == 0 ||
			strncmp(name, prop_pre_toignore2, strlen(prop_pre_toignore2)) == 0)
		return 1;
	return 0;
}


/** -.
 * \a md5, if not \c NULL, must point to at least MD5_DIGEST_LENGTH bytes.  
 * */
int hlp__stream_md5(svn_stream_t *stream, unsigned char md5[APR_MD5_DIGESTSIZE])
{
	int status;
	svn_error_t *status_svn;
	const int buffer_size=16384;
	char *buffer;
	apr_size_t len;
	apr_md5_ctx_t md5_ctx;


	status=0;
	STOPIF( hlp__alloc( &buffer, buffer_size), NULL);

	if (md5)
		apr_md5_init(&md5_ctx);
	DEBUGP("doing stream md5");

	len=buffer_size;
	while (len == buffer_size)
	{
		STOPIF_SVNERR( svn_stream_read, (stream, buffer, &len));
		if (md5)
			apr_md5_update(&md5_ctx, buffer, len);
	}

	if (md5)
		apr_md5_final(md5, &md5_ctx);

ex:
	return status;
}


/** Delays execution until the next second.
 * Needed because of filesystem granularities; FSVS only stores seconds, 
 * not more. */
int hlp__delay(time_t start, enum opt__delay_e which)
{
	if (opt__get_int(OPT__DELAY) & which)
	{
		DEBUGP("waiting ...");
		if (!start) start=time(NULL);

		/* We delay with 25ms accuracy. */
		while (time(NULL) <= start)
			usleep(25000);
	}

	return 0;
}


/** -.
 * We could either generate a name ourself, or just use this function - and 
 * have in mind that we open and close a file, just to overwrite it 
 * immediately.
 *
 * But by using that function we get the behaviour that 
 * subversion users already know. */
int hlp__rename_to_unique(char *fn, char *extension, 
		const char **unique_name, 
		apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;
	apr_file_t *tmp_f;


	/* Thank you, subversion 1.6.4. "Path not canonical" - pfft. */
	if (fn[0] == '.' && fn[1] == PATH_SEPARATOR)
		fn+=2;

	STOPIF_SVNERR( svn_io_open_unique_file2,
			(&tmp_f, unique_name, fn, extension, 
			 svn_io_file_del_on_close, pool));
	STOPIF( apr_file_close(tmp_f), NULL);

	DEBUGP("got unique name for local file: %s", *unique_name);

	if (rename(fn, *unique_name) == -1)
	{
		/* Remember error. */
		status=errno;
		DEBUGP("renaming %s to %s gives an error %d.", 
				fn, *unique_name, status);

		/* The rename() should kill the file.
		 * But if it fails, we'd keep that file here - and that's not 
		 * ncessary. */
		if (unlink(*unique_name) == -1)
			/* On error just put a debug message - we'll fail either way. */
			DEBUGP("Cannot unlink %s: %d", *unique_name, errno);

		STOPIF(status, 
				"Cannot rename local file to unique name %s", 
				*unique_name);
	}

ex:
	return status;
}


/** -.
 * Caches the result, so that the configuration is only fetched a single time.
 */
int hlp__get_svn_config(apr_hash_t **config)
{
	int status;
	svn_error_t *status_svn;
	static apr_hash_t *cfg=NULL;
	char *cp;
	int len;


	status=0;
	/* We assume that a config hash as NULL will never be returned.
	 * (Else we'd try to fetch it more than once.) */
	if (!cfg)
	{
		/* Subversion doesn't like "//" in pathnames - even if it's just the 
		 * local configuration area. So we have to normalize them. */
		len = opt__get_int(OPT__CONFIG_DIR)==0 ?
			opt__get_int(OPT__CONF_PATH)+strlen(DEFAULT_CONFIGDIR_SUB)+1 :
			opt__get_int(OPT__CONFIG_DIR);
		STOPIF( hlp__alloc( &cp, len), NULL);

		if (opt__get_int(OPT__CONFIG_DIR)==0)
			hlp__pathcopy(cp, &len, 
					opt__get_string(OPT__CONF_PATH), DEFAULT_CONFIGDIR_SUB, NULL);
		else
			hlp__pathcopy(cp, &len, 
					opt__get_string(OPT__CONFIG_DIR), NULL);

		opt__set_string(OPT__CONFIG_DIR, PRIO_MUSTHAVE, cp);
		opt__set_int(OPT__CONFIG_DIR, PRIO_MUSTHAVE, len);


		STOPIF_SVNERR( svn_config_get_config,
				(&cfg, opt__get_string(OPT__CONFIG_DIR), global_pool));
		DEBUGP("reading config from %s", opt__get_string(OPT__CONFIG_DIR));
	}

	*config=cfg;
ex:
	return status;
}


/** -.
 * If \a source is not \c NULL \a len bytes are copied.
 * The buffer is \b always \c \\0 terminated. */
int hlp__strnalloc(int len, char **dest, const char const *source)
{
	int status;

	STOPIF( hlp__alloc( dest, len+1), NULL);

	if (source)
		memcpy(*dest, source, len);
	(*dest)[len]=0;

ex:
	return status;
}

/** -. */
int hlp__strmnalloc(int len, char **dest, 
		const char const *source, ...)
{
	int status;
	va_list vl;
	char *dst;


	/* We don't copy now, because we want to know the end of the string 
	 * anyway. */
	STOPIF( hlp__alloc( dest, len), NULL);

	va_start(vl, source);
	dst=*dest;

	while (source)
	{
		while (1)
		{
			BUG_ON(len<=0);
			if (! (*dst=*source) ) break;
			source++, dst++, len--;
		}

		source=va_arg(vl, char*);
	}

ex:
	return status;
}


/** -.
 * That is not defined in terms of \c hlp__alloc(), because glibc might do 
 * some magic to get automagically 0-initialized memory (like mapping \c 
 * /dev/zero). */
int hlp__calloc(void *output, size_t nmemb, size_t count)
{
	int status;
	void **tgt=output;

	status=0;
	*tgt=calloc(nmemb, count);
	STOPIF_CODE_ERR(!*tgt, ENOMEM,
			"calloc(%llu, %llu) failed", (t_ull)nmemb, (t_ull)count);

ex:
	return status;
}


/** -. */
int hlp__realloc(void *output, size_t size)
{
	int status;
	void **tgt;

	status=0;
	tgt=output;
	*tgt=realloc(*tgt, size);
	/* Allocation of 0 bytes might (legitimately) return NULL. */
	STOPIF_CODE_ERR(!*tgt && size, ENOMEM,
			"(re)alloc(%llu) failed", (t_ull)size);

ex:
	return status;
}


/** -. */
char* hlp__get_word(char *input, char **word_start)
{
	input=hlp__skip_ws(input);
	if (word_start)
		*word_start=input;

	while (*input && !isspace(*input)) input++;
	return input;
}



#ifndef HAVE_STRSEP
/** -.
 * Copyright (C) 2004, 2007 Free Software Foundation, Inc.
 * Written by Yoann Vandoorselaere <yoann@prelude-ids.org>.
 * Taken from http://www.koders.com/c/fid4F16A5D73313ADA4FFFEEBA99BE639FEC82DD20D.aspx?s=md5 */
char * strsep (char **stringp, const char *delim)
{
	char *start = *stringp;
	char *ptr;

	if (start == NULL)
		return NULL;

	/* Optimize the case of no delimiters.  */
	if (delim[0] == '\0')
	{
		*stringp = NULL;
		return start;
	}

	/* Optimize the case of one delimiter.  */
	if (delim[1] == '\0')
		ptr = strchr (start, delim[0]);
	else
		/* The general case.  */
		ptr = strpbrk (start, delim);
	if (ptr == NULL)
	{
		*stringp = NULL;
		return start;
	}

	*ptr = '\0';
	*stringp = ptr + 1;

	return start;
}

#endif


int hlp__compare_string_pointers(const void *a, const void *b)
{
	const char * const *c=a;
	const char * const *d=b;
	return strcoll(*c,*d);
}


int hlp__only_dir_mtime_changed(struct estat *sts) 
{
	int st;

	st = sts->entry_status;

	return opt__get_int(OPT__DIR_EXCLUDE_MTIME) && 
		S_ISDIR(sts->st.mode) && (!(st & FS_CHILD_CHANGED)) &&
		(st & FS__CHANGE_MASK) == FS_META_MTIME;
}


