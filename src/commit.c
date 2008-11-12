/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
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



/** Typedef needed for \a ci___send_user_props(). See there.  */
typedef svn_error_t *(*change_any_prop_t) (void *baton,
		const char *name,
		const svn_string_t *value,
		apr_pool_t *pool);


unsigned committed_entries;


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
int ci__action(struct estat *sts)
{
	int status;
	char *path;


	STOPIF( ops__build_path(&path, sts), NULL);

	STOPIF_CODE_ERR( sts->flags & RF_CONFLICT, EBUSY,
			"!The entry \"%s\" is still marked as conflict.", path);

	if (sts->entry_status ||
			(sts->flags & RF___COMMIT_MASK) )
		ops__mark_parent_cc(sts, entry_status);

	STOPIF( st__progress(sts), NULL);

ex:
	return status;
}


/** Removes the flags saying that this entry was copied, recursively.
 *
 * Does stop on new copy-bases. 
 *
 * Is needed because a simple <tt>"cp -a"</tt> wouldn't even go down into 
 * the child-entries - there's nothing to do there! */
void ci___unset_copyflags(struct estat *root)
{
	struct estat **sts;

	/* Delete the RF_ADD and RF_COPY_BASE flag, but set the FS_NEW status 
	 * instead.  */
	root->flags &= ~(RF_ADD | RF_COPY_BASE | RF_COPY_SUB);
	/* Set the current url for this entry. */
	root->url=current_url;

	if (S_ISDIR(root->st.mode) && root->entry_count)
	{
		sts=root->by_inode;
		while (*sts)
		{
			if (! ( (*sts)->flags & RF_COPY_BASE) )
			{
				ci___unset_copyflags(*sts);
			}

			sts++;
		}
	}
}


/** Send the user-defined properties.
 *
 * The property table is left cleaned up, ie. any deletions that were 
 * ordered by the user have been done -- no properties with \c 
 * prp__prop_will_be_removed() will be here.
 * */
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
	status=prp__open_byestat( sts, GDBM_WRITER, &db);
	DEBUGP("prop open: %d", status);
	if (status == ENOENT)
		status=0;
	else
	{
		STOPIF( status, NULL);

		status=prp__first(db, &key);
		while (status==0)
		{
			STOPIF( prp__fetch(db, key, &value), NULL);

			if (hlp__is_special_property_name(key.dptr))
			{
				DEBUGP("ignoring %s - should not have been taken?", key.dptr);
			}
			else if (prp__prop_will_be_removed(value))
			{
				DEBUGP("removing property %s", key.dptr);

				STOPIF_SVNERR( function, (baton, key.dptr, NULL, pool) );
				STOPIF( hsh__register_delete(db, key), NULL);
			}
			else
			{
				DEBUGP("sending property %s=(%d)%.*s", key.dptr, 
						value.dsize, value.dsize, value.dptr);

				str=svn_string_ncreate(value.dptr, value.dsize-1, pool);
				STOPIF_SVNERR( function, (baton, key.dptr, str, pool) );
			}

			IF_FREE(value.dptr);

			status=prp__next( db, &key, &key);
		}

		/* Anything but ENOENT spells trouble. */
		if (status != ENOENT)
			STOPIF(status, NULL);
		status=0;
	}

	if (props)
	{
		STOPIF( hsh__collect_garbage(db, NULL), NULL);
		*props=db;
	}
	else
	{
		/* A hsh__close() does the garbage collection, too. */
		STOPIF( hsh__close(db, status), NULL);
	}

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
	int status;
	svn_error_t *status_svn;


	status=0;
	/* The meta-data properties are not sent for a symlink. */
	if (!S_ISLNK(sts->updated_mode))
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
	int transfer_text, has_manber;


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

	if (!S_ISLNK(sts->updated_mode))
		STOPIF_SVNERR( ci___set_props, 
				(baton, sts, editor->change_file_prop, pool) ); 

	/* By now we should know if our file really changed. */
	BUG_ON( sts->entry_status & FS_LIKELY );

	/* However, sending fulltext only if it really changed DOES make
	 * a difference if you do not have a gigabit pipe to your
	 * server. ;)
	 * The RF_ADD was replaced by FS_NEW above. */
	DEBUGP("%s: status %s; flags %s", sts->name,
			st__status_string(sts), 
      st__flags_string_fromint(sts->flags));


	transfer_text= sts->entry_status & (FS_CHANGED | FS_NEW | FS_REMOVED);
	/* In case the file is identical to the original copy source, we need 
	 * not send the data to the server.
	 * BUT we have to store the correct MD5 locally; as the source file may 
	 * have changed, we re-calculate it - that has the additional advantage 
	 * that the manber-hashes get written, for faster comparision next time.  
	 *
	 * I thought about using cs__compare_file() in the local check sequence 
	 * to build a new file; but if anything goes wrong later, the file would 
	 * be overwritten with the wrong data.
	 * That's true if something goes wrong here, too.
	 *
	 * Another idea would be to build the new manber file with another name, 
	 * and only rename if it actually was committed ... but there's a race, 
	 * too. And we couldn't abort the check on the first changed bytes, and 
	 * we'd need doubly the space, ...
	 *
	 * TODO: run the whole fsvs commit process against an unionfs, and use 
	 * that for local transactions. */
	if (!transfer_text && !(sts->flags & RF___IS_COPY))
	{
		DEBUGP("hasn't changed, and no copy.");
	}
	else
	{
		has_manber=0;
		switch (sts->st.mode & S_IFMT)
		{
			case S_IFLNK:
				STOPIF( ops__link_to_string(sts, filename, &cp), NULL);
				STOPIF( hlp__local2utf8(cp, &cp, -1), NULL);
				/* It is not defined whether svn_stringbuf_create copies the string,
				 * takes the character pointer into the pool, or whatever.
				 * Knowing people wanted. */
				str=svn_stringbuf_create(cp, pool);
				break;
			case S_IFBLK:
			case S_IFCHR:
				/* See above */
				/* We only put ASCII in this string */
				str=svn_stringbuf_create(
						ops__dev_to_filedata(sts), pool);
				break;
			case S_IFREG:
				STOPIF( apr_file_open(&a_stream, filename, APR_READ, 0, pool),
						"open file \"%s\" for reading", filename);

				s_stream=svn_stream_from_aprfile (a_stream, pool);

				/* We need the local manber hashes and MD5s to detect changes;
				 * the remote values would be needed for delta transfers. */
				has_manber= (sts->st.size >= CS__MIN_FILE_SIZE);
				if (has_manber)
					STOPIF( cs__new_manber_filter(sts, s_stream, &s_stream, pool), NULL );

				/* That's needed only for actually putting the data in the 
				 * repository - for local re-calculating it isn't. */
				if (transfer_text)
				{
					status= prp__get(db, propval_commitpipe, &encoder_prop);
					/* The user-defined properties have already been sent, so the 
					 * propval_commitpipe would already be cleared; we don't need to 
					 * check for prp__prop_will_be_removed().  */

					if (status == 0)
					{
						STOPIF( hlp__encode_filter(s_stream, encoder_prop.dptr, 0,
									filename, &s_stream, &encoder, pool), NULL );
						encoder->output_md5= &(sts->md5);
					}
				}
				break;
			default:
				BUG("invalid/unknown file type 0%o", sts->st.mode);
		}

		/* for special nodes */
		if (str) s_stream=svn_stream_from_stringbuf (str, pool);

		BUG_ON(!s_stream);

		if (transfer_text)
		{
			DEBUGP("really sending ...");
			STOPIF_SVNERR( editor->apply_textdelta,
					(baton, 
					 NULL, // checksum of old file,
					 pool,
					 &delta_handler,
					 &delta_baton));

			/* If we're transferring the data, we always get an MD5 here. We can 
			 * take the local value, if it had to be encoded. */
			STOPIF_SVNERR( svn_txdelta_send_stream,
					(s_stream, delta_handler,
					 delta_baton,
					 sts->md5, pool) );
			DEBUGP("after sending encoder=%p", encoder);
		}
		else
		{
			DEBUGP("doing local MD5.");
			/* For a non-changed entry, simply pass the data through the MD5 (and, 
			 * depending on filesize, the manber filter).
			 * If the manber filter already does the MD5, we don't need it a second 
			 * time. */
			STOPIF( hlp__stream_md5(s_stream, 
						has_manber ? NULL : sts->md5), NULL);
		}

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
			cp=cs__md52hex(sts->md5);
			DEBUGP("Sending original MD5 as %s", cp);

			stg=svn_string_create(cp, pool);
			STOPIF_SVNERR( editor->change_file_prop,
					(baton, propname_origmd5, stg, pool) );
		}

	}


	STOPIF( cs__set_file_committed(sts), NULL);

ex:
	if (a_stream)
	{
		/* As this file was opened read only, we can dismiss any errors.
		 * We could give them only if everything else worked ... */
		apr_file_close(a_stream);
	}
	if (db) hsh__close(db, status);

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
	int status;
	struct estat *sts;
	apr_pool_t *subpool;
	int i, exists_now;
	char *filename;
	char* utf8_filename;
	svn_error_t *status_svn;
	char *src_path;
	svn_revnum_t src_rev;
	struct sstat_t stat;
  

	status=0;
	subpool=NULL;
	DEBUGP("commit_dir with baton %p", dir_baton);
	for(i=0; i<dir->entry_count; i++)
	{
		sts=dir->by_inode[i];

		/* The flags are stored persistently; we have to check whether this 
		 * entry shall be committed. */
		if ( (sts->flags & RF___COMMIT_MASK) && sts->do_this_entry)
		{
			/* Did we change properties since last commit? Then we have something 
			 * to do. */
			if (sts->flags & RF_PUSHPROPS)
				sts->entry_status |= FS_PROPERTIES;
		}
		else if (sts->entry_status)
		{
			/* The entry_status is set depending on the do_this_entry already;
			 * if it's not 0, it's got to be committed. */
			/* Maybe a child needs attention (with FS_CHILD_CHANGED), so we have 
			 * to recurse.  */
		}
		else
			/* Completely ignore item if nothing to be done. */
			continue;


		/* clear an old pool */
		if (subpool) apr_pool_destroy(subpool);
		/* get a fresh pool */
		STOPIF( apr_pool_create_ex(&subpool, pool, NULL, NULL), 
				"no pool");

		STOPIF( ops__build_path(&filename, sts), NULL);
		/* as the path needs to be canonical we strip the ./ in front */
		STOPIF( hlp__local2utf8(filename+2, &utf8_filename, -1), NULL );

		DEBUGP("%s: action %X, updated mode 0%o, flags %X, filter %d", 
				filename, sts->entry_status, sts->updated_mode, sts->flags,
				ops__allowed_by_filter(sts));

		if (ops__allowed_by_filter(sts))
			STOPIF( st__status(sts), NULL);

		exists_now= !(sts->flags & RF_UNVERSION) && 
			( (sts->entry_status & (FS_NEW | FS_CHANGED | FS_META_CHANGED)) ||
				(sts->flags & (RF_ADD | RF_PUSHPROPS | RF_COPY_BASE)) 
			);

		if ( (sts->flags & RF_UNVERSION) ||
				(sts->entry_status & FS_REMOVED) )
		{
			DEBUGP("deleting %s", sts->name);
			/* that's easy :-) */
			STOPIF_SVNERR( editor->delete_entry,
					(utf8_filename, SVN_INVALID_REVNUM, dir_baton, subpool) );

			committed_entries++;

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
		/* TODO: Could we use FS_REMOVED here?? */
		if (hlp__lstat(filename, &stat))
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

		/* In case this entry is a directory that's only done because of its 
		 * children we shouldn't change its known data - we'd silently change 
		 * eg. the mtime. */
		if (sts->do_this_entry && ops__allowed_by_filter(sts))
		{
			sts->st=stat;
			DEBUGP("set st for %s", sts->name);
		}


		/* We need a baton. */
		baton=NULL;
		/* If this entry has the RF_ADD or RF_COPY_BASE flag set, or is FS_NEW, 
		 * it is new (as far as subversion is concerned).
		 * If this is an implicitly copied entry, subversion already knows 
		 * about it, so use open_* instead of add_*. */
		if ((sts->flags & (RF_ADD | RF_COPY_BASE) ) || 
				(sts->entry_status & FS_NEW) )
		{
			/* New entry, fetch handle via add_* below. */
		}
		else
		{
			status_svn=
				(S_ISDIR(sts->updated_mode) ?
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
				 * Now these directories need not exist in URL2 - we create them on 
				 * demand.  
				 * */
				if (S_ISDIR(sts->st.mode) &&
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
							S_ISDIR(sts->st.mode) ?
							"open_directory" : "open_file");
			}

			DEBUGP("baton for mod %s %p (parent %p)", 
					sts->name, baton, dir_baton);
		}

		if (!baton)
		{
			DEBUGP("new %s (parent %p)", 
					sts->name, dir_baton);

			/* Maybe that test should be folded into cm__get_source -- that would 
			 * save the assignments in the else-branch.
			 * But we'd have to check for ENOENT again - it's not allowed if 
			 * RF_COPY_BASE is set, but possible if this flag is not set. So we'd  
			 * not actually get much. */
			if (sts->flags & RF_COPY_BASE)
			{
				status=cm__get_source(sts, filename, &src_path, &src_rev, 1);
				BUG_ON(status == ENOENT, "copy but not copied?");
				STOPIF(status, NULL);
			}
			else
			{
				/* Set values to "not copied". */
				src_path=NULL;
				src_rev=SVN_INVALID_REVNUM;
			}

			/* TODO: src_sts->entry_status newly added? Then remember for second  
			 * commit!
			 * */

			DEBUGP("adding %s with %s:%ld",
					filename, src_path, src_rev);
			/** \name STOPIF_SVNERR_INDIR */
			STOPIF_SVNERR_TEXT( 
					(S_ISDIR(sts->updated_mode) ?
					 editor->add_directory : editor->add_file),
					(utf8_filename, dir_baton, 
					 src_path, src_rev, 
					 subpool, &baton),
					"%s(\"%s\", source=\"%s\"@%s)",
					S_ISDIR(sts->updated_mode) ? "add_directory" : "add_file",
					filename, src_path, hlp__rev_to_string(src_rev));
			DEBUGP("baton for new %s %p (parent %p)", 
					sts->name, baton, dir_baton);


			/* If it's copy base, we need to clean up all flags below; else we 
			 * just remove an (ev. set) add-flag. */
			if (sts->flags & RF_COPY_BASE)
				ci___unset_copyflags(sts);
			else
			{
				sts->flags &= ~RF_ADD;

				sts->entry_status |= FS_NEW | FS_META_CHANGED;
			}
		}


		committed_entries++;
		DEBUGP("doing changes, flags=%X", sts->flags);
		/* Now we have a baton. Do changes. */
		if (S_ISDIR(sts->updated_mode))
		{
			STOPIF_SVNERR( ci__directory, (editor, sts, baton, subpool) );
			STOPIF_SVNERR( editor->close_directory, (baton, subpool) );
		}
		else
		{
			STOPIF_SVNERR( ci__nondir, (editor, sts, baton, subpool) );
			STOPIF_SVNERR( editor->close_file, (baton, NULL, subpool) );
		}


		/* Now this paths exists in this URL. */
		if (url__current_has_precedence(sts->url))
		{
			DEBUGP("setting URL of %s", filename);
			sts->url=current_url;
			sts->repos_rev = SET_REVNUM;
		}
	}


	/* When a directory has been committed (with all changes), 
	 * we can drop the check flag.
	 * If we only do parts of the child list, we must set it, so that we know 
	 * to check for newer entries on the next status. (The directory 
	 * structure must possibly be built in the repository, so we have to do 
	 * each layer, and after a commit we take the current timestamp -- so we 
	 * wouldn't see changes that happened before the partly commit.) */
	if (! (dir->do_this_entry && ops__allowed_by_filter(dir)) )
		dir->flags |= RF_CHECK;
	else
		dir->flags &= ~RF_CHECK;


	/* That this entry belongs to this URL has already been set by the 
	 * parent loop.  */


	/* Given this example:
	 *   $ mkdir -p dir/sub/subsub
	 *   $ touch dir/sub/subsub/file
	 *   $ fsvs ci dir/sub/subsub
	 *
	 * Now "sub" gets committed because of its children; as having a 
	 * directory *without* meta-data in the repository is worse than having 
	 * valid data set, we push the meta-data properties for *new* 
	 * directories, and otherwise if they should be done and got something 
	 * changed. */
	/* Regarding the "dir->parent" check: If we try to send properties for 
	 * the root directory, we get "out of date" ... even if nothing changed.  
	 * So don't do that now, until we know a way to make that work. 
	 *
	 * Problem case: user creates an empty directory in the repository "svn 
	 * mkdir url:///", then sets this directory as base, and we try to commit - 
	 * "it's empty, after all".
	 * Needing an update is not nice - but maybe what we'll have to do. */
	if ((dir->do_this_entry && 
				ops__allowed_by_filter(dir) && 
				dir->parent &&
				/* Are there properties to push? */
				(dir->entry_status & (FS_META_CHANGED | FS_PROPERTIES))) ||
			(dir->entry_status & FS_NEW))
	{
		STOPIF_SVNERR( ci___set_props, 
				(dir_baton, dir, editor->change_dir_prop, pool) ); 

		STOPIF( ci___send_user_props(dir_baton, dir, 
					editor->change_dir_prop, NULL, pool), NULL);
	}


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
	apr_file_t *af;


	status=0;
	STOPIF( waa__get_tmp_name( NULL, filename, &af, global_pool), NULL);

	/* we close the file, as an editor might delete the file and
	 * write a new. */
	STOPIF( apr_file_close(af), "close commit message file");

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
	int status;
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
	time_t delay_start;


	status=0;
	status_svn=NULL;
	edit_baton=NULL;
	editor=NULL;

	opt__set_int(OPT__CHANGECHECK, 
			PRIO_MUSTHAVE,
			opt__get_int(OPT__CHANGECHECK) | CHCHECK_DIRS | CHCHECK_FILE);


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

	STOPIF( url__open_session(NULL), NULL);


	only_check_status=2;
	/* This is the first step that needs some wall time - descending
	 * through the directories, reading inodes */
	STOPIF( waa__read_or_build_tree(root, argc, normalized, argv, 
				NULL, 0), NULL);


	if (opt_commitmsgfile)
	{
		STOPIF_CODE_ERR( fstat(commitmsg_fh, &st) == -1, errno,
				"cannot estimate size of %s", opt_commitmsgfile);

		if (st.st_size == 0)
		{
			/* We're not using some mapped memory. */
			DEBUGP("empty file");
			opt_commitmsg="(none)";
		}
		else
		{
			DEBUGP("file is %llu bytes",  (t_ull)st.st_size);

			opt_commitmsg=mmap(NULL, st.st_size, PROT_READ, MAP_SHARED,
					commitmsg_fh, 0);
			STOPIF_CODE_ERR(!opt_commitmsg, errno,
					"mmap commit message (%s, %llu bytes)", 
					opt_commitmsgfile, (t_ull)st.st_size);
		}
		close(commitmsg_fh);
	}

	STOPIF( hlp__local2utf8(opt_commitmsg, &utf8_commit_msg, -1),
			"Conversion of the commit message to utf8 failed");

	printf("Committing to %s\n", current_url->url);

	STOPIF_SVNERR( svn_ra_get_commit_editor,
			(current_url->session,
			 &editor,
			 &edit_baton,
			 utf8_commit_msg,
			 ci__callback,
			 root,
			 NULL, // apr_hash_t *lock_tokens,
			 FALSE, // svn_boolean_t keep_locks,
			 global_pool) );

	if (opt_commitmsgfile && st.st_size != 0)
		STOPIF_CODE_ERR( munmap(opt_commitmsg, st.st_size) == -1, errno,
				"munmap()");
	if (commitmsg_is_temp)
		STOPIF_CODE_ERR( unlink(opt_commitmsgfile) == -1, errno,
				"Cannot remove temporary message file %s", opt_commitmsgfile);


	/* The whole URL is at the same revision - per definition. */
	STOPIF_SVNERR( editor->open_root,
			(edit_baton, current_url->current_rev, global_pool, &root_baton) );

	/* Only children are updated, not the root. Do that here. */
	if (ops__allowed_by_filter(root))
		STOPIF( hlp__lstat( root->name, &root->st), NULL);

	committed_entries=0;
	/* This is the second step that takes time. */
	STOPIF_SVNERR( ci__directory,
			(editor, root, root_baton, global_pool));

	/* If an error occurred, abort the commit. */
	if (!status)
	{
		if (opt__get_int(OPT__EMPTY_COMMIT)==OPT__NO && 
				committed_entries==0)
		{
			printf("Avoiding empty commit as requested.\n");
			goto abort_commit;
		}


		STOPIF_SVNERR( editor->close_edit, (edit_baton, global_pool) );
		edit_baton=NULL;

		delay_start=time(NULL);

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

		/* We do the delay here ... here we've got a chance that the second 
		 * wrap has already happened because of the IO above. */
		STOPIF( hlp__delay(delay_start, DELAY_COMMIT), NULL);
	}

ex:
	STOP_HANDLE_SVNERR(status_svn);

ex2:
	if (status && edit_baton)
	{
abort_commit:
		/* If there has already something bad happened, it probably
		 * makes no sense checking the error code. */
		editor->abort_edit(edit_baton, global_pool);
	}

	return status;
}

