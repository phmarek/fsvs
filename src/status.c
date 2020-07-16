/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>

#include "global.h"
#include "actions.h"
#include "status.h"
#include "helper.h"
#include "direnum.h"
#include "cache.h"
#include "url.h"
#include "cp_mv.h"
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
 * fsvs status [-C [-C]] [-v] [-f filter] [PATHs...]
 * \endcode
 *
 * This command shows the entries that have been changed locally since the 
 * last commit.
 *
 * The most important output formats are:
 * - A status columns of four (or, with \c -v , six) characters.
 *   There are either flags or a "." printed, so that it's easily parsed by 
 *   scripts -- the number of columns is only changed by \ref 
 *   glob_opt_verb.
 * - The size of the entry, in bytes, or \c "dir" for a directory, or \c 
 *   "dev" for a device.
 * - The path and name of the entry, formatted by the \ref o_opt_path 
 *   "path" option.
 *
 * Normally only changed entries are printed; with \c -v all are printed, 
 * but see the \ref o_filter "filter" option for more details.
 * 
 * The status column can show the following flags:
 * - \c 'D' and \c 'N' are used for \e deleted and \e new entries.
 * - \c 'd' and \c 'n' are used for entries which are to be unversioned or 
 *   added on the next commit; the characters were chosen as <i>little 
 *   delete</i> (only in the repository, not removed locally) and <i>little 
 *   new</i> (although \ref ignore "ignored"). See \ref add and \ref 
 *   unversion. \n
 *   If such an entry does not exist, it is marked with an \c "!" in the 
 *   last column -- because it has been manually marked, and so the removal 
 *   is unexpected.
 * - A changed type (character device to symlink, file to directory etc.)
 *   is given as \c 'R' (replaced), ie. as removed and newly added.
 * - \anchor status_possibly
 *   If the entry has been modified, the change is shown as \c 'C'.  \n
 *   If the modification or status change timestamps (mtime, ctime) are 
 *   changed, but the size is still the same, the entry is marked as 
 *   possibly changed (a question mark \c '?' in the last column) - but see 
 *   \ref o_chcheck "change detection" for details.
 * - A \c 'x' signifies a conflict.
 * - \anchor status_meta_changed
 *   The meta-data flag \c 'm' shows meta-data changes like properties, 
 *   modification timestamp and/or the rights (owner, group, mode); 
 *   depending on the \ref glob_opt_verb "-v/-q" command line parameters, 
 *   it may be split into \c 'P' (properties), \c 't' (time) and \c 'p' 
 *   (permissions). \n
 *   If \c 'P' is shown for the non-verbose case, it means \b only property 
 *   changes, ie. the entries filesystem meta-data is unchanged.
 * - A \c '+' is printed for files with a copy-from history; to see the URL 
 *   of the copyfrom source, see the \ref o_verbose "verbose" option.
 *
 * 
 * Here's a table with the characters and their positions:
 * \verbatim
 *   Without -v    With -v
 *     ....         ......
 *     NmC?         NtpPC?
 *     DPx!         D   x!
 *     R  +         R    +
 *     d            d
 *     n            n
 * \endverbatim
 *
 * Furthermore please take a look at the \ref o_status_color "stat_color" 
 * option, and for more information about displayed data the \ref o_verbose 
 * "verbose" option.
 * */


static FILE *progress_output=NULL;
static int max_progress_len=0;


/** Returns the visible file size.
 * For devices the string \c dev is printed; for directories \c dir; files 
 * and symlinks get their actual size printed. */
char * st___visible_file_size(struct estat *sts)
{
  static char buffer[20];

  switch ( (sts->st.mode ? sts->st.mode : sts->st.mode) & S_IFMT)
  {
		case S_IFBLK:
    case S_IFCHR:
      return "dev";
    case S_IFDIR:
      return "dir";
    default:
      /* When in doubt, believe it's a normal file.
       * We have that case for sync-repos - could be fixed some time. */
    case S_IFREG:
    case S_IFLNK:
      sprintf(buffer, "%llu", (t_ull) sts->st.size);
      break;
  }

  return buffer;
}


/** Meta-data status string. */
char * st___meta_string(int status_bits, int flags)
{
    static char buffer[4];
    int prop;

    prop=(status_bits & FS_PROPERTIES) | (flags & RF_PUSHPROPS);

    if (opt__is_verbose() > 0)
    {
        buffer[0] = status_bits & FS_META_MTIME ?  't' : '.';
        buffer[1] = status_bits & 
            (FS_META_OWNER | FS_META_GROUP | FS_META_UMODE) ?  'p' : '.';
        buffer[2] = prop ?  'P' : '.';
        buffer[3] = 0;
    }
    else
    {
        buffer[0] = prop ? 'P' : ((status_bits & FS_META_CHANGED) ? 'm' : '.');
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


/** Prints the entry in readable form.
 * This function uses the \c OPT__VERBOSE settings.  */
int st__print_status(char *path, int status_bits, int flags, char* size,
    struct estat *sts)
{
  int status;
	char *copyfrom, *url;
	int copy_inherited;
	FILE* output=stdout;


	DEBUGP("VERBOSITY=%d", opt__get_int(OPT__VERBOSE));
  status=0;
	/* Should we be quiet or _very_ quiet? */
  if (opt__verbosity() <= VERBOSITY_QUIET) goto ex;


	/* If the entry is new or deleted, got added or will be unversioned, we 
	 * know that all meta-data has changed; we show only the essential 
	 * information. */
  if ((status_bits & (FS_NEW | FS_REMOVED)) ||
      (flags & (RF_ADD | RF_UNVERSION)))
    status_bits &= ~(FS_META_CHANGED | FS_LIKELY | FS_CHANGED);


  /* For flags like RF_ADD or RF_UNVERSION, print.  Don't print for 
   * RF_CHECK. */
  if (opt__is_verbose() > 0 || 
      (status_bits & FS__CHANGE_MASK) ||
      (flags & ~RF_CHECK))
	{
		copyfrom=NULL;
		copy_inherited=0;

		/* Go to copied parent when RF_COPY_SUB is set, and re-construct the 
		 * entire copyfrom-URL?  */
		if (opt__get_int(OPT__VERBOSE) & VERBOSITY_COPYFROM)
		{
			copy_inherited= (flags & RF_COPY_SUB);

			if (flags & RF_COPY_BASE)
			{
				status=cm__get_source(sts, NULL, &copyfrom, NULL, 0);
				BUG_ON(status == ENOENT, "Marked as copied, but no info?");
				STOPIF(status, NULL);
			}
		}


		if (opt__get_int(OPT__VERBOSE) & VERBOSITY_TOP_URL)
			STOPIF( url__full_url(sts, &url), NULL);
		else
			url=NULL;


		/* We do this here, so that the debug output is not disturbed by the 
		 * printed status characters. */
		STOPIF( hlp__format_path(sts, path, &path), NULL);


		/* We're no longer doing a single printf(); but setbuf() et. al. write 
		 * that the default for terminals is line buffered, and block buffered 
		 * in the redirected case, and that's exactly what we want. */

		if (opt__get_int(OPT__STATUS_COLOR))
			STOPIF_CODE_EPIPE( fputs(st___color(status_bits), output), NULL);

		if (opt__get_int(OPT__VERBOSE) & VERBOSITY_SHOWCHG)
			STOPIF_CODE_EPIPE( fprintf(output, "%c%s%c%c  ",
						flags & RF_ADD ? 'n' : 
						flags & RF_UNVERSION ? 'd' : 
						(status_bits & FS_REPLACED) == FS_REPLACED ? 'R' : 
						status_bits & FS_NEW ? 'N' : 
						status_bits & FS_REMOVED ? 'D' : '.',

						st___meta_string(status_bits, flags),

						flags & RF_CONFLICT ? 'x' : 
						status_bits & FS_CHANGED ? 'C' : '.',

						flags & RF___IS_COPY ? '+' : 
						status_bits & FS_LIKELY ? '?' : 
						/* An entry marked for unversioning or adding, 
						 * which does not exist, gets a '!' */
						( ( status_bits & FS_REMOVED ) &&
							( flags & (RF_UNVERSION | RF_ADD) ) ) ? '!' : '.'
						), NULL);


		if (opt__get_int(OPT__VERBOSE) & VERBOSITY_SHOWSIZE)
			STOPIF_CODE_EPIPE( fprintf(output, "%8s  ", size), NULL);

		if (opt__get_int(OPT__VERBOSE) & VERBOSITY_GROUP)
			STOPIF_CODE_EPIPE( fprintf(output, "%-*s", 
						ign__max_group_name_len+2,
						sts->match_pattern ? sts->match_pattern->group_name :
						"(none)"), NULL);

		if (opt__get_int(OPT__VERBOSE) & VERBOSITY_SHOWNAME)
			STOPIF_CODE_EPIPE( fputs(path, output), NULL);

		if (opt__get_int(OPT__STATUS_COLOR))
			STOPIF_CODE_EPIPE( fputs(ANSI__NORMAL, output), NULL);

		/* Here the comparison of OPT__VERBOSE is already included in the check 
		 * on copyfrom above. */
		if (copyfrom || copy_inherited)
			STOPIF_CODE_EPIPE( fprintf(output,
						copy_inherited ? "  (inherited)" : "  (copied from %s)",
						copyfrom), NULL);

		if (url)
			STOPIF_CODE_EPIPE( fprintf(output, "  %s", url), NULL);


		STOPIF_CODE_EPIPE( fputs("\n", output), NULL);
	}


ex:
  return status;
}


/** -.
 * */
int st__status(struct estat *sts)
{
	int status;
	int e_stat, flags;
	char *path;
	int would_be_ignored;


	status=0;
	STOPIF( ops__build_path(&path, sts), NULL);

	/* Is this entry already done? */
	BUG_ON(sts->was_output, "%s was already output ...", path);
	sts->was_output=1;


	e_stat=sts->entry_status;
	flags=sts->flags;
	/* In case the file has been given directly as an argument to \ref 
	 * status, we wouldn't see that it's new - because ops__traverse() would 
	 * have created its path. */
	if (flags & RF_ISNEW)
	{
		e_stat = ( e_stat & ~FS_REPLACED) | FS_NEW;
		flags &= ~RF_ADD;
		DEBUGP("Re-create the NEW status.");

		if (opt__get_int(OPT__VERBOSE) & VERBOSITY_GROUP)
			STOPIF( ign__is_ignore(sts, &would_be_ignored), NULL);
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
int st__action(struct estat *sts)
{
	int status;

	status = 0;

	if (hlp__only_dir_mtime_changed(sts))
		return status;

	if (opt__get_int(OPT__STOP_ON_CHANGE) &&
			sts->entry_status && (!(sts->entry_status & FS_CHILD_CHANGED)))
		/* Status is a read-only operation, so that works. */
		exit(1);

	STOPIF( st__status(sts), NULL);

ex:
	return status;
}


/** -.
 * */
int st__rm_status(struct estat *sts)
{
	int status;
	char *path;


	status=0;
	STOPIF( ops__build_path(&path, sts), NULL);

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

	STOPIF( ign__load_list(NULL), NULL);

	if (opt__get_int(OPT__DIR_SORT) && 
			!opt__get_int(OPT__STOP_ON_CHANGE))
	{
		action->local_callback=st__progress;
		action->local_uninit=st__progress_uninit;
	}

	STOPIF( waa__read_or_build_tree(root, argc, normalized, argv, NULL, 0), 
			"No working copy data could be found.");


	if (opt__get_int(OPT__DIR_SORT))
	{
		action->local_callback=st__status;
		STOPIF( waa__do_sorted_tree(root, ac__dispatch), NULL);
	}

	if (opt__get_int(OPT__GROUP_STATS))
		STOPIF( ign__print_group_stats(stdout), NULL);

ex:
	return status;
}


#define BAR_CHART_WIDTH 20
/** -.
 * A secondary status function for commit and update (and other functions
 * which run silently through the filesystem), which shows local progress 
 * when run on a tty.
 * On larger working copies the stat()ing alone can take some time, and
 * we want to keep the user informed that something happens.
 *
 * Commit and update themselves print the information send to/received from
 * the repository. */
int st__progress(struct estat *sts)
{
	static unsigned int counter=0;
	static int is_tty=0;
	static int last_outp;
	static time_t last;
	static time_t too_many_new=0;
	int status;
	time_t now;
	int print;
	static const char bar_chart[BAR_CHART_WIDTH+1]="###################>";
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
				print = (int)((float)BAR_CHART_WIDTH*pct +0.5);
				/* In perl it works to say "%0*s"; gcc warns here 
				 * "'0' flag used with %s" and it doesn't work.
				 * So we have to show part of a (constant) string.
				 *
				 * (Character-wise outputting takes too much time). */
				print=fprintf(progress_output, 
						"\r%8d of %8d done (%5.1f%%); [%s%*s]",
						counter, approx_entry_count,
						pct*100.0,
						bar_chart+BAR_CHART_WIDTH-print,
						BAR_CHART_WIDTH-print, "");

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
	static const char err[]="Clearing the progress space";
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


/** Constructs a string from a bitmask, where one or more bits may be set.
 *
 * Must not be free()d. */
#define st___string_from_bits(v, a, t) _st___string_from_bits(v, a, sizeof(a)/sizeof(a[0]), t)
volatile char *_st___string_from_bits(int value, 
		const struct st___bit_info data[], int max,
		char *text_for_none)
{
	int status;
	static struct cache_t *cache=NULL;
	static const char sep[]=", ";
	char *string;
	int i;
	int last_len, new_len;
	struct cache_entry_t **cc;


	STOPIF( cch__new_cache(&cache, 4), NULL);
	STOPIF( cch__add(cache, 0, NULL, 128, &string), NULL);
	cc=cache->entries + cache->lru;

	/* To quiesce clang */
	assert(string);

	last_len=0;
	*string=0;
	for(i=0; i<max; i++)
	{
		if (value & data[i].val)
		{
			new_len = last_len + data[i].str_len + 
				(last_len ? strlen(sep) : 0);

			if (new_len + 8 > (*cc)->len)
			{
				STOPIF( cch__entry_set(cc, 0, NULL, new_len+64, 1, &string), NULL);
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

ex:
	/* Is that good? */
  if (status) return NULL;
	/* If no bits are set, return "empty" */
	return string && *string ? string : text_for_none;
}


volatile char* st__flags_string_fromint(int mask)
{
	const struct st___bit_info flags[]={
		BIT_INFO( RF_ADD,				"add"),
		BIT_INFO( RF_UNVERSION,	"unversion"),
		/* This flag is not shown, as it's always set when we get here.
		 * So there's no information. */
		//	 BIT_INFO( RF_PRINT,		"print"),
		BIT_INFO( RF_CHECK,			"check"),
		BIT_INFO( RF_COPY_BASE,	"copy_base"),
		BIT_INFO( RF_COPY_SUB,	"copy_sub"),
		BIT_INFO( RF_CONFLICT,	"conflict"),
		BIT_INFO( RF_PUSHPROPS,	"push_props"),
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


char *st__type_string(mode_t mode)
{
	switch (mode & S_IFMT)
	{
		case S_IFDIR:     return "directory";
		case S_IFBLK:     return "block-dev";
		case S_IFCHR:     return "char-dev";
		case S_IFREG:     return "file";
		case S_IFLNK:     return "symlink";
		case S_IFSOCK:    return "any-special";
		case S_IFGARBAGE: return "garbage";
	}

	return "invalid";
}


inline volatile char* st__status_string(const struct estat * const sts)
{
	return st__status_string_fromint(sts->entry_status);
}


int st__print_entry_info(struct estat *sts)
{
	int status;
	char *path, *waa_path, *url, *copyfrom;
	svn_revnum_t copy_rev;


	status=errno=0;
	STOPIF( ops__build_path(&path, sts), NULL);
	STOPIF( url__full_url(sts, &url), NULL);

	copyfrom=NULL;
	if ((opt__get_int(OPT__VERBOSE) & VERBOSITY_COPYFROM) && 
			(sts->flags & RF___IS_COPY))
	{
		STOPIF( cm__get_source(sts, path, &copyfrom, &copy_rev, 0), NULL);
	}

	STOPIF_CODE_EPIPE( printf("   Type:   \t%s\n", 
				st__type_string(sts->st.mode)), NULL);
	if (S_ISDIR(sts->st.mode))
		STOPIF_CODE_EPIPE( printf( "   ChildCount:\t%u\n", 
					sts->entry_count), NULL);
	STOPIF_CODE_EPIPE( printf("   URL:   \t%s\n", url), NULL);
	STOPIF_CODE_EPIPE( printf("   Status:\t0x%X (%s)\n", 
				sts->entry_status, st__status_string(sts)), NULL);
	STOPIF_CODE_EPIPE( printf("   Flags:\t0x%X (%s)\n", 
				sts->flags & ~RF_PRINT,
				st__flags_string_fromint(sts->flags)), NULL);

	if (copyfrom)
	{
		STOPIF_CODE_EPIPE( printf("   Copyfrom:\trev. %llu of %s\n", 
					(t_ull)copy_rev, copyfrom), NULL);
	}

	STOPIF_CODE_EPIPE( printf("   Dev:  \t%llu\n", 
				(t_ull)sts->st.dev), NULL);
	STOPIF_CODE_EPIPE( printf("   Inode:  \t%llu\n", 
				(t_ull)sts->st.ino), NULL);
	STOPIF_CODE_EPIPE( printf("   Mode:  \t0%4o\n", 
				sts->st.mode), NULL);
	STOPIF_CODE_EPIPE( printf("   UID/GID:\t%u (%s)/%u (%s)\n", 
				sts->st.uid, hlp__get_uname(sts->st.uid, "undefined"), 
				sts->st.gid, hlp__get_grname(sts->st.gid, "undefined") ), NULL);
	/* Remove the \n at the end */
	STOPIF_CODE_EPIPE( printf("   MTime:  \t%.24s\n", 
				ctime( &(sts->st.mtim.tv_sec) )), NULL);
	STOPIF_CODE_EPIPE( printf("   CTime:  \t%.24s\n", 
				ctime( &(sts->st.ctim.tv_sec) )), NULL);

	STOPIF( waa__get_waa_directory(path, &waa_path, NULL, NULL,
				GWD_WAA), NULL);
	STOPIF_CODE_EPIPE( printf("   WAA-Path:\t%s\n", 
				waa_path), NULL);

	if (!sts->parent)
	{
		STOPIF( waa__get_waa_directory(path, &waa_path, NULL, NULL,
					GWD_CONF), NULL);
		STOPIF_CODE_EPIPE( printf("   Conf-Path:\t%s\n", 
					waa_path), NULL);
	}

	/* The root entry has no URL associated, and so no revision number.
	 * Print the current revision of the highest priority URL. */
	STOPIF_CODE_EPIPE( printf("   Revision:\t%li\n", 
				sts->parent ? sts->repos_rev : urllist[0]->current_rev), NULL);

	if (S_ISREG(sts->st.mode))
		STOPIF_CODE_EPIPE( printf("   Repos-MD5:\t%s\n", 
					cs__md5tohex_buffered(sts->md5)), NULL);

	if (S_ISBLK(sts->st.mode) || S_ISCHR(sts->st.mode))
	{
#ifdef DEVICE_NODES_DISABLED
		DEVICE_NODES_DISABLED();
#else
		STOPIF_CODE_EPIPE( printf("   Device nr.:\t%llu:%llu\n", 
					(t_ull)MAJOR(sts->st.rdev),
					(t_ull)MINOR(sts->st.rdev)), NULL);
#endif
	}
	else
		STOPIF_CODE_EPIPE( printf("   Size:  \t%llu\n", 
					(t_ull)sts->st.size), NULL);

	/* Any last words? */
	STOPIF_CODE_EPIPE( printf("\n"), NULL);

ex:
	return status;
}


