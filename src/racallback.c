/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/


/** \file
 * Callback functions, and cb__record_changes() editor.
 * */

#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <subversion-1/svn_ra.h>
#include <subversion-1/svn_auth.h>
#include <subversion-1/svn_client.h>
#include <subversion-1/svn_cmdline.h>



#include "global.h"
#include "helper.h"
#include "update.h"
#include "est_ops.h"
#include "checksum.h"
#include "status.h"
#include "cache.h"
#include "url.h"
#include "racallback.h"


svn_error_t *cb__init(apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;
	apr_hash_t *cfg_hash;
	svn_config_t *cfg;
	char *cfg_usr_path;


	STOPIF( hlp__get_svn_config(&cfg_hash), NULL);

	cfg = apr_hash_get(cfg_hash, SVN_CONFIG_CATEGORY_CONFIG,
			APR_HASH_KEY_STRING);

	 /* make sure that folders for storing authentications credentials are created */
	STOPIF_SVNERR( svn_config_ensure, (cfg_usr_path, pool));

	/* Set up Authentication stuff. */
	STOPIF_SVNERR( svn_cmdline_setup_auth_baton,
			(&cb__cb_table.auth_baton,
			 !(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)),
			 opt__get_int(OPT__AUTHOR) ? 
			 opt__get_string(OPT__AUTHOR) : NULL,
			 NULL, /* Password */
			 cfg_usr_path,
			 0, /* no_auth_cache */
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


	STOPIF( waa__get_tmp_name( NULL, NULL, fp, pool), NULL);

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


/** If \a may_create is \c 0, \c ENOENT may be returned (ie. was not 
 * found).
 * 
 * If \a mode doesn't include some permission bits, like \c 0700 or \c 
 * 0600, a default value is chosen.
 *
 * If it didn't exist, or if this is a higher priority URL, the parents get 
 * FS_CHILD_CHANGED set.
 *
 * \a path gets set (if not \c NULL) to \a utf8_path in local encoding. */
int cb__add_entry(struct estat *dir, 
		const char *utf8_path, char **loc_path,
		const char *utf8_copy_path, 
		svn_revnum_t copy_rev,
		int mode,
		int *has_existed,
		int may_create,
		void **new)
{
	int status;
	struct estat *sts;
	char *filename;
	char* path;
	char* copy_path;
	int overwrite;
	struct sstat_t st;


	overwrite=0;
	STOPIF( hlp__utf82local(utf8_path, &path, -1), NULL );
	if (loc_path) *loc_path=path;
	STOPIF( hlp__utf82local(utf8_copy_path, &copy_path, -1), NULL );

	STOPIF_CODE_ERR(copy_path, EINVAL,
			"don't know how to handle copy_path %s@%ld", 
			copy_path, copy_rev);

	/* The path should be done by open_directory descending.
	 * We need only the file name. */
	filename = ops__get_filename(path);
	STOPIF( ops__find_entry_byname(dir, filename, &sts, 0),
			"cannot lookup entry %s", path);
	DEBUGP("entry %s, mode 0%03o; %sfound, may %screate", path, mode, 
			sts ? "" : "not ",
			may_create ? "" : "not ");


	if (sts)
	{
		if (has_existed) *has_existed=EEXIST;

		if (!url__current_has_precedence(sts->url))
			goto no_change;

		/* This file already exists, or an update from another URL just
		 * brought it in.
		 *
		 * The caller knows whether we should overwrite it silently. */
		if (sts->remote_status & FS_REMOVED)
		{
			STOPIF( ops__make_shadow_entry(sts, SHADOWED_BY_REMOTE), NULL);
			overwrite=1;
		}
	}
	else
	{
		STOPIF_CODE_ERR(!may_create, ENOENT, NULL);

		STOPIF( ops__allocate(1, &sts, NULL), NULL);
		memset(sts, 0, sizeof(*sts));

		/* To avoid the memory allocator overhead we would have to do our own 
		 * memory management here - eg. using dir->string.
		 * But that would have to be tuned for performance - we get here often.
		 * TODO.
		 * */
		STOPIF( hlp__strdup(&sts->name, filename), NULL);

		sts->remote_status = FS_NEW;
		overwrite=1;
		/* Put into tree. */
		STOPIF( ops__new_entries(dir, 1, &sts), NULL);

		/* Directory got changed. */
		dir->remote_status |= FS_CHANGED;

		if (has_existed) *has_existed=0;
	}


	if (overwrite)
	{
		sts->parent=dir;

		/* This memset above implicitly clears all other references to the copy 
		 * data - entry_count, by_inode, by_name, strings.
		 * But we need the copy itself. */
    sts->entry_count=0;
    sts->by_inode=NULL;
    sts->by_name=NULL;
    sts->strings=NULL;

    sts->decoder=NULL;
    sts->has_orig_md5=0;
		memset(& sts->md5, 0, sizeof(sts->md5));

		memset(& sts->st, 0, sizeof(sts->st));

		/* Some permission bits must be set; suid/sgid/sticky are not enough.  
		 * Directories need an "x" bit, too.
		 * */
		if (!(mode & 0777))
			mode |= S_ISDIR(mode) ? 0700 : 0600;
		/* Until we know better */
		sts->st.mode = mode; 

		/* Default is current time. */
		time( & sts->st.mtim.tv_sec );

		/* To avoid EPERM on chmod() etc. */
		sts->st.uid=getuid(); 
		sts->st.gid=getgid(); 
	}

	sts->url=current_url;
	ops__mark_parent_cc(sts, remote_status);

	/* If it's a new entry or not, we set the current type. */
	sts->new_rev_mode_packed = mode ? 
		MODE_T_to_PACKED(mode) : sts->old_rev_mode_packed;


	if (sts->local_mode_packed == S_IFUNDEF)
	{
		/* We want to know which type it is locally.
		 * We trust the filename we get from subversion. */
		status=hlp__lstat(path, &st);
		if (status == ENOENT)
		{
			/* sts->local_mode_packed is still 0(invalid), because of the 
			 * memset() above. */
		}
		else if (!status || status == -ENOENT)
		{
			/* That works for normal and garbage entries. */
			sts->local_mode_packed=MODE_T_to_PACKED(st.mode);
		}
		else
		{
			/* We have to ignore errors here, as we might add eg.  
			 * 'sub1/sub2/entry', but locally 'sub1' is a file - and then we'd get 
			 * ENOTDIR. */
		}
		/* No error to be delivered. */
		status=0;
	}

	DEBUGP("%s is locally a %s",
			path, st__type_string(PACKED_to_MODE_T(sts->local_mode_packed)));


no_change:	
	/* Even if this entry has lower priority, we have to have a baton for it.
	 * It may be a directory, and subentries might be visible. */
	*new = sts;

ex:
	return status;
}


inline int cb___store_prop(struct estat *sts, 
		const char *utf8_name, const svn_string_t *value,
		apr_pool_t *pool)
{
	int status;
	int user_prop;
	apr_pool_t *u_p_pool;
	char *copy;
	#ifdef DEBUG
	static long u_p_count=0,
							u_p_bytes=0;
	#endif


	status=0;
	if (!url__current_has_precedence(sts->url)) goto ex;


	user_prop=0;
	STOPIF( up__parse_prop(sts, utf8_name, value, &user_prop, pool), NULL);
	ops__mark_parent_cc(sts, remote_status);
	DEBUGP("have name=%s; user? %d", utf8_name, user_prop);


	if (action->keep_user_prop && user_prop)
	{
		if (!sts->user_prop)
		{
			/* The root entry has no associated URL, so it has no pool.
			 * Normally there shouldn'd be any user-properties, though. */
			STOPIF( apr_pool_create(&u_p_pool, sts->url ? 
						sts->url->pool : global_pool), NULL);

			sts->user_prop=apr_hash_make(u_p_pool);
			apr_hash_set(sts->user_prop, "", 0, u_p_pool);
		}
		else
			u_p_pool=apr_hash_get(sts->user_prop, "", 0);


		/* apr_hash_set() only stores an address; we have to take care to not 
		 * loose the length and data, because the pool they're in might be 
		 * invalid after closing this entry. */
		copy=apr_palloc(u_p_pool, strlen(utf8_name)+1);
		strcpy(copy, utf8_name);
		apr_hash_set(sts->user_prop, copy, APR_HASH_KEY_STRING, 
				svn_string_dup(value, u_p_pool) );

#ifdef ENABLE_DEBUG
		u_p_count++;
		u_p_bytes += value->len + sizeof(char*) + sizeof(*value);
		DEBUGP("%lu user-props stored, with %lu bytes.",
				u_p_count, u_p_bytes);
#endif
	}

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
	int chg;

	STOPIF( hlp__utf82local(utf8_path, &path, -1), NULL );

	STOPIF( ops__find_entry_byname(dir, path, &sts, 0), NULL);

	if (sts)
	{
		DEBUGP("deleting entry %s", path);

		ops__mark_parent_cc(sts, remote_status);
		STOPIF( cb__remove_from_url(sts, current_url, &chg), NULL);
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

	STOPIF( cb__add_entry(dir, utf8_path, NULL, utf8_copy_path, 
				copy_rev, S_IFDIR, &has_existed, 1,
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

	/** \todo conflict - removed locally? added */
	STOPIF( cb__add_entry(dir, utf8_path, NULL, NULL, 0, 
				S_IFDIR, NULL, 0, child_baton), NULL);

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___change_dir_prop(void *dir_baton,
		const char *utf8_name,
		const svn_string_t *value,
		apr_pool_t *pool)
{
	int status;

	/* We do this additional call to get a meaningful backtrace. */
	STOPIF( cb___store_prop(dir_baton, utf8_name, value, pool), NULL);

ex:
	RETURN_SVNERR(status);
}


int cb___close(struct estat *sts)
{
	int status;

	status=0;
	sts->repos_rev = cb___dest_rev;

	if (action->repos_feedback)
		STOPIF( action->repos_feedback(sts), NULL);

ex:
	return status;
}


svn_error_t *cb___close_directory(
		void *dir_baton,
		apr_pool_t *pool)
{
	struct estat *sts=dir_baton;
	int status;

	/* Release some memory; that was likely needed by cb__add_entry(), but is no
	 * longer. */
	IF_FREE(sts->by_name);

	STOPIF( cb___close(sts), NULL);

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

	/* Unless we get the svn:special property, we can assume that it's a 
	 * regular file. */
	STOPIF( cb__add_entry(dir, utf8_path, NULL, utf8_copy_path, 
				copy_rev, S_IFREG, NULL, 1, file_baton),
			NULL);
	sts=*file_baton;

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
	int was_there;

	/* Do we get an prop-del for "svn:special", if the entry reverts to being 
	 * a file?
	 *
	 * We don't get "svn:special" for an entry that's returned at the same 
	 * revision as we reported it, so we wouldn't know that it's eg. a 
	 * symlink.
	 *
	 * Keep the same type, unless we're being told otherwise.  */
	STOPIF( cb__add_entry(dir, utf8_path, NULL, NULL, 0,
				0, &was_there, 0, file_baton), NULL);
	sts=(struct estat*)*file_baton;

	/* Get the old value, so that we know what we had even if we don't get it 
	 * reported again. */
	if (was_there)
		STOPIF( up__fetch_decoder(sts), NULL);
	sts->decoder_is_correct=1;

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
		ops__mark_changed_parentcc(sts, remote_status);

	*handler = cb__txdelta_discard;
	*handler_baton=sts;

	RETURN_SVNERR(status);
}


svn_error_t *cb___change_file_prop(void *file_baton,
		const char *utf8_name,
		const svn_string_t *value,
		apr_pool_t *pool)
{
	int status;

	/* We do this additional call to get a meaningful backtrace. */
	STOPIF( cb___store_prop(file_baton, utf8_name, value, pool), NULL);

ex:
	RETURN_SVNERR(status);
}


svn_error_t *cb___close_file(void *file_baton,
		const char *text_checksum,
		apr_pool_t *pool)
{
	struct estat *sts=file_baton;
	int status;


	STOPIF( cb___close(sts), NULL);

	if (!S_ISDIR(sts->st.mode))
	{ 
		if (sts->has_orig_md5 || sts->decoder)
			DEBUGP("Has an original MD5, %s not used", text_checksum);
		else
			if (text_checksum)
				STOPIF( cs__char2md5(text_checksum, NULL, sts->md5 ), NULL);
	}

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

/** @} */


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

/** Helper function for cb__remove_from_url(). 
 *
 * Returns the highest-priority URL that's used by an entry below \a sts 
 * and which has a lower priority than \a to_remove in \a hp; this must be 
 * initialized to \c NULL before calling.  */
int cb___remover(struct estat *sts, struct url_t *to_remove,
		struct url_t **hp, int *has_changes)
{
	int status;
	struct estat **list;
	struct url_t *hp_url;
	int child_changes;

	status=0;
	DEBUGP("clean tree %s url %s", sts->name, to_remove->name);
	if (ops__has_children(sts))
	{
		hp_url=NULL;
		child_changes=0;
		list=sts->by_inode;

		while (*list)
		{
			STOPIF( cb___remover(*list, to_remove, &hp_url, &child_changes), NULL);
			list++;
		}

		if (sts->parent && hp_url)
		{
			/* It's an error if any child has a higher priority URL than the 
			 * parent, unless this gets removed now. */
			BUG_ON(sts->url != to_remove && url__sorter(hp_url, sts->url) < 0);

			sts->url=hp_url;
		}
	}

	if (!sts->parent)
	{
	}
	else
	{
		DEBUGP("entry %s has url %s", sts->name, sts->url->name);

		if (sts->url == to_remove)
		{
			DEBUGP("really removing");
			sts->remote_status=FS_REMOVED;
			ops__mark_changed_parentcc(sts, remote_status);

			*has_changes=1;
			if (action->repos_feedback)
				STOPIF( action->repos_feedback(sts), NULL);
		}
		else
		{
			if (!*hp)
				*hp=sts->url;
			else
				if (url__sorter(sts->url, *hp) < 0) *hp=sts->url;
			DEBUGP("New hp %s", (*hp)->name);
		}
	}

	if (child_changes)
		sts->remote_status |= FS_CHILD_CHANGED;

ex:
	return status;
}


/** -.
 * While recursion we look for the highest priority URL in the children 
 * (within each level); if there is one, we mark the directory as belonging 
 * to that URL.
 *
 * Will be easier with mixed-WC operation; currently it's not correct if 
 * there are overlayed non-directory entries.
 * */
int cb__remove_from_url(struct estat *root, struct url_t *to_remove, 
		int *was_changed)
{
	struct url_t *nevermind;
	int status;

	*was_changed=0;
	STOPIF( cb___remover(root, to_remove, &nevermind, was_changed), NULL);
	to_remove->current_rev=0;

ex:
	return status;
}


/** -. */
int cb__remove_url(struct estat *root, struct url_t *to_remove)
{
	int status;
	struct url_t *nevermind;
	int vvoid;

	STOPIF( cb___remover(root, to_remove, &nevermind, &vvoid), NULL);
	to_remove->current_rev=0;
	url__must_write_defs=1;

ex:
	return status;
}


/** -.
 * Just a proxy; calls cb__record_changes_mixed() with the \a root, \a target
 * and \a pool, and default values for the rest. */
int cb__record_changes(struct estat *root,
		svn_revnum_t target,
		apr_pool_t *pool)
{
	int status;

	STOPIF( cb__record_changes_mixed(root, target, 
				NULL, 0, pool), NULL);
ex:
	return status;
}


/** -.
 * Calls the svn libraries and records which entries would be changed
 * on this update on \c current_url.
 * \param root The root entry of this wc tree
 * \param target The target revision. \c SVN_INVALID_REVNUM is not valid.
 * \param other_paths A \c NULL-terminated list of paths that are sent to
 * the svn_ra_reporter2_t::set_path().
 * \param other_revs The revision to be sent for \a other_paths.
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
 *
 * Please note that it's not possible to run \e invisible entries (that are 
 * not seen because some higher priority URL overlays them) to run as \c 
 * baton==NULL (although that would save quite a bit of 
 * url__current_has_precedence() calls), because it's possible that some 
 * file in a directory below can be seen.
 *
 * \a other_paths is a \c NULL -terminated list of pathnames (which may 
 * have the \c "./" in front, ie. the \e normalized paths) that are to be 
 * reported at revision  \a other_revs.
 *
 * If \a other_paths is \c NULL, or doesn't include an <tt>"."</tt> entry, 
 * the WC root is reported to be at \c current_url->current_rev or, if this 
 * is \c 0, to be at \a target, but empty.
 * */
int cb__record_changes_mixed(struct estat *root,
		svn_revnum_t target,
		char *other_paths[], svn_revnum_t other_revs,
		apr_pool_t *pool)
{
	int status;
	svn_error_t *status_svn;
	void *report_baton;
	const svn_ra_reporter2_t *reporter;
	int sent_wcroot;
	char *cur, **op;


	status=0;
	cb___dest_rev=target;
	STOPIF_SVNERR( svn_ra_do_status,
			(current_url->session,
			 &reporter,
			 &report_baton,
			 "",
			 target,
			 TRUE,
			 &cb___change_recorder,
			 root,
			 pool) );

	sent_wcroot=0;
	cur=NULL;
	op=NULL;
	if (other_paths)
	{
		op=other_paths;
		while ( (cur=*op) )
		{
			if (cur[0] == '.' && cur[1] == 0)
				break;
			op++;
		}
	}

	/* If this is a checkout, we need to set the base directory at HEAD, but
	 * empty. We cannot use the base at revision 0, because it probably didn't
	 * exist there. */
	if (cur)
		STOPIF_SVNERR( reporter->set_path,
				(report_baton,
				 "", other_revs,
				 FALSE, NULL, pool));
	else if (current_url->current_rev == 0)
		STOPIF_SVNERR( reporter->set_path,
				(report_baton,
				 "", target,
				 TRUE, NULL, pool));
	else
		STOPIF_SVNERR( reporter->set_path,
				(report_baton,
				 "", current_url->current_rev,
				 FALSE, NULL, pool));

	if (other_paths)
	{
	/* The root entry must be the first to be reported (because of
		 * subversion/libsvn_repos/reporter.c).
		 * So we have to loop through the list - in case the user does
		 *   "fsvs diff file ."
		 * or something like that. */
		while ( (cur=*other_paths) )
		{
			/* cur loops through the entries, but *op is still set. */ 
			if (op != other_paths) 
			{
				DEBUGP("reporting %s@%llu", cur, (t_ull)other_revs);

				if (cur[0] == '.' && cur[1] == PATH_SEPARATOR) 
					cur+=2;

				STOPIF_SVNERR( reporter->set_path,
						(report_baton, cur, other_revs, FALSE, NULL, pool));
			}

			other_paths++;
		}
	}


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


/** -.
 * We need a valid revision number, \c SVN_INVALID_REVNUM (for \c HEAD) 
 * isn't. */
int cb__does_path_exist(svn_ra_session_t *session, 
		char *path, svn_revnum_t rev, 
		int *exists,
		apr_pool_t *pool)
{
	int status;
	svn_dirent_t *dirent;
	svn_error_t *status_svn;


	status=0;

	STOPIF_SVNERR( svn_ra_stat,
			(session, path, rev, &dirent, pool));
	*exists = dirent != NULL;

ex:
	return status;
}

