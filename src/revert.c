/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#include <subversion-1/svn_delta.h>
#include <subversion-1/svn_ra.h>

#include "revert.h"
#include "waa.h"
#include "est_ops.h"
#include "racallback.h"
#include "warnings.h"
#include "resolve.h"
#include "checksum.h"
#include "props.h"
#include "cache.h"
#include "helper.h"
#include "url.h"
#include "update.h"
#include "cp_mv.h"
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
 * This command undoes local modifications:
 * - An entry that is marked to be unversioned gets this flag removed.
 * - For a already versioned entry (existing in the repository), the local 
 *   entry is replaced with its repository version, and its status and 
 *   flags are cleared.
 * - An entry that is a copy destination, but modified, gets reverted to 
 *   the copy source data.
 * - An unmodified direct copy destination entry, and other uncommitted 
 *   entries with special flags (manually added, or defined as copied), are 
 *   changed back to "<i>N</i>"ew -- the copy definition and the special 
 *   status is removed. \n
 *   Please note that on implicitly copied entries (entries that are marked 
 *   as copied because some parent directory is the base of a copy) \b 
 *   cannot be un-copied; they can only be reverted to their original 
 *   (copied-from) data, or removed.
 *
 * See also \ref howto_entry_statii.
 * 
 * If a directory is given on the command line <b>all known entries in this 
 * directory</b> are reverted to the old state; this behaviour can be 
 * modified with \ref glob_opt_rec "-R/-N", or see below.
 *
 * The reverted entries are printed, along with the status they had \b 
 * before the revert (because the new status is per definition \e 
 * unchanged).
 *
 * If a revision is given, the entries' data is taken from this revision; 
 * furthermore, the \b new status of that entry is shown.
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
 * - and you cannot commit the old version with changes, as later changes 
 *   will create conflicts in the repository.
 *
 *
 * \subsection rev_del Currently only known entries are handled.
 * If you need a switch (like \c --delete in \c rsync(1) ) to remove 
 * unknown (new, not yet versioned) entries, to get the directory in  the 
 * exact state it is in the repository, say so.  
 *
 * \todo Another limitation is that just-deleted just-committed entries 
 * cannot be fetched via \c revert, as FSVS no longer knows about them. \n
 * TODO: If a revision is given, take a look there, and ignore the local 
 * data?
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
 *
 * \subsection rev_copied Working with copied entries
 * If an entry is marked as copied from another entry (and not committed!), 
 * a \c revert will undo the copy setting - which will make the entry 
 * unknown again, and reported as new on the next invocations.
 *
 * If a directory structure was copied, and the current entry is just a 
 * implicitly copied entry, \c revert would take the copy source as 
 * reference, and <b>get the file data</b> from there.
 *
 * Summary: <i>Only the base of a copy can be un-copied.</i> 
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
 * \c current_url \b must match, and be open.
 *
 * If \a only_tmp is not \c NULL, the temporary file is not renamed
 * to the real name; instead its path is returned, in a buffer which must
 * not be freed. There is a small number of such buffers used round-robin;
 * at least two slots are always valid. \n
 * No manber-hashes are done in this case.
 *
 * If \a url_to_use is not \c NULL, it is taken as source, and so must not 
 * be a directory.
 */
#define REV___GETFILE_MAX_CACHE (4)
int rev__get_file(struct estat *sts, 
		svn_revnum_t revision,
		char *url_to_use,
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
	char *relative_to_url;


	STOPIF( cch__new_cache(&cache, 8), NULL);
	target_stringbuf=NULL;
	encoder=NULL;

	BUG_ON(!pool);

	STOPIF( ops__build_path(&filename, sts), NULL);
	/* We cannot give the filename as parameter to copy.
	 * We need a few bytes more for the unique part, and this additional 
	 * length might not be addressible any more. */
	STOPIF( cch__add(cache, 0, NULL, sts->path_len+16, &filename_tmp), NULL);


	if (!url_to_use)
	{
		/* Skip ./ in front. */
		relative_to_url=filename+2;
	}
	else
	{
		/* Verify that the correct URL is taken. */
		if (strncmp(current_url->url, url_to_use, current_url->urllen) == 0)
		{
			DEBUGP("%s matches current_url.", url_to_use);
		}
		else
		{
			STOPIF(EINVAL, "%s not below %s", url_to_use, current_url->url);
		}

		relative_to_url=url_to_use + current_url->urllen+1;
	}

	DEBUGP("getting file %s@%llu from %s",
			relative_to_url, (t_ull)revision, current_url->url);


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
	if (!action->is_import_export && !only_tmp)
		STOPIF( cs__new_manber_filter(sts, stream, &stream, 
					subpool),
				NULL);	

	/* We need to skip the ./ in front. */
	STOPIF( hlp__local2utf8(relative_to_url, &utf8_path, -1), NULL);


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
				(current_url->session,
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
	{
		STOPIF( hlp__encode_filter(stream, encoder_str, 1, 
					&stream, &encoder, subpool), NULL);
		encoder->output_md5= &(sts->md5);
	}


	if (!fetched) fetched=&revision;
	/* \a fetched only gets set for \c SVN_INVALID_REVNUM , so set a default. */
	*fetched=revision;
	STOPIF_SVNERR( svn_ra_get_file,
			(current_url->session,
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


	STOPIF( prp__set_from_aprhash(sts, props, subpool), NULL);


	/* We write all meta-data. If we got no values from the repository, we just
	 * write what we have in the local filesystem back - the temporary file has
	 * just some default values, after all. */
	sts->entry_status |= FS_META_CHANGED;
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

	return status;
}


/** -.
 *
 * The base name of the \a sts gets written to.
 *
 * If the merge gives no errors, the temporary files get deleted.
 * */
int rev__merge(struct estat *sts,
		const char *file1, 
		const char *common, 
		const char *file2)
{
	int status;
	pid_t pid;
	char *output;
	int hdl;
	struct sstat_t stat;
	int retval;


	STOPIF( ops__build_path(&output, sts), NULL);

	/* Remember the meta-data of the target */
	STOPIF( hlp__lstat(file2, &stat), NULL);


	pid=fork();
	STOPIF_CODE_ERR( pid == -1, errno, "Cannot fork()" );
	if (pid == 0)
	{
		/* Child. */
		/* TODO: Is there some custom merge program defined?
		 * We always use the currently defined property. */
		/* TODO: how does that work if an update sends a wrong property? use 
		 * both? */

		/* Open the output file. */
		hdl=open(output, O_WRONLY | O_CREAT, 0700);
		STOPIF_CODE_ERR( hdl == -1, errno, 
				"Cannot open merge output \"%s\"", output);
		STOPIF_CODE_ERR( dup2(hdl, STDOUT_FILENO) == -1, errno,
				"Cannot dup2");
		/* No need to close hdl -- it's opened only for that process, and will 
		 * be closed when it exec()s. */

		STOPIF_CODE_ERR( execlp( opt__get_string(OPT__MERGE_PRG), 
					opt__get_string(OPT__MERGE_PRG),
					opt__get_string(OPT__MERGE_OPT),
					"-p",
					file1, common, file2,
					NULL) == -1, errno,
				"Starting the merge program \"%s\" failed",
				opt__get_string(OPT__MERGE_PRG));

	}

	STOPIF_CODE_ERR( pid != waitpid(pid, &retval, 0), errno, "waitpid");

	DEBUGP("merge returns %d (signal %d)", 
			WEXITSTATUS(retval), WTERMSIG(retval));

	/* Can that be? */
	STOPIF_CODE_ERR( WIFSIGNALED(retval) || !WIFEXITED(retval), EINVAL, 
			"\"%s\" quits by signal %d.", 
			opt__get_string(OPT__MERGE_PRG),
			WTERMSIG(retval));

	if (WEXITSTATUS(retval) == 0)
	{
		DEBUGP("Remove temporary files.");

		/* Ok, merge done. Remove temporary files, or at least try to. */
		if (unlink(file1) == -1) status=errno;
		if (unlink(file2) == -1) status=errno;
		if (unlink(common) == -1) status=errno;
		STOPIF(status, "Removing one or more temporary files "
				"(merge of \"%s\") failed", output);
	}
	else
	{
		DEBUGP("non-zero return");
		STOPIF_CODE_ERR( WEXITSTATUS(retval) != 1, EINVAL,
				"\"%s\" exited with error code %d",
				opt__get_string(OPT__MERGE_PRG),
				WEXITSTATUS(retval));

		STOPIF( res__mark_conflict(sts, 
					file1, file2, common, NULL), NULL);

		/* Means merge conflicts, but no error. */
	}

	/* As we've just changed the text, set the current mtime. */
	sts->st.mtim.tv_sec=time(NULL);
	/* Now set owner, group, and mode.
	 * This does a lstat(), to get the current ctime and so on;
	 * to make the changes visible, we use the meta-data of the target. */
	STOPIF( up__set_meta_data(sts, NULL), NULL);
	sts->st=stat;


ex:
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
			(current_url->session,
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
int rev__action(struct estat *sts)
{
	int status;
	svn_revnum_t fetched, wanted;
	struct estat copy;
	char *url_to_fetch;
	char *path;


	status=0;
	STOPIF( ops__build_path(&path, sts), NULL);

	/* Garbage collection for entries that should be ignored happens in 
	 * waa__output_tree(); changing the tree while it's being traversed is 
	 * a bit nasty. */

	if ( (sts->flags & RF_UNVERSION))
	{
		/* Was marked as to-be-unversioned? Just keep it. */
		sts->flags &= ~RF_UNVERSION;
		DEBUGP("removing unversion on %s", path);
	}
	else if ( (sts->flags & RF_COPY_BASE) &&
			!(sts->entry_status & (FS_META_CHANGED | FS_CHANGED) ) )
	{
		/* Directly copied, unchanged entry.
		 * Make it unknown - remove copy relation (ie. mark hash value for 
		 * deletion), and remove entry from local list. */
		STOPIF( cm__get_source(sts, path, NULL, NULL, 1), NULL);
		sts->flags &= ~RF_COPY_BASE;
		sts->entry_type = FT_IGNORE;
		DEBUGP("unchanged copy, reverting %s", path);
	}
	else if ( sts->flags & RF_ADD )
	{
		/* Added entry just gets un-added ... ie. unknown. */
		sts->entry_type = FT_IGNORE;
		DEBUGP("removing add-flag on %s", path);
	}
	else if (!( (sts->flags & (RF_COPY_BASE | RF_COPY_SUB)) || sts->url ) )
	{
		/* We have no URL, and no copyfrom source ... this is an unknown entry.
		 * Has to be given directly on the command line, but could have happened 
		 * via a wildcard - so don't stop working.
		 * Can't do anything about it. */
		printf("Cannot revert unknown entry %s.", path);
		status=0;
		goto ex;
	}
	else
	{
		/* We know where to get that from. */
		DEBUGP("have an URL for %s", path);

		if ( sts->flags & RF_CONFLICT )
			STOPIF( res__remove_aux_files(sts), NULL);

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


		url_to_fetch=NULL;
		if (sts->flags & RF___IS_COPY)
		{
			/* No URL yet, but we can reconstruct it. */
			/* We have to use the copyfrom */
			STOPIF( cm__get_source(sts, NULL, &url_to_fetch, &wanted, 0), NULL);

			/* \TODO: That doesn't work for unknown URLs. */
			STOPIF( url__find(url_to_fetch, &current_url), NULL);
		}
		else
		{
			STOPIF( !sts->url,
					"The entry '%s' has no URL associated.", path);
			current_url = sts->url;
		}


		if (sts->parent && (!number_reverted || last_rev != wanted))
		{
			printf("Reverting to revision %s:\n", hlp__rev_to_string(wanted));
			last_rev=wanted;
		}
		number_reverted++;


		/* see below */
		copy=*sts;

		/* Parent directories might just have been created. */
		if (sts->entry_type & FT_NONDIR)
		{
			/* We cannot give connection errors before stat()ing many thousand 
			 * files, because we do not know which URL to open -- until here. */
			STOPIF( url__open_session(NULL), NULL);

			DEBUGP("file was changed, reverting");

			/* \todo It would be nice if we could solve meta-data *for the current 
			 * revision* only changes without going to the repository - after all, 
			 * we know the old values.
			 * \todo Maybe we'd need some kind of parameter, --meta-only? Keep 
			 * data, reset rights.
			 * */
			/* TODO - opt_target_revision ? */
			STOPIF( rev__get_file(sts, wanted, url_to_fetch,
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
			/* up__set_meta_data() checks remote_status, while we here have 
			 * entry_status set. */
			sts->remote_status=sts->entry_status;
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
	}

	/* And if it was chosen directly, it should be printed, even if we have 
	 * the "old" revision.  */
	sts->flags |= RF_PRINT;

	STOPIF( st__status(sts), NULL);


	/* Entries that are now ignored don't matter for the parent directory; 
	 * but if we really changed something in the filesystem, we have to 
	 * update the parent status. */
	if (sts->entry_type != FT_IGNORE)
	{
		/* For files we have no changes anymore.
		 * Removing the FS_REMOVED flag for directories means that the children 
		 * will be loaded, and we get called again after they're done :-)
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
	}

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
	time_t delay_start;


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
				"!We know nothing about previous or current versions, as this tree\n"
				"was never checked in.\n"
				"If you need such an entry reverted, you could either write the needed\n"
				"patch (and send it to dev@fsvs.tigris.org), or try with a 'sync-repos'\n"
				"command before (if you know a good revision number)\n");
	else
		STOPIF(status, NULL);

	only_check_status=0;
	delay_start=time(NULL);
	STOPIF( waa__output_tree(root), NULL);
	STOPIF( hlp__delay(delay_start, DELAY_REVERT), NULL);

ex:
	return status;
}


/** -.
 * Used on update. */
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
	const char *unique_name_mine, 
				*unique_name_remote, 
				*unique_name_common;
	char revnum[12];


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


			unique_name_mine=NULL;

			/* Conflict handling; depends whether it has changed locally. */
			if (sts->entry_status & (FS_CHANGED | FS_CHILD_CHANGED))
				switch (opt__get_int(OPT__CONFLICT))
				{
					case CONFLICT_STOP:
						STOPIF( EBUSY, "!The entry %s has changed locally", fn);
						break;

					case CONFLICT_LOCAL:
						/* Next one, please. */
						printf("Conflict for %s skipped.\n", fn);
						continue;

					case CONFLICT_REMOTE:
						/* Just ignore local changes. */
						break;

					case CONFLICT_MERGE:
					case CONFLICT_BOTH:
						/* Rename local file to something like .mine. */
						STOPIF( hlp__rename_to_unique(fn, ".mine", 
									&unique_name_mine, pool), NULL);
						/* Now the local name is not used ... so get the file. */
						break;

					default:
						BUG("unknown conflict resolution");
				}


			/* If the entry has been removed in the repository, we remove it
			 * locally, too (if it wasn't changed).
			 * But the type in the repository may be another than the local one -
			 * so we have to check what we currently have. */
			/* An entry can be given as removed, and in the same step be created
			 * again - possibly as another type. */
			if (sts->remote_status & FS_REMOVED)
			{
				/* Is the entry already removed? */
				/* If there's a typechange involved, the old entry has been 
				 * renamed, and so doesn't exist in the filesystem anymore. */
				if ((sts->entry_status & FS_REPLACED) != FS_REMOVED &&
						!unique_name_mine)
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
					STOPIF( st__rm_status(sts), NULL);
					STOPIF( ops__delete_entry(dir, NULL, i, UNKNOWN_INDEX), NULL);
					i--;
					goto loop_end;
				}
			}


			/* If we change something in this directory, we have to re-sort the 
			 * entries by inode again. */
			dir->to_be_sorted=1;

			current_url=sts->url;

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
					STOPIF( rev__get_file(sts, sts->repos_rev, NULL,
								NULL, NULL, subpool),
							NULL);

					/* We had a conflict; rename the file fetched from the 
					 * repository to a unique name. */
					if (unique_name_mine)
					{
						/* If that revision number overflows, we've got bigger problems.  
						 * */
						snprintf(revnum, sizeof(revnum)-1, 
								".r%llu", (t_ull)sts->repos_rev);
						revnum[sizeof(revnum)-1]=0;

						STOPIF( hlp__rename_to_unique(fn, revnum,
									&unique_name_remote, pool), NULL);

						/* If we're updating and already have a conflict, we don't 
						 * merge again. */
						if (sts->flags & RF_CONFLICT)
						{
							printf("\"%s\" already marked as conflict.\n", fn);
							STOPIF( res__mark_conflict(sts, 
										unique_name_mine, unique_name_remote, NULL), NULL);
						}
						else if (opt__get_int(OPT__CONFLICT) == CONFLICT_BOTH)
						{
							STOPIF( res__mark_conflict(sts, 
										unique_name_mine, unique_name_remote, NULL), NULL);

							/* Create an empty file,
							 * a) to remind the user, and
							 * b) to avoid a "Deleted" status. */
							j=creat(fn, 0777);
							if (j != -1) j=close(j);

							STOPIF_CODE_ERR(j == -1, errno, 
									"Error creating \"%s\"", fn);

							/* up__set_meta_data() does an lstat(), but we want the 
							 * original values. */
							st=sts->st;
							STOPIF( up__set_meta_data(sts, fn), NULL);
							sts->st=st;
						}
						else if (opt__get_int(OPT__CONFLICT) == CONFLICT_MERGE)
						{
							{
								STOPIF( rev__get_file(sts, sts->old_rev, NULL,
											NULL, NULL, subpool),
										NULL);

								snprintf(revnum, sizeof(revnum)-1, 
										".r%llu", (t_ull)sts->old_rev);
								revnum[sizeof(revnum)-1]=0;

								STOPIF( hlp__rename_to_unique(fn, revnum,
											&unique_name_common, pool), NULL);

								STOPIF( rev__merge(sts, 
											unique_name_mine, 
											unique_name_common, 
											unique_name_remote), NULL);
							}
						}
						else
							BUG("why a conflict?");
					}
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
		STOPIF( st__rm_status(sts), NULL);

loop_end:
		apr_pool_destroy(subpool);
		subpool=NULL;
	}

	STOPIF( ops__free_marked(dir, 0), NULL);

	/* The root entry would not be printed; do that here. */
	if (!dir->parent)
		STOPIF( st__rm_status(dir), NULL);

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

