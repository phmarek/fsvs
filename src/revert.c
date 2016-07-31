/************************************************************************
 * Copyright (C) 2006-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/
#include <unistd.h>
#include <stdio.h>

#include <subversion-1/svn_delta.h>
#include <subversion-1/svn_ra.h>

#include "revert.h"
#include "waa.h"
#include "est_ops.h"
#include "racallback.h"
#include "warnings.h"
#include "checksum.h"
#include "props.h"
#include "cache.h"
#include "helper.h"
#include "url.h"
#include "update.h"
#include "status.h"


/** \file
 * \ref revert action.
 * This reverts local changes, ie. resets the given paths to the repository
 * versions.
 * This cannot be undone by fsvs - keep backups :-) */

/** \addtogroup cmds
 *
 * \section revert
 *
 * \code
 * fsvs revert [-rRev] [-R] PATH [PATH...]
 * \endcode
 * 
 * This command replaces a local entry with its repository version.
 * 
 * If a directory is given on the command line \b all known entries <b>in 
 * this directory</b> are reverted to the old state; this behaviour can be 
 * modified with \ref glob_opt_rec "-R/-N", or see below.
 *
 * The reverted entries are printed, along with the status they had \b 
 * before the revert (because the new status is \e unchanged).
 *
 * If a revision is given, its data is taken; furthermore, the \b new 
 * status of that entry is shown.
 * \note Please note that mixed revision working copies are not possible; 
 * the \e BASE revision is not changed, and a simple \c revert without a 
 * revision arguments gives you that.
 *
 *
 * \subsection rev_cmp_up Difference to update
 * If you find that something doesn't work as it should, you can revert 
 * entries until you are satisfied, and directly \ref commit "commit" the 
 * new state. 
 * 
 * In contrast, if you \ref update "update" to an older version, you 
 * - cannot choose single entries (no mixed revision working copies),
 * - and you cannot commit the old version with changes, if later changes 
 *   would conflict in the repository.
 *
 *
 * \subsection rev_del Currently only known (already versioned) entries 
 * are handled.
 * If you need a switch (like \c --delete in \c rsync(1) ) to remove 
 * unknown (new, not yet versioned) entries, to get the directory in  the 
 * exact state it is in the repository, say so.  
 *
 *
 * \subsection rev_p If a path is specified whose parent is missing, \c 
 * fsvs complains.
 * We plan to provide a switch (probably \c -p), which would create (a 
 * sparse) tree up to this entry.
 *
 *
 * \subsection rev_rec Recursive behaviour
 * When the user specifies a non-directory entry (file, device, symlink), 
 * this entry is reverted to the old state.  This is the easy case.
 *
 * If the user specifies a directory entry, see this table for the 
 * restoration results:
 * <table>
 * <tr><th>command line switch<th>result
 * <tr><td>\c -N <td>  this directory only (meta-data),
 * <tr><td>none  <td> this directory, and direct children of the directory,
 * <tr><td>\c -R <td>  this directory, and the complete tree below.
 * </table>
 * */
 
/** A count of files reverted in \b this run. */
static int number_reverted=0;
static svn_revnum_t last_rev;


/** -.
 * This function fetches a file from the current repository, and
 * applies it locally. 
 *
 * Meta-data is set; an existing local entry gets atomically removed by \c 
 * rename().
 * The \c sts->entry_type \b must be correct.
 * The \c sts->url->session \b must already be opened.
 *
 * If \a only_tmp is not \c NULL, the temporary file is not renamed
 * to the real name; instead its path is returned, in a buffer which must
 * not be freed. There is a small number of such buffers used round-robin;
 * at least two slots are always valid. 
 */
#define REV___GETFILE_MAX_CACHE (4)
int rev__get_file(struct estat *sts, 
		svn_revnum_t revision,
		svn_revnum_t *fetched,
		char **only_tmp,
		apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;

	static struct cache_t *cache=NULL;

	char *filename_tmp;
	char *utf8_path;

	char *filename;
	apr_hash_t *props;
	svn_stream_t *stream;
	apr_file_t *a_stream;
	svn_string_t *prop_val;
	apr_pool_t *subpool;
	svn_stringbuf_t *target_stringbuf;
	const char *encoder_str;
	struct encoder_t *encoder;


	STOPIF( cch__new_cache(&cache, 8), NULL);
	target_stringbuf=NULL;
	encoder=NULL;

	BUG_ON(!pool);

	STOPIF( ops__build_path(&filename, sts), NULL);
	/* We cannot give the filename as parameter to copy.
	 * We need a few bytes more for the unique part, and this additional 
	 * length might not be addressible any more. */
	STOPIF( cch__add(cache, 0, NULL, sts->path_len+16, &filename_tmp), NULL);

	DEBUGP("getting file %s/%s@%llu into cache %d", 
			sts->url->url, filename, (t_ull)revision, cache->lru);

	/* We could use a completely different mechanism for temp-file-names;
	 * but keeping it close to the target lets us see if we're out of
	 * disk space in this filesystem. (At least if it's not a binding mount
	 * or something similar - but then rename() should fail).
	 * If we wrote the data somewhere else, we'd risk moving it again -
	 * across filesystem boundaries. */
	strcpy(filename_tmp, filename);
	strcat(filename_tmp, ".XXXXXX");


	STOPIF( apr_pool_create(&subpool, pool),
			"Creating the filehandle pool");

	if (sts->entry_type == FT_FILE)
	{
		/* Files get written in files. */
		STOPIF( apr_file_mktemp(&a_stream, filename_tmp, 
					APR_WRITE | APR_CREATE | APR_EXCL | APR_READ | APR_TRUNCATE,
					subpool),
				"Cannot open/create file %s", filename_tmp);
		stream=svn_stream_from_aprfile(a_stream, subpool);
	}	
	else
	{
		/* Special entries are typically smaller than 1kByte, and get stored in 
		 * an in-memory buffer. */
		target_stringbuf=svn_stringbuf_create("", subpool);

		stream=svn_stream_from_stringbuf(target_stringbuf, subpool);
	}


	/* When we get a file, every old manber-hashes are stale.
	 * So remove them; if the file is big enough, we'll recreate it with 
	 * correct data. */
	STOPIF( waa__delete_byext(filename, WAA__FILE_MD5s_EXT, 1), NULL);


	/* Symlinks have a MD5, too ... so just do that here. */
	/* How do we get the filesize here, to determine whether it's big
	 * enough for manber block hashing? */
	/* Short answer: We don't.
	 * We need to get the MD5 anyway; there's svn_stream_checksummed(),
	 * but that's just one chainlink more, and so we simply use our own
	 * function. */
	if (!action->is_import_export)
		STOPIF( cs__new_manber_filter(sts, stream, &stream, 
					subpool),
				NULL);	

	/* We need to skip the ./ in front. */
	STOPIF( hlp__local2utf8(filename+2, &utf8_path, -1), NULL);


	/* If there's a fsvs:update-pipe, we would know when we have the file 
	 * here - which is a bit late, because we'd have to read/write the entire 
	 * file afresh. So we remember the property in the cb__record_changes() 
	 * call chain, and look for the string here. 
	 *
	 * But that works only if we *know* that we're processing the right 
	 * version. If the filter changed for this file, we might try to decode 
	 * with the wrong one - eg. for diff, where multiple versions are handled 
	 * in a single call.
	 * 
	 * We know that we have the correct value locally if the wanted revision 
	 * is the same as we've in the entry; if they are different, we have to 
	 * ask the repository for the data.
	 *
	 *
	 * Note: we're trading network-round-trips for local disk bandwidth.
	 * The other way would be to fetch the data encoded, *then* look in the 
	 * properties we just got for the pipe-command, and re-pipe the file 
	 * through the command given.
	 *
	 * But: the common case of updates uses cb__record_changes(), which 
	 * already gets the correct value. So in this case we need not look any 
	 * further.
	 */
	/* \todo For things like export we could provide a commandline parameter; 
	 * use network, or use disk.
	 * For a local-remote diff we could pipe the data into the diff program; 
	 * but that wouldn't work for remote-remote diffing, as diff(1) doesn't 
	 * accept arbitrary filehandles as input.
	 **/
	if (sts->decoder_is_correct)
	{
		encoder_str=sts->decoder;
		/* \todo: are there cases where we haven't seen the properties?
		 * Call up__fetch_encoder_str()? Should know whether the value is valid, or 
		 * just not set yet. */
	}
	else
	{
		/* Get the correct value for the update-pipe. */
		STOPIF_SVNERR( svn_ra_get_file,
				(sts->url->session,
				 utf8_path,
				 revision,
				 NULL,
				 NULL,
				 &props,
				 subpool) );

		prop_val=apr_hash_get(props, propval_updatepipe, APR_HASH_KEY_STRING);
		encoder_str=prop_val ? prop_val->data : NULL;
	}
	DEBUGP("updatepipe found as %s", encoder_str);

	/* First decode, then do manber-hashing. As the filters are prepended, we 
	 * have to do that after the manber-filter. */
	if (encoder_str)
		STOPIF( hlp__encode_filter(stream, encoder_str, 1, 
					&stream, &encoder, subpool), NULL);


	if (!fetched) fetched=&revision;
	/* \a fetched only gets set for \c SVN_INVALID_REVNUM , so set a default. */
	*fetched=revision;
	STOPIF_SVNERR( svn_ra_get_file,
			(sts->url->session,
			 utf8_path,
			 revision,
			 stream,
			 fetched,
			 &props,
			 subpool) );
	DEBUGP("got revision %llu", (t_ull)*fetched);

	/* svn_ra_get_file() doesn't close. */
  STOPIF_SVNERR( svn_stream_close, (stream));

	if (sts->entry_type != FT_FILE)
	{
		target_stringbuf->data[ target_stringbuf->len ]=0;
		DEBUGP("got special value %s", target_stringbuf->data);
		STOPIF( up__handle_special(sts, filename_tmp, 
					target_stringbuf->data, subpool), NULL);
	}

	/* If the file got decoded, use the original MD5 for comparision. */
	if (encoder)
	  memcpy(sts->md5, encoder->md5, sizeof(sts->md5));


	STOPIF( prp__set_from_aprhash(sts, props, subpool), NULL);


	/* We unconditionally set the meta-data we have - either it's been read 
	 * via the properties just above, it will have been read when getting the 
	 * file, or we take the values it had. */
	sts->remote_status |= FS_META_CHANGED;
	DEBUGP("setting meta-data");
	STOPIF( up__set_meta_data(sts, filename_tmp), NULL);

	if (only_tmp)
	{
		DEBUGP("returning temporary file %s", filename_tmp);
		*only_tmp=filename_tmp;
	}
	else
	{
		DEBUGP("rename to %s", filename);
		/* rename to correct filename */
		STOPIF_CODE_ERR( rename(filename_tmp, filename)==-1, errno,
				"Cannot rename '%s' to '%s'", filename_tmp, filename);

		/* The rename changes the ctime. */
		STOPIF( hlp__lstat( filename, &(sts->st)),
				"Cannot lstat('%s')", filename);
	}

	sts->url=current_url;
	/* We have to re-sort the parent directory, as the inode has changed
	 * after an rename(). */
	sts->parent->to_be_sorted=1;

	apr_pool_destroy(subpool);
	subpool=NULL;

ex:
	/* On error remove the temporary file. */
	/* Return the original error. */
	if (status)
		unlink(filename_tmp);
	IF_FREE(encoder);

	return status;
}


/** -. 
 * */
int rev__get_props(struct estat *sts, 
		char *utf8_path,
		svn_revnum_t revision,
		apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;
	apr_hash_t *props;
	char *filename;


	if (!utf8_path)
	{
		STOPIF( ops__build_path(&filename, sts), NULL);
		STOPIF( hlp__local2utf8(filename+2, &utf8_path, -1), NULL);
	}

	STOPIF_SVNERR( svn_ra_get_file,
			(sts->url->session,
			 utf8_path,
			 revision,
			 NULL, 
			 NULL,
			 &props,
			 pool) );
	STOPIF( prp__set_from_aprhash(sts, props, pool), NULL);

ex:
	return status;
}


/** Revert action, called for every wanted entry.
 * Please note that contacting the repository is allowed, as we're only 
 * looping through the local entries. 
 * 
 * Doing operations against the repository while being called *from* the ra 
 * layer (eg. during an update) is not allowed! See also \c 
 * svn_ra_do_update():
 * \code
 * The caller may not perform any RA operations using @a session before
 * finishing the report, and may not perform any RA operations using
 * @a session from within the editing operations of @a update_editor.
 * \endcode
 * */
int rev__action(struct estat *sts, char *path)
{
	int status;
	svn_revnum_t fetched, wanted;
	struct estat copy;


	status=0;
	DEBUGP("got %s", path);
	/* If not seen as changed, but target is BASE, we don't need to do 
	 * anything. */
	if (!opt_target_revisions_given &&
			!(sts->entry_status & FS__CHANGE_MASK))
		goto ex;


	wanted=opt_target_revisions_given ? opt_target_revision : sts->repos_rev;
	/* The base directory has no revision, and so can't have a meaningful 
	 * value printed. */
	/* In case we're doing a revert which concernes multiple URLs, they might 
	 * have different BASE revisions.
	 * Print the current revision. */
	if (sts->parent && (!number_reverted || last_rev != wanted))
	{
		printf("Reverting to revision %s:\n", hlp__rev_to_string(wanted));
		last_rev=wanted;
	}
	number_reverted++;


	/* see below */
	copy=*sts;

	if (sts->entry_type & FT_NONDIR)
	{
		/* Parent directories might just have been created. */
		STOPIF( !sts->url,
				"The entry '%s' has no URL associated.", path);

		/* We cannot give connection errors before stat()ing many thousand 
		 * files, because we do not know which URL to open -- until here. */
		current_url = sts->url;
		STOPIF( url__open_session(NULL), NULL);

		DEBUGP("file was changed, reverting");

		/* \todo It would be nice if we could solve meta-data *for the current 
		 * revision* only changes without going to the repository - after all, 
		 * we know the old values.
		 * \todo Maybe we'd need some kind of parameter, --meta-only? Keep 
		 * data, reset rights.
		 * */
		/* TODO - opt_target_revision ? */
		STOPIF( rev__get_file(sts, wanted,
					&fetched, NULL, current_url->pool),
				"Unable to revert entry '%s'", path);
	}
	else
	{
		if (sts->entry_status & FS_REMOVED)
		{
			status = (mkdir(path, sts->st.mode & 07777) == -1) ? errno : 0;
			DEBUGP("mkdir(%s) says %d", path, status);
			STOPIF(status, "Cannot create directory '%s'", path);
		}

		/* Code for directories.
		 * As the children are handled by the recursive options and by \a 
		 * ops__set_to_handle_bits(), we only have to restore the directories' 
		 * meta-data here. */
		STOPIF( up__set_meta_data(sts, NULL), NULL);
		if (sts->entry_status)
			sts->flags |= RF_CHECK;
	}


	/* We do not allow mixed revision working copies - we'd have to store an 
	 * arbitrary number of revision/URL pairs for directories.
	 * See cb__record_changes(). 
	 *
	 * What we *do* allow is to switch entries' *data* to other revisions; but 
	 * for that we store the old values of BASE, so that the file is displayed 
	 * as modified.
	 * An revert without "-r" will restore BASE, and a commit will send the 
	 * current (old :-) data.
	 *
	 * But we show that it's modified. */
	if (opt_target_revisions_given)
	{
		copy.entry_status = ops__stat_to_action(&copy, & sts->st);
		if (sts->entry_type != FT_DIR && copy.entry_type != FT_DIR)
			/* Same MD5? => Same file. Not fully true, though. */
			if (memcmp(sts->md5, copy.md5, sizeof(copy.md5)) == 0)
				copy.entry_status &= ~FS_LIKELY;
		*sts=copy;
	}
	else
	{
		/* There's no change anymore, we're at BASE.
		 * But just printing "...." makes no sense ... show the old status. */
	}

	/* And if it was chosen directly, it should be printed, even if we have 
	 * the "old" revision.  */
	sts->flags |= RF_PRINT;

	STOPIF( st__status(sts, path), NULL);


	/* For files we have no changes anymore.
	 * Removing the FS_REMOVED flag means that the children will be loaded, 
	 * and we get called again after they're done :-)
	 * See also actionlist_t::local_callback. */
	sts->entry_status=0;

	/* For directories we set that we still have meta-data to do - children 
	 * might change our mtime. */
	if (S_ISDIR(sts->st.mode))
		sts->entry_status=FS_META_MTIME;

	/* Furthermore the parent should be re-stat()ed after the children have 
	 * finished. */
	if (sts->parent)
		sts->parent->entry_status |= FS_META_MTIME;

ex:
	return status;
}


/** -.
 * Loads the stored tree (without updating), looks for the wanted entries,
 * and restores them from the repository. */
int rev__work(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;


	status=0;
	/* For revert the default is non-recursive. */
	opt_recursive--;

	if (!argc) ac__Usage_this();

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);


	STOPIF( url__load_nonempty_list(NULL, 0), NULL);


	if (opt_target_revisions_given)
		STOPIF( wa__warn( WRN__MIXED_REV_WC, EINVAL,
					"Sorry, fsvs currently doesn't allow mixed revision working copies.\n"
					"Entries will still be compared against the BASE revision.\n"),
				NULL);


	/* This message can be seen because waa__find_common_base() looks for
	 * an "url" file and not for a "dir" -- which means that this tree
	 * was never committed, so we don't know what HEAD is. */
	/* Maybe the user could still try with some revision number and we simply
	 * check for the existence of the given path there? */
	only_check_status=1;
	status=waa__read_or_build_tree(root, argc, normalized, argv, NULL, 1);
	if (status == ENOENT)
		STOPIF(status,
				"We know nothing about previous or current versions, as this tree\n"
				"was never checked in.\n"
				"If you need such an entry reverted, you could either write the needed\n"
				"patch (and send it to dev@fsvs.tigris.org), or try with a 'sync-repos'\n"
				"command before (if you know a good revision number)\n");

	only_check_status=0;
	STOPIF( waa__output_tree(root), NULL);

ex:
	return status;
}


int rev__do_changed(svn_ra_session_t *session, 
		struct estat *dir, 
		apr_pool_t *pool)
{
	int status;
	int i, j;
	struct estat *sts;
	char *fn;
	struct sstat_t st;
	apr_pool_t *subpool;


	status=0;
	subpool=NULL;
	for(i=0; i<dir->entry_count; i++)
	{
		sts=dir->by_inode[i];

		STOPIF( apr_pool_create(&subpool, pool), "Cannot get a subpool");

		if (sts->remote_status & FS__CHANGE_MASK)
		{
			STOPIF( ops__build_path( &fn, sts), NULL);
			DEBUGP("%s has changed: %X, mode=0%o (local %X - %s)", 
					fn, sts->remote_status, sts->st.mode, sts->entry_status,
					st__status_string(sts));

			/* If we remove an entry, the entry_count gets decremented; 
			 * we have to repeat the loop *for the same index*. */

			/** \todo Conflict handling */
			STOPIF_CODE_ERR( sts->entry_status & FS_CHANGED, EBUSY,
					"The entry %s has changed locally", fn);

			/* If the entry has been removed in the repository, we remove it
			 * locally, too (if it wasn't changed).
			 * But the type in the repository may be another than the local one -
			 * so we have to check what we currently have. */
			/* An entry can be given as removed, and in the same step be created
			 * again - possibly as another type. */
			if (sts->remote_status & FS_REMOVED)
			{
				/* Already removed? */
				if ((sts->entry_status & FS_REPLACED) != FS_REMOVED)
				{
					/* Find type. Small race condition - it might be removed now. */
					/** \todo - remember what kind it was *before updating*, and
					 * only remove if still of same kind. 
					 * make switch depend on old type? */
					STOPIF( hlp__lstat(fn, &st), NULL);
					if (S_ISDIR(st.mode))
					{
						STOPIF( up__rmdir(sts), NULL);
						/* Free space used by children */
						for(j=0; j<sts->entry_count; j++)
							STOPIF( ops__free_entry(sts->by_inode+j), NULL);
						/* Now there're none */
						sts->entry_count=0;
						sts->by_inode=sts->by_name=NULL;
					}
					else
						STOPIF( up__unlink(sts, fn), NULL);
				}

				/* Only removed, or added again? */
				if (sts->remote_status & FS_NEW)
					DEBUGP("entry %s added again", fn);
				else
				{
					/* We must do that here ... later the entry is no longer valid. */
					STOPIF( st__rm_status(sts, NULL), NULL);
					STOPIF( ops__delete_entry(dir, NULL, i, UNKNOWN_INDEX), NULL);
					i--;
					goto loop_end;
				}
			}


			/* If we change something in this directory, we have to re-sort the 
			 * entries by inode again. */
			dir->to_be_sorted=1;

			if (S_ISDIR(sts->st.mode))
			{
				/* Just try to create the directory, and ignore EEXIST. */
				status = (mkdir(fn, sts->st.mode & 07777) == -1) ? errno : 0;
				DEBUGP("mkdir(%s) says %d", fn, status);
				if (status == EEXIST)
				{
					/* Make sure it's a directory */
					STOPIF( hlp__lstat(fn, &st), NULL);
					STOPIF_CODE_ERR( !S_ISDIR(st.mode), EEXIST,
							"%s does exist as a non-directory entry!", fn);
					/* \todo conflict? */
				}
				else
					STOPIF( status, "Cannot create directory %s", fn);

				/* Meta-data is done later. */

				/* An empty directory need not be sorted; if we get entries,
				 * we'll mark it with \c to_be_sorted .*/
			}
			else /* Not a directory */
			{
				if (sts->remote_status & (FS_CHANGED | FS_REPLACED))
				{
					STOPIF( rev__get_file(sts, sts->repos_rev,
								NULL, NULL, subpool),
							NULL);
				}
				else 
				{
					/* If user-defined properties have changed, we have to fetch them 
					 * from the repository, as we don't store them in RAM (due to the  
					 * amount of memory possibly needed). */
					if (sts->remote_status & FS_PROPERTIES)
						STOPIF( rev__get_props(sts, NULL, sts->repos_rev, subpool), NULL);

					if (sts->remote_status & FS_META_CHANGED)
					{
						/* If we removed the file, it has no meta-data any more;
						 * if we fetched it via rev__get_file(), it has it set already.
						 * Only the case of *only* meta-data-change is to be done. */
						STOPIF( up__set_meta_data(sts, fn), NULL);
					}
				}
			}
		}


		/* We always recurse now, even if the directory has no children.
		 * Else we'd have to check for children in a few places above, which would
		 * make the code unreadable. */
		if (S_ISDIR(sts->st.mode)
				/* && 
					 (sts->remote_status & FS_CHILD_CHANGED)  */
			 )
		{
			apr_pool_destroy(subpool);
			subpool=NULL;
			STOPIF( apr_pool_create(&subpool, pool), "subpool creation");

			STOPIF( rev__do_changed(session, sts, subpool), NULL);
		}	

		/* After the recursive call the path string may not be valid anymore. 
		 * */
		STOPIF( st__rm_status(sts, NULL), NULL);

loop_end:
		apr_pool_destroy(subpool);
		subpool=NULL;
	}

	STOPIF( ops__free_marked(dir), NULL);

	/* The root entry would not be printed; do that here. */
	if (!dir->parent)
		STOPIF( st__rm_status(dir, NULL), NULL);

	/* If the directory had local modifications, we need to check it 
	 * next time -- as we take its current timestamp,
	 * we'd miss the new or deleted entries. 
	 * Must be done before \c ops__update_single_entry() - That sets 
	 * \c dir->entry_status .*/
	if (dir->entry_status & FS__CHANGE_MASK)
		dir->flags |= RF_CHECK;

	/* Now, after all has been said and done for the children, set and re-get 
	 * the actual meta-data - the mtime has been changed in the meantime 
	 * (because of child node creation), and maybe this filesystem's 
	 * granularity is worse than on commit; then the timestamps would be 
	 * wrong. */
	/* TODO: should the newer of both timestamps be taken (or current time), 
	 * if the directory has changed against the directory version? */
	STOPIF( up__set_meta_data(dir, NULL), NULL);
	STOPIF( ops__update_single_entry(dir, NULL), NULL);

ex:
	return status;
}

