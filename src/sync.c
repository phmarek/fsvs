/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
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
 * This command loads the file list afresh from the repository. \n
 * A following commit will send all differences and make the repository data
 * identical to the local.
 * 
 * This is normally not needed; the only use cases are
 * - debugging and
 * - recovering from data loss in the \ref o_waa "$FSVS_WAA" area.
 * 
 * It might be of use if you want to backup two similar machines. Then you 
 * could commit one machine into a subdirectory of your repository, make a 
 * copy of that directory for another machine, and
 * \c sync this other directory on the other machine.
 *
 * A commit then will transfer only _changed_ files; so if the two machines 
 * share 2GB of binaries (\c /usr , \c /bin , \c /lib , ...) then 
 * these 2GB are still shared in the repository, although over 
 * time they will deviate (as both committing machines know 
 * nothing of the other path with identical files).
 * 
 * This kind of backup could be substituted by two or more levels of 
 * repository paths, which get \e overlaid in a defined priority.
 * So the base directory, which all machines derive from, will be committed 
 * from one machine, and it's no longer necessary for all machines to send 
 * identical files into the repository.
 * 
 * The revision argument should only ever be used for debugging; if you fetch
 * a filelist for a revision, and then commit against later revisions, 
 * problems are bound to occur.
 *
 *
 * \note There's issue 2286 in subversion which describes sharing
 * identical files in the repository in unrelated paths. By using this 
 * relaxes the storage needs; but the network transfers would still be much 
 * larger than with the overlaid paths.
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
#include "cache.h"
#include "revert.h"
#include "props.h"
#include "commit.h"
#include "waa.h"
#include "url.h"
#include "status.h"
#include "update.h"
#include "racallback.h"
#include "helper.h"


/** Get entries of directory, and fill tree.
 *
 * Most of the data should already be here; we just
 * fill the length of the entries in.
 * */
int sync___recurse(struct estat *cur_dir,
		apr_pool_t *pool)
{	
	int status;
	svn_error_t *status_svn;
	apr_pool_t *subpool, *subsubpool;
	apr_hash_t *dirents;
	char *path;
	const char *name;
	const void *key;
	void *kval;
	apr_hash_index_t *hi;
	svn_dirent_t *val;
	char *url, *path_utf8;
	struct svn_string_t *decoder;
	struct estat *sts;
	svn_stringbuf_t *entry_text;
	char *link_local;


	status=0;
	subpool=subsubpool=NULL;

	/* get a fresh pool */
	STOPIF( apr_pool_create_ex(&subpool, pool, NULL, NULL), 
			"no pool");

	STOPIF( ops__build_path( &path, cur_dir), NULL);
	DEBUGP("list of %s", path);
	STOPIF( hlp__local2utf8(path, &path_utf8, -1), NULL);

	STOPIF_SVNERR( svn_ra_get_dir2,
			(current_url->session, 
			 &dirents, NULL, NULL,
			 /* Use "" for the root, and cut the "./" for everything else. */
			 (cur_dir->parent) ? path_utf8 + 2 : "", 
			 current_url->current_rev,
			 SVN_DIRENT_HAS_PROPS | SVN_DIRENT_HAS_PROPS | 
			 SVN_DIRENT_KIND | SVN_DIRENT_SIZE,
			 subpool));

	for( hi=apr_hash_first(subpool, dirents); hi; hi = apr_hash_next(hi))
	{
		apr_hash_this(hi, &key, NULL, &kval);
		name=key;
		val=kval;


		STOPIF( cb__add_entry(cur_dir, name, NULL,
					NULL, 0, 0, NULL, 0, (void**)&sts), NULL);

		if (url__current_has_precedence(sts->url) &&
				!S_ISDIR(sts->st.mode))
		{
			/* File or special entry. */
			sts->st.size=val->size;

			decoder= sts->user_prop ? 
				apr_hash_get(sts->user_prop, 
						propval_updatepipe, APR_HASH_KEY_STRING) : 
					NULL;

			if (S_ISREG(sts->st.mode) && !decoder)
			{
				/* Entry finished. */
			}
			else if (S_ISREG(sts->st.mode) && val->size > 8192)
			{
				/* Make this size configurable? Remove altogether? After all, the 
				 * processing time needs not be correlated to the encoded size. */
				DEBUGP("file encoded, but too big for fetching (%llu)", 
						(t_ull)val->size);
			}
			else
			{
				/* Now we're left with special devices and small, encoded files. */
				STOPIF( url__full_url(sts, &url), NULL);

				/* get a fresh pool */
				STOPIF( apr_pool_create_ex(&subsubpool, subpool, NULL, NULL), 
						"no pool");

				/* That's the third time we access this file ...
				 * svn_ra needs some more flags for the directory listing functions. */
				STOPIF( rev__get_text_into_buffer(url, sts->repos_rev,
							decoder ? decoder->data : NULL,
							&entry_text, NULL, sts, NULL, subsubpool), NULL);

				sts->st.size=entry_text->len;
				DEBUGP("parsing %s as %llu: %s", url,
						(t_ull)sts->st.size, entry_text->data);

				/* If the entry exists locally, we might have a more detailed value 
				 * than FT_ANYSPECIAL. */
				if (!S_ISREG(sts->st.mode))
					/* We don't need the link destination; we already got the MD5. */
					STOPIF( ops__string_to_dev(sts, entry_text->data, NULL), NULL);

				/* For devices there's no length to compare; the rdev field 
				 * shares the space.
				 * And for normal files the size is already correct. */
				if (S_ISLNK(sts->st.mode))
				{
					/* Symlinks get their target translated to/from the locale, so 
					 * they might have a different length. */
					STOPIF( hlp__utf82local(entry_text->data+strlen(link_spec),
								&link_local, -1), NULL);
					sts->st.size = strlen(link_local);
				}

				if (subsubpool) apr_pool_destroy(subsubpool);
			}

			/* After this entry is done we can return a bit of memory. */
			if (sts->user_prop)
			{
				apr_pool_destroy(apr_hash_get(sts->user_prop, "", 0));
				sts->user_prop=NULL;
			}

			DEBUGP_dump_estat(sts);
		}

		/* We have to loop even through obstructed directories - some
		 * child may not be overlaid. */
		if (val->kind == svn_node_dir)
		{
			STOPIF( sync___recurse( sts, subpool), NULL);
		}

	}

ex:
	if (subpool) apr_pool_destroy(subpool);

	return status;
}


/** Repository callback.
 *
 * Here we get most data - all properties and the tree structure. */
int sync__progress(struct estat *sts)
{
	int status;
	struct sstat_t st;
	char *path;


	status=0;
	STOPIF( ops__build_path(&path, sts), NULL);

	STOPIF( waa__delete_byext( path, WAA__FILE_MD5s_EXT, 1), NULL);
	STOPIF( waa__delete_byext( path, WAA__PROP_EXT, 1), NULL);


	/* We get the current type in sts->new_rev_mode_packed, but we need 
	 * sts->st.mode set for writing. */
	sts->st.mode = (sts->st.mode & ~S_IFMT) | 
		PACKED_to_MODE_T(sts->new_rev_mode_packed);


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
		if ((sts->st.mode & S_IFMT) == 0)
		{
			sts->st=st;
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
		if (S_ISANYSPECIAL(sts->st.mode))
		{
			/* We don't know what it really is. BUG? */
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


	status=0;
	status_svn=NULL;
	STOPIF( waa__find_base(root, &argc, &argv), NULL);
	STOPIF( url__load_nonempty_list(NULL, 0), NULL);

	/* We cannot easily format the paths for arguments ... first, we don't 
	 * have any (normally) */

	while ( ! ( status=url__iterator(&rev) ) )
	{
		if (opt__verbosity() > VERBOSITY_VERYQUIET)
			printf("sync-repos for %s rev\t%llu.\n",
					current_url->url, (t_ull)rev);

		/* We have nothing ... */
		current_url->current_rev=0;
		STOPIF( cb__record_changes(root, rev, current_url->pool), NULL);

		/* set new revision */
		current_url->current_rev=rev;
		STOPIF( ci__set_revision(root, rev), NULL);

		STOPIF( sync___recurse(root, current_url->pool), NULL);
	}
	STOPIF_CODE_ERR( status != EOF, status, NULL);

	/* Take the correct values for the root. */
	STOPIF( hlp__lstat( ".", &root->st), NULL);
	root->flags |= RF_CHECK;


	/* See the comment at the end of commit.c - atomicity for writing
	 * these files. */
	STOPIF( waa__output_tree(root), NULL);
	/* The current revisions might have changed. */
	STOPIF( url__output_list(), NULL);
	/* The copyfrom database is no longer valid. */
	STOPIF( waa__delete_byext(wc_path, WAA__COPYFROM_EXT, 1), NULL);


ex:
	STOP_HANDLE_SVNERR(status_svn);
ex2:
	return status;
}

