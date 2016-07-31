/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/


/** \file
 * Callback functions, and cb__record_changes() editor.
 * */

#include <errno.h>
#include <unistd.h>

#include <subversion-1/svn_ra.h>
#include <subversion-1/svn_auth.h>
#include <subversion-1/svn_client.h>
#include <subversion-1/svn_config.h>
#include <subversion-1/svn_cmdline.h>



#include "global.h"
#include "helper.h"
#include "update.h"
#include "est_ops.h"
#include "checksum.h"
#include "url.h"
#include "racallback.h"


svn_error_t *cb__init(apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;
	apr_hash_t *cfg_hash;
	svn_config_t *cfg;


	status=0;
	STOPIF_SVNERR(svn_config_get_config,
			(&cfg_hash, NULL, pool) );

	cfg = apr_hash_get(cfg_hash, SVN_CONFIG_CATEGORY_CONFIG,
			APR_HASH_KEY_STRING);

	/* Set up Authentication stuff. */
	STOPIF_SVNERR( svn_cmdline_setup_auth_baton,
			(&cb__cb_table.auth_baton,
			 !(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)),
			 NULL, /* Username */
			 NULL, /* Password */
			 NULL, /* Config dir */
			 1, /* no_auth_cache */
			 cfg,
			 NULL, /* cancel function */
			 NULL, /* cancel baton */
			 pool)
			);

	BUG_ON(!cb__cb_table.auth_baton);

ex:
	RETURN_SVNERR(status);
}


/*----------------------------------------------------------------------------
 * RA-layer callback functions
 *--------------------------------------------------------------------------*/
/// FSVS GCOV MARK: cb__open_tmp should not be executed
/** This function has to be defined, but gets called only with
 * http-URLs. */
svn_error_t *cb__open_tmp (apr_file_t **fp,
		void *callback_baton,
		apr_pool_t *pool)
{
	int status;
	static char *buffer=NULL, *fn_pos;
	const char *tmp;
	int len;
	char filename[]="fsvs.XXXXXX";

	if (!buffer)
	{
		STOPIF( apr_temp_dir_get(&tmp, pool),
				"Getting a temporary directory path");

		len=strlen(tmp);
		/* Directory PATH_SEPARATOR Filename '\0' (+space) */
		buffer=malloc(len + 1 + strlen(filename) + 1 + 4);
		STOPIF_ENOMEM(!buffer);

		strcpy(buffer, tmp);
		fn_pos=buffer+len;
		*(fn_pos++) = PATH_SEPARATOR;
	}

	strcpy(fn_pos, filename);
	STOPIF( apr_file_mktemp(fp, buffer, 0, pool),
			"Cannot create a temporary file");
ex:
	RETURN_SVNERR(status);
}


struct svn_ra_callbacks_t cb__cb_table=
{
	.open_tmp_file = cb__open_tmp,
	.auth_baton = NULL,

	.get_wc_prop = NULL,
	.set_wc_prop=NULL,
	.push_wc_prop=NULL,
	.invalidate_wc_props=NULL,
};


/*----------------------------------------------------------------------------
 * \defgroup changerec Change-Recorder
 * An editor which simply remembers which entries are changed.
 * @{
 *--------------------------------------------------------------------------*/

svn_revnum_t cb___dest_rev;

/** A txdelta consumer which ignores the data. */
svn_error_t *cb__txdelta_discard(svn_txdelta_window_t *window UNUSED, 
		void *baton UNUSED)
{
	return NULL;
}


void cb___mark_parents(struct estat *sts)
{
	/* mark the entry as to-be-done.
	 * mark the parents too, so that we don't have to search
	 * in-depth. */
	while (sts->parent && !(sts->parent->remote_status & FS_CHILD_CHANGED))
	{
		sts->parent->remote_status |= FS_CHILD_CHANGED;
		sts=sts->parent;
	}
}
	

/** This function adds a new entry below dir, setting it to
 * \c FS_NEW or \c FS_REPLACED. */
int cb___add_entry(struct estat *dir, 
		const char *utf8_path,
		const char *utf8_copy_path, 
		svn_revnum_t copy_rev,
		int mode,
		int *has_existed,
		void **new)
{
	int status;
	struct estat *sts;
	const char *filename;
	char* path;
	char* copy_path;


	STOPIF( hlp__utf82local(utf8_path, &path, -1), NULL );
	STOPIF( hlp__utf82local(utf8_copy_path, &copy_path, -1), NULL );

	STOPIF_CODE_ERR(copy_path, EINVAL,
			"don't know how to handle copy_path %s@%ld", 
			copy_path, copy_rev);

	DEBUGP("add entry %s, mode 0%03o", path, mode);
	/* The path should be done by open_directory descending. 
	 * We need only the file name. */
	filename = ops__get_filename(path);
	STOPIF( ops__find_entry_byname(dir, filename, &sts, 0),
			"cannot lookup entry %s", path);

	if (sts)
	{
		/* This file already exists, or an update from another URL just
		 * brought it in.
		 *
		 * The caller knows whether we should overwrite it silently. */
		if (sts->remote_status & FS_REMOVED)
			sts->remote_status = FS_REPLACED;

		if (has_existed) *has_existed=EEXIST;
		if (!url__current_has_precedence(sts->url))
			goto no_change;
	}
	else
	{
		STOPIF( ops__allocate(1, &sts, NULL), NULL);
		sts->name = strdup(filename);
		STOPIF_ENOMEM(!sts->name);
		sts->parent = dir;
		sts->remote_status = FS_NEW;

		/* Put into tree */
		STOPIF( ops__new_entries(dir, 1, &sts), NULL);
		if (has_existed) *has_existed=0;
	}

	cb___mark_parents(sts);

	sts->st.mode = mode | 0700; /* until we know better */
	sts->entry_type = ops___filetype( &(sts->st) );
	sts->url=current_url;
	/* To avoid EPERM on chmod() etc. */
	sts->st.uid=getuid(); 
	sts->st.gid=getgid(); 

	sts->url=current_url;

	dir->remote_status |= FS_CHANGED;

no_change:	
	/* Even if this entry has lower priority, we have to have a baton for it.
	 * It may be a directory, and subentries might be visible. */
	*new = sts;

ex:
	return status;
}


int cb___open_entry(const char *utf8_path,
		struct estat *dir,
		void **child_baton)
{
	struct estat *sts;
	int status;
	char* path;


	STOPIF( hlp__utf82local(utf8_path, &path, -1), NULL );

	status=0;
	STOPIF( ops__find_entry_byname(dir, path, &sts, 0), 
			"cannot find entry %s", path);

	if (!sts) status=ENOENT;
	else
	{
		if (!sts->url || url__current_has_precedence(sts->url))
			sts->url = current_url;
	}

	*child_baton = sts;
ex:
	return status;
}


svn_error_t *cb___set_target_revision(void *edit_baton,
		svn_revnum_t rev,
		apr_pool_t *pool)
{
	int status;
	struct estat *root UNUSED=edit_baton;

	status=0;
	DEBUGP("setting revision to %llu", (t_ull)rev);
	cb___dest_rev=rev;
	RETURN_SVNERR(status);
}


svn_error_t *cb___open_root(void *edit_baton,
		svn_revnum_t base_revision,
		apr_pool_t *dir_pool UNUSED,
		void **root_baton)
{
	struct estat *sts=edit_baton;

	*root_baton=sts;

	return SVN_NO_ERROR; 
}


svn_error_t *cb___delete_entry(const char *utf8_path,
		svn_revnum_t revision UNUSED,
		void *parent_baton,
		apr_pool_t *pool)
{
	int status;
	struct estat *dir=parent_baton;
	struct estat *sts;
	char* path;

	STOPIF( hlp__utf82local(utf8_path, &path, -1), NULL );

	STOPIF( ops__find_entry_byname(dir, path, &sts, 0), NULL);

	if (sts)
	{
		DEBUGP("deleting entry %s", path);
		cb___mark_parents(sts);
		dir->remote_status |= FS_CHANGED;
		sts->remote_status = FS_REMOVED;

		if (action->repos_feedback)
			STOPIF( action->repos_feedback(sts, NULL), NULL);

		/** Allow lower priority entries to be seen. 
		 * A bit of a hack. 
		 * \see url___sorter(). */
		sts->url = urllist[urllist_count-1];
	}
	else
	{
		DEBUGP("entry %s not found!", path);
		/** \todo conflict? */
	}

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___add_directory(const char *utf8_path,
		void *parent_baton,
		const char *utf8_copy_path,
		svn_revnum_t copy_rev,
		apr_pool_t *dir_pool,
		void **child_baton)
{
	struct estat *dir=parent_baton;
	struct estat *sts;
	int status;
	int has_existed;

	STOPIF( cb___add_entry(dir, utf8_path, utf8_copy_path, 
				copy_rev, S_IFDIR, &has_existed,
				child_baton), NULL );
	sts=*child_baton;

	if (!has_existed)
	{
		/* Initialize the directory-specific data. 
		 * If this was a file before, it may have old values in the 
		 * shared storage space. */
		sts->entry_count=0;
		sts->by_inode = sts->by_name = NULL;
		sts->strings = NULL;
		sts->other_revs=sts->to_be_sorted=0;
	}

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___open_directory(const char *utf8_path,
		void *parent_baton,
		svn_revnum_t base_revision UNUSED,
		apr_pool_t *dir_pool,
		void **child_baton)
{
	struct estat *dir=parent_baton;
	int status;

	status=cb___open_entry( utf8_path, dir, child_baton);
	if (status == ENOENT)
	{
		/** \todo conflict - removed locally? added */
		STOPIF( cb___add_entry(dir, utf8_path, NULL, 0, 
					S_IFDIR, NULL, child_baton), NULL );
		cb___mark_parents(dir);
	}
	else
	{
		STOPIF( status, NULL);
	}

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___change_dir_prop(void *dir_baton,
		const char *utf8_name,
		const svn_string_t *value,
		apr_pool_t *pool)
{
	struct estat *sts=dir_baton;
	int status;

	status=0;

/** The root entry has per definition \b no URL. */
	if (!sts->url || url__current_has_precedence(sts->url))
	{
		STOPIF( up__parse_prop(sts, utf8_name, value, NULL, pool), NULL);
		cb___mark_parents(sts);
	}

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___close_directory(
		void *dir_baton,
		apr_pool_t *pool)
{
	struct estat *sts=dir_baton;
	int status;

	status=0;
	sts->repos_rev = cb___dest_rev;

	if (action->repos_feedback)
	  STOPIF( action->repos_feedback(sts, NULL), NULL);

ex:
	RETURN_SVNERR(status);
}


/// FSVS GCOV MARK: cb___absent_directory should not be executed
svn_error_t *cb___absent_directory(const char *utf8_path,
		void *parent_baton,
		apr_pool_t *pool)
{
	struct estat *dir UNUSED =parent_baton;

	DEBUGP("in %s", __PRETTY_FUNCTION__);

	return SVN_NO_ERROR; 
}


svn_error_t *cb___add_file(const char *utf8_path,
		void *parent_baton,
		const char *utf8_copy_path,
		svn_revnum_t copy_rev,
		apr_pool_t *file_pool,
		void **file_baton)
{
	struct estat *dir=parent_baton;
	struct estat *sts;
	int status;

	STOPIF( cb___add_entry(dir, utf8_path, utf8_copy_path, 
				copy_rev, S_IFREG, NULL, file_baton),
			NULL);
	sts=*file_baton;

	/* In case this was a directory, we may need to remove its children; 
	 * for that we use the \c entry_count and \c by_inode members, 
	 * so don't overwrite them.
	 *
	 * The MD5 gets filled by \c svn_ra_get_file(), \c change_flag must not be
	 * used. 
	 * */

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___open_file(const char *utf8_path,
		void *parent_baton,
		svn_revnum_t base_revision,
		apr_pool_t *file_pool,
		void **file_baton)
{
	struct estat *dir=parent_baton;
	struct estat *sts;
	int status;

	status=cb___open_entry( utf8_path, dir, file_baton);
	if (status == ENOENT)
	{
		/** \todo conflict - removed locally */
		STOPIF( cb___add_entry(dir, utf8_path, NULL, 0,
					S_IFREG, NULL, file_baton), NULL);
		sts=*file_baton;
		if (url__current_has_precedence(sts->url))
		{
			cb___mark_parents(dir);
		}
	}
	else
	{
		STOPIF( status, NULL);
		STOPIF( up__fetch_decoder(*(struct estat**)file_baton), NULL);
	}

	((struct estat*)*file_baton)->decoder_is_correct=1;

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___apply_textdelta(void *file_baton,
		const char *base_checksum UNUSED,
		apr_pool_t *pool UNUSED,
		svn_txdelta_window_handler_t *handler,
		void **handler_baton)
{
	struct estat *sts UNUSED=file_baton;
	int status;

	status=0;
	if (url__current_has_precedence(sts->url))
	{
		sts->remote_status |= FS_CHANGED;
		cb___mark_parents(sts);
	}

	*handler = cb__txdelta_discard;
	*handler_baton=sts;

	RETURN_SVNERR(status);
}


svn_error_t *cb___change_file_prop(void *file_baton,
		const char *utf8_name,
		const svn_string_t *value,
		apr_pool_t *pool)
{
	struct estat *sts=file_baton;
	int status;

	status=0;
	if (url__current_has_precedence(sts->url))
	{
		STOPIF( up__parse_prop(sts, utf8_name, value, NULL, pool), NULL);
		cb___mark_parents(sts);
	}

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___close_file(void *file_baton,
		const char *text_checksum,
		apr_pool_t *pool)
{
	struct estat *sts UNUSED=file_baton;
	int status;

	status=0;
	if (sts->remote_status)
		sts->repos_rev = cb___dest_rev;

	if (action->repos_feedback)
	  STOPIF( action->repos_feedback(sts, NULL), NULL);

	BUG_ON(S_ISDIR(sts->st.mode));
	if (!S_ISDIR(sts->st.mode) && text_checksum)
		STOPIF( cs__char2md5(text_checksum, sts->md5 ), NULL);

	if (sts->has_orig_md5 || sts->decoder)
		DEBUGP("Has an original MD5, %s not used", text_checksum);
	else
		if (text_checksum)
			STOPIF( cs__char2md5(text_checksum, sts->md5 ), NULL);

ex:
	RETURN_SVNERR(status);
}


/// FSVS GCOV MARK: cb___absent_file should not be executed
svn_error_t *cb___absent_file(const char *utf8_path,
		void *parent_baton,
		apr_pool_t *pool)
{
	struct estat *dir UNUSED=parent_baton;

	DEBUGP("in %s", __PRETTY_FUNCTION__);
	return SVN_NO_ERROR; 
}


svn_error_t *cb___close_edit(void *edit_baton, 
		apr_pool_t *pool UNUSED)
{
	int status;
	struct estat *root UNUSED=edit_baton;

	status=0;
	/* For sync-repos the root was printed with a close_directory call, and 
	 * others print it in rev__do_changed().  */

	RETURN_SVNERR(status);
}


/// FSVS GCOV MARK: cb___abort_edit should not be executed
svn_error_t *cb___abort_edit(void *edit_baton,
		apr_pool_t *pool UNUSED)
{
	struct estat *sts UNUSED=edit_baton;

	return SVN_NO_ERROR; 
}




const svn_delta_editor_t cb___change_recorder = 
{
	.set_target_revision 	= cb___set_target_revision,

	.open_root 						= cb___open_root,

	.delete_entry				 	= cb___delete_entry,
	.add_directory 				= cb___add_directory,
	.open_directory 			= cb___open_directory,
	.change_dir_prop 			= cb___change_dir_prop,
	.close_directory 			= cb___close_directory,
	.absent_directory 		= cb___absent_directory,

	.add_file 						= cb___add_file,
	.open_file 						= cb___open_file,
	.apply_textdelta 			= cb___apply_textdelta,
	.change_file_prop 		= cb___change_file_prop,
	.close_file 					= cb___close_file,
	.absent_file 					= cb___absent_file,

	.close_edit 					= cb___close_edit,
	.abort_edit 					= cb___abort_edit,
};


int cb___report_path_rev(struct estat *dir, 
		const svn_ra_reporter2_t *reporter,
		void *report_baton, 
		apr_pool_t *pool)
{
	int status, i;
	struct estat *sts;
	svn_error_t *status_svn;
	char *fn;


	status=0;
	for(i=0; i<dir->entry_count; i++)
	{
		sts=dir->by_inode[i];

		STOPIF( ops__build_path(&fn, sts), NULL );
		/* We have to cut the "./" in front. */
		/* We report the directories' revision too. */
		/* As we're doing the children of a directory, there must always
		 * be a parent. */
		/* \todo: the parent might be from another URL. What should we do? */
		if ( sts->repos_rev != sts->parent->repos_rev)
		{
			DEBUGP("reporting %s at %llu", fn, (t_ull)sts->repos_rev);
			STOPIF_SVNERR( reporter->set_path,
					(report_baton, fn+2, sts->repos_rev, 0, "", pool));
		}

		if (S_ISDIR(sts->st.mode) && sts->other_revs)
		{
			STOPIF( cb___report_path_rev(sts, reporter, report_baton, pool),
					NULL);
		}	
	}

ex:
	return status;
}



/** -.
 * Calls the svn libraries and records which entries would be changed
 * on this update. 
 * \param session An opened session
 * \param root The root entry of this wc tree
 * \param target The target revision. \c SVN_INVALID_REVNUM is not valid.
 * \param pool An APR-pool.
 *
 * When a non-directory entry gets replaced by a directory, its 
 * MD5 is lost (because the directory is initialized to 
 * \c entry_count=0 , \c by_inode=by_name=NULL ); that should not matter, 
 * since we have modification flags in \c entry_status .
 *
 * If a non-directory gets replaced by a directory, \c entry_count and 
 * \c by_inode are kept - we need them for up__rmdir() to remove 
 * known child entries.
 * */
int cb__record_changes(svn_ra_session_t *session,
		struct estat *root,
		svn_revnum_t target,
		apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;
	void *report_baton;
	const svn_ra_reporter2_t *reporter;


	status=0;
	cb___dest_rev=target;
	STOPIF_SVNERR( svn_ra_do_status,
			(session,
			 &reporter,
			 &report_baton,
			 "",
			 target,
			 TRUE,
			 &cb___change_recorder,
			 root,
			 pool) );

	/* If this is a checkout, we need to set the base directory at HEAD, but
	 * empty. We cannot use the base at revision 0, because it probably didn't
	 * exist there. */
	if (current_url->current_rev == 0)
		STOPIF_SVNERR( reporter->set_path,
				(report_baton,
				 "", target,
				 TRUE, NULL, global_pool));
	else
		STOPIF_SVNERR( reporter->set_path,
				(report_baton,
				 "", current_url->current_rev,
				 FALSE, NULL, global_pool));

	DEBUGP("Getting changes from %llu to %llu", 
			(t_ull)current_url->current_rev,
			(t_ull)target);
			#if 0
	STOPIF( cb___report_path_rev( root, reporter, report_baton, pool), NULL);
	#endif

	STOPIF_SVNERR( reporter->finish_report, 
			(report_baton, global_pool));

	current_url->current_rev=cb___dest_rev;

ex:
	return status;
}

/** @} */
