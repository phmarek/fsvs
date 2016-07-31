/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>


#include "global.h"
#include "revert.h"
#include "helper.h"
#include "interface.h"
#include "url.h"
#include "status.h"
#include "options.h"
#include "est_ops.h"
#include "ignore.h"
#include "waa.h"
#include "racallback.h"
#include "cp_mv.h"
#include "warnings.h"
#include "diff.h"


/** \file
 * The \ref diff command source file.
 *
 * Currently only diffing single files is possible; recursive diffing
 * of trees has to be done.
 *
 * For trees it might be better to fetch all files in a kind of 
 * update-scenario; then we'd avoid the many round-trips we'd have with
 * single-file-fetching.
 * Although an optimized file-fetching (rsync-like block transfers) would
 * probably save a lot of bandwidth.
 * */

/** \addtogroup cmds
 * 
 * \section diff
 *
 * \code
 * fsvs diff [-v] [-r rev[:rev2]] [-R] PATH [PATH...]
 * \endcode
 * 
 * This command gives you diffs between local and repository files.
 *
 * With \c -v the meta-data is additionally printed, and changes shown.
 * 
 * If you don't give the revision arguments, you get a diff of the base 
 * revision in the repository (the last commit) against your current local file.
 * With one revision, you diff this repository version against you local file.
 * With both revisions given, the difference between these repository versions
 * is calculated.
 * 
 * You'll need the \c diff program, as the files are simply passed as 
 * parameters to it.
 * 
 * The default is to do non-recursive diffs; so <tt>fsvs diff .</tt> will 
 * output the changes in all files <b>in the current directory</b>.
 *
 * The output for non-files is not defined.
 *
 * For entries marked as copy the diff against the (clean) source entry is 
 * printed.
 *
 * Please see also \ref o_diff and \ref o_colordiff. 
 *
 * \todo Two revisions diff is buggy in that it (currently) always fetches 
 * the full tree from the repository; this is not only a performance 
 * degradation, but you'll see more changed entries than you want. This 
 * will be fixed.
 * */


int cdiff_pipe=STDOUT_FILENO;
pid_t cdiff_pid=0;


/** A number that cannot be a valid pointer. */
#define META_DIFF_DELIMITER (0xf44fee31)
/** How long may a meta-data diff string be? */
#define META_DIFF_MAXLEN (256)


/** Diff the given meta-data.
 * The given \a format string is used with the va-args to generate two 
 * strings. If they are equal, one is printed (with space at front); else 
 * both are shown (with '-' and '+').
 * The delimiter between the two argument lists is via \ref 
 * META_DIFF_DELIMITER. (NULL could be in the data, eg. as integer \c 0.) 
 *
 * It would be faster to simply compare the values given to \c vsnprintf(); 
 * that could even be done here, by using two \c va_list variables and 
 * comparing. But it's not a performance problem.
 */
int df___print_meta(char *format, ... )
{
	int status;
	va_list va;
	char buf_old[META_DIFF_MAXLEN], 
			 buf_new[META_DIFF_MAXLEN];
	int l1, l2;


	status=0;
	va_start(va, format);

	l1=vsnprintf(buf_old, META_DIFF_MAXLEN-1, format, va);
	DEBUGP("meta-diff: %s", buf_old);

	l2=0;
	while (va_arg(va, int) != META_DIFF_DELIMITER)
	{
		l2++;
		BUG_ON(l2>5, "Parameter list too long");
	}

	l2=vsnprintf(buf_new, META_DIFF_MAXLEN-1, format, va);
	DEBUGP("meta-diff: %s", buf_new);

	STOPIF_CODE_ERR( l1<0 || l2<0 || 
			l1>=META_DIFF_MAXLEN || l2>=META_DIFF_MAXLEN, EINVAL,
			"Printing meta-data strings format error");

		/* Different */
	STOPIF_CODE_EPIPE( 
			printf( 
				(l1 != l2 || strcmp(buf_new, buf_old) !=0) ? 
				"-%s\n+%s\n" : " %s\n", 
				buf_old, buf_new), NULL);

ex:
	return status;
}



/** Get a file from the repository, and initiate a diff.
 *
 * Normally <tt>rev1 == root->repos_rev</tt>; to diff against
 * the \e base revision of the file.
 *
 * If the user specified only a single revision (<tt>rev2 == 0</tt>), 
 * the local file is diffed against this; else against the
 * other repository version.
 *
 * \a rev2_file is meaningful only if \a rev2 is 0; this file gets removed 
 * after printing the difference!
 * */
int df__do_diff(struct estat *sts, 
		svn_revnum_t rev1, 
		svn_revnum_t rev2, char *rev2_file)
{
	int status;
	int ch_stat;
	static pid_t last_child=0;
	static char *last_tmp_file=NULL;
	static char *last_tmp_file2=NULL;
	pid_t tmp_pid;
	char *path, *disp_dest, *disp_source;
	int len_d, len_s;
	char *b1, *b2;
	struct estat sts_r2;
	char short_desc[10];
	char *new_mtime_string, *other_mtime_string;
	char *url_to_fetch, *other_url;
	int is_copy;
	int fdflags;
	apr_hash_t *props_r1, *props_r2;


	status=0;

	/* Check whether we have an active child; wait for it. */
	if (last_child)
	{
		/* Keep the race window small. */
		tmp_pid=last_child;
		last_child=0;
		STOPIF_CODE_ERR( waitpid(tmp_pid, &ch_stat, 0) == -1, errno,
				"Waiting for child gave an error");
		DEBUGP("child %d exitcode %d - status 0x%04X", 
				tmp_pid, WEXITSTATUS(ch_stat), ch_stat);

		STOPIF_CODE_ERR( !WIFEXITED(ch_stat), EIO,
				"!Child %d terminated abnormally", tmp_pid);

		if (WEXITSTATUS(ch_stat) == 1)
			DEBUGP("exit code 1 - file has changed.");
		else
		{
			STOPIF( wa__warn(WRN__DIFF_EXIT_STATUS, EIO,
						"Child %d gave an exit status %d", 
						tmp_pid, WEXITSTATUS(ch_stat)),
					NULL);
		}
	}

	/* \a last_tmp_file should only be set when last_child is set; 
	 * but who knows.
	 *
	 * This cleanup must be done \b after waiting for the child - else we
	 * might delete the file before it was opened!
	 * */
	if (last_tmp_file)
	{
		STOPIF_CODE_ERR( unlink(last_tmp_file) == -1, errno,
				"Cannot remove temporary file %s", last_tmp_file);
		last_tmp_file=NULL;
	}

	if (last_tmp_file2)
	{
		STOPIF_CODE_ERR( unlink(last_tmp_file2) == -1, errno,
				"Cannot remove temporary file %s", last_tmp_file2);
		last_tmp_file2=NULL;
	}

	/* Just uninit? */
	if (!sts) goto ex;


	STOPIF( ops__build_path( &path, sts), NULL);


	url_to_fetch=NULL;
	/* If this entry is freshly copied, get it's source URL. */
	is_copy=sts->flags & RF___IS_COPY;
	if (is_copy)
	{
		/* Should we warn if any revisions are given? Can we allow one? */
		STOPIF( cm__get_source(sts, NULL, &url_to_fetch, &rev1, 0), NULL);
		/* \TODO: That doesn't work for unknown URLs - but that's needed as 
		 * soon as we allow "fsvs cp URL path".  */
		STOPIF( url__find(url_to_fetch, &sts->url), NULL);
	}
	else
		url_to_fetch=path+2;

	current_url = sts->url;


	/* We have to fetch a file and do the diff, so open a session. */
	STOPIF( url__open_session(NULL), NULL);

	/* The function rev__get_file() overwrites the data in \c *sts with 
	 * the repository values - mtime, ctime, etc.
	 * We use this as an advantage and remember the current time - so that
	 * we can print both. */
	/* \e From is always the "old" - base revision, or first given revision.
	 * \e To is the newer version - 2nd revision, or local file. */
	/* TODO: use delta transfers for 2nd file. */
	sts_r2=*sts;
	if (rev2 != 0)
	{
		STOPIF( url__full_url(sts, NULL, &other_url), NULL);

		STOPIF( url__canonical_rev(current_url, &rev2), NULL);
		STOPIF( rev__get_text_to_tmpfile(other_url, rev2, DECODER_UNKNOWN,
					NULL, &last_tmp_file2, 
					NULL, &sts_r2, &props_r2, 
					current_url->pool),
				NULL);
	}
	else if (rev2_file)
	{
		DEBUGP("diff against %s", rev2_file);
		/* Let it get removed. */
		last_tmp_file2=rev2_file;
	}

	/* Now fetch the \e old version. */
	STOPIF( url__canonical_rev(current_url, &rev1), NULL);
	STOPIF( rev__get_text_to_tmpfile(url_to_fetch, rev1, DECODER_UNKNOWN,
				NULL, &last_tmp_file, 
				NULL, sts, &props_r1, 
				current_url->pool), NULL);

	/* If we didn't flush the stdio buffers here, we'd risk getting them 
	 * printed a second time from the child. */
	fflush(NULL);


	last_child=fork();
	STOPIF_CODE_ERR( last_child == -1, errno,
			"Cannot fork diff program");

	if (!last_child)
	{
		STOPIF( hlp__format_path(sts, path, &disp_dest), NULL);

		/* Remove the ./ at the front */
		setenv(FSVS_EXP_CURR_ENTRY, path+2, 1);

		disp_source= is_copy ? url_to_fetch : disp_dest;

		len_d=strlen(disp_dest);
		len_s=strlen(disp_source);


		if (cdiff_pipe != STDOUT_FILENO)
		{
			STOPIF_CODE_ERR( dup2(cdiff_pipe, STDOUT_FILENO) == -1, errno,
					"Redirect output");

			/* Problem with svn+ssh - see comment below. */
			fdflags=fcntl(STDOUT_FILENO, F_GETFD);
			fdflags &= ~FD_CLOEXEC;
			/* Does this return errors? */
			fcntl(STDOUT_FILENO, F_SETFD, fdflags);
		}


		/* We need not be nice with memory usage - we'll be replaced soon. */

		/* 30 chars should be enough for everyone */
		b1=malloc(len_s + 60 + 30);
		b2=malloc(len_d + 60 + 30);

		new_mtime_string=strdup(ctime(& sts_r2.st.mtim.tv_sec ));
		STOPIF_ENOMEM(!new_mtime_string);
		other_mtime_string=strdup(ctime(&sts->st.mtim.tv_sec ));
		STOPIF_ENOMEM(!other_mtime_string);

		sprintf(b1, "%s  \tRev. %llu  \t(%-24.24s)", 
				disp_source, (t_ull) rev1, other_mtime_string);

		if (rev2 == 0)
		{
			sprintf(b2, "%s  \tLocal version  \t(%-24.24s)", 
					disp_dest, new_mtime_string);
			strcpy(short_desc, "local");
		}
		else
		{
			sprintf(b2, "%s  \tRev. %llu  \t(%-24.24s)", 
					disp_dest, (t_ull) rev2, new_mtime_string);
			sprintf(short_desc, "r%llu", (t_ull) rev2);
		}


		/* Print header line, just like a recursive diff does. */
		STOPIF_CODE_EPIPE( printf("diff -u %s.r%llu %s.%s\n", 
					disp_source, (t_ull)rev1, 
					disp_dest, short_desc),
				"Diff header");


		if (opt_verbose>0) // TODO: && !symlink ...)
		{
			STOPIF(	df___print_meta( "Mode: 0%03o",
						sts->st.mode & 07777,
						META_DIFF_DELIMITER,
						sts_r2.st.mode & 07777), 
					NULL);
			STOPIF(	df___print_meta( "MTime: %.24s", 
						other_mtime_string,
						META_DIFF_DELIMITER,
						new_mtime_string),
					NULL);
			STOPIF(	df___print_meta( "Owner: %d (%s)",
						sts->st.uid, hlp__get_uname(sts->st.uid, "undefined"),
						META_DIFF_DELIMITER,
						sts_r2.st.uid, hlp__get_uname(sts_r2.st.uid, "undefined") ),
					NULL);
			STOPIF(	df___print_meta( "Group: %d (%s)", 
						sts->st.gid, hlp__get_grname(sts->st.gid, "undefined"),
						META_DIFF_DELIMITER,
						sts_r2.st.gid, hlp__get_grname(sts_r2.st.gid, "undefined") ),
					NULL);
		}
		fflush(NULL);

		// TODO: if special_dev ...

		/* Checking \b which return value we get is unnecessary ...  On \b 
		 * every error we get \c -1 .*/
		execlp( opt__get_string(OPT__DIFF_PRG),
				opt__get_string(OPT__DIFF_PRG),
				opt__get_string(OPT__DIFF_OPT),
				last_tmp_file, 
				"--label", b1,
				(rev2 != 0 ? last_tmp_file2 : 
				 rev2_file ? rev2_file : path),
				"--label", b2,
				opt__get_string(OPT__DIFF_EXTRA),
				NULL);
		STOPIF_CODE_ERR( 1, errno, 
				"Starting the diff program \"%s\" failed",
				opt__get_string(OPT__DIFF_PRG));
	}

ex:
	return status;
}


/** Cleanup rests. */
int df___cleanup(void)
{
	int status;
	int ret;


	if (cdiff_pipe != STDOUT_FILENO)
		STOPIF_CODE_ERR( close(cdiff_pipe) == -1, errno,
				"Cannot close colordiff pipe");

	if (cdiff_pid)
	{
		/* Should we kill colordiff? Let it stop itself? Wait for it?
		 * It should terminate itself, because STDIN gets no more data.
		 *
		 * But if we don't wait, it might get scheduled after the shell printed 
		 * its prompt ... and that's not fine. But should we ignore the return 
		 * code? */
		STOPIF_CODE_ERR( waitpid( cdiff_pid, &ret, 0) == -1, errno, 
				"Can't wait");
		DEBUGP("child %d exitcode %d - status 0x%04X", 
				cdiff_pid, WEXITSTATUS(ret), ret);
	}

	STOPIF( df__do_diff(NULL, 0, 0, 0), NULL);

ex:
	return status;
}


/// FSVS GCOV MARK: df___signal should not be executed
/** Signal handler function.
 * If the user wants us to quit, we remove the temporary files, and exit.  
 *
 * Is there a better/cleaner way?
 * */
static void df___signal(int sig)
{
	DEBUGP("signal %d arrived!", sig);
	df___cleanup();
	exit(0);
}


/** Does a diff of the local non-directory against the given revision.
 * */
int df___type_def_diff(struct estat *sts, svn_revnum_t rev, 
		apr_pool_t *pool)
{
	int status;
	char *special_stg, *fn;
	apr_file_t *apr_f;
	apr_size_t wr_len, exp_len;


	status=0;
	special_stg=NULL;
	switch (sts->updated_mode & S_IFMT)
	{
		case S_IFREG:
			STOPIF( df__do_diff(sts, rev, 0, NULL), NULL);
			break;

		case S_IFCHR:
		case S_IFBLK:
			special_stg=ops__dev_to_filedata(sts);

			/* Fallthrough, ignore first statement. */

		case S_IFLNK:
			if (!special_stg)
				STOPIF( ops__link_to_string(sts, NULL, &special_stg), NULL);

			STOPIF( ops__build_path( &fn, sts), NULL);
			STOPIF_CODE_EPIPE( printf("Special entry changed: %s\n", fn), NULL);

			/* As "diff" cannot handle special files directly, we have to 
			 * write the expected string into a file, and diff against 
			 * that.
			 * The remote version is fetched into a temporary file anyway. */
			STOPIF( waa__get_tmp_name(NULL, &fn, &apr_f, pool), NULL);

			wr_len=exp_len=strlen(special_stg);
			STOPIF( apr_file_write(apr_f, special_stg, &wr_len), NULL);
			STOPIF_CODE_ERR( wr_len != exp_len, ENOSPC, NULL);

			STOPIF( apr_file_close(apr_f), NULL);


			STOPIF( df__do_diff(sts, rev, 0, fn), NULL);
			break;

		default:
			BUG("type?");
	}

ex:
	return status;
}


/** -. */
int df___direct_diff(struct estat *sts)
{
	int status;
	svn_revnum_t rev1, rev2;
	char *fn;


	STOPIF( ops__build_path( &fn, sts), NULL);

	status=0;
	if (!S_ISDIR(sts->updated_mode))
	{
		DEBUGP("doing %s", fn);

		/* Has to be set per sts. */
		rev1=sts->repos_rev;
		rev2=0;
		if ( (sts->entry_status & FS_REMOVED))
		{
			STOPIF_CODE_EPIPE( printf("Only in repository: %s\n", fn), NULL);
			goto ex;
		}

		if (sts->to_be_ignored) goto ex;

		if ( (sts->entry_status & FS_NEW) || !sts->url)
		{
			if (sts->flags & RF___IS_COPY)
			{
				/* File was copied, we have a source */
			}
			else
			{
				if (opt_verbose>0)
					STOPIF_CODE_EPIPE( printf("Only in local filesystem: %s\n", 
								fn), NULL);
				goto ex;
			}
		}

		/* Local files must have changed; for repos-only diffs do always. */
		if (sts->entry_status || opt_target_revisions_given)
		{
			DEBUGP("doing diff rev1=%llu", (t_ull)rev1);
			if (S_ISDIR(sts->updated_mode))
			{
				/* TODO: meta-data diff? */
			}
			else
			{				
				/* TODO: Some kind of pool handling in recursion. */
				STOPIF( df___type_def_diff(sts, rev1, global_pool), NULL);
			}
		}
	}
	else
	{
		/* Nothing to do for directories? */
	}

ex:
	return status;
}


/** A cheap replacement for colordiff.
 * Nothing more than a \c cat. */
int df___cheap_colordiff(void)
{
	int status;
	char *tmp;
	const int tmp_size=16384;

	status=0;
	tmp=alloca(tmp_size);
	while ( (status=read(STDIN_FILENO,tmp, tmp_size)) > 0 )
		if ( (status=write(STDOUT_FILENO, tmp, status)) == -1)
			break;

	if (status == -1)
	{
		STOPIF_CODE_ERR(errno != EPIPE, errno, 
				"Getting or pushing diff data");
		status=0;
	}

ex:
	return status;
}


/** Tries to start colordiff.
 * If colordiff can not be started, but the option says \c auto, we just 
 * forward the data. Sadly neither \c splice nor \c sendfile are available  
 * everywhere.
 * */
int df___colordiff(int *handle, pid_t *cd_pid)
{
	const char *program;
	int status;
	int pipes[2], fdflags, success[2];

	status=0;
	program=opt__get_int(OPT__COLORDIFF) ?
		opt__get_string(OPT__COLORDIFF) : 
		"colordiff";

	STOPIF_CODE_ERR( pipe(pipes) == -1, errno,
			"No more pipes");
	STOPIF_CODE_ERR( pipe(success) == -1, errno,
			"No more pipes, case 2");

	/* There's a small problem if the parent gets scheduled before the child, 
	 * and the child doesn't find the colordiff binary; then the parent might 
	 * only find out when it tries to send the first data across the pipe.
	 *
	 * But the successfully spawned colordiff won't report success, so the 
	 * parent would have to wait for a fail message - which delays execution 
	 * unnecessary - or simply live with diff getting EPIPE.
	 *
	 * Trying to get it scheduled by sending it a signal (which will be 
	 * ignored) doesn't work reliably, too.
	 *
	 * The only way I can think of is opening a second pipe in reverse 
	 * direction; if there's nothing to be read but EOF, the program could be 
	 * started - else we get a single byte, signifying an error. */

	*cd_pid=fork();
	STOPIF_CODE_ERR( *cd_pid == -1, errno,
			"Cannot fork colordiff program");

	if (!*cd_pid)
	{
		close(success[0]);

		fdflags=fcntl(success[1], F_GETFD);
		fdflags |= FD_CLOEXEC;
		fcntl(success[1], F_SETFD, fdflags);


		STOPIF_CODE_ERR( 
				( dup2(pipes[0], STDIN_FILENO) |
					close(pipes[1]) |
					close(pipes[0]) ) == -1, errno,
				"Redirecting IO didn't work");

		execlp( program, program, NULL);


		/* "" as value means best effort, so no error; any other string should 
		 * give an error. */
		if (opt__get_int(OPT__COLORDIFF) != 0)
		{
			fdflags=errno;
			if (!fdflags) fdflags=EINVAL;

			/* Report an error to the parent. */
			write(success[1], &fdflags, sizeof(fdflags));

			STOPIF_CODE_ERR_GOTO(1, fdflags, quit,
					"!Cannot start colordiff program \"%s\"", program);
		}

		close(success[1]);

		/* Well ... do the best. */
		/* We cannot use STOPIF() and similar, as that would return back up to 
		 * main - and possibly cause problems somewhere else. */
		status=df___cheap_colordiff();

quit:
		exit(status ? 1 : 0);
	}

	close(pipes[0]);
	close(success[1]);

	status=read(success[0], &fdflags, sizeof(fdflags));
	close(success[0]);
	STOPIF_CODE_ERR( status>0, fdflags,
			"!The colordiff program \"%s\" doesn't accept any data.\n"
			"Maybe it couldn't be started, or stopped unexpectedly?",
			opt__get_string(OPT__COLORDIFF) );	


	/* For svn+ssh connections a ssh process is spawned off.
	 * If we don't set the CLOEXEC flag, it inherits the handle, and so the 
	 * colordiff child will never terminate - it might get data from ssh, after 
	 * all. */
	fdflags=fcntl(pipes[1], F_GETFD);
	fdflags |= FD_CLOEXEC;
	/* Does this return errors? */
	fcntl(pipes[1], F_SETFD, fdflags);

	*handle=pipes[1];
	DEBUGP("colordiff is %d", *cd_pid);

ex:
	return status;
}


/** Prints diffs for all entries with estat::entry_status or 
 * estat::remote_status set. */
int df___diff_wc_remote(struct estat *entry, apr_pool_t *pool)
{
	int status;
	struct estat **sts;
	int removed;
	char *fn, *special_stg;
	apr_pool_t *subpool;


	status=0;
	subpool=NULL;
	STOPIF( apr_pool_create(&subpool, pool), NULL);

	removed = 
		( ((entry->remote_status & FS_REPLACED) == FS_REMOVED) ? 1 : 0 ) |
		( ((entry->entry_status  & FS_REPLACED) == FS_REMOVED) ? 2 : 0 );

	STOPIF( ops__build_path(&fn, entry), NULL);
	DEBUGP("%s: removed=%X loc=%s rem=%s", fn, removed,
			st__status_string_fromint(entry->entry_status),
			st__status_string_fromint(entry->remote_status));

	/* TODO: option to print the whole lot of removed and "new" lines for 
	 * files existing only at one point? */
	switch (removed)
	{
		case 3:
			/* Removed both locally and remote; no change to print. (?) */
			break;

		case 1:
			/* Remotely removed. */
			STOPIF_CODE_EPIPE( printf("Only locally: %s\n", fn), NULL);
			break;

		case 2:
			/* Locally removed. */
			STOPIF_CODE_EPIPE( printf("Only in the repository: %s\n", fn), NULL);
			break;

		case 0:
			/* Exists on both; do recursive diff. */

			if (entry->entry_status || entry->remote_status)
			{
				special_stg=NULL;

				if (S_ISDIR(entry->updated_mode))
				{
					/* TODO: meta-data diff? */
					if (entry->entry_count)
					{
						sts=entry->by_inode;
						while (*sts)
						{
							STOPIF( df___diff_wc_remote(*sts, subpool), NULL);
							sts++;
						}
					}
				}
				else
					STOPIF( df___type_def_diff(entry, entry->repos_rev, subpool), NULL);
			}

			break;
	}

ex:
	/* This is of type (void), so we don't have any status to check. */
	if (subpool) apr_pool_destroy(subpool);

	return status;
}


/** Set the entry as BASE (has no changes). */
int df___reset_remote_st(struct estat *sts)
{
	sts->remote_status=0;
	return 0;
}


/** Does a repos/repos diff.
 * Currently works only for files. */
int df___repos_repos(struct estat *sts)
{
	int status;
	char *fullpath, *path;
	struct estat **children;


	STOPIF( ops__build_path( &fullpath, sts), NULL);
	DEBUGP("%s: %s", fullpath, st__status_string_fromint(sts->remote_status));

	STOPIF( hlp__format_path( sts, fullpath, &path), NULL);

	if ((sts->remote_status & FS_REPLACED) == FS_REPLACED)
		STOPIF_CODE_EPIPE( 
				printf("Completely replaced: %s\n", path), NULL);
	else if (sts->remote_status & FS_NEW)
		STOPIF_CODE_EPIPE( 
				printf("Only in r%llu: %s\n", 
					(t_ull)opt_target_revision2, path), NULL);
	else if ((sts->remote_status & FS_REPLACED) == FS_REMOVED)
		STOPIF_CODE_EPIPE( 
				printf("Only in r%llu: %s\n", 
					(t_ull)opt_target_revision, path), NULL);
	else if (sts->remote_status)
		switch (sts->st.mode & S_IFMT)
		{
			case S_IFDIR:
				/* TODO: meta-data diff? */
				if (sts->entry_count)
				{
					children=sts->by_inode;
					while (*children)
						STOPIF( df___repos_repos(*(children++)), NULL);
				}

				break;

				/* Normally a repos-repos diff can only show symlinks changing - 
				 * all other types of special entries get *replaced*. */
			case S_IFANYSPECIAL:
				/* We don't know yet which special type it is. */
			case S_IFLNK:
			case S_IFBLK:
			case S_IFCHR:
				STOPIF_CODE_EPIPE( printf("Special entry changed: %s\n", 
							path), NULL);
				/* Fallthrough */

			case S_IFREG:
				STOPIF( df__do_diff(sts, 
							opt_target_revision, opt_target_revision2, NULL), 
						NULL);
				break;

			default:
				BUG("type?");
		}

ex:
	return status;
}


/** -.
 *
 * We get the WC status, fetch the named changed entries, and call
 * an external diff program for each.
 *
 * As a small performance optimization we do that kind of parallel - 
 * while we're fetching a file, we run the diff. */
int df__work(struct estat *root, int argc, char *argv[])
{
	int status;
	int i, deinit;
	char **normalized;
	svn_revnum_t rev, base;
	char *norm_wcroot[2]= {".", NULL};


	status=0;
	deinit=1;

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	STOPIF( url__load_nonempty_list(NULL, 0), NULL);
	STOPIF(ign__load_list(NULL), NULL);

	signal(SIGINT, df___signal);
	signal(SIGTERM, df___signal);
	signal(SIGHUP, df___signal);
	signal(SIGCHLD, SIG_DFL);


	/* check for colordiff */
	if (( opt__get_int(OPT__COLORDIFF)==0 ||
				opt__doesnt_say_off(opt__get_string(OPT__COLORDIFF)) ) &&
			(isatty(STDOUT_FILENO) || 
			 opt__get_prio(OPT__COLORDIFF) > PRIO_PRE_CMDLINE) )
	{
		DEBUGP("trying to use colordiff");
		STOPIF( df___colordiff(&cdiff_pipe, &cdiff_pid), NULL);
	}

	/* TODO: If we get "-u X@4 Y@4:3 Z" we'd have to do different kinds of 
	 * diff for the URLs.
	 * What about filenames? */
	STOPIF( url__mark_todo(), NULL);

	switch (opt_target_revisions_given)
	{
		case 0:
			/* Diff WC against BASE. */

			action->local_callback=df___direct_diff;
			/* We know that we've got a wc base because of 
			 * waa__find_common_base() above. */
			STOPIF( waa__read_or_build_tree(root, argc, 
						normalized, argv, NULL, 1), NULL);
			break;

		case 1:
			/* WC against rX. */
			/* Fetch local changes ... */
			action->local_callback=st__progress;
			action->local_uninit=st__progress_uninit;
			STOPIF( waa__read_or_build_tree(root, argc, normalized, argv, NULL, 1), NULL);
			//			Has to set FS_CHILD_CHANGED somewhere

			/* Fetch remote changes ... */
			while ( ! ( status=url__iterator(&rev) ) )
			{
				STOPIF( cb__record_changes(root, rev, current_url->pool), NULL);
			}
			STOPIF_CODE_ERR( status != EOF, status, NULL);

			STOPIF( df___diff_wc_remote(root, current_url->pool), NULL);
			break;

		case 2:
			/* rX:Y.
			 * This works in a single loop because the URLs are sorted in 
			 * descending priority, and an entry removed at a higher priority 
			 * could be replaced by one at a lower. */
			/* TODO: 2 revisions per-URL. */

			/* If no entries are given, do the whole working copy. */
			if (!argc)
				normalized=norm_wcroot;

			while ( ! ( status=url__iterator(&rev) ) )
			{
				STOPIF( url__canonical_rev(current_url, &opt_target_revision), NULL);
				STOPIF( url__canonical_rev(current_url, &opt_target_revision2), NULL);

				/* Take the values at the first revision as base; say that we've 
				 * got nothing.  */
				current_url->current_rev=0;
				action->repos_feedback=df___reset_remote_st;
				STOPIF( cb__record_changes(root, opt_target_revision, 
							current_url->pool), NULL);

				/* Now get changes. We cannot do diffs directly, because
				 * we must not use the same connection for two requests 
				 * simultaneously. */
				action->repos_feedback=NULL;

				/* We say that the WC root is at the target revision, but that some 
				 * paths are not. */
				base=current_url->current_rev;
				current_url->current_rev=opt_target_revision2;
				STOPIF( cb__record_changes_mixed(root, opt_target_revision2, 
							normalized, base, current_url->pool), 
						NULL);
			}
			STOPIF_CODE_ERR( status != EOF, status, NULL);


			/* If we'd use the log functions to get a list of changed files 
			 * we'd be slow for large revision ranges; for the various 
			 * svn_ra_do_update, svn_ra_do_diff2 and similar functions we'd 
			 * need the (complete) working copy base to get deltas against (as 
			 * we don't know which entries are changed).
			 *
			 * This way seems to be the fastest, and certainly the easiest for 
			 * now. */
			/* "time fsvs diff -r4:4" on "ssh+svn://localhost/..." for 8400 
			 * files gives a real time of 3.6sec.
			 * "time fsvs diff > /dev/null" on "ssh+svn://localhost/..." for 840 
			 * of 8400 files changed takes 1.8sec.
			 * */
			/* A possible idea would be to have a special delta-editor that 
			 * accepts (not already known) directories as unchanged.
			 * Then it should be possible [1] to ask for the *needed* parts 
			 * only, which should save a fair bit of bandwidth. 
			 *
			 * Ad 1: Ignoring "does not exist" messages when we say "directory 
			 * 'not-needed' is already at revision 'target'" and this isn't 
			 * true. TODO: Test whether all ra layers make that possible. */

			STOPIF( df___repos_repos(root), NULL);
			status=0;
			break;
		default:
			BUG("what?");
	}

	STOPIF( df__do_diff(NULL, 0, 0, 0), NULL);

ex:
	if (deinit)
	{
		deinit=0;
		i=df___cleanup();
		if (!status && i)
			STOPIF(i, NULL);
	}

	return status;
}

