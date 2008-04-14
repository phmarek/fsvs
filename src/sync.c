/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

/** \file
 * Synchronize from repository - \ref sync-repos command.
 *
 * Load the repository tree and store it as last used-
 * so that the next commit sends all changes against this
 * current repository state.
 *
 * */

/** \addtogroup cmds
 * 
 * \section sync-repos
 *
 * \code
 * fsvs sync-repos [-r rev] [working copy base]
 * \endcode
 * 
 * This command loads the file list from the repository.
 * A following commit will send all differences and make the repository data
 * identical to the local.
 * 
 * This is normally not needed; the use cases are
 * - debugging and
 * - recovering from data loss in \c $FSVS_WAA (\c /var/spool/fsvs ).
 * 
 * It is (currently) important if you want to backup two similar 
 * machines. Then you can commit one machine into a subdirectory of your 
 * repository, make a copy of that directory for another machine, and
 * sync this other directory on the other machine.
 *
 * A commit then will transfer only _changed_ files; so if the two machines 
 * share 2GB of binaries (\c /usr , \c /bin , \c /lib , ...) then 
 * these 2GB are still shared in the repository, although over 
 * time they will deviate (as both committing machines know 
 * nothing of the other path with identical files).
 * 
 * This kind of backup could be substituted by several levels 
 * of repository paths, which get "overlayed" in a defined priority.
 * So the base directory, which all machines derive from, will be committed 
 * from one machine, and it's no longer necessary for all machines to send 
 * identical files into the repository.
 * 
 * The revision argument should only ever be used for debugging; if you fetch
 * a filelist for a revision, and then commit against later revisions, 
 * problems are bound to occur.
 *
 *
 * \note There's an issue in subversion, to collapse identical files in the
 * repository into a single storage. That would ease the simple backup 
 * example, in that there's not so much storage needed over time; but the 
 * network transfers would still be much more than needed.
 *
 * */


#include <apr_md5.h>
#include <apr_pools.h>
#include <apr_user.h>
#include <apr_file_io.h>
#include <subversion-1/svn_delta.h>
#include <subversion-1/svn_ra.h>
#include <subversion-1/svn_error.h>
#include <subversion-1/svn_string.h>
#include <subversion-1/svn_time.h>

#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>


#include "global.h"
#include "status.h"
#include "checksum.h"
#include "est_ops.h"
#include "commit.h"
#include "waa.h"
#include "url.h"
#include "status.h"
#include "update.h"
#include "racallback.h"
#include "helper.h"


int sync__progress(struct estat *sts)
{
	int status;
	struct sstat_t st;
	char *path;


	status=0;
	STOPIF( ops__build_path(&path, sts), NULL);

	STOPIF( st__rm_status(sts), NULL);

	/* If the entry is a special node (symlink or device), we have
	 * a little problem here.
	 *
	 * On a sync we don't get the text of the entries - so we don't
	 * know which kind of special entry we have, and so we don't know
	 * which unix-mode (S_ISCHR etc.) we have to use and write.
	 *
	 * We could do one of those:
	 * - Fetch the entry to know the type.
	 *   This is slow, because we have to do a roundtrip for each entry, 
	 *   and that perhaps a thousand times.
	 * - We could use another property.
	 *   That makes us incompatible to subversion.
	 * - We could remove the check in ops__save_1entry().
	 *   Which mode should we write?
	 *
	 * If the entry exists and we can lstat() it, we have no problem -
	 * we know a correct mode, and the MD5 says whether the data matches.
	 * We just have to repair the entry_type field.
	 *
	 *
	 * The old sync-repos didn't set FT_ANYSPECIAL, and just wrote 
	 * this entry as a file.
	 * So it would be shown as removed.
	 *
	 * We do that now, too. If the entry gets reverted, we have it's
	 * correct meta-data - until then we don't worry.
	 * */
	if ( hlp__lstat(path, &st) == 0 )
	{
		if (sts->entry_type == FT_ANYSPECIAL)
		{
			sts->st=st;
			sts->entry_type=ops___filetype(& sts->st);
		}

		/* We fetch the dev/inode to get a correct sorting.
		 *
		 * We don't use the whole inode - we'd store the *current* mtime
		 * and ctime and don't know whether this file has changed.
		 * We use ctime/mtime only *if they are empty*, ie. haven't been given 
		 * from the repository. */
		sts->st.ino=st.ino;
		sts->st.dev=st.dev;
		sts->st.size=st.size;
		/* We don't store that in the repository, so take the current value.  
		 * */
		sts->st.ctim=st.ctim;

		if (!(sts->remote_status & FS_META_MTIME))
			sts->st.mtim=st.mtim;
		if (!(sts->remote_status & FS_META_OWNER))
			sts->st.uid=st.uid;
		if (!(sts->remote_status & FS_META_GROUP))
			sts->st.gid=st.gid;
		if (!(sts->remote_status & FS_META_UMODE))
			sts->st.mode=st.mode;

		/* If we do a directory, we set the \c RF_CHECK flag, so that new 
		 * entries will be found. */
		if (S_ISDIR(sts->st.mode))
			sts->flags |= RF_CHECK;
	}
	else
	{
		if (sts->entry_type == FT_ANYSPECIAL)
		{
			sts->entry_type=FT_FILE;
			sts->st.mode= (sts->st.mode & ~S_IFMT) | S_IFREG;
		}
	}

	/* We have to re-sort the directories. */
	if (S_ISDIR(sts->st.mode))
		sts->to_be_sorted=1;


ex:
	return status;
}

	
/** -.
 *
 * Could possibly be folded into the new update. */
int sync__work(struct estat *root, int argc, char *argv[])
{
	int status;
	svn_error_t *status_svn;
	svn_revnum_t rev;
	int i;


	status=0;
	status_svn=NULL;
	STOPIF( waa__find_base(root, &argc, &argv), NULL);
	STOPIF( url__load_nonempty_list(NULL, 0), NULL);

	/* We cannot easily format the paths for arguments ... first, we don't 
	 * have any (normally) */

	for(i=0; i<urllist_count; i++)
	{
		current_url=urllist[i];
		STOPIF( url__open_session(&session), NULL);

		DEBUGP("doing URL %s", current_url->url);


		if (opt_target_revisions_given)
			rev=opt_target_revision;
		else
			rev=current_url->target_rev;

		/* Giving a simple SVN_INVALID_REVNUM to ->set_path() doesn't work - we 
		 * get an error "Bogus revision report". Get the real HEAD. */
		/* \todo: get latest for each url, let user specify rev for each url */
		if (rev == SVN_INVALID_REVNUM)
		{
			STOPIF_SVNERR( svn_ra_get_latest_revnum,
					(session, &rev, global_pool));
			DEBUGP("HEAD is at %ld", rev);
		}
		else
			rev=opt_target_revision;

		/* Say we don't have any data. */
		current_url->current_rev=0;

		STOPIF( cb__record_changes(session, root, rev, global_pool), NULL);
		printf("%s for %s rev\t%llu.\n",
				action->name[0], current_url->url,
				(t_ull)rev);
		if (!action->is_compare)
		{
			/* set new revision */
			DEBUGP("setting revision to %llu", (t_ull)rev);
			STOPIF( ci__set_revision(root, rev), NULL);
		}
	}

	if (action->is_compare)
	{
		/* This is for remote-status. Just nothing to be done. */
	}
	else
	{
		/* See the comment at the end of commit.c - atomicity for writing
		 * these files. */
		STOPIF( waa__output_tree(root), NULL);
		STOPIF( url__output_list(), NULL);
		/* The copyfrom database is no longer valid. */
		STOPIF( waa__delete_byext(wc_path, WAA__COPYFROM_EXT, 1), NULL);
	}


ex:
	STOP_HANDLE_SVNERR(status_svn);
ex2:
	return status;
}

