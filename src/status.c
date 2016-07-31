/************************************************************************
 * Copyright (C) 2005-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include "global.h"
#include "actions.h"
#include "status.h"
#include "helper.h"
#include "direnum.h"
#include "cache.h"
#include "url.h"
#include "ignore.h"
#include "options.h"
#include "est_ops.h"
#include "waa.h"
#include "checksum.h"
#include "url.h"


/** \file
 * Functions for \ref status reporting.
 * */

/** \addtogroup cmds
 * 
 * \section status
 *
 * \code
 * fsvs status [-C [-C]] [-v] [PATHs...]
 * \endcode
 *
 * This command shows the entries that have changed since the last commit.
 *
 * The output is formatted as follows:
 * - A status columns of four (or, with \c -v , five) characters.
 *   There are either flags or a "." printed, so that it's easily parsed by 
 *   scripts -- the number of columns is only changed by \ref 
 *   glob_opt_verb.
 * - The size of the entry, in bytes, or \c "dir" for a directory, or \c 
 *   "dev" for a device.
 * - The path and name of the entry, formatted by the option \ref 
 *   o_opt_path.
 * 
 * The status column can show the following flags:
 * - Normally only changed entries are printed; with -v all are printed.
 *   The command line option \c -v additionally causes the \c 'm' -flag to 
 *   be split into two, see below.
 * - \c 'D' and \c 'N' are used for \e deleted and \e new entries.
 * - \c 'd' and \c 'n' are used for entries which are to be unversioned or 
 *   added on the next commit; the characters were chosen as <i>little 
 *   delete</i> (only in the repository, not removed locally) and <i>little 
 *   new</i> (although \ref ignore "ignored"). See \ref add and \ref 
 *   unversion. \n
 *   If such an entry does not exist, it is marked with an \c '!' -- 
 *   because it has been manually marked, and for both types removing the 
 *   entry makes no sense.
 * - A changed type (character device to symlink, file to directory etc.)
 *   is given as \c 'R' (replaced), ie. as removed and newly added.
 * - If the entry has been modified, the change is shown as \c 'C'. \n
 *   If the modification or status change timestamps (mtime, ctime) are 
 *   changed, but the size is still the same, the entry is marked as 
 *   possibly changed (a question mark \c '?' is printed). See \ref 
 *   opt_checksum.
 * - The meta-data flag \c 'm' shows meta-data changes like properties, 
 *   modification timestamp and/or the rights (owner, group, mode); 
 *   depending on the \ref glob_opt_verb "verbose/quiet" command line 
 *   parameters, it may be splitted into \c 'P' (properties), \c 't' (time) 
 *   and \c 'p' (permissions). \n
 *   If \c 'P' is shown for the non-verbose case, it means \b only property 
 *   changes, ie. the entries filesystem meta-data is unchanged.
 * - A \c '+' is reserved for files with a copy-from history, and not 
 *   currently used.
 *
 * 
 * Here's a table with the characters and their positions:
 * \verbatim
 *   Without -v    With -v
 *     ....         ......
 *     NmC?         NtpPC?
 *     DP !         D    !
 *     R  +         R    +
 *     d            d
 *     n            n
 * \endverbatim
 *
 * Furthermore please take a look at \ref o_status_color.
 * */


static FILE *progress_output=NULL;
static int max_progress_len=0;


/** Returns the visible file size.
 * For devices the string \c dev is printed; for directories \c dir; files 
 * and symlinks get their actual size printed. */
char * st___visible_file_size(struct estat *sts)
{
  static char buffer[20];

  switch (sts->entry_type)
  {
    case FT_CDEV:
    case FT_BDEV:
      return "dev";
    case FT_DIR:
      return "dir";
    default:
      /* When in doubt, believe it's a normal file.
       * We have that case for sync-repos - could be fixed some time. */
    case FT_FILE:
    case FT_SYMLINK:
      sprintf(buffer, "%llu", (t_ull) sts->st.size);
      break;
  }

  return buffer;
}


/** Meta-data status string. */
inline char * st___meta_string(int status_bits, int flags)
{
  static char buffer[4];
	int prop;

	prop=(status_bits & FS_PROPERTIES) | (flags & RF_PUSHPROPS);

  if (opt_verbose>0)
  {
    buffer[0] = status_bits & FS_META_MTIME ?  't' : '.';
    buffer[1] = status_bits & 
      (FS_META_OWNER | FS_META_GROUP | FS_META_UMODE) ?  'p' : '.';
    buffer[2] = prop ?  'P' : '.';
    buffer[3] = 0;
  }
  else
  {
    buffer[0] = status_bits & FS_META_CHANGED ? 'm' : 
			prop ? 'P' : '.';
    buffer[1]=0;
  }

  return buffer;
}


/** Roses are red, grass is green ... */
char *st___color(int status_bits)
{
  if ((status_bits & FS_REPLACED) == FS_REMOVED)
    return ANSI__RED;
  if (status_bits & FS_NEW)
    return ANSI__GREEN;
  if (status_bits & FS_CHANGED)
    return ANSI__BLUE;
  return "";
}


int st__print_status(char *path, int status_bits, int flags, char* size,
    struct estat *sts)
{
  int status;


  status=0;
  /* Should we be quiet or _very_ quiet? */
  if (opt_verbose <0) goto ex;


  /* If the entry is new, got added or will be unversioned, we know that 
   * all meta-data has changed; we show only the essential information. */
  if ((status_bits & FS_NEW) ||
      (flags & (RF_ADD | RF_UNVERSION)))
    status_bits &= ~(FS_META_CHANGED | FS_LIKELY | FS_CHANGED);


  /* For flags like RF_ADD or RF_UNVERSION, print.  Don't print for 
   * RF_CHECK. */
  if (opt_verbose>0 || 
      (status_bits & FS__CHANGE_MASK) ||
      (flags & ~RF_CHECK))
  {
    /* We do this here, so that the debug output is not disturbed by the 
     * printed status characters. */
    STOPIF( hlp__format_path(sts, path, &path), NULL);

    status= 0>
      printf("%s%c%s%c%c  %8s  %s%s\n",
          opt__get_int(OPT__STATUS_COLOR) ? st___color(status_bits) : "",

          flags & RF_ADD ? 'n' : 
          flags & RF_UNVERSION ? 'd' : 
          (status_bits & FS_REPLACED) == FS_REPLACED ? 'R' : 
          status_bits & FS_NEW ? 'N' : 
          status_bits & FS_REMOVED ? 'D' : '.',

          st___meta_string(status_bits, flags),

          status_bits & FS_CHANGED ? 'C' : '.',

          status_bits & FS_LIKELY ? '?' : 
          /* An entry marked for unversioning or adding, 
           * which does not exist, gets a '!' */
          ( ( status_bits & FS_REMOVED ) &&
            ( flags & (RF_UNVERSION | RF_ADD) ) ) ? '!' : '.',

          size, path,
          opt__get_int(OPT__STATUS_COLOR) ? ANSI__NORMAL : "");
    /* possibly a EPIPE */
    STOPIF_CODE_ERR(status, errno, "Broken Pipe");
  }


ex:
  return status;
}


/** -.
 * */
int st__status(struct estat *sts, char *path)
{
  int status;
	int e_stat, flags;


  status=0;
  if (!path) STOPIF( ops__build_path(&path, sts), NULL);

  /* Is this entry already done? */
  if (sts->was_output) 
  {
    DEBUGP("%s was already output ...", path);
    goto ex;
  }
  sts->was_output=1;


	e_stat=sts->entry_status;
	flags=sts->flags;
	/* In case the file has been given directly as an argument to \ref status, 
	 * we wouldn't see that it's new - because ops__traverse() would created 
	 * its path. */
	if (flags & RF_ISNEW)
	{
		e_stat = ( e_stat & ~FS_REPLACED) | FS_NEW;
		flags &= ~RF_ADD;
		DEBUGP("Re-create the NEW status.");
	}

	STOPIF( st__print_status(path, 
				e_stat, flags,
				st___visible_file_size(sts),
				sts), NULL);

ex:
	return status;
}


/** -.
 * */
int st__rm_status(struct estat *sts, char *path)
{
	int status;


	status=0;
	if (!path) STOPIF( ops__build_path(&path, sts), NULL);

	STOPIF( st__print_status(path, 
				sts->remote_status, 0,
				st___visible_file_size(sts),
				sts), NULL);

ex:
	return status;
}


/** -.
 * */
int st__work(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;


	/* On ENOENT (no working copy committed yet) - should we take the common 
	 * denominator as base, or the current directory? */
	/* We do not call with FCB__WC_OPTIONAL; a base must be established (via 
	 * "urls" or "ignore"), so we always know where we are relative to our 
	 * base directory. */
	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);


	status=url__load_list(NULL, 0);
	/* Maybe no URL have been defined yet */
	if (status != ENOENT) STOPIF(status, NULL);

	STOPIF(ign__load_list(NULL), NULL);

	if (opt__get_int(OPT__DIR_SORT))
	{
		action->local_callback=st__progress;
		action->local_uninit=st__progress_uninit;
	}

	STOPIF( waa__read_or_build_tree(root, argc, normalized, argv, NULL, 0), 
			"No working copy data could be found.");


	if (opt__get_int(OPT__DIR_SORT))
		STOPIF( waa__do_sorted_tree(root, st__status), NULL);

ex:
	return status;
}


/** -.
 * A secondary status function for commit and update (and other functions
 * which run silently through the filesystem), which shows local progress 
 * when run on a tty.
 * On larger working copies the stat()ing alone can take some time, and
 * we want to keep the user informed that something happens.
 *
 * Commit and update themselves print the information send to/received from
 * the repository. */
int st__progress(struct estat *sts, char *path)
{
	static unsigned int counter=0;
	static int is_tty=0;
	static int last_outp;
	static time_t last;
	static time_t too_many_new=0;
	int status;
	time_t now;
	int print;
	const int bar_chart_width=20;
	const char bar_chart[bar_chart_width+1]="###################>";
	float pct;


	status=0;
	now=time(NULL);

	/* gcc won't let us initialize that - it's not a constant. */
	if (!progress_output) progress_output=stderr;

	if (is_tty == 0)
	{
		is_tty= isatty(fileno(progress_output)) ? +1 : -1;
		DEBUGP("we're on a tty");
	}

	if (is_tty == +1)
	{
		/* We're on a tty. Give progress reports now and then. */
		counter++;

		/* We do give a progress report for at least every ~2000 entries done.
		 * For slow machines (or an empty dentry cache, eg. after an OOM
		 * situation) we check every ~50 entries if there's been
		 * more than a second between reports, and if there was, we show, too.
		 * We take this (complicated) route because time() takes some time, 
		 * too; and I've seen too many programs spend 50% of runtime in
		 * gettimeofday() to see whether they should print something. */
		/* Mind: for & we need powers of 2 minus 1. */
		print= (counter & 0xfff) == 0 ? 1 : 0;
		if (!print && ((counter & 0x3f) == 0))
		{
			now=time(NULL);
			/* If ntp turns the clock back, the user gets what he deserves -
			 * output. */
			if (now != last) print=1;
		}

		if (print)
		{
			/* If we're at 99% percent for too long, we only print 
			 * the entries found. */
			if (counter <= approx_entry_count && now<too_many_new)
			{
				pct=(float)counter/approx_entry_count;
				print = (int)((float)bar_chart_width*pct +0.5);
				/* In perl it works to say "%0*s"; gcc warns here 
				 * "'0' flag used with %s" and it doesn't work.
				 * So we have to show part of a (constant) string.
				 *
				 * (Character-wise outputting takes too much time). */
				print=fprintf(progress_output, 
						"\r%8d of %8d done (%5.1f%%); [%s%*s]",
						counter, approx_entry_count,
						pct*100.0,
						bar_chart+bar_chart_width-print,
						bar_chart_width-print, "");

				if (pct > 0.96 && !too_many_new) 
					too_many_new=now+5;
			}
			else
			{
				/* If we don't know how many entries there are (first-time commit),
				 * or when we find that the estimate was wrong (too small),
				 * we just write how many were processed. */
				print=fprintf(progress_output, "\r%8d entries done",
						counter);
			}

			STOPIF_CODE_ERR(print < 0, errno,
					"Progress status could not be written");

			/* In the case of an invalid approx_entry_count we may have 
			 * to print some spaces after the string, to clean up the 
			 * previously printed bar chart. */
			if (print < last_outp)
				fprintf(progress_output, "%*s ", last_outp-print, "");
			last_outp=print;

			/* We store the maximum number of characters printed, to
			 * print the "right" number of spaces later on.
			 * This number should be constant - as long as we don't have more than
			 * 100M entries to do. */
			if (print > max_progress_len) max_progress_len=print;

			/* I thought briefly whether I should assign "now", and have "now"
			 * recalculated whenever &~128 gets true (as then &~2048 must be true,
			 * too) ... but that's not maintainer-friendly.
			 * Although ... it would save a few microseconds :-) */
			time(&last);
		}
		//		usleep(171000);
	}

ex:
	return status;
}


/** -.
 * Mostly needed to clear the cursor line, to avoid having part of a 
 * progress line mixed with some other output. */ 
int st__progress_uninit(void)
{
	const char err[]="Clearing the progress space";
	int status;
	char buff[max_progress_len+3];

	status=0;
	if (max_progress_len>0)
	{
		/* if EOF were always -1, we could just OR the return values 
		 * together. */

		buff[0]='\r';
		memset(buff+1, ' ', max_progress_len);
		buff[1+max_progress_len]='\r';
		buff[2+max_progress_len]=0;
		/* Maybe a fprintf(... "%*s" ) would be better? */
		STOPIF_CODE_ERR( fputs(buff, progress_output) == EOF, errno, err);
		fflush(progress_output);
	}

ex:
	return status;
}


struct st___bit_info
{
	int val;
	char *string;
	int str_len;
};
#define BIT_INFO(v, s) { .val=v, .string=s, .str_len=strlen(s) }


/** Constructs a string from a bitmask, where one or more bits may be set. */
#define st___string_from_bits(v, a, t) _st___string_from_bits(v, a, sizeof(a)/sizeof(a[0]), t)
volatile char *_st___string_from_bits(int value, 
		const struct st___bit_info data[], int max,
		char *text_for_none)
{
	const char sep[]=", ";
	static char *string=NULL;
	static int len=0;
	int i;
	int last_len, new_len;


	last_len=0;
	if (string) *string=0;
	for(i=0; i<max; i++)
	{
		if (value & data[i].val)
		{
			new_len = last_len + data[i].str_len + 
				(last_len ? strlen(sep) : 0);

			while (new_len + 8 > len)
			{
				if (!len) len=256;
				len *= 2;
				string=realloc(string, len);
				/* Cannot use STOPIF_ENOMEM() - we want to return a char* */
				if (!string) return NULL;
				string[last_len]=0;
			}

			if (last_len)
			{
				strcpy(string + last_len, sep);
				last_len += strlen(sep);
			}

			strcpy(string + last_len, data[i].string);
			last_len=new_len;
#if 0
			// Too verbose. 
			DEBUGP("match bit 0x%X on 0x%X: %s", 
					data[i].val, value, 
					data[i].string);
#endif
		}
	}

	/* If no bits are set, return "empty" */
	return string && *string ? string : text_for_none;
}


inline volatile char* st__flags_string_fromint(int mask)
{
	const struct st___bit_info flags[]={
		BIT_INFO( RF_ADD,				"add"),
		BIT_INFO( RF_UNVERSION,	"unversion"),
		/* This flag is not shown, as it's always set when we get here.
		 * So there's no information. */
		//	 BIT_INFO( RF_PRINT,		"print"),
		BIT_INFO( RF_CHECK,			"check"),
		BIT_INFO( RF_PUSHPROPS,  "push_props"),
	};	

	return st___string_from_bits(mask, flags, "none");
}


inline volatile char* st__status_string_fromint(int mask)
{
	const struct st___bit_info statii[]={
		BIT_INFO( FS_NEW,							"new"),
		BIT_INFO( FS_REMOVED,					"removed"),
		BIT_INFO( FS_CHANGED,   			"changed"),
		BIT_INFO( FS_META_OWNER,			"owner"),
		BIT_INFO( FS_META_GROUP,			"group"),
		BIT_INFO( FS_META_MTIME,			"mtime"),
		BIT_INFO( FS_META_UMODE,			"umode"),
		BIT_INFO( FS_PROPERTIES,			"props"),
		BIT_INFO( FS_CHILD_CHANGED,		"child"),
		BIT_INFO( FS_LIKELY,					"likely"), 
	};		
	return st___string_from_bits(mask, statii, "unmodified");
}


inline volatile char* st__status_string(const struct estat * const sts)
{
	return st__status_string_fromint(sts->entry_status);
}


int st__print_entry_info(const struct estat *const sts, int with_type)
{
	int status;
	const struct st___bit_info types[]={
		BIT_INFO( FT_IGNORE, 	"ignored"),
		BIT_INFO( FT_CDEV,  		"char-dev"),
		BIT_INFO( FT_BDEV,  		"block-dev"),
		BIT_INFO( FT_DIR,  		"directory"),
		BIT_INFO( FT_SYMLINK,	"symlink"),
		BIT_INFO( FT_FILE,  		"file"),
		BIT_INFO( FT_UNKNOWN,	"unknown"),
	};

	char *path, *waa_path, *url;


	status=errno=0;
	STOPIF( ops__build_path(&path, (struct estat*)sts), NULL);
	STOPIF( urls__full_url((struct estat*)sts, path, &url), NULL);

	if (with_type)
		status=printf("\tType:\t\t%s\n", 
				st___string_from_bits(sts->entry_type, types, "invalid") );
	if (sts->entry_type == FT_DIR)
		status |= printf("\tChildCount:\t%u\n", sts->entry_count);
	status |= printf("\tURL:\t\t%s\n", url);
	status |= printf("\tStatus:\t\t0x%X (%s)\n", sts->entry_status,
			st__status_string(sts));
	status |= printf("\tFlags:\t\t0x%X (%s)\n", 
			sts->flags & ~RF_PRINT,
			st__flags_string_fromint(sts->flags));
	status |= printf("\tDev:\t\t%llu\n", (t_ull)sts->st.dev);
	status |= printf("\tInode:\t\t%llu\n", (t_ull)sts->st.ino);
	status |= printf("\tMode:\t\t0%4o\n", sts->st.mode);
	status |= printf("\tUID/GID:\t%u (%s)/%u (%s)\n", 
			sts->st.uid, hlp__get_uname(sts->st.uid, "undefined"), 
			sts->st.gid, hlp__get_grname(sts->st.gid, "undefined") );
	/* Remove the \n at the end */
	status |= printf("\tMTime:\t\t%.24s\n", ctime( &(sts->st.mtim.tv_sec) ));
	status |= printf("\tCTime:\t\t%.24s\n", ctime( &(sts->st.ctim.tv_sec) ));

	STOPIF( waa__get_waa_directory(path, &waa_path, NULL, NULL,
				GWD_WAA), NULL);
	status |= printf("\tWAA-Path:\t%s\n", waa_path);

	if (!sts->parent)
	{
		STOPIF( waa__get_waa_directory(path, &waa_path, NULL, NULL,
					GWD_CONF), NULL);
		status |= printf("\tConf-Path:\t%s\n", waa_path);
	}

	status |= printf("\tRevision:\t%li\n", sts->repos_rev);

	if (sts->entry_type==FT_FILE)
		status |= printf("\tRepos-MD5:\t%s\n", 
				cs__md52hex(sts->md5));

	if (sts->entry_type==FT_BDEV || sts->entry_type==FT_CDEV)
		status |= printf("\tDevice number:\t%llu:%llu\n", 
				(t_ull)MAJOR(sts->st.rdev),
				(t_ull)MINOR(sts->st.rdev));
	else
		status |= printf("\tSize:\t\t%llu\n", (t_ull)sts->st.size);

	/* Any last words? */
	status |= printf("\n");

	DEBUGP("status at end of info is 0x%X", status);

	/* Had a printf a negative return value? */
	if (status < 0)
		STOPIF_CODE_ERR( 1, errno ? errno : ENOSPC,
				"Output error on printing entry info");
	else
		status=0;

ex:
	return status;
}


