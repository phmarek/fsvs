/************************************************************************
 * Copyright (C) 2005-2009,2015 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <apr_pools.h>
#include <apr_md5.h>
#include <subversion-1/svn_md5.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>
#include <sys/mman.h>


#include "waa.h"
#include "interface.h"
#include "direnum.h"
#include "options.h"
#include "add_unvers.h"
#include "cache.h"
#include "checksum.h"
#include "helper.h"
#include "global.h"
#include "status.h"
#include "est_ops.h"
#include "ignore.h"
#include "actions.h"


/** \file
 * Handling of multiple <tt>struct estat</tt>s, WAA (working copy 
 * administrative area) function.
 *
 * In other words, handling single directories or complete trees of entries
 * (whereas est_ops.c is concerned with operations on single entries).
 *
 * \note \e WAA is short for <b>W</b>orking copy <b>A</b>dministrative
 * <b>A</b>rea, ie. the directory hierarchy where local data concerning
 * the remote state and some caches are stored.
 *
 * This is not needed for all operations; eg. an \a export works without it.
 * */


/** The extension temporary files in the WAA get. */
static const char ext_tmp[]=".tmp";


/** -.
 * They are long enough to hold the \ref OPT__WAA_PATH "waa path" plus the 
 * 3-level deep subdirectory structure for cache and data files.
 * The \ref OPT__CONF_PATH "conf path" plus additional data gets it own 
 * buffers.
 * @{ */
char *waa_tmp_path, *waa_tmp_fn,
						*conf_tmp_path, *conf_tmp_fn;
/** @} */
/** The meta-data for the WAA base directory.
 * The WAA itself doesn't get committed; checked via this inode. */
static struct sstat_t waa_stat;

/** The maximum path length encountered so far. Stored in the \a dir-file,
 * to enable construction of paths without reallocating. */
static unsigned max_path_len;

/** -.
 * This gets sorted by priority and URL on reading in \a url__load_list() . */
struct url_t **urllist=NULL;
/** -. */
int urllist_count=0;
/** How many entries we have; this is used to show the user
 * some kind of progress report, in percent. */
unsigned approx_entry_count;


struct waa___temp_names_t {
  char *temp_name;
	char *dest_name;
};
/** This array stores the target names.
 * Writes to the waa-area use temporary files, 
 * which get renamed on waa__close(). */
static struct waa___temp_names_t *target_name_array=NULL;
/** How many entries have been in use in the \ref target_name_array. */
static int target_name_array_len=0;

/** -. */
int waa_tmp_path_len;

/** -. */
struct waa__entry_blocks_t waa__entry_block;


/** -.
 * Valid after a successful call to \ref waa__find_common_base(). */
char *wc_path;
/** -. */
int wc_path_len;


const char
/** The header line of the dir-files.
 *
 * Consists of
 * - header version (for verification), 
 * - header length (for verification), 
 * - number of entries (for space allocation), 
 * - subdirectory count (currently only informational), 
 * - needed string space (in bytes), 
 * - length of longest path in bytes.
 * */
waa__header_line[]="%u %lu %u %u %u %u";


/** Convenience function for creating two paths. */
static inline void waa___init_path(enum opt__settings_e which,
		char *dest, char **eos)
{
	int l;


	l=0;
	if (strncmp(opt__get_string(OPT__SOFTROOT), 
				opt__get_string(which),
				opt__get_int(OPT__SOFTROOT)) != 0 )
	{
		strcpy(dest, opt__get_string(OPT__SOFTROOT));
		l=opt__get_int(OPT__SOFTROOT);
		/* OPT__SOFTROOT is defined to have *no* PATH_SEPARATOR at the end.  
		 * */
	  dest[l++]=PATH_SEPARATOR;
	}

	l+= strlen( strcpy(dest+l, opt__get_string(which) ) );

	/* ensure a delimiter */
	if (dest[l-1] != PATH_SEPARATOR)
	{
		dest[l++]=PATH_SEPARATOR;
		dest[l]='\0';
	}

	*eos=dest + l;
	opt__set_int(which, PRIO_MUSTHAVE, l);
}


/** -.
 * If not a WAA-less operation, find the WAA and define an ignore
 * pattern. */
int waa__init(void)
{
	int status;
	char *cp;


	status=0;
	/* If we're doing an import/export operation, we must not use the waa
	 * area. We may be running off a KNOPPIX CD, or whatever.
	 *
	 * What we *need* is the conf directory ... it might have options for us.  
	 * */

	/** \todo remove when gcc doesn't warn about \c strlen("const") 
	 * initializers. See debian bug #60xxxx.
	 * And see below for WAA_PATH, too. */
	if (opt__get_int(OPT__CONF_PATH)==0)
	{
		opt__set_string(OPT__CONF_PATH, PRIO_MUSTHAVE, DEFAULT_CONF_PATH);
		opt__set_int(OPT__CONF_PATH, PRIO_MUSTHAVE, strlen(DEFAULT_CONF_PATH));
	}


	/* at least /w or some such */
	STOPIF_CODE_ERR( opt__get_int(OPT__CONF_PATH)<3, EINVAL, 
			"The CONF path is invalid; a (non-root) path is expected.");


	if (action->is_import_export)
	{
		/* So the WAA path is NULL, and serves as a validation point - every 
		 * access tried will get a SEGV and can be debugged. */
		opt__set_string(OPT__WAA_PATH, PRIO_MUSTHAVE, NULL);
		opt__set_int(OPT__WAA_PATH, PRIO_MUSTHAVE, 0);
	}
	else
	{
		if (opt__get_int(OPT__WAA_PATH)==0)
		{
			opt__set_string(OPT__WAA_PATH, PRIO_MUSTHAVE, DEFAULT_WAA_PATH);
			opt__set_int(OPT__WAA_PATH, PRIO_MUSTHAVE, strlen(DEFAULT_WAA_PATH));
		}

		STOPIF_CODE_ERR( opt__get_int(OPT__WAA_PATH)<3, EINVAL, 
				"The WAA path should be set to a directory below \"/\".");
	}


	/* This memory has lifetime of the process.
	 *   /path/to/waa / 01/02/03..0F/ extension .tmp
	 * The memory allocated is enough for the longest possible path. */
	waa_tmp_path_len=
		opt__get_int(OPT__SOFTROOT) + 1 +
		( max(opt__get_int(OPT__WAA_PATH), 
					opt__get_int(OPT__CONF_PATH)) ) + 1 + 
		WAA_WC_MD5_CHARS + 1 +
		APR_MD5_DIGESTSIZE*2 + 3 + 
		WAA__MAX_EXT_LENGTH + strlen(ext_tmp) + 1 +4;
	DEBUGP("using %d bytes for temporary WAA+conf paths", waa_tmp_path_len);


	/* Here the paths are set at highest priority, so they can't get changed 
	 * afterwards. */
	STOPIF( hlp__alloc( &conf_tmp_path, waa_tmp_path_len), NULL);
	waa___init_path(OPT__CONF_PATH, conf_tmp_path, &conf_tmp_fn);

	if (!action->is_import_export)
	{
		STOPIF( hlp__alloc( &waa_tmp_path, waa_tmp_path_len), NULL);

		waa___init_path(OPT__WAA_PATH, waa_tmp_path, &waa_tmp_fn);

		/* validate existence and save dev/inode for later checking */
		STOPIF( hlp__lstat(waa_tmp_path, &waa_stat),
				"!stat() of waa-path \"%s\" failed. "
				"Does your local WAA storage area exist? ", 
				waa_tmp_path);
		DEBUGP("got the WAA as inode %llu", (t_ull)waa_stat.ino);

		/* Only check whether it's there. */
		STOPIF_CODE_ERR( access(conf_tmp_path, 
					action->is_readonly ? R_OK : W_OK)==-1, errno,
				"!Cannot %s to the FSVS_CONF path \"%s\".",
				action->is_readonly ? "read" : "write",
				conf_tmp_path);
	}

	/* Now no more changes of the softroot (eg. via the per-WC configuration) 
	 * are allowed. */
	opt__set_int( OPT__SOFTROOT, PRIO_MUSTHAVE, 
			opt__get_int(OPT__SOFTROOT));
	cp=opt__variable_from_option(OPT__SOFTROOT);
	/* Solaris 10 compatibility. */
	if (opt__get_string(OPT__SOFTROOT))
		setenv(cp, opt__get_string(OPT__SOFTROOT), 1);
	else
		unsetenv(cp);


ex:
	return status;
}


/** -.
 * This is more or less a portable reimplementation of GNU \c 
 * getcwd(NULL,0) ... self-allocating the needed buffer.
 *
 * \a where gets the cwd, and \b must be free()d; the optional \a ret_len 
 * can be set to the actual length of the cwd.
 *
 * If the caller wants to append some path to the end, and knows how many 
 * bytes are needed, the \a additional bytes can be requested.
 * 
 * If the cwd has been removed, we get \c ENOENT. But returning that would 
 * not necessarily signal a fatal error to all callers, so we return \c 
 * ENOTDIR in that case. */ 
int waa__save_cwd(char **where, int *ret_len, 
		int additional)
{
	int status;
	/* We remember how many bytes we used last time, hoping that we need no 
	 * realloc() call in later invocations. */
	static int len=256;
	char *path;


	path=NULL;
	status=0;
	while (1)
	{
		STOPIF( hlp__realloc( &path, len + additional + 4), NULL);

		/* We allocate the needed amount, but lie to getcwd() about the available 
		 * space - so the caller surely has space left. */
		if (getcwd(path, len-1)) break;

		STOPIF_CODE_ERR(errno != ERANGE, errno == ENOENT ? ENOTDIR : errno,
				"Cannot get the current directory.");

		len += 512;
		STOPIF_CODE_ERR(len > 1<<13, ERANGE,
				"You have mighty long paths. Too long. More than %d bytes? Sorry.",
				len);
	}

	if (ret_len) *ret_len=strlen(path);
	*where=path;

ex:
	return status;
}


/** -.
 *
 * \note The mask used is \c 0777 - so mind your umask! */
int waa__mkdir(char *dir, int including_last)
{
	int status;
	STOPIF( waa__mkdir_mask(dir, including_last, 0777), NULL);
ex:
	return status;
}


/** -.
 *
 * If it already exists, no error is returned.
 *
 * If needed, the structure is generated recursively.
 *
 * With \a including_last being \c 0 you can give a filename, and make sure 
 * that the directories up to there are created. Because of this we can't 
 * use \c apr_dir_make_recursive() - We'd have to cut the filename away, 
 * and this is done here anyway.
 * */
int waa__mkdir_mask(char *dir, int including_last, int mask)
{
	int status;
	char *last_ps;
	struct stat buf;


	status=0;
	/* Does something exist here? */
	if (lstat(dir, &buf) == -1)
	{
		if (errno == ENOENT)
		{
			/* Some intermediate levels are still missing; try again 
			 * recursively. */
			last_ps=strrchr(dir, PATH_SEPARATOR);
			BUG_ON(!last_ps);

			/* Strip last directory, and *always* undo the change. */
			*last_ps=0;
			status=waa__mkdir(dir, 1);
			*last_ps=PATH_SEPARATOR;
			STOPIF( status, NULL);

			DEBUGP("%s: last is %d", dir, including_last);
			/* Now the parent was done ... so we should not get ENOENT again. */
			if (including_last)
				STOPIF_CODE_ERR( mkdir(dir, mask & 07777) == -1, errno, 
						"cannot mkdir(%s)", dir);
		}
		else
			STOPIF(status, "cannot lstat(%s)", dir);
	}
	else
	{
		STOPIF_CODE_ERR( including_last && !S_ISDIR(buf.st_mode), ENOTDIR, 
				"\"%s\" is not a directory", dir);
	}

ex:
	return status;
}


/** Returns the MD5 of the given path, taking the softroot into account. */
int waa___get_path_md5(const char * path, 
		unsigned char digest[APR_MD5_DIGESTSIZE])
{
	int status;
	int plen, wdlen;
	char *cp;
	static const char root[]= { PATH_SEPARATOR, 0};


	status=0;
	cp=NULL;
	plen=strlen(path);
	DEBUGP("path is %s", path);

	/* If we have a relative path, ie. one without / as first character,
	 * we have to take the current directory first. */
	if (path[0] != PATH_SEPARATOR)
	{
		/* This may be suboptimal for performance, but the only usage
		 * currently is for MD5 of large files - and there it doesn't
		 * matter, because shortly afterwards we'll be reading many KB. */
		STOPIF( waa__save_cwd(&cp, &wdlen, 1 + plen + 1 + 3), NULL);

		path= hlp__pathcopy(cp, NULL, cp, "/", path, NULL);
		/* hlp__pathcopy() can return shorter strings, eg. by removing ./././// 
		 * etc. So we have to count again. */
		plen=strlen(path);
	}

	while (plen>1 && path[plen-1] == PATH_SEPARATOR)
		plen--;

	if (opt__get_string(OPT__SOFTROOT))
	{
		DEBUGP("have softroot %s for %s, compare %d bytes", 
				opt__get_string(OPT__SOFTROOT), path, opt__get_int(OPT__SOFTROOT));
		if (strncmp(opt__get_string(OPT__SOFTROOT), 
					path, opt__get_int(OPT__SOFTROOT)) == 0 )
			path+=opt__get_int(OPT__SOFTROOT);

		/* We need to be sure that the path starts with a PATH_SEPARATOR.
		 * That is achieved in waa__init(); the softroot path gets normalized 
		 * there. */
		/* In case both the argument and the softroot are identical, 
		 * we end up with *path==0. Change that to the root directory. */
		if (!*path)
			path=(char*)root;

		plen=strlen(path);
	}

	DEBUGP("md5 of %s", path);
	apr_md5(digest, path, plen);
	IF_FREE(cp);

ex:
	return status;
}


/** -.
 *
 * In \a erg a pointer to an static buffer (at least as far as the caller
 * should mind!) is returned; \a eos, if not \c NULL, is set to the end of 
 * the string. \a start_of_spec points at the first character specific to 
 * this file, ie. after the constant part of \c $FSVS_WAA or \c $FSVS_CONF 
 * and the \c PATH_SEPARATOR.
 *
 * \a flags tell whether the path is in the WAA (\ref GWD_WAA) or in the 
 * configuration area (\ref GWD_CONF); furthermore you can specify that 
 * directories should be created as needed with \ref GWD_MKDIR.
 *
 * The intermediate directories are created, so files can be created
 * or read directly after calling this function. */
int waa__get_waa_directory(const char *path,
		char **erg, char **eos, char **start_of_spec,
		int flags)
{
	static int waa_init_for_wc = 0;
	int status, len;
	char *cp;
	unsigned char digest[APR_MD5_DIGESTSIZE], *p2dig;

	status=0;
	cp=NULL;

	/* Do that before the apr_md5 call, so we can use the digest. */
	if ((flags & GWD_WAA) && !waa_init_for_wc)
	{
		waa_init_for_wc=1;

		/* We avoid that if it's 0 (backward compatibility). */
		if (WAA_WC_MD5_CHARS)
		{
			BUG_ON(!wc_path);

			STOPIF( waa___get_path_md5(wc_path, digest), NULL);

			/* We have enough space for the full MD5, even if it's overwritten 
			 * later on; and as it's no hot path (in fact it's called only once), 
			 * the performance doesn't matter, too.
			 * So just use the function that we already have. */
			cs__md5tohex(digest, waa_tmp_fn);
			waa_tmp_fn += WAA_WC_MD5_CHARS;
			*waa_tmp_fn = PATH_SEPARATOR;
			waa_tmp_fn++;
		}
		/* Termination is needed only for the output below. */
		*waa_tmp_fn = 0;

		DEBUGP("init wc base:%s %s", wc_path+opt__get_int(OPT__SOFTROOT), waa_tmp_path);
	}


	STOPIF( waa___get_path_md5(path, digest), NULL);

	p2dig=digest;
	len=APR_MD5_DIGESTSIZE;

	if (flags & GWD_WAA)
	{
		*erg = waa_tmp_path;
		cp = waa_tmp_fn;
		if (start_of_spec) *start_of_spec=cp;


		Mbin2hex(p2dig, cp, 1);
		len--;

		*(cp++) = PATH_SEPARATOR;

		Mbin2hex(p2dig, cp, 1);
		len--;

		*(cp++) = PATH_SEPARATOR;
	}
	else if (flags & GWD_CONF)
	{
		*erg = conf_tmp_path;
		cp = conf_tmp_fn;
		if (start_of_spec) *start_of_spec=cp;
	}
	else
	{
		BUG(".:8:.");
	}

	Mbin2hex(p2dig, cp, len);
	if (flags & GWD_MKDIR)
		STOPIF( waa__mkdir(*erg, 1), NULL);

	*(cp++) = PATH_SEPARATOR;
	*cp = '\0';

	if (eos) *eos=cp;

	DEBUGP("returning %s", *erg);

ex:
	return status;
}


/** Base function to open files in the WAA.
 *
 * For the \a flags the values of \c creat or \c open are used;
 * the mode is \c 0777, so take care of your umask. 
 *
 * If the flags include one or more of \c O_WRONLY, \c O_TRUNC or \c O_RDWR
 * the file is opened as a temporary file and \b must be closed with 
 * waa__close(); depending on the success value given there it is renamed
 * to the destination name or deleted. 
 *
 * This temporary path is stored in a per-filehandle array, so there's no
 * limit here on the number of written-to files. 
 *
 * If the flags include \c O_APPEND, no temporary file is used, and no 
 * filehandle is stored - do simply a \c close().
 *
 * For read-only files simply do a \c close() on their filehandles.
 *
 * Does return \c ENOENT without telling the user.
 *
 * \note If \a extension is given as \c NULL, only the existence of the 
 * given WAA directory is checked. So the caller gets a \c 0 or an error 
 * code (like \c ENOENT); \a flags and \a filehandle are ignored.
 * */
int waa__open(char *path,
		const char *extension,
		int flags,
		int *filehandle)
{
	char *cp, *orig, *eos, *dest, *start_spec;
	int fh, status;
	int use_temp_file;
	int old_len;


	fh=-1;
	orig=NULL;

	/* O_APPEND means that we have to append to the *existing* file, so we 
	 * may not use the temporaray name.
	 * But using O_APPEND normally means using O_CREAT, too - so we have to 
	 * do the specifically. */
	use_temp_file=(flags & O_APPEND) ? 0 : 
		(flags & (O_WRONLY | O_RDWR | O_CREAT));

	STOPIF( waa__get_waa_directory(path, &dest, &eos, &start_spec,
				waa__get_gwd_flag(extension) ), NULL);

	if (!extension)
	{
		/* Remove the last PATH_SEPARATOR. */
		BUG_ON(eos == dest);
		eos[-1]=0;
		return hlp__lstat(dest, NULL);
	}

	strcpy(eos, extension);
	BUG_ON( action->is_readonly &&
			(flags & (O_WRONLY | O_RDWR | O_APPEND | O_CREAT)),
			"Action marked read-only, got flags 0x%x for %s", flags, eos);

	if (use_temp_file)
	{
		STOPIF( hlp__strdup( &orig, dest), NULL);


		strcat(eos, ext_tmp);

		/* In order to avoid generating directories (eg. for md5s-files) that 
		 * aren't really used (because the data files are < 128k, and so the md5s 
		 * files get deleted again), we change the PATH_SEPARATOR in the 
		 * destination filename to '_' - this way we get different filenames 
		 * and avoid collisions with more than a single temporary file (as 
		 * would happen with just $FSVS_WAA/tmp).
		 *
		 * Can that filename get longer than allowed? POSIX has 255 characters, 
		 * IIRC - that should be sufficient. */
		cp=strchr(start_spec, PATH_SEPARATOR);
		while (cp)
		{
			*cp='_';
			cp=strchr(cp+1, PATH_SEPARATOR);
		}

		/* We want to know the name later, so keep a copy. */
		STOPIF( hlp__strdup( &dest, dest), NULL);
		DEBUGP("tmp for target %s is %s", orig, dest);
	}
	else
		DEBUGP("reading target %s", dest);


	if (flags & O_APPEND)
		STOPIF( waa__mkdir(dest, 0), NULL);


	/* in case there's a O_CREAT */
	fh=open(dest, flags, 0777);
	if (fh<0) 
	{
		status=errno;
		if (status == ENOENT) goto ex;
		STOPIF(status, "open %s with flags 0x%X",
				dest, flags);
	}

	DEBUGP("got fh %d", fh);

	/* For files that are written to, remember the original filename, indexed 
	 * by the filehandle. That must be done *after* the open - we don't know 
	 * the filehandle in advance! */
	if (use_temp_file)
	{
		if (fh >= target_name_array_len) 
		{
			/* store old length */
			old_len=target_name_array_len;

			/* Assume some more filehandles will be opened */
			target_name_array_len=fh+8;
			DEBUGP("reallocate target name array to %d", target_name_array_len);
			STOPIF( hlp__realloc( &target_name_array, 
					sizeof(*target_name_array) * target_name_array_len), NULL);

			/* zero out */
			memset(target_name_array + old_len, 0, 
					sizeof(*target_name_array) * (target_name_array_len-old_len));
		}

		/* These are already copies. */
		target_name_array[fh].dest_name=orig;
		target_name_array[fh].temp_name=dest;
	}

	*filehandle=fh;
	status=0;

ex:
	if (status && fh>-1) close(fh);
	return status;
}


/** -.
 *
 * If \a has_failed is !=0, the writing to the file has
 * failed somewhere; so the temporary file is not renamed to the 
 * destination name, just removed.
 *
 * This may be called only for \b writeable files of waa__open() and 
 * similar; readonly files should just be \c close()d. */
int waa__close(int filehandle, int has_failed)
{
	int status, do_unlink;
	struct waa___temp_names_t *target;


	/* Assume we have to remove the file; only if the rename
	 * is successful, this ain't true. */
	do_unlink=1;
	status=0;

	target= target_name_array ? target_name_array+filehandle : NULL;

	if (target)
		DEBUGP("filehandle %d should be %s", filehandle, target->dest_name);
	else
		DEBUGP("filehandle %d wasn't opened via waa__open()!", filehandle);

	status=close(filehandle);
	if (!has_failed) 
	{
		STOPIF_CODE_ERR(status == -1, errno, "closing tmp file");

		if (target)
		{
			/* Now that we know we'd like to keep that file, make the directories 
			 * as needed. */
			STOPIF( waa__mkdir(target->dest_name, 0), NULL);

			/* And give it the correct name. */
			STOPIF_CODE_ERR( 
					rename(target->temp_name, target->dest_name) == -1, 
					errno, "renaming tmp file from %s to %s",
					target->temp_name, target->dest_name);
		}

		do_unlink=0;
	}

	status=0;

ex:
	/* If there's an error while closing the file (or already given 
	 * due to has_failed), unlink the file. */
	if (do_unlink && target)
	{
		do_unlink=0;
		STOPIF_CODE_ERR( unlink(target->temp_name) == -1, errno,
				"Cannot remove temporary file %s", target->temp_name);
	}

	if (target)
	{
		IF_FREE(target->temp_name);
		IF_FREE(target->dest_name);
	}

	return status;
}


/** -.
 *
 * Normally this is used to mark the base directory used in some WAA path,
 * ie. if you are versioning \c /etc, you'll get a symlink
 * \c $WAA/18/2f/153bd94803955c2043e6f2581d5d/_base
 * pointing to \c /etc . */
int waa__make_info_file(char *directory, char *name, char *dest)
{
	int status;
	int hdl;
	static const char readme_1[]="This directory is used by FSVS.\n"
		"Please see https://github.com/phmarek/fsvs for more details.\n\n"
		"The working copy for this hash value is\n"
		"\t";
	static const char readme_2[]="\n";

#define w(_buffer, _l) (write(hdl, _buffer, _l) != _l)

	STOPIF( waa__open(directory, name, O_CREAT | O_WRONLY, &hdl), NULL);
	STOPIF_CODE_ERR( 
			w(readme_1, sizeof(readme_1)-1) ||
			w(dest, strlen(dest)) ||
			w(readme_2, sizeof(readme_2)-1),
			errno,
			"Cannot create the readme file.");
	STOPIF( waa__close(hdl, 0), NULL);

#undef w

ex:
	return status;
}


/** -.
 *
 * This function takes the parameter \a name, and returns a freshly 
 * allocated bit of memory with the given value or - if \c NULL -
 * the current working directory.
 *
 * That the string is always freshly allocated on the heap makes
 * sense in that callers can \b always just free it. */
int waa__given_or_current_wd(const char *name, char **erg)
{
	int status;


	status=0;
	if (name)
		STOPIF( hlp__strdup( erg, name), NULL);
	else
		STOPIF( waa__save_cwd( erg, NULL, 0), NULL);

ex:
	return status;
}


/** -.
 *
 * If the \c unlink()-call succeeds, the (max. 2) directory levels above 
 * are removed, if possible.
 *
 * Via the parameter \a ignore_not_exist the caller can say whether a
 * \c ENOENT should be returned silently.
 *
 * If \a extension is \c NULL, the given path already specifies a file, and 
 * is not converted into a WAA path.
 * 
 * \see waa_files. */
int waa__delete_byext(char *path, 
		char *extension,
		int ignore_not_exist)
{
	int status;
	char *cp, *eos;
	int i;


	status=0;
	if (extension)
	{
		STOPIF( waa__get_waa_directory(path, &cp, &eos, NULL,
					waa__get_gwd_flag(extension)), NULL);
		strcpy(eos, extension);

		/* Make eos point at the PATH_SEPARATOR. */
		eos--;
		BUG_ON(*eos != PATH_SEPARATOR);
	}
	else
	{
		cp=path;
		eos=strrchr(cp, PATH_SEPARATOR);
		BUG_ON(!eos);
	}

	DEBUGP("unlink %s", cp);
	if (unlink(cp) == -1)
	{
		status=errno;
		if (status == ENOENT && ignore_not_exist) status=0;

		STOPIF(status, "Cannot remove spool entry %s", cp);
	}

	/* Try to unlink the (possibly) empty directory. 
	 * If we get an error don't try further, but don't give it to 
	 * the caller, either.
	 * After all, it's just a clean-up. */
	/* eos is currently at a PATH_SEPARATOR; we have to clean that. */
	for(i=0; i<3; i++)
	{
		*eos=0;

		if (rmdir(cp) == -1) break;

		eos=strrchr(cp, PATH_SEPARATOR);
		/* That should never happen. */
		BUG_ON(!eos, "Got invalid path to remove");
	}

	DEBUGP("last removed was %s", cp);

ex:
	return status;
}


/** -.
 *
 * The \a entry_name may be \c NULL; then the current working directory
 * is taken.
 * \a write is open mode, like used for \c open(2) (<tt>O_CREAT | O_WRONLY 
 * | O_TRUNC</tt>) and is given to \c waa__open().
 * 
 * \c ENOENT is returned without giving an error message. */
int waa__open_byext(const char *entry_name,
		const char *extension,
		int mode,
		int *fh)
{
	int status;
	char *entry;


	status=0;
	entry=NULL;
	STOPIF( waa__given_or_current_wd(entry_name, &entry), NULL );

	status=waa__open(entry, extension, mode, fh);
	if (status == ENOENT) goto ex;
	STOPIF(status, NULL);

ex:
	IF_FREE(entry);
	return status;
}


/** -.
 * */
int waa__open_dir(char *wc_base,
		int write,
		int *fh)
{
	return waa__open_byext(wc_base, WAA__DIR_EXT, write, fh);
}


/** -.
 *
 * All entries are defined as new. */
int waa__build_tree(struct estat *dir)
{
	int status;
	struct estat *sts;
	int i, ignore, have_ignored, have_found;

	status=0;
	/* no stat info on first iteration */
	STOPIF( waa__dir_enum( dir, 0, 0), NULL);


	DEBUGP("found %d entries ...", dir->entry_count);
	have_ignored=0;
	have_found=0;
	for(i=0; i<dir->entry_count; i++)
	{
		sts=dir->by_inode[i];

		STOPIF( ign__is_ignore(sts, &ignore), NULL);
		if (ignore>0)
		{
			DEBUGP("ignoring entry %s", sts->name);

			sts->to_be_ignored=1;
			have_ignored=1;
			continue;
		}

		/* in build_tree, it must be a new entry. */
		sts->entry_status=FS_NEW;
		ops__set_todo_bits(sts);
		approx_entry_count++;
		have_found++;

		if (S_ISDIR(sts->st.mode))
		{
			if (ops__are_children_interesting(sts))
			{
				STOPIF_CODE_ERR( chdir(sts->name) == -1, errno,
						"chdir(%s)", sts->name);

				STOPIF( waa__build_tree(sts), NULL );

				/* this can fail if the parent directories have been removed. */
				STOPIF_CODE_ERR( chdir("..") == -1, errno,
						"parent has gone");
			}
		}

		STOPIF( ac__dispatch(sts), NULL);
	}

	if (have_ignored)
		/* Delete per index faster */
		STOPIF( ops__free_marked(dir, 0), NULL);

	if (have_found)
		ops__mark_changed_parentcc(dir, entry_status);

ex:
	return status;
}


/** Returns the index at which the element should be
 * (the index at which an equal or first bigger inode is). */
int waa___find_position(struct estat **new, 
		struct estat ***array, int count)
{
	int smaller, middle, bigger_eq;
	int status;


	/* That's easy. */
	if (count == 0) 
		return 0;

	/* A special case. As the directories are normally laid out sequentially 
	 * on a hard disk, the inodes are often grouped in their directories.
	 * In a test case (my /etc) this shortcut was taken 1294 times, and
	 * didn't catch 1257 times (with up to 80 entries in the array). */
	/* Recent gcov tests are still better:
	 * - of ~24000 calls of this function
	 * - 118 times the above shortcut for only a single directory was taken,
	 * - ~23730 times this single comparison was enough,
	 * - and the binary search loop below was called only 16 times.
	 * Hooray! */
	if (dir___f_sort_by_inode(new, array[0]) < 0)
	{
		DEBUGP("short path taken for 0<1");
		return 0;
	}
	/* if only one element, and not on first position ... */
	if (count == 1) return 1;

	/* some more cheating :-) */
	if (dir___f_sort_by_inode(new, array[count-1]) >= 0)
	{
		DEBUGP("short path taken for >count");
		return count;
	}
	smaller=1; 

	/* bsearch can only find the _equal_ element - we need 
	 * the first one higher. */
	/* order is wrong - find new place for this element. */
	bigger_eq=count-1;
	/* i is a smaller element, k a possibly higher */
#if 0
	if (1)
	{
		char tmp[count*(18+1)+10];
		int i, n;
		for (i=n=0; i<count; i++)
		{
			n += sprintf(tmp+n,"%llu ", (t_ull)(*array[i])->st.ino);
		}
		DEBUGP("having %d [ %s]", count, tmp);
		DEBUGP("looking for %llu", (t_ull)(*new)->st.ino);
	}
#endif

	while (1) //bigger_eq>smaller+1)
	{
		middle=(bigger_eq+smaller)/2;
		DEBUGP("at %d=%llu - %d=%llu - %d=%llu", 
				smaller, (t_ull)(*array[smaller])->st.ino,
				middle, (t_ull)(*array[middle])->st.ino,
				bigger_eq, (t_ull)(*array[bigger_eq])->st.ino);

		status=dir___f_sort_by_inode(new, array[middle]);
		if (status > 0)
			smaller=middle+1;
		else if (status	< 0)
			bigger_eq=middle;
		else 
		{
			/* status==0 means identical inodes => hardlinks.
			 * Now these are directories ... but we see hardlinks eg. for binding 
			 * mounts, so we cannot just abort. */
			DEBUGP("Jackpot, hardlink!");
			bigger_eq=middle;
			break;
		}
		if (bigger_eq<=smaller) break;
	}

	DEBUGP("believing in %d %llu",
			bigger_eq, (t_ull)(*array[bigger_eq])->st.ino);
	/* now we have an index bigger_eq, whose element is bigger or equal 
	 * than the new, and its left is smaller or equal: */
#if DEBUG
	BUG_ON((bigger_eq < count-1 && dir___f_sort_by_inode(new, array[bigger_eq])>0) ||
			(bigger_eq >0 && dir___f_sort_by_inode(new, array[bigger_eq-1])<0));
#endif

	return bigger_eq;
}


/** -.
 *
 * Here the complete entry tree gets written to a file, which is used on the
 * next invocations to determine the entries' statii. It contains the names,
 * sizes, MD5s, devices, inode numbers, parent, mode and time information,
 * and a reference to the parent to re-build the tree.
 *
 * \todo Currently hardlinks with duplicate inode-numbers are not well done
 * in fsvs.
 *
 *
 * <h3>Format</h3>
 * This file has a single header line with a defined length; it is padded
 * before the newline with spaces, and the last character before the newline
 * is a \c $ .
 * The other lines have space-delimited fields, and a \\0 delimited name 
 * at the end, followed by a newline.
 *
 * <h3>Order of entries in the file</h3>
 * We always write parents before children, and (mostly) lower inode numbers 
 * before higher; mixing the subdirectories is allowed.
 * This allows us to rebuild the tree in one pass (because the parents are
 * already known), and gives us nearly linear reading on the storage media
 * (because the inodes are mostly in harddisk order, there's not much 
 * back-seeking necessary).
 *
 * As a consequence the root entry \c . is \b always the first one in
 * the written file. 
 *
 * \note 
 * If we were going \b strictly in inode-order, we would have to jump over 
 * some entries (if the parent directory has a higher inode
 * number than this entry and the [hard disk] head is already further down),
 * and then have a second run through ... (or possibly a third, and so on).
 * That's more complexity than wanted, and doesn't bring performance.
 * So currently only one run; hard disk must move back sometimes. 
 *
 *
 * <h3>\c directory Array</h3>
 * We use one array, named \c directory , to store pointers in the 
 * \a estat::by_inode arrays we're traversing (which are defined to 
 * be NULL-terminated).
 *
 * We just have to find the next inode in the active directories; they are
 * already sorted by inode, so that's very easy. 
 *
 * Here's a small drawing in ASCII, followed by a graphviz version.
 *
 * \verbatim
 *      (struct estat)
 *        ^ 
 *        |
 *    xxxxxxxxxxxxxxxxN    xxxxxxxxxxxxxxxN    xxN       xxxxN
 *          ^                          ^       ^           ^
 *   /->d >-/                          |       |           |
 *   |  d >----------------------------/       |           |
 *   |  d >------------------------------------/           |
 *   |  d >------------------------------------------------/
 *   |  
 *   directory
 * \endverbatim
 * \dot
 * digraph directory 
 * {
 *   node [shape=record, fontsize=9, height=0, width=0];
 *   rankdir=BT;
 *
 *   directory [label=" directory | { <1> 1 | <2> 2 | <3> 3 | <4> 4 }"];
 *   List1 [ label="<1>1|<2>2|<3>3|<4>4|NULL" ];
 *   List2 [ label="<1>1|<2>2|<3>3|<4>4|<5>5|NULL" ];
 *   List3 [ label="<1>1|<2>2|<3>3|<4>4|NULL" ];
 *   List4 [ label="<1>1|<2>2|<3>3|NULL" ];
 *   sts [label="(struct estat)"];
 *
 *
 *   directory:4:e -> List1:2:s;
 *   directory:3:e -> List2:3:s;
 *   directory:2:e -> List3:4:s;
 *   directory:1:e -> List4:1:s;
 *
 *   List1:1 -> sts;
 *
 *   node [style=invis];
 *   edge [style=invis];
 *
 *   directory:1:e -> Hidden1 -> List4:n;
 *   directory:2:e -> Hidden1 -> List3:n;
 * }
 * \enddot
 * The x's are the by_inode-arrays of pointers to struct, NULL-terminated.
 *      
 * The d's show the directories-array with 4 entries.
 * 
 * We don't really store the parent inode numbers in the file; that wouldn't
 * be enough, anyway - as soon as there are two or more filesystems, they
 * would collide.
 *
 * So instead of the inode number we store the number of the entry *in the
 * file*; so the root inode (which is always first) has parent_ino=0 (none),
 * its children get 1, and so on.
 * That means that as long as we allocate the memory block in a single
 * continuous block, we don't have to search any more; we can just reconstruct
 * the pointers to the parent. 
 * We keep the directory-array sorted; so we have to insert a new directory
 * at the correct position, but can otherwise output very fast. 
 * So for the array
 *   [10 20 30 40 50 60 70]
 * the element 10 is written; the next one, say 35, is inserted at the
 * correct position:
 *   [20 30 35 40 50 60 70]
 * Again the first (smallest) element is written, and so on.
 */ 
int waa__output_tree(struct estat *root)
{
	struct estat ***directory, *sts, **sts_pp;
	int max_dir, i, alloc_dir;
	unsigned this_len;
	int status, waa_info_hdl;
	unsigned complete_count, string_space;
	char header[HEADER_LEN] = "UNFINISHED";


	waa_info_hdl=-1;
	directory=NULL;
	STOPIF( waa__open_dir(NULL, WAA__WRITE, &waa_info_hdl), NULL);

	/* allocate space for later use - entry count and similar. */
	status=strlen(header);
	memset(header + status, '\n', sizeof(header)-status);
	i=write(waa_info_hdl, header, sizeof(header));
	STOPIF_CODE_ERR( i != sizeof(header), errno,
			"header was not written");


	/* Take a page of pointers (on x86-32). Will be reallocated if
	 * necessary. */
	alloc_dir=1024;
	STOPIF( hlp__calloc( &directory, alloc_dir+1, sizeof(*directory)), NULL);


	/* The root entry is visible above all URLs. */
	root->url=NULL;

	STOPIF( ops__save_1entry(root, 0, waa_info_hdl), NULL);
	root->file_index=complete_count=1;


	root->path_len=string_space=strlen(root->name);
	max_path_len=root->path_len;

	/* an if (root->entry_count) while (...) {...}
	 * would be possible, but then an indentation level would
	 * be wasted :-) ! */
	if (!root->entry_count) goto save_header;

	/* This check is duplicated in the loop.
	 * We could do that in ops__save_1entry(), but it doesn't belong here. */
	if (root->to_be_sorted)
	{
		DEBUGP("re-sorting root");
		STOPIF( dir__sortbyinode(root), NULL);
	}

	/* by_inode might be reallocated by dir__sortbyinode(); so it has be used 
	 * after that. */
	directory[0]=root->by_inode;
	max_dir=1;

	/* as long as there are directories to do... */
	while (max_dir)
	{
		// get current entry
		sts=( *directory[0] );


		/* find next element */
		directory[0]++;

		/* end of this directory ?*/
		if (*directory[0] == NULL)
		{
			/* remove this directory by shifting the list */
			max_dir--;
			DEBUGP("finished subdir");
			memmove(directory, directory+1, sizeof(*directory)*max_dir);
		}
		else if (max_dir>1)
		{
			/* check if it stays or gets moved.
			 * ignore element 0, as this is the new one. */
			i=waa___find_position(directory[0], directory+1, max_dir-1);
			if (i)
			{
				/* order is wrong - move elements.
				 * Mind that returned index is one element further in directory[]! 
				 * 
				 * [ 55 20 30 40 50 60 ] max_dir=6, i=4
				 * new^ ^0          ^4
				 *
				 * [ 20 30 40 50 55 60 ]
				 * */
				sts_pp=directory[0];
				memmove(directory, directory+1, sizeof(*directory)*i);
				directory[i]=sts_pp;
				DEBUGP("old current moves to #%u: %llu < %llu",
						i, 
						(t_ull)(*directory[i-1])->st.ino, 
						(t_ull)(*directory[i  ])->st.ino);
			}
		}

		/* If this takes too much performance, we might have to duplicate that 
		 * check before the waa___find_position() call above. */
		if (!ops__should_entry_be_written_in_list(sts))
			continue;


		// do current entry
		STOPIF( ops__save_1entry(sts, sts->parent->file_index, waa_info_hdl), 
				NULL);

		complete_count++;
		/* store position number for child -> parent relationship */
		sts->file_index=complete_count;

		this_len=strlen(sts->name)+1;
		string_space += this_len;

		if (!sts->path_len)
			ops__calc_path_len(sts);
		if (sts->path_len > max_path_len)
			max_path_len = sts->path_len;


		if (ops__has_children(sts))
		{
			/* It's easy and possible to have always the correct number
			 * of subdirectories in root->subdir_count. We'd just have
			 * to walk up to the root in waa__build_tree and add_directory
			 * and increment the number there.
			 *
			 * But 
			 * - we don't really know if this size is really required and
			 * - we'd like to decrease the size of the structure,
			 * so we don't use that really any more - we realloc the pointers
			 * if necessary. */
			if (max_dir >= alloc_dir)
			{
				alloc_dir *= 2;
				STOPIF( hlp__realloc( &directory, (alloc_dir+1) * sizeof(*directory)), NULL);
				DEBUGP("reallocated directory pointers to %u entries", alloc_dir);
			}

			/* Has this directory to be sorted, because it got new elements?
			 * Must be done *before* inserting into the array. */
			if (sts->to_be_sorted)
				STOPIF( dir__sortbyinode(sts), NULL);


			/* sort into array */
			i=waa___find_position(sts->by_inode, directory, max_dir);

			/* this time we have to shift all bigger elements one further:
			 *   new=45, max_dir=7,
			 *   [10 20 30 40 50 60 70]
			 *                i=4  
			 * results in
			 *   [10 20 30 40 45 50 60 70] */
			memmove(directory+i+1, directory+i, 
					sizeof(*directory)*(max_dir-i));

			directory[i]=sts->by_inode;
			DEBUGP("new subdir %llu #%u", (t_ull)(*directory[i])->st.ino, i);
			max_dir++;
		}

#ifdef DEBUG
		for(i=1; i<max_dir; i++)
			BUG_ON(
					dir___f_sort_by_inode( directory[i-1], directory[i] ) >0);
#endif
	}


save_header:
	/* save header information */
	/* path_len needs a terminating \0, so add a few bytes. */
	status=snprintf(header, sizeof(header), waa__header_line,
			WAA_VERSION, (t_ul)sizeof(header),
			complete_count, alloc_dir, string_space+4,
			max_path_len+4);
	BUG_ON(status >= sizeof(header)-1, "header space not large enough");

	/* keep \n at end */
	memset(header + status, ' ', sizeof(header)-1 -status);
	header[sizeof(header)-2]='$';
	STOPIF_CODE_ERR( lseek(waa_info_hdl, 0, SEEK_SET) == -1, errno,
			"seeking to start of file");
	status=write(waa_info_hdl, header, sizeof(header));
	STOPIF_CODE_ERR( status != sizeof(header), errno,
			"re-writing header failed");

	status=0;

ex:
	if (waa_info_hdl != -1)
	{
		i=waa__close(waa_info_hdl, status);
		waa_info_hdl=-1;
		STOPIF( i, "closing tree handle");
	}

	if (directory) IF_FREE(directory);

	return status;
}


static struct estat *old;
static struct estat current;
static int nr_new;
/** Compares the directories.
 * Every element found in old will be dropped from current;
 * only new elements are added to old, by temporarily using 
 * current.by_inode.
 *
 * Example:
 *
 * Old has these elements.
 *    b   c   e   g   h
 *
 * current gets these entries in by_name before the correlation
 * (by_inode has just another order):
 *    A   b   c   D   e   F   g   h   NULL
 * Now we need A, D, and F.
 * 
 * After the loop current has:
 *   by_inode   A   D   F
 *   by_name    NULL   b   c   NULL   e   NULL   g   h   NULL
 * with nr_new=3.
 *
 * */
int new_entry(struct estat *sts, struct estat **sts_p)
{
	int status;
	int ignore;

	STOPIF( ign__is_ignore(sts, &ignore), NULL);
	if (ignore>0)
		DEBUGP("ignoring entry %s", sts->name);
	else
	{
		sts->parent=old;

		*sts_p=NULL;
		current.by_inode[nr_new]=sts;
		nr_new++;

		DEBUGP("found a new one!");
		sts->entry_status=FS_NEW;
		sts->flags |= RF_ISNEW;

		/* Has to be done in that order, so that ac__dispatch() already finds 
		 * sts->do_filter_allows set. */
		ops__set_todo_bits(sts);
		STOPIF( ac__dispatch(sts), NULL);

		ops__mark_parent_cc(sts, entry_status);
		approx_entry_count++;


		/* if it's a directory, add all subentries, too. */
		if (S_ISDIR(sts->st.mode) && 
				ops__are_children_interesting(sts) &&
				(opt__get_int(OPT__FILTER) & FS_NEW))
		{
			STOPIF_CODE_ERR( chdir(sts->name) == -1, errno,
					"chdir(%s)", sts->name);

			STOPIF( waa__build_tree(sts), NULL);

			STOPIF_CODE_ERR( chdir("..") == -1, errno,
					"parent went away");
		}

	}

ex:
	return status;
}


/** Checks for new entries in this directory, and updates the
 * directory information. 
 *
 * Gets called after all \b expected (known) entries of this directory have 
 * been (shallowly!) read - so subdirectories might not yet be up-to-date 
 * yet.
 *
 * The estat::do_this_entry and estat::do_userselected flags are set, and 
 * depending on them (and opt_recursive) estat::entry_status is set.
 *
 * On \c chdir() an eventual \c EACCES is ignored, and the "maybe changed" 
 * status returned. */
int waa__update_dir(struct estat *_old)
{
	int dir_hdl, status;
	int i;
	char *path;


	old = _old;
	status=nr_new=0;
	dir_hdl=-1;

	current=*old;
	current.by_inode=current.by_name=NULL;
	current.entry_count=0;

	STOPIF( ops__build_path(&path, old), NULL);

	/* To avoid storing arbitrarily long pathnames, we just open this
	 * directory and do a fchdir() later. */
	dir_hdl=open(".", O_RDONLY | O_DIRECTORY);
	STOPIF_CODE_ERR( dir_hdl==-1, errno, 
			"saving current directory with open(.)");

	DEBUGP("update_dir: chdir(%s)", path);
	if (chdir(path) == -1)
	{
		if (errno == EACCES) goto ex;
		STOPIF( errno, "chdir(%s)", path);
	}

	/* Here we need the entries sorted by name. */
	STOPIF( waa__dir_enum( &current, 0, 1), NULL);
	DEBUGP("update_dir: direnum found %d; old has %d (%d)", 
			current.entry_count, old->entry_count,
			status);
	/* No entries means no new entries; but not old entries deleted! */
	if (current.entry_count == 0) goto after_compare;


	nr_new=0;
	STOPIF( ops__correlate_dirs( old, &current, 
				NULL, NULL, new_entry, NULL), NULL);


	DEBUGP("%d new entries", nr_new);
	/* no new entries ?*/
	status=0;
	if (nr_new)
	{
		STOPIF( ops__new_entries(old, nr_new, current.by_inode), 
				"adding %d new entries", nr_new);
	}

	/* Free unused struct estats. */
	/* We use by_name - there the pointers are sorted by usage. */
	for(i=0; i < current.entry_count; i++)
		if (current.by_name[i] )
			STOPIF( ops__free_entry( current.by_name+i ), NULL);

	/* Current is allocated on the stack, so we don't free it. */
	IF_FREE(current.by_inode);
	IF_FREE(current.by_name);
	/* The strings are still used. We would have to copy them to a new area, 
	 * like we're doing above in the by_name array. */
	//	IF_FREE(current.strings);

after_compare:
	/* There's no doubt now.
	 * The old entries have already been checked, and if there are new
	 * we're sure that this directory has changed. */
	old->entry_status &= ~FS_LIKELY;

	/* If we find a new entry, we know that this directory has changed.
	 * We cannot use the ops__mark_parent_* functions, as old can have no 
	 * children that we could give. */
	if (nr_new)
		ops__mark_changed_parentcc(old, entry_status);

ex:
	if (dir_hdl!=-1) 
	{
		i=fchdir(dir_hdl);
		STOPIF_CODE_ERR(i == -1 && !status, errno,
				"cannot fchdir() back");
		i=close(dir_hdl);
		STOPIF_CODE_ERR(i == -1 && !status, errno,
				"cannot close dirhandle");
	}
	DEBUGP("update_dir reports %d new found, status %d", nr_new, status);
	return status;
}


/** Small helper macro for telling the user that the file is damaged. */
#define TREE_DAMAGED(condition, ...)                           \
	STOPIF_CODE_ERR( condition, EINVAL,                          \
			"!The entries file seems to be damaged -- \n"            \
			"  %s.\n"                                                \
			"\n"                                                     \
			"Please read the users@ mailing list.\n"                 \
			"  If you know what you're doing you could "             \
			"try using 'sync-repos'\n"                               \
			"  (but please _read_the_documentation_!)\n"             \
			"  'We apologize for the inconvenience.'",               \
			__VA_ARGS__);


/** -.
 * This may silently return -ENOENT, if the waa__open fails.
 *
 * The \a callback is called for \b every entry read; but for performance 
 * reasons the \c path parameter will be \c NULL.
 * */
int waa__input_tree(struct estat *root,
		struct waa__entry_blocks_t **blocks,
		action_t *callback)
{
	int status, waa_info_hdl=-1;
	int i, cur, first;
	unsigned count, subdirs, string_space;
	/* use a cache for directories, so that the parent can be located quickly */
	/* substitute both array with one struct estat **cache, 
	 * which runs along ->by_inode until NULL */
	ino_t parent;
	char header[HEADER_LEN];
	char *filename;
	struct estat *sts, *stat_mem;
	char *strings;
	int sts_free;
	char *dir_mmap, *dir_end, *dir_curr;
	off_t length;
	t_ul header_len;
	struct estat *sts_tmp;


	waa__entry_block.first=root;
	waa__entry_block.count=1;
	waa__entry_block.next=waa__entry_block.prev=NULL;

	length=0;
	dir_mmap=NULL;
	status=waa__open_dir(NULL, WAA__READ, &waa_info_hdl);
	if (status == ENOENT) 
	{
		status=-ENOENT;
		goto ex;
	}
	STOPIF(status, "cannot open .dir file");

	length=lseek(waa_info_hdl, 0, SEEK_END);
	STOPIF_CODE_ERR( length == (off_t)-1, errno, 
			"Cannot get length of .dir file");

	DEBUGP("mmap()ping %llu bytes", (t_ull)length);
	dir_mmap=mmap(NULL, length,
			PROT_READ, MAP_SHARED, 
			waa_info_hdl, 0);
	/* If there's an error, return it.
	 * Always close the file. Check close() return code afterwards. */
	status=errno;
	i=close(waa_info_hdl);
	STOPIF_CODE_ERR( !dir_mmap, status, "mmap failed");
	STOPIF_CODE_ERR( i, errno, "close() failed");

	dir_end=dir_mmap+length;

	TREE_DAMAGED( length < (HEADER_LEN+5) || 
			dir_mmap[HEADER_LEN-1] != '\n' || 
			dir_mmap[HEADER_LEN-2] != '$',
			"the header is not correctly terminated");

	/* Cut $ and beyond. Has to be in another buffer, as the file's
	 * mmap()ed read-only. */
	memcpy(header, dir_mmap, HEADER_LEN-2);
	header[HEADER_LEN-2]=0;
	status=sscanf(header, waa__header_line,
			&i, &header_len,
			&count, &subdirs, &string_space,
			&max_path_len);
	DEBUGP("got %d header fields", status);
	TREE_DAMAGED( status != 6,
			"not all needed header fields could be parsed");
	dir_curr=dir_mmap+HEADER_LEN;

	TREE_DAMAGED( i != WAA_VERSION || header_len != HEADER_LEN, 
			"the header has a wrong version");

	/* For progress display */
	approx_entry_count=count;

	/* for new subdirectories allow for some more space.
	 * Note that this is not clean - you may have to have more space
	 * than that for large structures!
	 */
	max_path_len+=1024;

	DEBUGP("reading %d subdirs, %d entries, %d bytes string-space",
			subdirs, count, string_space);


	/* Isn't there a snscanf() or something similar? I remember having seen
	 * such a beast. There's always the chance of a damaged file, so 
	 * I wouldn't depend on sscanf staying in its buffer.
	 *
	 * I now check for a \0\n at the end, so that I can be sure 
	 * there'll be an end to sscanf. */
	TREE_DAMAGED( dir_mmap[length-2] != '\0' || dir_mmap[length-1] != '\n',
			"the file is not correctly terminated");

	DEBUGP("ok, found \\0 or \\0\\n at end");

	STOPIF( hlp__alloc( &strings, string_space), NULL);
	root->strings=strings;

	/* read inodes */
	cur=0;
	sts_free=1;
	first=1;
	/* As long as there should be entries ... */
	while ( count > 0)
	{
		DEBUGP("curr=%p, end=%p, count=%d",
				dir_curr, dir_end, count);
		TREE_DAMAGED( dir_curr>=dir_end, 
				"An entry line has a wrong number of entries");

		if (sts_free == 0)
		{
			/* In all situations I can think about this will simply
			 * result in a big calloc, as at this time no block will
			 * have been freed, and the freelist will be empty. */
			STOPIF( ops__allocate(count, &stat_mem, &sts_free), NULL );
			/* This block has to be updated later. */
			STOPIF( waa__insert_entry_block(stat_mem, sts_free), NULL);
		}

		sts_free--;
		count--;

		sts=first ? root : stat_mem+cur;

		DEBUGP("about to parse %p = '%-.40s...'", dir_curr, dir_curr);
		STOPIF( ops__load_1entry(&dir_curr, sts, &filename, &parent), NULL);

		/* Should this just be a BUG_ON? To not waste space in the release 
		 * binary just for people messing with their dir-file?  */
		TREE_DAMAGED( (parent && first) ||
				(!parent && !first) ||
				(parent && parent-1>cur), 
				"the parent pointers are invalid");

		if (first) first=0;
		else cur++;

		/* First - set all fields of this entry */
		strcpy(strings, filename);
		sts->name=strings;
		strings += strlen(filename)+1;
		BUG_ON(strings - root->strings > string_space);

		if (parent)
		{
			if (parent == 1) sts->parent=root;
			else
			{
				i=parent-2;
				BUG_ON(i >= cur);
				sts->parent=stat_mem+i;
			}


			sts->parent->by_inode[ sts->parent->child_index++ ] = sts;
			BUG_ON(sts->parent->child_index > sts->parent->entry_count,
					"too many children for parent");

			/* Check the revision */
			if (sts->repos_rev != sts->parent->repos_rev)
			{
				sts_tmp=sts->parent;
				while (sts_tmp && !sts_tmp->other_revs)
				{
					sts_tmp->other_revs = 1;
					sts_tmp=sts_tmp->parent;
				}
			}
		} /* if parent */

		/* if it's a directory, we need the child-pointers. */
		if (S_ISDIR(sts->st.mode))
		{
			/* if it had children, we need to read them first - so make an array. */
			if (sts->entry_count)
			{
				STOPIF( hlp__alloc( &sts->by_inode,
							sizeof(*sts->by_inode) * (sts->entry_count+1)), NULL);
				sts->by_inode[sts->entry_count]=NULL;
				sts->child_index=0;
			}
		}

		if (callback)
			STOPIF( callback(sts), NULL);
	} /* while (count)  read entries */


ex:
	/* Return the first block even if we had eg. ENOENT */
	if (blocks)
		*blocks=&waa__entry_block;

	if (dir_mmap)
	{
		i=munmap(dir_mmap, length);
		if (!status)
			STOPIF_CODE_ERR(i, errno, "munmap() failed");
	}

	return status;
}


/** Check whether the conditions for update and/or printing the directory
 * are fulfilled.
 *
 * A directory has to be printed
 * - when it is to be fully processed (and not only walked through 
 *   because of some children),
 * - and either
 *   - it has changed (new or deleted entries), or
 *   - it was freshly added.
 *
 * */
static inline int waa___check_dir_for_update(struct estat *sts)
{
	int status;


	status=0;

	if (!sts->do_this_entry) goto ex;

	/* If we have only do_a_child set, we don't update the directory - 
	 * so the changes will be found on the next commit. */
	/* If this directory has changed, check for new files. */
	/* If this entry was replaced, it must not have been a directory 
	 * before, so ->entry_count is defined as 0 (see ops__load_1entry()).
	 * For replaced entries which are _now_ directories we'll always
	 * get here, and waa__update_dir() will give us the children. */
	if ((sts->entry_status || 
			 (opt__get_int(OPT__CHANGECHECK) & CHCHECK_DIRS) || 
			 (sts->flags & RF_ADD) ||
			 (sts->flags & RF_CHECK) ) &&
			ops__are_children_interesting(sts) &&
			action->do_update_dir)
	{

		DEBUGP("dir_to_print | CHECK for %s", sts->name);
		STOPIF( waa__update_dir(sts), NULL);

		/* Now the status could have changed, and therefore the filter might 
		 * now apply.  */
		ops__calc_filter_bit(sts);
	}

	/* Whether to do something with this directory or not shall not be 
	 * decided here. Just pass it on. */
	if (ops__allowed_by_filter(sts))
		STOPIF( ac__dispatch(sts), NULL);

ex:
	return status;
}


/** Does an update on the specified directory, and checks for completeness.
 * We get here if all \b known children have been loaded, and have to look 
 * whether the subchildren are finished, too.
 *  */
int waa___finish_directory(struct estat *sts)
{
	int status;
	struct estat *walker;


	status=0;

	walker=sts;
	while (1)
	{
		DEBUGP("checking directory %s: %u unfini, %d of %d (%s)", 
				walker->name,
				walker->unfinished,
				walker->child_index, walker->entry_count,
				st__status_string(walker));

		if (walker->unfinished > 0) break;

		/* This (parent) might not be finished yet; but don't discard empty 
		 * directories (should be only on first loop invocation - all other 
		 * entries *have* at least a single child). */
		if (walker->entry_count == 0)
			BUG_ON(walker != sts);
		else if (walker->child_index < walker->entry_count)
			break;

		DEBUGP("walker=%s; status=%s",
				walker->name, 
				st__status_string_fromint(walker->entry_status));

		if (!TEST_PACKED(S_ISDIR, walker->local_mode_packed) ||
				(walker->entry_status & FS_REPLACED) == FS_REMOVED)
		{
			/* If 
			 * - it got replaced by another type, or
			 * - the directory doesn't exist anymore,
			 * we have already printed it. */
		}
		else if (!(opt__get_int(OPT__FILTER) & FS_NEW))
		{
			/* If new entries are not wanted, we simply do the callback - if it 
			 * matches the users' wishes.  */
			if (ops__allowed_by_filter(walker))
				STOPIF( ac__dispatch(walker), NULL);
		}
		else
		{
			/* Check the parent for added entries.  Deleted entries have already 
			 * been found missing while running through the list. */
			STOPIF( waa___check_dir_for_update(walker), NULL);
			/* We increment the unfinished value, so that this entry won't be 
			 * done again. */
			walker->unfinished+=0x1000;
		}


		/* This directory is done, tell the parent. */
		walker=walker->parent;
		if (!walker) break;


		DEBUGP("%s has a finished child, now %d unfinished", 
				walker->name, walker->unfinished);

		/* We must not decrement if we don't count them. */
		if (walker->unfinished)
			walker->unfinished--;
	}

	if (walker == sts->parent && walker)
		DEBUGP("deferring parent %s/%s (%d unfinished)", 
				walker->name, sts->name, walker->unfinished);

ex:
	return status;
}


/** -.
 *
 * On input we expect a tree of nodes starting with \a root; the entries 
 * that need updating have estat::do_userselected set, and their children 
 * get marked via ops__set_todo_bits().
 *
 * On output we have estat::entry_status set; and the current
 * \ref actionlist_t::local_callback "action->local_callback" gets called.
 *
 * It's not as trivial to scan the inodes in ascending order as it was when 
 * this part of code was included in 
 * \c waa__input_tree(); but we get a list of <tt>(location, number)</tt> 
 * blocks to run through, so it's the same performance-wise.
 *
 * This function \b consumes the list of entry blocks, ie. it destroys
 * their data - the \a first pointer gets incremented, \a count 
 * decremented.
 *
 * <h3>Threading</h3>
 * We could use several threads, to get more that one \c lstat() to run at
 * once. I have done this and a patch available, but testing on linux/x86 on 
 * ext3 seems to readahead the inodes, so the wall time got no shorter.
 *
 * If somebody wants to test with threads, I'll post the patch.
 *
 * For threading there has to be some synchronization - an entry can be done
 * only if its parent has been finished. That makes sense insofar, as when
 * some directory got deleted we don't need to \c lstat() the children - they
 * must be gone, too.
 *
 * <h3>KThreads</h3>
 * On LKML there was a discussion about making a list of syscalls, for 
 * getting them done without user/kernel switches. (About 2007 or so?  
 * Don't even know whether that was merged in the end.)
 *
 * On cold caches this won't really help, I think; but I didn't test 
 * whether that would help for the hot-cache case.
 *
 * <h3>Design</h3>
 * <ol>
 * <li>If the parent of the current entry is removed, this entry is too; 
 * skip the other checks.
 * <li>Check current status.
 * <li>All entries that are not a directory \b now can be printed 
 * immediately; decrement parent's \c unfinished counter.
 * <li>Directory entries<ul>
 *   <li>These increment their parent's \c unfinished value, as they might 
 *   have children to do.
 *   <li>If they have \b no known entries (have been empty) they may get 
 *   checked for changes (\c waa__update_dir()), and are finished - 
 *   decrement parent's \c unfinished counter.
 *   <li>Else they wait for their \c child_index value to reach the \c 
 *   entry_count number; then, as soon as their \c unfinished value gets 
 *   zero, they're really done.
 *   </ul>
 * <li>If a directory has no more \c unfinished entries, it can be checked 
 * for changes, and is finished - decrement parent's \c unfinished counter.
 * </ol>
 *
 * The big obstacle is that arbitrary (sub-)paths might be wanted by the 
 * user; so we have to take great care about the child-counting.
 * */
int waa__update_tree(struct estat *root,
		struct waa__entry_blocks_t *cur_block)
{
	int status;
	struct estat *sts;


	if (! (root->do_userselected || root->do_child_wanted) )
	{
		/* If neither is set, waa__partial_update() wasn't called, so
		 * we start from the root. */
		root->do_userselected = 
			root->do_this_entry =
			root->do_filter_allows_done =
			root->do_filter_allows = 1;
		DEBUGP("Full tree update");
	}

	/* TODO: allow non-remembering behaviour */
	action->keep_children=1;

	status=0;
	while (cur_block)
	{
		/* For convenience */
		sts=cur_block->first;
		DEBUGP("doing update for %s ... %d left in %p",
				sts->name, cur_block->count, cur_block);

		/* For directories initialize the child counter.
		 * We don't know the current type yet! */
		if (S_ISDIR(sts->st.mode))
			sts->child_index = sts->unfinished = 0;

		/* If the entry was just added, we already set its estat::st and filter 
		 * bits. */
		if (!(sts->flags & RF_ISNEW))
			STOPIF( ops__update_filter_set_bits(sts), NULL);

		if (!(sts->do_this_entry || sts->do_child_wanted))
			goto next;


		/* Now sts->local_mode_packed has been set. */
		if (sts->entry_status) 
			ops__mark_parent_cc(sts, entry_status);

		if (sts->parent)
		{
			if (TEST_PACKED(S_ISDIR, sts->old_rev_mode_packed))
				sts->parent->unfinished++;

			if (sts->parent->entry_status & FS_REMOVED)
				goto next;
		}

		if (sts->entry_status & FS_REMOVED) 
		{
			if (sts->parent)
			{
				/* If this entry is removed, the parent has changed. */
				sts->parent->entry_status &= (~FS_LIKELY);
				sts->parent->entry_status |= FS_CHANGED;
				/* The FS_CHILD_CHANGED markings are already here. */
			}

			/* If a directory is removed, we don't allocate the by_inode
			 * and by_name arrays, and it is set to no child-entries. */
			if (TEST_PACKED(S_ISDIR, sts->old_rev_mode_packed) && 
					!action->keep_children)
				sts->entry_count=0;


			/* One worry less for the parent. */
			if (TEST_PACKED(S_ISDIR, sts->old_rev_mode_packed))
//			if (S_ISDIR(sts->st.mode))
//			if (TEST_PACKED(S_ISDIR, sts->local_mode_packed))
				sts->parent->unfinished--;
		}


		if (S_ISDIR(PACKED_to_MODE_T(sts->local_mode_packed)) && 
				(sts->entry_status & FS_REPLACED) == FS_REPLACED)
		{
			/* This entry was replaced, ie. was another type before, and is a 
			 * directory *now*.
			 *
			 * So the shared members (entry_count, by_inode) have wrong data.
			 * We have to correct that here.
			 *
			 * That causes a call of waa__update_dir(), which is exactly what we 
			 * want. */
			DEBUGP("new directory %s", sts->name);
			sts->entry_count=0;
			sts->unfinished=0;
			sts->by_inode=sts->by_name=NULL;
			sts->strings=NULL;
			/* TODO: fill this members from the ignore list */
			// sts->active_ign=sts->subdir_ign=NULL;
		}


next:
		/* This is more or less the same as below, only for this entry and not 
		 * its parent. */
		if (TEST_PACKED(S_ISDIR, sts->local_mode_packed) && 
				sts->entry_count==0)
		{
			DEBUGP("doing empty directory %s %d", sts->name, sts->do_this_entry);
			/* Check this entry for added entries. There cannot be deleted 
			 * entries, as this directory had no entries before. */
			STOPIF( waa___finish_directory(sts), NULL);
		}


		/* If this is a normal entry *now*, we print it.
		 * Non-empty directories are shown after all child nodes have been 
		 * checked. */
		if (!TEST_PACKED(S_ISDIR, sts->local_mode_packed) && 
				sts->do_this_entry) 
			STOPIF( ac__dispatch(sts), NULL);


		/* The parent must be done *after* the last child node ... at least 
		 * that's what's documented above :-) */
		/* If there's a parent, and it's still here *or* we have to remember 
		 * the children anyway ... */
		if (sts->parent && action->keep_children )
		{
			sts->parent->child_index++;

			/* If we did the last child of a directory ... */
			if (sts->parent->child_index >= sts->parent->entry_count 
					&& sts->parent->do_this_entry)
			{
				DEBUGP("checking parent %s/%s", sts->parent->name, sts->name);
				/* Check the parent for added entries. 
				 * Deleted entries have already been found missing while 
				 * running through the list. */

				STOPIF( waa___finish_directory(sts->parent), NULL);
			}
			else
				DEBUGP("deferring parent %s/%s%s: %d of %d, %d unfini", 
						sts->parent->name, sts->name,
						sts->parent->do_this_entry ? "" : " (no do_this_entry)",
						sts->parent->child_index, sts->parent->entry_count, 
						sts->parent->unfinished);
		}


		/* Sadly there's no continue block, like in perl.
		 * Advance the pointers. */
		cur_block->first++;
		cur_block->count--;
		if (cur_block->count <= 0)
		{
			/* We should possibly free this memory, but as there's normally only 1
			 * struct allocated (the other declared static) we'd save about 16 bytes. */
			cur_block=cur_block->next;
		}
	}


ex:
	return status;
}


/** -.
 *
 * \a argc and \a normalized tell which entries should be updated.
 *
 * We return the \c -ENOENT from waa__input_tree() if <b>no working 
 * copy</b> could be found. \c ENOENT is returned for a non-existing entry 
 * given on the command line.
 *
 * The \a callback is called for \b every entry read by waa__input_tree(), 
 * not filtered like the normal actions.
 */
int waa__read_or_build_tree(struct estat *root, 
		int argc, char *normalized[], char *orig[],
		action_t *callback,
		int return_ENOENT)
{
	int status;
	struct waa__entry_blocks_t *blocks;


	status=0;
	status=waa__input_tree(root, &blocks, callback);
	DEBUGP("read tree = %d", status);

	if (status == -ENOENT)
	{
		/* Some callers want to know whether we *really* know these entries. */
		if (return_ENOENT) 
			return -ENOENT;
	}
	else 
		STOPIF( status, NULL);

	if (opt__get_int(OPT__PATH) == PATH_CACHEDENVIRON)
		STOPIF( hlp__match_path_envs(root), NULL);

	/* Do update. */
	STOPIF( waa__partial_update(root, argc, normalized, 
				orig, blocks), NULL);

	/* In case we're doing commit or something with progress report,
	 * uninit the progress. */
	if (action->local_uninit)
		STOPIF( action->local_uninit(), NULL);

ex:
	return status;
}


/** -.
 *
 * This function calculates the common root of the given paths, and tries
 * to find a working copy base there (or above).
 * It returns the paths of the parameters relative to the base found.
 *
 * Eg.: for \c /a/wc/sub/sub2 and \c /a/wc/file it returns
 * - \c base = \c /a/wc
 * - \c normalized[0] = \c sub/sub2
 * - \c normalized[1] = \c file
 *
 * We have to find a wc root before we can load the entries file; so we'd
 * have to process the given paths twice, possibly each time by prepending
 * the current working directory and so on; that's why this function returns
 * a block of relative path pointers. These have just to be walked up to the
 * root to process them (eg. mark for processing).
 *
 *
 * \c *normalized should be \c free()d after use; but as the converted 
 * arguments are all allocated one by one it won't help that much.
 *
 * \note In case \b no matching base is found, the common part of the paths 
 * is returned as base, and the paths are normalized relative to it. \c 
 * ENOENT is returned.
 * \todo Should that be changed to base="/"?
 * 
 *
 * If we get \b no parameters, we fake the current working directory as 
 * parameter and return it in \c normalized[0]. \c argc in the caller will 
 * still be \c 0!
 *
 * - If we have a dir-file, we look only from the current directory below - 
 *   so fake a parameter.
 * - If we have no dir-file:
 *   - If we find a base, we fake a parameter and show only below.
 *   - If we find no base, we believe that we're at the root of the wc.
 *
 * The parameter must not be shown as "added" ("n...") - because it isn't.
 *
 * For the case that the WC root is \c "/", and we shall put a \c "./" in 
 * front of the normalized paths, we need an additional byte per argument, 
 * so that eg. \c "/etc" can be changed to \c "./etc" - see the PDS 
 * comments.
 *
 * In order to correctly handle cases like \c 
 * "/symlink/to/a/directory/subd/file", we do a realpath() call of the \b 
 * directory of the argument (with \c ".../" assumed to be \c ".../."), and 
 * use that as base path for the filename.
 * */
int waa__find_common_base2(int argc, char *args[], 
		char ***normalized,
	 	int flags)
{
	int status, i, j;
	int len;
	char *cp, *confname;
	char *paths[argc], *base_copy;
	char **norm;
	char *nullp[2];
	char *last_ps;
	const char *path2copy, *basepath2copy;
	/* A bit long, but it doesn't really matter whether it's on the stack or 
	 * the heap. */
	char canon[PATH_MAX];
	static const char ps[]={PATH_SEPARATOR, 0};
	int fnlen;


	status=0;
	norm=NULL;

	/* Step 0: Special case for *no* arguments. */
	if (argc == 0)
	{
		argc=1;
		nullp[0]=start_path;
		nullp[1]=NULL;
		args=nullp;
		DEBUGP("faked a single parameter to %s", *args);
	}


	/* Step 1: Allocation.
	 * We need (argc || 1) pointers (plus a NULL) and the base path.
	 * The relative paths are done in-place; we waste a bit of memory, but 
	 * there won't be that many arguments normally.
	 * */
	len = argc * sizeof(char*) + sizeof(NULL);
	STOPIF( hlp__alloc( &norm, len), NULL);


	/* Step 2: Get the real path of all filenames, and store them.
	 * Delimiters are \0. */
	len=0;
	status=0;
	for(i=0; i<argc; i++)
	{
		last_ps=strrchr(args[i], PATH_SEPARATOR);

		if (!last_ps)
		{
only_rel_path:
			/* Only a filename (in the current directory), no path, so no parent 
			 * directory check needed.
			 * Or, second case: relative path, no check wanted. */
			fnlen=start_path_len+1+strlen(args[i])+1;
			path2copy=args[i];
			basepath2copy=start_path;
		}
		else if (flags & FCB__NO_REALPATH)
		{
			/* Don't check for existence, just use hlp__pathcopy. */
			if (args[i][0] != PATH_SEPARATOR) goto only_rel_path;

			fnlen=strlen(args[i])+1;
			basepath2copy=ps;
			path2copy=args[i];
		}
		else if (last_ps == args[i])
		{
			/* File below the root, eg. "/bin" */
			fnlen=strlen(args[i])+1;
			basepath2copy=ps;
			path2copy=args[i];
		}
		else if (last_ps)
		{
			/* Filename with some path given. */
			*last_ps=0;
			status=realpath(args[i], canon) ? 0 : errno;
			/* Do we need to ignore ENOENT? */
			STOPIF( status, "realpath(%s)", args[i]);
			*last_ps=PATH_SEPARATOR;

			fnlen=strlen(canon)+1+strlen(last_ps+1)+1;
			BUG_ON(fnlen >= PATH_MAX, "path longer than PATH_MAX");

			path2copy=last_ps;
			basepath2copy=canon;
		}
		else
			BUG_ON(1);

		/* +1 because of PDS, both times. */
		STOPIF( hlp__alloc( paths+i, fnlen+1), NULL);
		paths[i]++;
		hlp__pathcopy(paths[i], &j, basepath2copy, ps, path2copy, NULL);

		if (len<j) len=j;
		while (len > 1 && paths[i][len-1] == PATH_SEPARATOR)
			paths[i][--len]=0;
		DEBUGP("got argument #%d as %s[%d]", i, paths[i], len);
	}


	/* Step 3: find the common base. */
	/* len always points to the *different* character (or to \0). */
	len=strlen(paths[0]);
	for(i=1; i<argc; i++)
	{
		DEBUGP("len before #%d is %d", i, len);
		for(j=0; j<len; j++)
			/* Shorten the common part ? */
			if (paths[i][j] != paths[0][j])
				len=j;
	}
	DEBUGP("len after is %d", len);

	/* Now [len] could be here:
	 *                   |             |
	 *    /fff/aaa/ggg/ggsfg          /a/b/c
	 *    /fff/aaa/ggg/ggtfg          /d/e/f
	 *
	 *                 |                     |
	 *    /fff/aaa/ggg/ggsfg          /a/b/c/d
	 *    /fff/aaa/ggg/agtfg
	 *    /fff/aaa/ggg/agt2g
	 *
	 *
	 * Or, in an invalid case:
	 *    |
	 *    hsh
	 *    zf
	 *
	 * We look for the first path separator before, and cut there.  */
	/* We must not always simply cut the last part off.
	 * 
	 * If the user gives *only* the WC as parameter (wc/), we'd not find the
	 * correct base!
	 * 
	 * But wc/1a, wc/1b, wc/1c must be cut to wc/.  So we look whether
	 * there's a PATH_SEPARATOR or a \0 after the current position.  */
	/* Note: Change for windows paths would be necessary, at least if the
	 * paths there are given as C:\X and D:\Y.
	 * 
	 * Using \\.\devices\Harddrive0\Partition1\... or similar we could avoid
	 * that.  */
	if (paths[0][len] == PATH_SEPARATOR ||
			paths[0][len] == 0)
	{
		DEBUGP("Is a directory, possible a wc root.");
	}
	else
	{
		DEBUGP("Reverting to next %c", PATH_SEPARATOR);
		/* Walk off the different character. */
		len--;
		/* And look for a PATH_SEPARATOR. */
		while (paths[0][len] != PATH_SEPARATOR && len >0)
			len--;
	}

	BUG_ON(len < 0, "Paths not even equal in separator - "
			"they have nothing in common!");

	/* paths[0][0] == PATH_SEPARATOR is satisfied by both branches above. */
	if (len == 0)
	{
		/* Special case - all paths are starting from the root. */
		len=1;
		DEBUGP("we're at root.");
	}

	STOPIF( hlp__strnalloc(len, &base_copy, paths[0]), NULL);
	DEBUGP("starting search at %s", base_copy);


	/* Step 4: Look for a wc.
	 * The given value could possible describe a file (eg. if the only 
	 * argument is its path) - we have to find a directory. */
	while (1)
	{
		/* We cannot look for the entry file, because on the first commit it
		 * doesn't exist.
		 * A wc is defined by having an URL defined. */
		DEBUGP("looking for %s", base_copy);
		status=waa__open(base_copy, NULL, 0, 0);

		/* Is there a base? */
		if (!status) break;

		if (len <= 1) break;

		base_copy[len]=0;
		cp=rindex(base_copy, PATH_SEPARATOR);
		if (cp)
		{
			/* If we're at "/", don't delete the root - try with it, and stop. */
			if (cp == base_copy)
				cp++;

			*cp=0;
			len=cp - base_copy;
		}
	}

	DEBUGP("after loop is len=%d, base=%s, and status=%d",
			len, base_copy, status);

	/* Now status is either 0, or eg. ENOENT - just what we'd like to return.
	 * But do that silently. 
	 *
	 * Note: if there's *no* base found, we take the common path. */
	STOPIF( status, "!Couldn't find a working copy with matching base.");

	/* We hope (?) that the action won't overwrite these strings. */
	wc_path=base_copy;
	wc_path_len=len;

	DEBUGP("found working copy base at %s", wc_path);
	STOPIF_CODE_ERR( chdir(wc_path) == -1, errno, "chdir(%s)", wc_path);

	setenv(FSVS_EXP_WC_ROOT, wc_path, 1);


	/* Step 5: Generate pointers to normalized paths.
	 * len is still valid, so we just have to use paths[i]+len. */
	for(i=0; i<argc; i++)
	{
		DEBUGP("path is %s", paths[i]);
		/* If the given parameter equals *exactly* the wc root, we'd jump off 
		 * with that +1. So return . for that case. */
		if (paths[i][len] == 0)
			norm[i] = ".";
		else
		{
			if (len == 1)
				/* Special case for start_path="/". */
				norm[i]=paths[i]+1;
			else
				norm[i]=paths[i]+len+1;

			/* PDS: norm[i] points after a PATH_SEPARATOR, and we have always space 
			 * for the "." in front.  */
			if (flags & FCB__PUT_DOTSLASH)
			{
				norm[i]-=2;
				*norm[i] = '.';
			}
		}
		DEBUGP("we set norm[%d]=%s from %s", i,  norm[i], paths[i]);
	}
	norm[argc]=NULL;


	/* Step 6: Read wc-specific config file.
	 * It might be prettier to name the file "Conf" or such, so that we can 
	 * directly use the waa__open() function; but then it couldn't be copied 
	 * from other locations. (And the config read function would have to 
	 * accept a filehandle.) */
	STOPIF( waa__get_waa_directory( wc_path, &confname, &cp, NULL, GWD_CONF),
			NULL);
	setenv( FSVS_EXP_WC_CONF, confname, 1);
	STOPIF( opt__load_settings(confname, "config", PRIO_ETC_WC ), NULL);


	/* If this command is not filtered, or an invalid filter is defined, 
	 * change filter to output all entries.
	 *
	 * The config file can be parsed only after we know what our base 
	 * directory is ... so this can't be done directly after command line 
	 * parsing.
	 * waa__get_waa_directory() wouldn't be enough - some commands, like 
	 * export, don't use it - but only 2 actions currently allow filtering: 
	 * status, and commit. And both get here. */
	if (!action->only_opt_filter ||
			opt__get_int(OPT__FILTER) == 0) 
		opt__set_int(OPT__FILTER, PRIO_MUSTHAVE, FILTER__ALL);

	DEBUGP("filter has mask 0x%X (%s)", 
			opt__get_int(OPT__FILTER),
			st__status_string_fromint(opt__get_int(OPT__FILTER)));


ex:
	if (status && status!=ENOENT)
	{
		/* Free only if error encountered */
		IF_FREE(norm);
	}
	else 
	{
		/* No problems, return pointers. */
		if (normalized)
			*normalized=norm;
	}

	return status;
}


/** -.
 *
 * We get a tree starting with \a root, and all entries from \a normalized 
 * get estat::do_userselected and estat::do_this_entry set. These flag gets 
 * used by waa__update_tree().
 * */
int waa__partial_update(struct estat *root, 
		int argc, char *normalized[], char *orig[],
		struct waa__entry_blocks_t *blocks)
{
	int status;
	struct estat *sts;
	int i, flags, ign;
	int faked_arg0;


	status=0;

	/* If the user gave no path argument to the action, the current directory 
	 * is faked into the first path, but without changing argc. (Some actions  
	 * want to know whether *any* path was given). */
	faked_arg0=(argc == 0 && *normalized);
	/* Not fully correct - we fake now, haven't faked ;-) */
	if (faked_arg0) argc=1;

	for(i=0; i<argc; i++)
	{
		DEBUGP("update %d=%s", i, normalized[i]);
		/* The given entry must either exist in the filesystem (then we'd 
		 * create it in the entry list, if necessary), or it must be already in 
		 * the list.
		 *
		 * So a removed entry must have been known, a new entry can be added.
		 * But a non-existing, unknown entry gives an error. */
		status=hlp__lstat(normalized[i], NULL); 

		if (abs(status) == ENOENT) 
			flags=OPS__ON_UPD_LIST | OPS__FAIL_NOT_LIST;
		else 
		{
			STOPIF( status, "Cannot query entry %s", normalized[i]);
			flags = OPS__ON_UPD_LIST | OPS__CREATE;
		}

		status=ops__traverse(root, normalized[i], flags, RF_ADD, &sts);
		if (status == ENOENT)
		{
			STOPIF_CODE_ERR( !(flags & OPS__CREATE), ENOENT,
					"!Entry '%s' is not known.", normalized[i]);
			BUG_ON(1);
		}
		else
			STOPIF(status, NULL);

		/* Remember which argument relates to this entry. */
		if (opt__get_int(OPT__PATH) == PATH_PARMRELATIVE && !sts->arg)
			sts->arg= faked_arg0 ? "" : orig[i];

		/* This entry is marked as full, parents as "look below". */
		sts->do_userselected = sts->do_this_entry = 1;
		/* Set auto-props as needed. See
		 * http://fsvs.tigris.org/ds/viewMessage.do?dsForumId=3928&dsMessageId=2981798 
		 * */
		STOPIF(prp__sts_has_no_properties(sts, &ign), NULL);
		if (ign)
			STOPIF( ign__is_ignore(sts, &ign), NULL);
		while ( 1 )
		{
			/* This new entry is surely updated.
			 * But what about its (new) parents?
			 * They're not in the blocks list (that we get as parameter), so
			 * they'd get wrong information on commit. */

			/* Without the 2nd parameter sts->st might not get set, depending on 
			 * action->overwrite_sts_st (implemented in another branch). */
			if (sts->flags & RF_ISNEW)
			{
				STOPIF( ops__update_single_entry(sts, &sts->st), NULL);
				sts->entry_status=FS_NEW;
				ops__calc_filter_bit(sts);
			}

			sts = sts->parent;
			if (!sts) break;

			sts->do_child_wanted = 1;
		}
	}

	STOPIF( waa__update_tree(root, blocks), NULL);

ex:
	return status;
}


/** -. */
int waa__new_entry_block(struct estat *entry, int count, 
		struct waa__entry_blocks_t *previous)
{
	int status;
	struct waa__entry_blocks_t *eblock;


	status=0;
	STOPIF( hlp__alloc( &eblock, sizeof(*eblock)), NULL);
	eblock->first=entry;
	eblock->count=count;

	/* The block is appended after the given block.
	 * - The root node is still the first entry.
	 * - We need not go to the end of the list, we have O(1). */
	eblock->next=previous->next;
	eblock->prev=previous;
	previous->next=eblock;
	if (eblock->next)
		eblock->next->prev=eblock;

ex:
	return status;
}


/** -.
 * */
int waa__find_base(struct estat *root, int *argc, char ***args)
{
	int status;
	char **normalized;

	status=0;
	/* Per default we use (shortened) per-wc paths, as there'll be no 
	 * arguments. */
	root->arg="";

	STOPIF( waa__find_common_base( *argc, *args, &normalized), NULL);
	if (*argc > 0 && strcmp(normalized[0], ".") == 0)
	{
		/* Use it for display, but otherwise ignore it. */
		root->arg = **args;

		(*args) ++;
		(*argc) --;
	}

	STOPIF_CODE_ERR( *argc, EINVAL,
			"!Only a working copy root is a valid path.");

	/* Return the normalized value */
	**args = normalized[0];

ex:
	return status;
}


/** Abbreviation function for tree recursion. */
static inline int waa___recurse_tree(struct estat **list, action_t handler, 
		int (*me)(struct estat *, action_t ))
{
	struct estat *sts;
	int status;

	status=0;
	while ( (sts=*list) )
	{
		if (sts->do_this_entry && ops__allowed_by_filter(sts))
			STOPIF( handler(sts), NULL);

		/* If the entry was removed, sts->updated_mode is 0, so we have to take 
		 * a look at the old sts->st.mode to determine whether it was a 
		 * directory. */
		/* The OPT__ALL_REMOVED check is duplicated from ac__dispatch, to avoid 
		 * recursing needlessly. */
		if ((sts->do_child_wanted || sts->do_userselected) && 
				sts->entry_count &&
				(sts->local_mode_packed ? 
				 TEST_PACKED(S_ISDIR, sts->local_mode_packed) :
				 ((sts->entry_status & FS_REMOVED) && 					
					S_ISDIR(sts->st.mode) &&
					opt__get_int(OPT__ALL_REMOVED)==OPT__YES)) )
/*		if (TEST_PACKED( S_ISDIR, sts->local_mode_packed) &&
				(sts->do_child_wanted || sts->do_userselected) && 
				sts->entry_count &&
				((opt__get_int(OPT__ALL_REMOVED)==OPT__YES) ||
				 ((sts->entry_status & FS_REPLACED) != FS_REMOVED)) ) */
			STOPIF( me(sts, handler), NULL);
		list++;
	}

ex:
	return status;
}


/** -.
 * */
int waa__do_sorted_tree(struct estat *root, action_t handler)
{
	int status;

	status=0;

	/* Do the root as first entry. */
	if (!root->parent && root->do_this_entry)
		STOPIF( handler(root), NULL);

	if ( !root->by_name)
		STOPIF( dir__sortbyname(root), NULL);

	STOPIF( waa___recurse_tree(root->by_name, handler, 
				waa__do_sorted_tree), NULL);

ex:
	IF_FREE(root->by_name);

	return status;
}


/** -.
 *
 * The cwd is the directory to be looked at.
 *
 * IIRC the inode numbers may change on NFS; but having the WAA on NFS 
 * isn't a good idea, anyway.
 * */
int waa__dir_enum(struct estat *this,
		int est_count,
		int by_name)
{
	int status;
	struct sstat_t cwd_stat;


	status=0;
	STOPIF( hlp__lstat(".", &cwd_stat), NULL);

	DEBUGP("checking: %llu to %llu",
			(t_ull)cwd_stat.ino,
			(t_ull)waa_stat.ino);
	/* Is the parent the WAA? */
	if (cwd_stat.dev == waa_stat.dev &&
			cwd_stat.ino == waa_stat.ino)
		goto ex;

	/* If not, get a list. */
	STOPIF( dir__enumerator(this, est_count, by_name), NULL);

ex:
	return status;
}


static struct estat **to_append;
static int append_count;
int remember_to_copy(struct estat *sts, struct estat **sts_p)
{
	char *path;
	ops__build_path(&path, sts);
	DEBUGP("copy %s", path);
	to_append[append_count]=sts;
	append_count++;
	return 0;
}


/** -.
 *
 * \a dest must already exist; its name is \b not overwritten, as it is 
 * (usually) different for the copy base entry.
 *
 * Existing entries of \a dest are not replaced or deleted;
 * other entries are appended, with a status of \c FS_REMOVED. 
 *
 * This works for both directory and non-directory entries. */
int waa__copy_entries(struct estat *src, struct estat *dest)
{
	int status;
	struct estat *newdata, **tmp, **_old_to_append;
	int left, space;
	int _old_append_count;


	_old_append_count = append_count;
	_old_to_append = to_append;

	to_append=NULL;
	append_count = 0;
	status=0;

	ops__copy_single_entry(src, dest);
	if (!S_ISDIR(src->st.mode))
		goto ex;


	append_count=0;
	STOPIF( hlp__calloc( &to_append, 
				src->entry_count+1, sizeof(src->by_name[0])), NULL);

	STOPIF( ops__correlate_dirs( src, dest, 
				remember_to_copy, 
				waa__copy_entries, 
				NULL, NULL), NULL);

	/* Now we know how many new entries there are. */


	/* Now the data in to_append gets switched from old entry to newly 
	 * allocated entry; we count in reverse direction, to know how many 
	 * entries are left and must be allocated.  */
	/* We re-use the name string. */
	space=0;
	for( tmp=to_append, left=append_count; left>0; left--, tmp++, space--)
	{
		if (space)
			newdata++;
		else
			STOPIF( ops__allocate( left, &newdata, &space), NULL);

		newdata->parent=dest;
		newdata->name=(*tmp)->name;
		/* Copy old data, and change what's needed. */
		STOPIF( waa__copy_entries(*tmp, newdata), NULL);
		/* If URL is different from parent URL, it's a new base. */

		/* Remember new address. */
		(*tmp) = newdata;
	}

	STOPIF( ops__new_entries(dest, append_count, to_append), NULL);


ex:
	IF_FREE(to_append);
	append_count = _old_append_count ;
	to_append = _old_to_append ;
	return status;
}


/** -.
 * 
 * If \a base_dir is \c NULL, a default path is taken; else the string is 
 * copied and gets an arbitrary postfix. If \a base_dir ends in \c 
 * PATH_SEPARATOR, \c "fsvs" is inserted before the generated postfix.
 *
 * \a *output gets set to the generated filename, and must not be \c 
 * free()d. */
int waa__get_tmp_name(const char *base_dir, 
		char **output, apr_file_t **handle,
		apr_pool_t *pool)
{
	int status;
	static struct cache_t *cache;
	static struct cache_entry_t *tmp_cache=NULL;
	static const char to_append[]=".XXXXXX";
	static const char to_prepend[]="fsvs";
	char *filename;
	int len;


	STOPIF( cch__new_cache(&cache, 12), NULL);

	len= base_dir ? strlen(base_dir) : 0;
	if (!len)
	{
		if (!tmp_cache)
		{
			/* This function caches the value itself, but we'd have to store the 
			 * length ourselves; furthermore, we get a copy every time - which 
			 * fills the pool, whereas we could just use our cache.  */
			STOPIF( apr_temp_dir_get(&base_dir, pool),
					"Getting a temporary directory path");

			len=strlen(base_dir);
			/* We need an extra byte for the PATH_SEPARATOR, and a \0. */
			STOPIF( cch__entry_set( &tmp_cache, 0, base_dir, 
						len +1 +1, 0, NULL), NULL);

			tmp_cache->data[len++]=PATH_SEPARATOR;
			tmp_cache->data[len]=0;

			/* We set tmp_cache->len, which would be inclusive the alignment space 
			 * at end, to the *actual* length, because we need that on every 
			 * invocation.
			 * That works because tmp_cache is never changed again.  */
			tmp_cache->len=len;
		}

		len=tmp_cache->len;
		base_dir=tmp_cache->data;
		BUG_ON(base_dir[len] != 0);
	}

	STOPIF( cch__add(cache, 0, base_dir, 
				/* Directory PATH_SEPARATOR pre post '\0' */
				len + 1 + strlen(to_prepend) + strlen(to_append) + 1 + 3,
				&filename), NULL);

	if (base_dir[len-1] == PATH_SEPARATOR)
	{
		strcpy( filename + len, to_prepend);
		len+=strlen(to_prepend);
	}

	strcpy( filename + len, to_append);
	/* The default values include APR_DELONCLOSE, which we only want if the
	 * caller is not interested in the name. */
	STOPIF( apr_file_mktemp(handle, filename, 
				APR_CREATE | APR_READ | APR_WRITE | APR_EXCL |
				(output ? 0 : APR_DELONCLOSE),
				pool),
			"Cannot create a temporary file for \"%s\"", filename);

	if (output) *output=filename;

ex:
	return status;
}

/** -.
 * The \a dir must be absolute; this function makes an own copy, so the 
 * value will be unchanged. */
int waa__set_working_copy(const char * const wc_dir)
{
	int status;

	status=0;
	BUG_ON(*wc_dir != PATH_SEPARATOR);
	wc_path_len=strlen(wc_dir);
	STOPIF( hlp__strnalloc( wc_path_len, &wc_path, wc_dir), NULL);

ex:
	return status;
}


/** -.
 * The \a dir must be absolute; this function makes an own copy, so the 
 * value will be unchanged. */
int waa__create_working_copy(const char * const wc_dir)
{
	int status;
	char *dir;

	if (wc_dir)
		STOPIF(waa__set_working_copy(wc_dir), NULL);

	BUG_ON(!wc_path);

	/* Create the WAA base directory. */
	STOPIF( waa__get_waa_directory( wc_path, &dir, NULL, NULL, 
				GWD_WAA | GWD_MKDIR), NULL);
	STOPIF( waa__mkdir(dir, 1), NULL);

	/* Create the CONF base directory. */
	STOPIF( waa__get_waa_directory( wc_path, &dir, NULL, NULL, 
				GWD_CONF | GWD_MKDIR), NULL);
	STOPIF( waa__mkdir(dir, 1), NULL);

	/* Make an informational file to point to the base directory. */
	/* Should we ignore errors? */
	STOPIF( waa__make_info_file(wc_path, WAA__README, wc_path), NULL);

ex:
	return status;
}

