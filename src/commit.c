/************************************************************************
 * Copyright (C) 2005-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

/** \file
 * \ref commit action.
 *
 * This is a bit hairy in that the order in which we process files (sorted
 * by inode, not in the directory structure) is not allowed 
 * for a subversion editor.
 *
 * We have to read the complete tree, get the changes and store what we 
 * want to do, and send these changes in a second run.
 *
 *
 * \section commit_2_revs Committing two revisions at once
 * Handling identical files; using hardlinks; creating two revisions on 
 * commit.
 *
 * There are some use-cases where we'd like to store the data only a single 
 * time in the repository, so that multiple files are seen as identical:
 * - hardlinks should be stored as hardlink; but subversion doesn't allow 
 *   something like that currently. Using some property pointing to the 
 *   "original" file would be some way; but for compatibility with other 
 *   subversion clients the data would have to be here, too. \n
 *   Using copy-from would mess up the history of the file.
 * - Renames of changed files. Subversion doesn't accept copy-from links to 
 *   new files; we'd have to create two revisions: one with the data, and 
 *   the other with copyfrom information (or the other way around).
 * */

/** \addtogroup cmds
 * 
 * \section commit
 *
 * \code
 * fsvs commit [-m "message"|-F filename] [-v] [-C [-C]] [PATH [PATH ...]]
 * \endcode
 * 
 * Commits the current state into the repository.
 * It is possible to commit only parts of a working copy into the repository.
 * 
 * 
 * <example>
 * Your working copy is \c /etc , and you've set it up and committed already.
 * Now you've changed \c /etc/hosts , and \c /etc/inittab . 
 * Since these are non-related changes, you'd like them to be 
 * in separate commits.
 *
 * So you simply run these commands:
 * \code
 * fsvs commit -m "Added some host" /etc/hosts
 * fsvs commit -m "Tweaked default runlevel" /etc/inittab
 * \endcode
 *
 * If you're currently in \c /etc , you can even drop the \c /etc/ in 
 * front, and just use the filenames.
 * 
 * This extended path handling on the commandline is not yet available for
 * every command. Most of them still expect you to be in the 
 * working copy root.
 * 
 * Please see \ref status for explanations on \c -v and \c -C .
 * For advanced backup usage see also \ref FSVS_PROP_COMMIT_PIPE. 
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
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>


#include "global.h"
#include "status.h"
#include "checksum.h"
#include "waa.h"
#include "est_ops.h"
#include "props.h"
#include "options.h"
#include "ignore.h"
#include "cp_mv.h"
#include "racallback.h"
#include "url.h"
#include "helper.h"



/** Typedef needed for \a ci__set_props() and \a prp__send(). See there. */
typedef svn_error_t *(*change_any_prop_t) (void *baton,
		const char *name,
		const svn_string_t *value,
		apr_pool_t *pool);



/** -.
 * */
int ci__set_revision(struct estat *this, svn_revnum_t rev)
{
	int i;

	/* should be benchmarked.
	 * perhaps use better locality by doing levels at once. */
	if (this->url == current_url)
		this->repos_rev=rev;

	if (S_ISDIR(this->st.mode))
		for(i=0; i<this->entry_count; i++)
			ci__set_revision(this->by_inode[i], rev);

	return 0;
}


/** Callback for successfull commits.
 *
 * This is the only place that gets the new revision number 
 * told.
 *
 * \c svn_ra.h does not tell whether these strings are really UTF8. I think 
 * they must be ASCII, except if someone uses non-ASCII-user names ...  
 * which nobody does.  */
svn_error_t * ci__callback (
		svn_revnum_t new_revision,
		const char *utf8_date,
		const char *utf8_author,
		void *baton)
{
	struct estat *root UNUSED=baton;
	int status;


	status=0;
	printf("committed revision\t%ld on %s as %s\n",
			new_revision, utf8_date, utf8_author);

	/* recursively set the new revision */
//	STOPIF( ci__set_revision(root, new_revision), NULL);
	current_url->current_rev = new_revision;
	
//ex:
	RETURN_SVNERR(status);
}


/** -.
 *
 * This callback is called by input_tree and build_tree. */
int ci__action(struct estat *sts,
		char *path)
{
	if (sts->entry_status ||
			(sts->flags & (RF_ADD | RF_UNVERSION | RF_PUSHPROPS)) )
	{
		/* mark the entry as to-be-done.
		 * mark the parents too, so that we don't have to search
		 * in-depth. */
		while (sts->parent && !(sts->parent->entry_status & FS_CHILD_CHANGED))
		{
			sts->parent->entry_status |= FS_CHILD_CHANGED;
			sts=sts->parent;
		}
	}

	return st__progress(sts, path);
}


/** Send the user-defined properties.
 * See also \a ci__set_props(). */
int ci___send_user_props(void *baton, 
		struct estat *sts,
		change_any_prop_t function,
		hash_t *props,
		apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;
	datum key, value;
	hash_t db;
	svn_string_t *str;


	db=NULL;

	/* Do user-defined properties.
	 * Could return ENOENT if none. */
	status=prp__open_byestat( sts, GDBM_READER, &db);
	DEBUGP("prop open: %d", status);
	if (status == ENOENT)
		status=0;
	else
	{
		STOPIF( status, NULL);

		prp__first( db, &key);
		while (key.dptr)
		{
			STOPIF( prp__fetch(db, key, &value), NULL);
			str=svn_string_ncreate(value.dptr, value.dsize-1, pool);

			DEBUGP("sending property %s=(%d)%.*s", key.dptr, 
			value.dsize, value.dsize, value.dptr);
			STOPIF_SVNERR( function, (baton, key.dptr, str, pool) );

			/* Ignore errors - we'll surely get ENOENT. */
			prp__next( db, &key, key);
		}
	}

	if (props)
		*props=db;
	else
		STOPIF(hsh__close(db), NULL);

ex:
	return status;
}


/** Send the meta-data-properties for \a baton.
 *
 * We hope that group/user names are ASCII; the names of "our" properties 
 * are known, and contain no characters above \\x80. 
 *
 * We get the \a function passed, because subversion has different property 
 * setters for files and directories.
 *
 * If \a props is not \c NULL, we return the properties' handle. */
svn_error_t *ci___set_props(void *baton, 
		struct estat *sts,
		change_any_prop_t function,
		apr_pool_t *pool)
{
	const char *ccp;
	svn_string_t *str;
	apr_status_t status;
	svn_error_t *status_svn;


	status=0;
	/* The meta-data properties are not sent for a symlink. */
	if (sts->entry_type != FT_SYMLINK)
	{
		/* owner */
		str=svn_string_createf (pool, "%u %s", 
				sts->st.uid, hlp__get_uname(sts->st.uid, "") );
		STOPIF_SVNERR( function, (baton, propname_owner, str, pool) );

		/* group */
		str=svn_string_createf (pool, "%u %s", 
				sts->st.gid, hlp__get_grname(sts->st.gid, "") );
		STOPIF_SVNERR( function, (baton, propname_group, str, pool) );

		/* mode */
		str=svn_string_createf (pool, "0%03o", sts->st.mode & 07777);
		STOPIF_SVNERR( function, (baton, propname_umode, str, pool) );

		/* mtime. Extra const char * needed. */
		ccp=(char *)svn_time_to_cstring (
				apr_time_make( sts->st.mtim.tv_sec, sts->st.mtim.tv_nsec/1000),
				pool);
		str=svn_string_create(ccp, pool);
		STOPIF_SVNERR( function, (baton, propname_mtime, str, pool) );
	}

ex:
	RETURN_SVNERR(status);
}


/** Commit function for non-directory entries.
 *
 * Here we handle devices, symlinks and files.
 *
 * The given \a baton is already for the item; we got it from \a add_file 
 * or \a open_file.
 * We just have to put data in it.
 * */
svn_error_t *ci__nondir(const svn_delta_editor_t *editor, 
		struct estat *sts, 
		void *baton, 
		apr_pool_t *pool)
{
	svn_txdelta_window_handler_t delta_handler;
	void *delta_baton;
	svn_error_t *status_svn;
	svn_stream_t *s_stream;
	char *cp;
	char *filename;
	int status;
	svn_string_t *stg;
	apr_file_t *a_stream;
	svn_stringbuf_t *str;
	hash_t db;
	datum encoder_prop;
	struct encoder_t *encoder;


	str=NULL;
	a_stream=NULL;
	s_stream=NULL;
	db=NULL;
	encoder=NULL;

	STOPIF( ops__build_path(&filename, sts), NULL);


	/* The only "real" information symlinks have is the target
	 * they point to. We don't set properties which won't get used on 
	 * update anyway - that saves a tiny bit of space.
	 * What we need to send (for symlinks) are the user-defined properties.  
	 * */
	/* Should we possibly send the properties only if changed? Would not make 
	 * much difference, bandwidth-wise. */
	/* if ((sts->flags & RF_PUSHPROPS) ||
		 (sts->entry_status & (FS_META_CHANGED | FS_NEW)) ) */
	STOPIF( ci___send_user_props(baton, sts, 
				editor->change_file_prop, &db, pool), NULL);

	if (sts->entry_type != FT_SYMLINK)
		STOPIF_SVNERR( ci___set_props, 
				(baton, sts, editor->change_file_prop, pool) ); 

	/* By now we should know if our file really changed. */
	BUG_ON( sts->entry_status & FS_LIKELY );

	/* However, sending fulltext only if it really changed DOES make
	 * a difference if you do not have a gigabit pipe to your
	 * server. ;)
	 * The RF_ADD was replaced by FS_NEW above. */
	DEBUGP("%s: status %s, flags %s", sts->name,
			st__status_string(sts), 
      st__flags_string_fromint(sts->flags));
	if (!(sts->entry_status & (FS_CHANGED | FS_NEW | FS_REMOVED)))
	{
		DEBUGP("File has not actually changed, skipping fulltext upload.");

		/* mark file as commited */
		STOPIF( cs__set_file_committed(sts), NULL);
	}
	else
	{
		/* The data has changed - send the "body" (or "full-text"). */
		DEBUGP("sending text");
		switch (sts->entry_type)
		{
			case FT_SYMLINK:
				STOPIF( ops__link_to_string(sts, filename, &cp), NULL);
				STOPIF( hlp__local2utf8(cp, &cp, -1), NULL);
				/* It is not defined whether svn_stringbuf_create copies the string,
				 * takes the character pointer into the pool, or whatever.
				 * Knowing people wanted. */
				str=svn_stringbuf_create(cp, pool);
				break;
			case FT_BDEV:
			case FT_CDEV:
				/* See above */
				/* We only put ASCII in this string */
				str=svn_stringbuf_create(
						ops__dev_to_filedata(sts), pool);
				break;
			case FT_FILE:
				STOPIF( apr_file_open(&a_stream, filename, APR_READ, 0, pool),
						"open file \"%s\" for reading", filename);

				s_stream=svn_stream_from_aprfile (a_stream, pool);

				/* We need the local manber hashes and MD5s to detect changes;
				 * the remote values would be needed for delta transfers. */
				if (sts->st.size >= CS__MIN_FILE_SIZE)
					STOPIF( cs__new_manber_filter(sts, s_stream, &s_stream, pool), NULL );

				status= prp__get(db, propval_commitpipe, &encoder_prop);

				if (status == 0)
				{
					STOPIF( hlp__encode_filter(s_stream, encoder_prop.dptr, 0,
								&s_stream, &encoder, pool), NULL );
				}
				break;
			default:
				BUG("invalid/unknown file type 0x%X", sts->entry_type);
		}

		/* for special nodes */
		if (str) s_stream=svn_stream_from_stringbuf (str, pool);

		BUG_ON(!s_stream);

		STOPIF_SVNERR( editor->apply_textdelta,
				(baton, 
				 NULL, // checksum of old file,
				 pool,
				 &delta_handler,
				 &delta_baton));

		DEBUGP("sending ...");
		STOPIF_SVNERR( svn_txdelta_send_stream,
				(s_stream, 
				 delta_handler,
				 delta_baton,
				 sts->md5, pool) );
		DEBUGP("after sending encoder=%p", encoder);

		STOPIF_SVNERR( svn_stream_close, (s_stream) );

		/* If it's a special entry (device/symlink), set the special flag. */
		if (str)
		{
			stg=svn_string_create(propval_special, pool);
			STOPIF_SVNERR( editor->change_file_prop,
					(baton, propname_special, stg, pool) );
		}

		/* If the entry was encoded, send the original MD5 as well. */
		if (encoder)
		{
			memcpy(sts->md5, encoder->md5, sizeof(sts->md5));
			cp=cs__md52hex(sts->md5);
			DEBUGP("Sending original MD5 as %s", cp);

			stg=svn_string_create(cp, pool);
			STOPIF_SVNERR( editor->change_file_prop,
					(baton, propname_origmd5, stg, pool) );
		}
	} /* send full-text if changed*/

	STOPIF( cs__set_file_committed(sts), NULL);


ex:
	if (a_stream)
	{
		/* As this file was opened read only, we can dismiss any errors.
		 * We could give them only if everything else worked ... */
		apr_file_close(a_stream);
	}
	if (db) hsh__close(db);
	IF_FREE(encoder);

	RETURN_SVNERR(status);
}


/** Commit function for directories.
 * */
svn_error_t *ci__directory(const svn_delta_editor_t *editor, 
		struct estat *dir, 
		void *dir_baton, 
		apr_pool_t *pool)
{
	void *baton;
	apr_status_t status;
	struct estat *sts;
	apr_pool_t *subpool;
	int i, exists_now;
	char *filename;
	char* utf8_filename;
	svn_error_t *status_svn;
	struct stat dummy_stat64;
	struct estat *src_sts;
	char *src_path;
	svn_revnum_t src_rev;


	status=0;
	src_path=NULL;
	subpool=NULL;
	DEBUGP("commit_dir with baton %p", dir_baton);
	for(i=0; i<dir->entry_count; i++)
	{
		sts=dir->by_inode[i];

		/* Did we change properties since last commit? Then we have something 
		 * to do. */
		if (sts->flags & RF_PUSHPROPS)
			sts->entry_status |= FS_PROPERTIES;

		/* completely ignore item if nothing to be done */
		if (!(sts->entry_status || sts->flags))
			continue;

		/* clear an old pool */
		if (subpool) apr_pool_destroy(subpool);
		/* get a fresh pool */
		STOPIF( apr_pool_create_ex(&subpool, pool, NULL, NULL), 
				"no pool");

		STOPIF( ops__build_path(&filename, sts), NULL);
		/* as the path needs to be canonical we strip the ./ in front */
		STOPIF( hlp__local2utf8(filename+2, &utf8_filename, -1), NULL );

		STOPIF( st__status(sts, filename), NULL);

		DEBUGP("%s: action is %X, type is %X, flags %X", 
				filename, sts->entry_status, sts->entry_type, sts->flags);

		exists_now= !(sts->flags & RF_UNVERSION) && 
			( (sts->entry_status & (FS_NEW | FS_CHANGED | FS_META_CHANGED)) ||
				(sts->flags & (RF_ADD | RF_PUSHPROPS)) 
			);

		if ( (sts->flags & RF_UNVERSION) ||
				(sts->entry_status & FS_REMOVED) )
		{
			DEBUGP("deleting %s", sts->name);
			/* that's easy :-) */
			STOPIF_SVNERR( editor->delete_entry,
					(utf8_filename, SVN_INVALID_REVNUM, dir_baton, subpool) );

			if (!exists_now)
			{
				DEBUGP("%s=%d doesn't exist anymore", sts->name, i);
				/* remove from data structures */

				STOPIF( ops__delete_entry(dir, NULL, i, UNKNOWN_INDEX),
						NULL);
				STOPIF( waa__delete_byext(filename, WAA__FILE_MD5s_EXT, 1), NULL);
				STOPIF( waa__delete_byext(filename, WAA__PROP_EXT, 1), NULL);
				i--;
				continue;
			}
		} 


		/* If there something to do - get a baton.
		 * Else we're finished with this one. */
		if (!exists_now && !(sts->entry_status & FS_CHILD_CHANGED))
			continue;

		/* If we would send some data, verify the state of the entry.
		 * Maybe it's a temporary file, which is already deleted.
		 * As we'll access this entry in a few moments, the additional lookup
		 * doesn't hurt much.
		 * 
		 * (Although I'd be a bit happier if I found a way to do that better ...
		 * currently I split by new/existing entry, and then by 
		 * directory/everything else.
		 * Maybe I should change that logic to *only* split by entry type.
		 * But then I'd still have to check for directories ...)
		 * 
		 * So "Just Do It" (tm). */
		/* access() would possibly be a bit lighter, but doesn't work
		 * for broken symlinks. */
		if (lstat(filename, &dummy_stat64) == -1)
		{
			/* If an entry doesn't exist, but *should*, as it's marked RF_ADD,
			 * we fail (currently).
			 * Could be a warning with a default action of STOP. */
			STOPIF_CODE_ERR( sts->flags & RF_ADD, ENOENT,
					"Entry %s should be added, but doesn't exist.",
					filename);

			DEBUGP("%s doesn't exist, ignoring (%d)", filename, errno);
			continue;
		}

		/* We need a baton. */
		baton=NULL;
		if (!((sts->flags & RF_ADD) || 
					(sts->entry_status & FS_NEW)) )
		{
			status_svn=
				(sts->entry_type == FT_DIR ?
				 editor->open_directory : editor->open_file)
				( utf8_filename, dir_baton, 
					current_url->current_rev,
					subpool, &baton);		

			if (status_svn)
			{
					DEBUGP("got error %d %d", status_svn->apr_err, SVN_ERR_FS_PATH_SYNTAX);
				/* In case we're having more than a single URL, it is possible that 
				 * directories we got from URL1 get changed, and that these changes 
				 * should be committed to URL2.
				 * Now these directories need not exist in URL2 - we create them on demand.  
				 * */
				if (sts->entry_type == FT_DIR &&
				(
						status_svn->apr_err == SVN_ERR_FS_PATH_SYNTAX ||
						status_svn->apr_err == SVN_ERR_FS_NOT_DIRECTORY ) &&
						urllist_count>1)
				{
					DEBUGP("dir %s didn't exist, trying to create", filename);
					/* Make sure we'll re-try */
					baton=NULL;
					status_svn=NULL;
				}
				else
					STOPIF_CODE_ERR(1, status_svn->apr_err,
							sts->entry_type == FT_DIR ?
							"open_directory" : "open_file");
			}

			DEBUGP("baton for mod %s %p (parent %p)", 
					sts->name, baton, dir_baton);
		}

		if (!baton)
		{
			DEBUGP("new %s (parent %p)", 
					sts->name, dir_baton);

			status=cm__get_source(sts, filename, NULL, 
					&src_sts, NULL, &src_rev);
			if (status == ENOENT)
			{
			  src_path=NULL;
				src_rev=SVN_INVALID_REVNUM;
			}
			else
			{
				STOPIF(status, NULL);

				STOPIF( urls__full_url( src_sts, NULL, &src_path), NULL);
			}

			/* TODO: src_sts->entry_status newly added? Then remember for second  
			 * commit!
			 * */

			/* TODO: remember this filename for purging from the database after 
			 * commit. */

			DEBUGP("adding %s with %s:%ld",
					filename, src_path, src_rev);
			/** \name STOPIF_SVNERR_INDIR */
			STOPIF_SVNERR_TEXT( 
					(sts->entry_type == FT_DIR ?
					 editor->add_directory : editor->add_file),
					sts->entry_type == FT_DIR ? "add_directory" : "add_file",
					(utf8_filename, dir_baton, 
					 src_path, src_rev, 
					 subpool, &baton) 
					);
			DEBUGP("baton for new %s %p (parent %p)", 
					sts->name, baton, dir_baton);

			/* Delete the RF_ADD flag, but set the FS_NEW status instead. */
			sts->flags &= ~RF_ADD;
			sts->entry_status = FS_NEW | FS_META_CHANGED;

			/* Set the current url for this entry. */
			sts->url=current_url;
		}


		DEBUGP("doing changes, flags=%X", sts->flags);
		/* Now we have a baton. Do changes. */
		if (sts->entry_type == FT_DIR)
		{
			STOPIF_SVNERR( ci__directory, (editor, sts, baton, subpool) );
			STOPIF_SVNERR( editor->close_directory, (baton, subpool) );
		}
		else
		{
			STOPIF_SVNERR( ci__nondir, (editor, sts, baton, subpool) );
			STOPIF_SVNERR( editor->close_file, (baton, NULL, subpool) );
		}

		/* Update data structures.
		 * This must be done here, as updating a directory will change
		 * its mtime, link count, ...
		 * In case a directory had many changed files it's possible that
		 * the old filename cache is no longer valid, so get it afresh. */
		STOPIF( ops__build_path(&filename, sts), NULL);
		/* We already stat()ed this file, so just copy. */
		hlp__copy_stats(&dummy_stat64, &(sts->st));

		sts->url=current_url;
	}


	/* If we try to send properties for the root directory, we get "out of 
	 * date" ... even if nothing changed. So don't do that now, until we
	 * know a way to make that work. 
	 *
	 * Problem case: user creates an empty directory in the repository "svn 
	 * mkdir url:///", then sets this directory as base, and we try to commit - 
	 * "it's empty, after all".
	 * Needing an update is not nice - but maybe what we'll have to do. */
	if (dir->parent)
	{
		/* Do the meta-data and other properties of the current directory. */
		// TODO			if (sts->do_full)
		STOPIF( ci___send_user_props(dir_baton, dir, 
					editor->change_dir_prop, NULL, pool), NULL);

		if (dir->entry_status & (FS_META_CHANGED | FS_NEW))
			STOPIF_SVNERR( ci___set_props, 
					(dir_baton, dir, editor->change_dir_prop, pool) ); 
	}


	/* When a directory has been committed (with all changes), 
	 * we can drop the check flag.
	 * If we only do parts of the child list, we must set it, so that we know 
	 * to check for newer entries on the next status. (The directory 
	 * structure must possibly be built in the repository, so we have to do 
	 * each layer, and after a commit we take the current timestamp -- so we 
	 * wouldn't see changes that happened before the partly commit.) */
	if (dir->do_full_child)
		dir->flags &= ~RF_CHECK;
	else
		dir->flags |= RF_CHECK;

	/* The root entry has no URL, so no revision. */
	if (dir->parent)
		dir->repos_rev = SET_REVNUM;

ex:
	if (subpool) 
		apr_pool_destroy(subpool);
	RETURN_SVNERR(status);
}


/** Start an editor, to get a commit message. 
 *
 * We look for \c $EDITOR and $VISUAL -- to fall back on good ol' vi. */
int ci__getmsg(char **filename)
{
	char *editor_cmd, *cp;
	int l,status;
	int fh;


	status=0;
	*filename=strdup("/tmp/commit-tmp.XXXXXX");
	fh=mkstemp(*filename);
	STOPIF_CODE_ERR(fh == -1, errno,
			"mkstemp(%s)", *filename);

	/* we close the file, as an editor might delete the file and
	 * write a new. */
	STOPIF_CODE_ERR( close(fh) == -1,
			errno, "close commit message file");

	editor_cmd=getenv("EDITOR");
	if (!editor_cmd) editor_cmd=getenv("VISUAL");
	if (!editor_cmd) editor_cmd="vi";

	l=strlen(editor_cmd) + 1 + strlen(opt_commitmsgfile) + 1;
	cp=malloc(l);
	STOPIF_CODE_ERR(!cp, ENOMEM, 
			"cannot allocate command buffer (%d bytes)", l);

	strcpy(cp, editor_cmd);
	strcat(cp, " ");
	strcat(cp, opt_commitmsgfile);

	l=system(cp);
	STOPIF_CODE_ERR(l == -1, errno, "fork() failed");

	STOPIF_CODE_ERR(l, WEXITSTATUS(l),
			"spawned editor exited with %d, signal %d", 
			WEXITSTATUS(l),
			WIFSIGNALED(l) ? WTERMSIG(l) : 0);
	status=0;

ex:
	return status;
}


/** The main commit function.
 *
 * It does as much setup as possible before traversing the tree - to find 
 * errors (no network, etc.) as soon as possible.
 *
 * The message file gets opened here to verify its existence,
 * and to get a handle to it. If we're doing \c chdir()s later we don't 
 * mind; the open handle let's us read when we need it. And the contents 
 * are cached only as long as necessary. */
int ci__work(struct estat *root, int argc, char *argv[])
{
	apr_status_t status;
	svn_error_t *status_svn;
	const svn_delta_editor_t *editor;
	void *edit_baton;
	void *root_baton;
	struct stat st;
	int commitmsg_fh,
			commitmsg_is_temp;
	char *utf8_commit_msg;
	char **normalized;
	const char *url_name;


	status=0;
	status_svn=NULL;
	edit_baton=NULL;
	editor=NULL;

	if (!opt_checksum) opt_checksum++;

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	/* Check if there's an URL defined before asking for a message */
	STOPIF( url__load_nonempty_list(NULL, 0), NULL);

	if (urllist_count==1)
		current_url=urllist[0];
	else
	{
		url_name=opt__get_string(OPT__COMMIT_TO);
		STOPIF_CODE_ERR( !url_name || !*url_name, EINVAL,
				"!Which URL would you like to commit to?\n"
				"Please choose one (config option \"commit_to\").");

		STOPIF( url__find_by_name(url_name, &current_url), 
				"!No URL named \"%s\" could be found.", url_name);
	}


	/* That was done with do_chdir==1, but we don't do that anymore - 
	 * that would barf if a subentry was given on the command line. */
	STOPIF(ign__load_list(NULL), NULL);

	commitmsg_is_temp=!opt_commitmsg && !opt_commitmsgfile;
	if (commitmsg_is_temp)
		STOPIF( ci__getmsg(&opt_commitmsgfile), NULL);

	/* This cannot be used uninitialized, but gcc doesn't know */
	commitmsg_fh=-1;

	/* If there's a message file, open it here. (Bug out early, if necessary) */
	if (opt_commitmsgfile)
	{
		commitmsg_fh=open(opt_commitmsgfile, O_RDONLY);
		STOPIF_CODE_ERR( commitmsg_fh<0, errno, 
				"cannot open file %s", opt_commitmsgfile);
	}

	/* warn/break if file is empty ?? */

	STOPIF( url__open_session(&session), NULL);


	/* This is the first step that needs some wall time - descending
	 * through the directories, reading inodes */
	STOPIF( waa__read_or_build_tree(root, argc, normalized, argv, 
				NULL, 0), NULL);


	if (opt_commitmsgfile)
	{
		STOPIF_CODE_ERR( fstat(commitmsg_fh, &st) == -1, errno,
				"cannot estimate size of %s", opt_commitmsgfile);

		DEBUGP("file is %llu bytes",  (t_ull)st.st_size);
		opt_commitmsg=mmap(NULL, st.st_size, PROT_READ, MAP_SHARED,
				commitmsg_fh, 0);
		STOPIF_CODE_ERR(!opt_commitmsg, errno,
				"mmap commit message (%s, %llu bytes)", 
				opt_commitmsgfile, (t_ull)st.st_size);

		close(commitmsg_fh);
	}

	STOPIF( hlp__local2utf8(opt_commitmsg, &utf8_commit_msg, -1),
			"Conversion of the commit message to utf8 failed");

	printf("Committing to %s\n", current_url->url);

	STOPIF_SVNERR( svn_ra_get_commit_editor,
			(session,
			 &editor,
			 &edit_baton,
			 utf8_commit_msg,
			 ci__callback,
			 root,
			 NULL, // apr_hash_t *lock_tokens,
			 FALSE, // svn_boolean_t keep_locks,
			 global_pool) );

	if (opt_commitmsgfile)
		STOPIF_CODE_ERR( munmap(opt_commitmsg, st.st_size) == -1, errno,
				"munmap()");
	if (commitmsg_is_temp)
		STOPIF_CODE_ERR( unlink(opt_commitmsgfile) == -1, errno,
				"Cannot remove temporary message file %s", opt_commitmsgfile);


	/* The whole URL is at the same revision - per definition. */
	STOPIF_SVNERR( editor->open_root,
			(edit_baton, current_url->current_rev, global_pool, &root_baton) );

	/* This is the second step that takes time. */
	STOPIF_SVNERR( ci__directory,
			(editor, root, root_baton, global_pool));

	if (!status)
	{
		STOPIF_SVNERR( editor->close_edit, (edit_baton, global_pool) );
		edit_baton=NULL;

		/* Has to write new file, if commit succeeded. */
		if (!status)
		{
			/* We possibly have to use some generation counter:
			 * - write the URLs to a temporary file,
			 * - write the entries,
			 * - rename the temporary file.
			 * Although, if we're cut off anywhere, we're not consistent with the
			 * data.
			 * Just use unionfs - that's easier. */
			STOPIF( waa__output_tree(root), NULL);
			STOPIF( url__output_list(), NULL);
		}
	}

ex:
	STOP_HANDLE_SVNERR(status_svn);

ex2:
	if (status && edit_baton)
	{
		/* If there has already something bad happened, it probably
		 * makes no sense checking the error code. */
		editor->abort_edit(edit_baton, global_pool);
	}

	return status;
}

