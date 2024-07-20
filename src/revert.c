/************************************************************************
 * Copyright (C) 2006-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/
#include <stdio.h>
#include <fcntl.h>
#include <time.h>

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
 * - For a already versioned entry (existing in the repository) the local 
 *   entry is replaced with its repository version, and its status and 
 *   flags are cleared.
 * - An entry that is a \b modified copy destination gets reverted to the 
 *   copy source data.
 * - Manually added entries are changed back to <i>"N"</i>ew.
 *
 * Please note that implicitly copied entries, ie. entries that are marked 
 * as copied because some parent directory is the base of a copy, <b>can 
 * not</b> be un-copied; they can only be reverted to their original 
 * (copied-from) data, or removed.
 *
 * If you want to undo a \c copy operation, please see the \ref uncopy 
 * "uncopy" command.
 *
 * See also \ref howto_entry_statii.
 * 
 * If a directory is given on the command line <b>all versioned entries in 
 * this directory</b> are reverted to the old state; this behaviour can be 
 * modified with \ref glob_opt_rec "-R/-N", or see below.
 *
 * The reverted entries are printed, along with the status they had \b 
 * before the revert (because the new status is per definition \e 
 * unchanged).
 *
 * If a revision is given, the entries' data is taken from this revision; 
 * furthermore, the \b new status of that entry is shown.
 *
 * \note Please note that mixed revision working copies are not (yet) 
 * possible; the \e BASE revision is not changed, and a simple \c revert 
 * without a revision arguments gives you that. \n
 * By giving a revision parameter you can just choose to get the text from 
 * a different revision.
 *
 *
 * \subsection rev_cmp_up Difference to update
 * If something doesn't work as it should in the installation you can 
 * revert entries until you are satisfied, and directly \ref commit 
 * "commit" the new state. 
 * 
 * In contrast, if you \ref update "update" to an older version, you 
 * - cannot choose single entries (no mixed revision working copies yet),
 * - and you cannot commit the old version with changes, as the "skipped" 
 *   (later) changes will create conflicts in the repository.
 *
 *
 * \subsection rev_del Currently only known entries are handled.
 * 
 * If you need a switch (like \c --delete in \c rsync(1) ) to remove 
 * unknown (new, not yet versioned) entries, to get the directory in  the 
 * exact state it is in the repository, please tell the <tt>dev\@</tt> 
 * mailing list.  
 *
 * \todo Another limitation is that just-deleted just-committed entries 
 * cannot be fetched via \c revert, as FSVS no longer knows about them. \n
 * TODO: If a revision is given, take a look there, and ignore the local 
 * data? \n
 * As a workaround you could use the \ref cat "cat" and/or \ref checkout 
 * "checkout" commands to fetch repository-only data.
 *
 *
 * \subsection rev_p Removed directory structures
 *
 * If a path is specified whose parent is missing, \c fsvs complains. \n
 * We plan to provide a switch (probably \c -p), which would create (a 
 * sparse) tree up to this entry.
 *
 *
 * \subsection rev_rec Recursive behaviour
 * When the user specifies a non-directory entry (file, device, symlink), 
 * this entry is reverted to the old state.
 *
 * If the user specifies a directory entry, these definitions should apply:
 * <table>
 * <tr><th>command line switch<th>result
 * <tr><td>\c -N <td>  this directory only (meta-data),
 * <tr><td>none  <td> this directory, and direct children of the directory,
 * <tr><td>\c -R <td>  this directory, and the complete tree below.
 * </table>
 *
 *
 * \subsection rev_copied Working with copied entries
 * If an entry is marked as copied from another entry (and not committed!), 
 * a \c revert will fetch the original copyfrom source. To undo the copy 
 * setting use the \ref uncopy "uncopy" command.
 *
 * */


/** List of (bit-)flags for rev___undo_change().
 * These have an order, ie. SET_CURRENT overrides REVERT_MTIME. */
enum rev___dir_change_flag_e {
	NOT_CHANGED=0,
	REVERT_MTIME=1,
	SET_CURRENT=2,
	GET_TSTAMP=0x1000,
};

 
/** A count of files reverted in \b this run. */
static int number_reverted=0;
static svn_revnum_t last_rev;


#define REV___GETFILE_MAX_CACHE (4)


/** -.
 *
 * This function fetches an non-directory entry \a loc_url from the 
 * repository in \c current_url, and writes it to \a output - which gets 
 * closed via \c svn_stream_close(). 
 *
 * \a decoder should be set correctly.
 * \todo if it's \c NULL, but an update-pipe is set on the entry, the data 
 * has to be read from disk again, to be correctly processed.
 *
 * No meta-data is set, and the \c svn:special attribute is ignored.
 *
 * The revision number must be valid, it may not be \c SVN_INVALID_REVNUM.
 *
 * If \a sts_for_manber is \c NULL, no manber hashes are calculated.
 *
 * If \a output_sts is \c NULL, the meta-data properties are kept in \a 
 * props; else its fields are filled (as far as possible) with data. That 
 * includes the estat::repos_rev field.
 *
 * The user-specified properties can be returned in \a props.
 *
 * As this just returns the data in a stream, the files' type mostly 
 * doesn't matter; it just may not be a directory, because we'd get an 
 * error from subversion.
 *
 * \a loc_url must be given in the current locale; it will be converted to 
 * UTF8 before being sent to the subversion libraries. */
int rev__get_text_to_stream( char *loc_url, svn_revnum_t revision,
		const char *decoder,
		svn_stream_t *output,
		struct estat *sts_for_manber,
		struct estat *output_sts,
		apr_hash_t **props,
		apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;
	svn_string_t *prop_val;
	struct encoder_t *encoder;
	char *utf8_url;
	apr_hash_t *properties;
	char target_rev[10];


	encoder=NULL;
	status=0;
	DEBUGP("getting file %s@%s from %s", loc_url, 
			hlp__rev_to_string(revision), current_url->url);


	if (strncmp(loc_url, "./", 2) == 0)
	{
		/* Skip ./ in front. */
	}
	else
	{
		/* It could be an absolute value. */

		/* Verify that the correct URL is taken.
		 * The "/" that's not stored at the end of the URL must be there, too.  
		 * */
		if (strncmp(current_url->url, loc_url, current_url->urllen) == 0 &&
				loc_url[current_url->urllen] == '/')
			loc_url += current_url->urllen+1;

		/* If the string doesn't match, it better be a relative value already 
		 * ... else we'll get an error.  */
		//	else STOPIF(EINVAL, "%s not below %s", loc_url, current_url->url);
	}

	STOPIF( hlp__local2utf8(loc_url, &utf8_url, -1), NULL);
	DEBUGP("Got utf8=%s", utf8_url);


	/* Symlinks have a MD5, too ... so just do that here. */
	/* How do we get the filesize here, to determine whether it's big
	 * enough for manber block hashing? */
	/* Short answer: We don't.
	 * We need to get the MD5 anyway; there's svn_stream_checksummed(),
	 * but that's just one chainlink more, and so we simply use our own
	 * function. */
	if (sts_for_manber)
		STOPIF( cs__new_manber_filter(sts_for_manber, 
					output, &output, pool), NULL);	


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
	 * accept arbitrary filehandles as input (and /proc/self/fd/ isn't 
	 * portable). */

	/* Fetch decoder from repository. */
	if (decoder == DECODER_UNKNOWN)
	{
		STOPIF_SVNERR_TEXT( svn_ra_get_file,
				(current_url->session,
				 utf8_url, revision,
				 NULL,
				 &revision, &properties,
				 pool),
			 "Fetching entry \"%s/%s\"@%s",	
			 current_url->url,
			 loc_url, hlp__rev_to_string(revision));

		prop_val=(svn_string_t*)apr_hash_get(properties,
						propval_updatepipe,
						APR_HASH_KEY_STRING);
		decoder=prop_val ? prop_val->data : NULL;
	}


	/* First decode, then do manber-hashing. As the filters are prepended, we 
	 * have to do that after the manber-filter. */
	if (decoder)
	{
		snprintf(target_rev, sizeof(target_rev), 
				"%llu", (t_ull)revision);
		setenv(FSVS_EXP_TARGET_REVISION, target_rev, 1);

		STOPIF( hlp__encode_filter(output, decoder, 1, 
					loc_url, &output, &encoder, pool), NULL);
		if (output_sts)
			encoder->output_md5= &(output_sts->md5);
	}


	STOPIF_SVNERR_TEXT( svn_ra_get_file,
			(current_url->session,
			 utf8_url, revision,
			 output,
			 &revision, &properties,
			 pool),
			"Fetching entry %s/%s@%s",	
			current_url->url,
			loc_url, hlp__rev_to_string(revision));
	DEBUGP("got revision %llu", (t_ull)revision);

	/* svn_ra_get_file doesn't close the stream. */
	STOPIF_SVNERR( svn_stream_close, (output));
	output=NULL;

	if (output_sts)
	{
		output_sts->repos_rev = revision;
		STOPIF( prp__set_from_aprhash( output_sts, properties, 
					STORE_IN_FS | ONLY_KEEP_USERDEF, NULL, pool), NULL);
	}

	if (props)
		*props=properties;


ex:
	return status;
}


/** -.
 * Mostly the same as \c rev__get_text_to_stream(), but returning a 
 * (temporary) \a filename based on \a filename_base, if this is not \c 
 * NULL.
 *
 * The entries' file type isn't taken into account; the file may have the 
 * data <tt>"symlink XXX"</tt>, etc.
 *
 * If \a filename_base is \c NULL, the file will be put in a real temporary 
 * location.
 *
 * \a output_stat is used to store the parsed properties of the entry.
 * */
int rev__get_text_to_tmpfile(char *loc_url, svn_revnum_t revision,
		char *encoder,
		char *filename_base, char **filename,
		struct estat *sts_for_manber, 
		struct estat *output_sts, apr_hash_t **props,
		apr_pool_t *pool)
{
	int status;
	apr_file_t *apr_f;
	svn_stream_t *output;


	status=0;

	STOPIF( waa__get_tmp_name( filename_base, filename, &apr_f, pool), NULL);
	output=svn_stream_from_aprfile(apr_f, pool);

	STOPIF( rev__get_text_to_stream( loc_url, revision, encoder,
				output, sts_for_manber, output_sts, props, pool), NULL);

	/* svn_ra_get_file() doesn't close. */
	STOPIF( apr_file_close(apr_f), NULL);

ex:
	return status;
}


/** -.
 *
 * Does no validation of input - might fill entire memory. */
int rev__get_text_into_buffer(char *loc_url, svn_revnum_t revision,
		const char *decoder,
		svn_stringbuf_t **output,
		struct estat *sts_for_manber,
		struct estat *output_sts,
		apr_hash_t **props,
		apr_pool_t *pool)
{
	int status;
	svn_stringbuf_t *string;
	svn_stream_t *stream;

	status=0;
	string=svn_stringbuf_create("", pool);
	stream=svn_stream_from_stringbuf(string, pool);

	STOPIF( rev__get_text_to_stream(loc_url, revision,
				decoder, stream, sts_for_manber, output_sts, props, pool), NULL);

	*output=string;
ex:
	return status;
}


/** -.
 *
 * Meta-data is set; an existing local entry gets atomically removed by \c 
 * rename().
 *
 * If the entry has no URL defined yet, but has a copy flag set (\c 
 * RF_COPY_BASE or \c RF_COPY_SUB), this URL is taken.
 *
 * If \a revision is 0, the \c BASE revision is and \a decoder is used; 
 * this is the copy base for copied entries.
 */
int rev__install_file(struct estat *sts, svn_revnum_t revision,
		char *decoder,
		apr_pool_t *pool)
{
	int status;
	char *filename;
	char *filename_tmp;
	apr_hash_t *props;
	svn_stream_t *stream;
	apr_file_t *a_stream;
	apr_pool_t *subpool;
	char *special_data;
	char *url;
	svn_revnum_t rev_to_take;


	BUG_ON(!pool);
	filename_tmp = NULL;
	url = NULL;
	STOPIF( ops__build_path(&filename, sts), NULL);


	/* We know that we have to do something here; but because the order is 
	 * depth-first, the parent directory isn't done yet (and shouldn't be, 
	 * because it needs permissions and mtime set!).
	 * So it's possible that the target directory doesn't yet exist.
	 *
	 * Note: because we're here for *non-dir* entries, we always have a 
	 * parent. */
	STOPIF( waa__mkdir(filename, 0), NULL);


	STOPIF( apr_pool_create(&subpool, pool),
			"Creating the filehandle pool");


	/* When we get a file, old manber-hashes are stale.
	 * So remove them; if the file is big enough, we'll recreate it with 
	 * correct data. */
	STOPIF( waa__delete_byext(filename, WAA__FILE_MD5s_EXT, 1), NULL);


	/* Files get written in files; we use the temporarily generated name for  
	 * special entries, too. */
	/* We could use a completely different mechanism for temp-file-names;
	 * but keeping it close to the target lets us see if we're out of
	 * disk space in this filesystem. (At least if it's not a binding mount
	 * or something similar - but then rename() should fail).
	 * If we wrote the data somewhere else, we'd risk moving it again, across 
	 * filesystem boundaries. */
	STOPIF( waa__get_tmp_name( filename, &filename_tmp, &a_stream, subpool), 
			NULL);


	/* It's a bit easier to just take the (small) performance hit, and always 
	 * (temporarily) write the data in a file.
	 * If it's a special entry, that will just get read immediately back and 
	 * changed to the correct type.
	 *
	 * It doesn't really make much difference, as the file is always created 
	 * to get a distinct name. */
	stream=svn_stream_from_aprfile(a_stream, subpool);


	if (sts->url)
	{
		url=filename+2;
		rev_to_take=sts->repos_rev;
		current_url=sts->url;
	}
	else if (sts->flags & RF___IS_COPY)
	{
		STOPIF( cm__get_source( sts, filename, &url, &rev_to_take, 0), NULL);
		STOPIF( url__find( url, &current_url), NULL);
	}
	else
		BUG("cannot get file %s", filename);

	if (revision == 0) 
	{
		/* BASE wanted; get decoder. */
		STOPIF( up__fetch_decoder(sts), NULL);
		decoder=sts->decoder;
	}
	else
	{
		/* Arbitrary revision - get decoder. */
		rev_to_take=revision;
		decoder=DECODER_UNKNOWN;
	}


	STOPIF( url__open_session(NULL, NULL), NULL);

	/* We don't give an estat for meta-data parsing, because we have to loop 
	 * through the property list anyway - for storing locally. */
	STOPIF( rev__get_text_to_stream( url, rev_to_take, decoder, 
				stream, sts, NULL, &props, pool), NULL);


	if (apr_hash_get(props, propname_special, APR_HASH_KEY_STRING))
	{
		STOPIF( ops__read_special_entry( a_stream, &special_data, 
					0, NULL, filename_tmp, subpool), NULL);

		/* The correct type gets set on parsing. */
		STOPIF( up__handle_special(sts, filename_tmp, 
					special_data, subpool), NULL);
	}
	else
	{
		/* If it's not special, it must be an ordinary file. */
		/* This is a kind of default value, the mode is set to the repository 
		 * value in up__set_meta_data(). */
		sts->st.mode = (sts->st.mode & ~S_IFMT) | S_IFREG;
		sts->local_mode_packed=sts->new_rev_mode_packed=
			MODE_T_to_PACKED(sts->st.mode);
	}


	STOPIF( prp__set_from_aprhash(sts, props, STORE_IN_FS, 
				NULL, subpool), NULL);


	/* We write all meta-data. If we got no values from the repository, we just
	 * write what we have in the local filesystem back - the temporary file has
	 * just some default values, after all. */
	sts->remote_status |= FS_META_CHANGED;
	DEBUGP("setting meta-data");
	STOPIF( up__set_meta_data(sts, filename_tmp), NULL);

	STOPIF( apr_file_close(a_stream), NULL);


	DEBUGP("rename to %s", filename);
	/* rename to correct filename */
	STOPIF_CODE_ERR( rename(filename_tmp, filename)==-1, errno,
			"Cannot rename '%s' to '%s'", filename_tmp, filename);

	/* The rename changes the ctime. */
	STOPIF( hlp__lstat( filename, &(sts->st)),
			"Cannot lstat('%s')", filename);


	sts->url=current_url;
	/* We have to re-sort the parent directory, as the inode has changed
	 * after an rename(). */
	sts->parent->to_be_sorted=1;

	apr_pool_destroy(subpool);
	subpool=NULL;


ex:
	/* On error remove the temporary file. */
	/* Return the original error. */
	if (status && filename_tmp)
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

		/* Remove the ./ at the front */
		setenv(FSVS_EXP_CURR_ENTRY, output+2, 1);

		STOPIF_CODE_ERR( execlp( opt__get_string(OPT__MERGE_PRG), 
					opt__get_string(OPT__MERGE_PRG),
					opt__get_string(OPT__MERGE_OPT),
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
	STOPIF( prp__set_from_aprhash(sts, props, STORE_IN_FS, NULL, pool), NULL);

ex:
	return status;
}


/** Set, reset or fetch the mtime of a directory.
 * */
int rev___handle_dir_mtime(struct estat *dir,
		enum rev___dir_change_flag_e dir_flag)
{
	int status;
	char *path;


	/* Now, after all has been said and done for the children, set and re-get 
	 * the actual meta-data - the mtime has been changed in the meantime 
	 * (because of child node creation), and maybe this filesystem's 
	 * granularity is worse than on commit; then the timestamps would be 
	 * wrong. */
	status=0;
	DEBUGP("dir_flag says %X", dir_flag);


	if (dir_flag & SET_CURRENT) goto a;
	if (dir_flag & REVERT_MTIME) goto b;
	if (dir->remote_status & FS_META_CHANGED) goto c;
	if (dir_flag & GET_TSTAMP) goto d;
	/* Is there some better syntax? Some kind of switch with 
	 * case-expressions?
	 *
	 * I had a lot of if () { } with partially-overlapping conditions:
	 *   if (dir->flag &  x) A();
	 *   if (dir->flag & (x|y)) B();
	 * but gcc wouldn't simply emit a "jmp" to B() after the A() - and I 
	 * couldn't easily see that the statements were accumulative.
	 * */
	goto x;

a: 
	/* If there's an intentional change (like merging), the current time is 
	 * taken. */
	time( & dir->st.mtim.tv_sec );
b:
	/* Make sure that the value is written back to the filesystem.*/
	dir->remote_status |= FS_META_MTIME;
c:
	STOPIF( up__set_meta_data(dir, NULL), NULL);
d:
	/* ops__update_single_entry() would trash the entry_status field! */
  STOPIF( ops__build_path(&path, dir), NULL);
	STOPIF( hlp__lstat(path, &dir->st), NULL);

	/* If it had changes, we'll have to check next time. */
	if (dir->entry_status & FS_CHANGED)
		dir->flags |= RF_CHECK;

x:

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
 *
 * We may not change \c sts->entry_status - the caller still needs it; and 
 * as this is a revert to \c BASE, we must not modify the entry list 
 * either.
 * */
int rev___revert_to_base(struct estat *sts,
		enum rev___dir_change_flag_e *dir_change_flag,
		apr_pool_t *pool)
{
	int status;
	svn_revnum_t wanted;
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
	else if ( sts->flags & RF_ADD )
	{
		/* Added entry just gets un-added ... ie. unknown. */
		sts->to_be_ignored=1;
		DEBUGP("removing add-flag on %s", path);
	}
	else if (!( (sts->flags & (RF_COPY_BASE | RF_COPY_SUB)) || sts->url ) )
	{
		/* We have no URL, and no copyfrom source ... this is an unknown entry.
		 * Has to be given directly on the command line, but could have happened 
		 * via a wildcard - so don't stop working.
		 * Can't do anything about it. */
		printf("Cannot revert unknown entry \"%s\".\n", path);
		status=0;
		goto ex;
	}
	else
	{
		/* We know where to get that from. */
		DEBUGP("have an URL for %s", path);

		if ( sts->flags & RF_CONFLICT )
		{
			*dir_change_flag |= REVERT_MTIME;
			STOPIF( res__remove_aux_files(sts), NULL);
		}

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
			if (opt__verbosity() > VERBOSITY_VERYQUIET)
				printf("Reverting to revision %s:\n", hlp__rev_to_string(wanted));
			last_rev=wanted;
		}
		number_reverted++;


		DEBUGP("l_st=%s, r_st=%s, old=%p", 
				st__status_string_fromint(sts->entry_status), 
				st__status_string_fromint(sts->remote_status), 
				sts->old);

		/* Parent directories might just have been created. */
		if (!S_ISDIR(sts->st.mode))
		{
			DEBUGP("file was changed, reverting");

			/* \todo It would be nice if we could solve meta-data *for the current 
			 * revision* only changes without going to the repository - after all, 
			 * we know the old values.
			 * \todo Maybe we'd need some kind of parameter, --meta-only? Keep 
			 * data, reset rights.
			 * */
			/* TODO - opt_target_revision ? */
			STOPIF( rev__install_file(sts, wanted, sts->decoder, pool),
					"Unable to revert entry '%s'", path);
			*dir_change_flag |= REVERT_MTIME;
		}
		else
		{
#if 0
			if (sts->entry_status & FS_NEW)
				conflict();
#endif
			if (sts->entry_status & FS_REMOVED)
			{
//				sts->st.mode ist 040755 (file); old ist NULL.
				status = (mkdir(path, sts->st.mode & 07777) == -1) ? errno : 0;
				if (status == EEXIST)
				{
				DEBUGP("old=%p", sts->old);
				}
				DEBUGP("mkdir(%s) says %d", path, status);
				STOPIF(status, "Cannot create directory '%s'", path);
				*dir_change_flag |= REVERT_MTIME;

				/* As we just created the directory, we need *all* meta-data reset.  
				 * */
				sts->remote_status |= FS_META_CHANGED;
			}
			else
			{
				/* Code for directories.
				 * As the children are handled by the recursive options and by \a 
				 * ops__set_to_handle_bits(), we only have to restore the directories' 
				 * meta-data here. */
				/* up__set_meta_data() checks remote_status, while we here have 
				 * entry_status set. */
				sts->remote_status=sts->entry_status;
			}

			STOPIF( up__set_meta_data(sts, NULL), NULL);
			if (sts->entry_status)
				sts->flags |= RF_CHECK;
		}
	}

	/* There's no change anymore, we're at BASE.
	 * But just printing "...." makes no sense ... show the old status. */
	sts->flags |= RF_PRINT;

ex:
	return status;
}


/** Reset local changes. */
int rev___no_local_change(struct estat *sts)
{
  sts->entry_status=0;
	return st__progress(sts);
}


/** -.
 * Recurses for rev___revert_to_base.
 *
 * There's a bit of ugliness here, regarding deleted directories ...
 *
 * 1) If we do the tree depth-first, we have to build multiple levels of 
 * directories at once - and store which have to have their meta-data 
 * reset.
 *
 * 2) If we do level after level, we might end up with either
 * a) re-creating a directory, doing its children, then have to re-set the 
 * meta-data of this directory, or
 * b) just store that the meta-data has to be done for later.
 *
 * Currently we do 2a - that seems the simplest, and has no big performance 
 * penalty. */
int rev___local_revert(struct estat *dir, 
		apr_pool_t *pool)
{
	int status;
	int i, do_undo;
	struct estat *sts;
	apr_pool_t *subpool;
	enum rev___dir_change_flag_e dir_flag;


	status=0;
	subpool=NULL;
	dir_flag= NOT_CHANGED;

	for(i=0; i<dir->entry_count; i++)
	{
		sts=dir->by_inode[i];
		STOPIF( apr_pool_create(&subpool, pool), "Cannot get a subpool");

		do_undo = sts->do_this_entry && 
			(sts->entry_status & FS__CHANGE_MASK) &&
			ops__allowed_by_filter(sts);

		DEBUGP("on %s: do_undo=%d, st=%s", sts->name, do_undo,
				st__status_string_fromint(sts->entry_status));

		if (do_undo)
			STOPIF( rev___revert_to_base(sts, &dir_flag, subpool), NULL);

		if (S_ISDIR(sts->st.mode) && 
				(sts->entry_status & FS_CHILD_CHANGED))
			STOPIF( rev___local_revert(sts, subpool), NULL);
	
		if (do_undo)
			STOPIF( st__status(sts), NULL);


		apr_pool_destroy(subpool);
		subpool=NULL;
	}

	/* We cannot free the memory earlier - the data is needed for the status 
	 * output and recursion. */
	STOPIF( ops__free_marked(dir, 0), NULL);

	/* The root entry would not be printed; do that here. */
	if (!dir->parent)
		STOPIF( st__status(dir), NULL);


	STOPIF( rev___handle_dir_mtime(dir, dir_flag), NULL);

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
	svn_revnum_t rev;


	status=0;
	/* For revert the default is non-recursive. */
	opt_recursive--;

	if (!argc) ac__Usage_this();

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	STOPIF( url__load_nonempty_list(NULL, 0), NULL);

	if (opt_target_revisions_given)
	{
		STOPIF( wa__warn( WRN__MIXED_REV_WC, EINVAL,
					"Sorry, fsvs currently doesn't allow mixed revision working copies.\n"
					"Entries will still be compared against the BASE revision.\n"),
				NULL);

		// TODO: necessary?
		action->local_callback=rev___no_local_change;
	}
	else
	{
		/* No revision given - just go back to BASE. */
		action->local_callback=st__progress;
	}


	/* This message can be seen because waa__find_common_base() looks for
	 * an "url" file and not for a "dir" -- which means that this tree
	 * was never committed, so we don't know what HEAD is. */
	/* Maybe the user could still try with some revision number and we simply
	 * check for the existence of the given path there? */
	status=waa__read_or_build_tree(root, argc, normalized, argv, NULL, 1);
	if (status == -ENOENT)
		STOPIF(status,
				"!We know nothing about previous or current versions, as this tree\n"
				"was never checked in.\n"
				"If you need such an entry reverted, you could either write the needed\n"
				"patch (and create a pull request on github), or try with a 'sync-repos'\n"
				"command before (if you know a good revision number)\n");
	else
		STOPIF(status, NULL);

	STOPIF( st__progress_uninit(), NULL);


	if (opt_target_revisions_given)
	{
		while ( ! ( status=url__iterator(&rev) ) )
		{
			STOPIF( cb__record_changes(root, rev, current_url->pool), NULL);
		}
		STOPIF_CODE_ERR( status != EOF, status, NULL);

		STOPIF( rev__do_changed(root, global_pool), NULL);
	}
	else
	{
		/* The local changes are taken as to be undone.
		 *
		 * We cannot go by estat::entry_status - things like RF_ADD have to be 
		 * undone, too.
		 *
		 * waa__do_sorted_tree() can't be used, either, because it does the 
		 * directory *before* the children - which makes the directories' mtime 
		 * wrong if children get created or deleted. */
		STOPIF( rev___local_revert(root, global_pool), NULL);
	}

	
	/* If this was a revert with destination revision, we might have changed 
	 * the entire hierarchy - replaced directories with files, etc.
	 * This changed tree must not be written, because it's not the state of 
	 * BASE.
	 *   [ And if we had to write the original (BASE) list for some cause,
	 *     we'd have to read the list afresh, and change what we have to.
	 *     Or, the other way: when getting the changes for the given revision 
	 *     from the repository we'd have to put them in the estat::old shadow 
	 *     tree, to keep the entry list correct. ]
	 *
	 * If this was a revert to BASE, we have to write the list, because the 
	 * ctime of the inodes will be changed - and would mark the entries as 
	 * "maybe changed". */
	if (!opt_target_revisions_given)
	{
		delay_start=time(NULL);
		STOPIF( waa__output_tree(root), NULL);
		STOPIF( hlp__delay(delay_start, DELAY_REVERT), NULL);
	}

ex:
	return status;
}


/** Takes the \c sts->remote_status, and does the changes mentioned there.
 * Depending on \c sts->entry_status a conflict might be set.
 *
 * Convenience function to reduce indenting.  */
int rev___undo_change(struct estat *sts, 
		enum rev___dir_change_flag_e *dir_change_flag,
		apr_pool_t *pool)
{
	int status;
	char *fn;
	const char *unique_name_mine, 
				*unique_name_remote, 
				*unique_name_common;
	char revnum[12];
	int j;
	struct estat *removed;
	struct sstat_t st;


	STOPIF( ops__build_path( &fn, sts), NULL);
	DEBUGP_dump_estat(sts);

	/* If we remove an entry, the entry_count gets decremented; 
	 * we have to repeat the loop *for the same index*. */


	unique_name_mine=NULL;

	/* Conflict handling; depends whether it has changed locally. */
	if (sts->entry_status & FS_CHANGED)
		switch (opt__get_int(OPT__CONFLICT))
		{
			case CONFLICT_STOP:
				STOPIF( EBUSY, "!The entry %s has changed locally", fn);
				break;

			case CONFLICT_LOCAL:
				/* Next one, please. */
				STOPIF_CODE_EPIPE( printf("Conflict for %s skipped.\n", fn), NULL);
				goto ex;

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

	/* If the entry wasn't replaced, but only removed, there's no sts->old. 
	 * */
	removed=sts->old ? sts->old : sts;
	if (removed->remote_status & FS_REMOVED)
	{
		/* Is the entry already removed? */
		/* If there's a typechange involved, the old entry has been 
		 * renamed, and so doesn't exist in the filesystem anymore. */
		if ((sts->entry_status & FS_REPLACED) != FS_REMOVED &&
				!unique_name_mine)
		{
			/* Find type. Small race condition - it might be removed now. */
			if (TEST_PACKED(S_ISDIR, removed->old_rev_mode_packed))
			{
				STOPIF( up__rmdir(removed, sts->url), NULL);
			}
			else
				STOPIF( up__unlink(removed, fn), NULL);
		}

		*dir_change_flag|=REVERT_MTIME;
	}


	/* Is there some kind of garbage that has to be removed? */
	if (TEST_PACKED(S_ISGARBAGE, sts->local_mode_packed))
	{
		DEBUGP("cleaning garbage");
		STOPIF_CODE_ERR( unlink(fn) == -1, errno,
				"Cannot remove garbage entry %s", fn);
	}



	/* If we change something in this directory, we have to re-sort the 
	 * entries by inode again. */
	sts->parent->to_be_sorted=1;

	if ((sts->remote_status & FS_REPLACED) == FS_REMOVED)
	{
		sts->to_be_ignored=1;
		goto ex;
	}
	current_url=sts->url;

	if (S_ISDIR(sts->st.mode))
	{
		*dir_change_flag|=REVERT_MTIME;
		STOPIF( waa__mkdir_mask(fn, 1, sts->st.mode), NULL);

		/* Meta-data is done later. */

		/* An empty directory need not be sorted; if we get entries,
		 * we'll mark it with \c to_be_sorted .*/
	}
	else if (sts->remote_status & (FS_CHANGED | FS_REPLACED))
		/* Not a directory */
	{
		STOPIF( rev__install_file(sts, 0, sts->decoder, pool), NULL);
		*dir_change_flag|=REVERT_MTIME;

		/* We had a conflict; rename the file fetched from the 
		 * repository to a unique name. */
		if (unique_name_mine)
		{
			*dir_change_flag|=SET_CURRENT;

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
				STOPIF_CODE_EPIPE(
						printf("\"%s\" already marked as conflict.\n", fn), 
						NULL);
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
				STOPIF( rev__install_file(sts, sts->old_rev, 
							NULL, pool), NULL);

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
			STOPIF( rev__get_props(sts, NULL, sts->repos_rev, pool), NULL);

		if (sts->remote_status & FS_META_CHANGED)
		{
			/* If we removed the file, it has no meta-data any more;
			 * if we fetched it via rev__get_file(), it has it set already.
			 * Only the case of *only* meta-data-change is to be done. */
			STOPIF( up__set_meta_data(sts, fn), NULL);
		}
	}


ex:
	return status;
}


/** -.
 * Used on update. */
int rev__do_changed(struct estat *dir, 
		apr_pool_t *pool)
{
	int status;
	int i;
	struct estat *sts;
	apr_pool_t *subpool;
	enum rev___dir_change_flag_e dir_flag;


	status=0;
	subpool=NULL;
	dir_flag= (dir->entry_status & FS_NEW) || 
		(dir->remote_status & FS_NEW) ? REVERT_MTIME : NOT_CHANGED;

	/* If some children have changed, do a full run.
	 * Else just repair meta-data. */
	if (!(dir->remote_status & FS_CHILD_CHANGED))
		DEBUGP("%s: no children changed", dir->name);
	else for(i=0; i<dir->entry_count; i++)
	{
		sts=dir->by_inode[i];

		STOPIF( apr_pool_create(&subpool, pool), "Cannot get a subpool");

		if (sts->remote_status & FS__CHANGE_MASK)
			STOPIF( rev___undo_change(sts, &dir_flag, subpool), NULL);

		/* We always recurse now, even if the directory has no children.
		 * Else we'd have to check for children in a few places above, which would
		 * make the code unreadable. */
		if (S_ISDIR(sts->st.mode) &&
				(sts->remote_status & FS_REPLACED) != FS_REMOVED)
		{
			apr_pool_destroy(subpool);
			subpool=NULL;
			STOPIF( apr_pool_create(&subpool, pool), "subpool creation");

			STOPIF( rev__do_changed(sts, subpool), NULL);
		}	

		STOPIF( st__rm_status(sts), NULL);

		apr_pool_destroy(subpool);
		subpool=NULL;
	}

	/* We cannot free the memory earlier - the data is needed for the status 
	 * output and recursion. */
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

	STOPIF( rev___handle_dir_mtime(dir, dir_flag), NULL);


ex:
	return status;
}



