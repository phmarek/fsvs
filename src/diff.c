/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>


#include "global.h"
#include "revert.h"
#include "helper.h"
#include "interface.h"
#include "url.h"
#include "options.h"
#include "est_ops.h"
#include "ignore.h"
#include "waa.h"
#include "racallback.h"
#include "cp_mv.h"
#include "warnings.h"


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
 * Please see also \ref o_diff and \ref o_colordiff. */


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

	if (l1 != l2 || strcmp(buf_new, buf_old) !=0)
		/* Different */
		status=printf("-%s\n+%s\n", buf_old, buf_new);
	else
		status=printf(" %s\n", buf_old);

	STOPIF_CODE_ERR(status<0, errno,
			"Meta-data diff output error");

	status=0;

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
 * other repository version. */
int df__do_diff(struct estat *sts, 
		svn_revnum_t rev1, svn_revnum_t rev2)
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
	struct sstat_t new_stat;
	svn_revnum_t from, to;
	char short_desc[10];
	char *new_mtime_string, *other_mtime_string;
	char *url_to_fetch;
	int is_copy;
	int fdflags;


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
				"Child %d terminated abnormally", last_child);

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

	current_url = sts->url;


	/* We have to fetch a file and do the diff, so open a session. */
	STOPIF( url__open_session(NULL), NULL);

	/* The function rev__get_file() overwrites the data in \c *sts with 
	 * the repository values - mtime, ctime, etc.
	 * We use this as an advantage and remember the current time - so that
	 * we can print both. */
	/* \e From is always the "old" - base revision, or first given revision.
	 * \e To is the newer version - 2nd revision, or local file. */
	if (rev2 != 0)
	{
		STOPIF( rev__get_file(sts, rev2, NULL,
					&to, &last_tmp_file2,
					global_pool),
				NULL);
	}
	new_stat=sts->st;

	/* Now fetch the \e old version. */
	STOPIF( rev__get_file(sts, rev1, url_to_fetch,
				&from, &last_tmp_file,
				global_pool),
			NULL);


	last_child=fork();
	STOPIF_CODE_ERR( last_child == -1, errno,
			"Cannot fork diff program");

	if (!last_child)
	{
		STOPIF( ops__build_path( &path, sts), NULL);
		STOPIF( hlp__format_path(sts, path, &disp_dest), NULL);

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


		/* Checking \b which return value we get is unnecessary ... 
		 * On \b every error we get \c -1 .*/
		/* We need not be nice with memory usage - we'll be replaced soon. */

		/* 30 chars should be enough for everyone */
		b1=malloc(len_s + 60 + 30);
		b2=malloc(len_d + 60 + 30);

		new_mtime_string=strdup(ctime(&new_stat.mtim.tv_sec ));
		STOPIF_ENOMEM(!new_mtime_string);
		other_mtime_string=strdup(ctime(&sts->st.mtim.tv_sec ));
		STOPIF_ENOMEM(!other_mtime_string);

		sprintf(b1, "%s  \tRev. %llu  \t(%-24.24s)", 
				disp_source, (t_ull) from, other_mtime_string);

		if (rev2 == 0)
		{
			sprintf(b2, "%s  \tLocal version  \t(%-24.24s)", 
					disp_dest, new_mtime_string);
			strcpy(short_desc, "local");
		}
		else
		{
			sprintf(b2, "%s  \tRev. %llu  \t(%-24.24s)", 
					disp_dest, (t_ull) to, new_mtime_string);
			sprintf(short_desc, "r%llu", (t_ull) to);
		}


		/* Print header line, just like a recursive diff does. */
		STOPIF_CODE_ERR( printf("diff -u %s.r%llu %s.%s\n", 
					disp_source, (t_ull)from, 
					disp_dest, short_desc) < 0, errno,
				"Diff header");


		if (opt_verbose>0)
		{
			STOPIF(	df___print_meta( "Mode: 0%03o",
						sts->st.mode & 07777,
						META_DIFF_DELIMITER,
						new_stat.mode & 07777), 
					NULL);
			STOPIF(	df___print_meta( "MTime: %.24s", 
						other_mtime_string,
						META_DIFF_DELIMITER,
						new_mtime_string),
					NULL);
			STOPIF(	df___print_meta( "Owner: %d (%s)",
						sts->st.uid, hlp__get_uname(sts->st.uid, "undefined"),
						META_DIFF_DELIMITER,
						new_stat.uid, hlp__get_uname(new_stat.uid, "undefined") ),
					NULL);
			STOPIF(	df___print_meta( "Group: %d (%s)", 
						sts->st.gid, hlp__get_grname(sts->st.gid, "undefined"),
						META_DIFF_DELIMITER,
						new_stat.gid, hlp__get_grname(new_stat.gid, "undefined") ),
					NULL);
		}
		fflush(NULL);
		

		STOPIF_CODE_ERR( execlp( opt__get_string(OPT__DIFF_PRG), 
					opt__get_string(OPT__DIFF_PRG),
					opt__get_string(OPT__DIFF_OPT),
					last_tmp_file, 
					"--label", b1,
					(rev2 == 0 ? path : last_tmp_file2), 
					"--label", b2,
					opt__get_string(OPT__DIFF_EXTRA),
					NULL) == -1, errno,
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

	STOPIF( df__do_diff(NULL, 0, 0), NULL);

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


/** -. */
int df__action(struct estat *sts, char *fn)
{
	int status;
	svn_revnum_t rev1, rev2;


	status=0;
	if (sts->entry_type & FT_NONDIR)
	{
		DEBUGP("doing %s", fn);

		/* Find correct revision */
		switch (opt_target_revisions_given)
		{
			case 0:
				/* Has to be set per sts. */
				rev1=sts->repos_rev;
				rev2=0;
				if ( (sts->entry_status & FS_REMOVED))
				{
					printf("Only in repository: %s\n", fn);
					goto ex;
				}
				break;
			case 1:
				rev1=opt_target_revision;
				rev2=0;
				break;
			case 2:
				rev1=opt_target_revision;
				rev2=opt_target_revision2;
				break;
			default:
				BUG("too many revisions");
				/* To avoid "used uninitialized" */
				goto ex;
		}

		if (sts->entry_type & FT_IGNORE)
			goto ex;

		if ( (sts->entry_status & FS_NEW) || !sts->url)
		{
			if (sts->flags & RF___IS_COPY)
			{
				/* File was copied, we have a source */
			}
			else
			{
				if (opt_verbose>0)
					printf("Only in local filesystem: %s\n", fn);
				goto ex;
			}
		}

		/* Local files must have changed; for repos-only diffs do always. */
		if (sts->entry_status || opt_target_revisions_given)
		{
			DEBUGP("doing diff rev1=%llu rev2=%llu",
					(t_ull)rev1, (t_ull)rev2);
			STOPIF( df__do_diff(sts, rev1, rev2), NULL);


			/* If we don't wait for completion here, our temporary files are 
			 * found and reported as new. I don't want to compare strings ... are 
			 * there any better ideas?
			 * Maybe rev__get_file() should have a parameter telling *where* to 
			 * put that file - and for diff we'd use /tmp. */
			STOPIF( df__do_diff(NULL, 0, 0), NULL);
		}
	}
	else
	{
		/* Nothing to do for directories? */
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
	static const char program[]="colordiff";
	int status;
	char *tmp;
	const int tmp_size=16384;
	int pipes[2], fdflags;


	status=0;
	STOPIF_CODE_ERR( pipe(pipes) == -1, errno,
			"No more pipes");


	*cd_pid=fork();
	STOPIF_CODE_ERR( *cd_pid == -1, errno,
			"Cannot fork colordiff program");

	if (!*cd_pid)
	{
		STOPIF_CODE_ERR( 
				( dup2(pipes[0], STDIN_FILENO) |
					close(pipes[1]) |
					close(pipes[0]) ) == -1, errno,
				"Redirecting IO didn't work");

		execlp( program, program, NULL);

		if (opt__get_int(OPT__COLORDIFF) == OPT__YES)
			STOPIF_CODE_ERR(1, errno,
					"Cannot start \"%s\" program", program);

		/* Well ... do the best. */
		tmp=alloca(tmp_size);
		while ( (status=read(STDIN_FILENO,tmp, tmp_size)) > 0 )
			write(STDOUT_FILENO, tmp, status);

		STOPIF_CODE_ERR( status == -1, errno, "Cannot get data");
		exit(0);
	}

	close(pipes[0]);


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
	char **normalized;


	status=0;
	/* Default to non-recursive. */
	opt_recursive--;

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	STOPIF( url__load_nonempty_list(NULL, 0), NULL);
	STOPIF(ign__load_list(NULL), NULL);

	signal(SIGINT, df___signal);
	signal(SIGTERM, df___signal);
	signal(SIGHUP, df___signal);

	/* check for colordiff */
	if (opt__get_int(OPT__COLORDIFF) &&
			(isatty(STDOUT_FILENO) || 
			 opt__get_prio(OPT__COLORDIFF) > PRIO_PRE_CMDLINE) )
	{
		DEBUGP("trying to use colordiff");
		STOPIF( df___colordiff(&cdiff_pipe, &cdiff_pid), NULL);
	}


	status=waa__read_or_build_tree(root, argc, normalized, argv, NULL, 1);
	if (status == ENOENT)
		STOPIF( status, 
				"!No WAA information found; you probably didn't commit.");
	STOPIF( status, "No working copy base found?");

	STOPIF( df___cleanup(), NULL);

ex:
	return status;
}

