/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

/** \file
 * \ref update action.
 *
 * When we've loaded the "old" version from disk,
 * we fetch the new values from the repository.
 *
 * \todo Could be made a bit faster. Either with multiple threads, or
 * changing the subversion API to get all text-base changes in full-text.
 * For a small change fsvs could query whole new trees with an "old" update.
 * */

/** \addtogroup cmds
 *
 * \section update
 *
 * \code
 * fsvs update [-r rev] [working copy base]
 * fsvs update [-u url@rev ...] [working copy base]
 * \endcode
 *
 * This command does an update on all specified URLs for the current 
 * working copy, or, if none is given via \ref glob_opt_urls "-u", \b all 
 * URLs.
 *
 * It first reads all changes in the repositories, overlays them (so that
 * only the highest-priority entries are used), and fetches all necessary
 * changes.
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
#include "helper.h"
#include "url.h"
#include "status.h"
#include "racallback.h"
#include "props.h"
#include "checksum.h"
#include "revert.h"
#include "warnings.h"
#include "est_ops.h"
#include "waa.h"
#include "commit.h"
#include "racallback.h"



static char *filename,
						*filename_tmp=NULL;
static unsigned tmp_len=0;


/** Prefetch update-pipe property.
 * In case we're updating an existing file, we won't get \b all properties 
 * sent - only changed. So we have to look for existing properties <b>if we 
 * need them</b>.
 * */
int up__fetch_decoder(struct estat *sts)
{
	int status, st2;
	hash_t db;
	datum value;


	db=NULL;
	status=0;
	/* Need it, but don't have it? */
	if (!action->needs_decoder || 
			sts->decoder) goto ex;

	status=prp__open_byestat(sts, GDBM_READER, &db);
	if (status == ENOENT)
		status=0;
	else
	{
		STOPIF(status, NULL);

		/* Currently we don't need all properties - we just read the ones we 
		 * know we'll need. */

		if (prp__get(db, propval_updatepipe, &value) == 0)
		{
			sts->decoder=strdup(value.dptr);
			STOPIF_ENOMEM( !sts->decoder );
		}

	}

	ex:
	if (db)
	{
		st2=hsh__close(db, status);
		if (!status) STOPIF(st2, NULL);
	}
	return status;
}


/** -.
 *
 * If \a not_handled is not \c NULL, it gets set to \c 1 if this property 
 * is \b not handled; so the caller knows that he has to to write the 
 * property into some other storage if he wants to keep it.
 *
 * \note \a not_handled does \b not get set to \c 0; pre-populating is left 
 * to the caller.
 * */
int up__parse_prop(struct estat *sts, 
		const char *utf8_name, 
		const svn_string_t *utf8_value,
		int *not_handled,
		apr_pool_t *pool)
{
	char *cp, *loc_name, *loc_value;
	int i,status;
	apr_uid_t uid;
	apr_gid_t gid;
	apr_time_t at;
	svn_error_t *status_svn;


	/* We get the name and value in UTF8.
	 * For the currently used properties it makes no difference;
	 * but see doc/develop/UTF8. */
	/* We need the localized name only for debug and error messages;
	 * we still compare the utf8-name, and work with the utf8-data. */
	STOPIF( hlp__utf82local(utf8_name, &loc_name, -1), NULL);

	status=0;
	if (!utf8_value)
	{
		DEBUGP("got NULL property for %s: %s", sts->name, loc_name);
		//goto ex;
		loc_value=NULL;
	}
	else
	{
		STOPIF( hlp__utf82local(utf8_value->data, &loc_value, -1), NULL);
		DEBUGP("got property for %s: %s=%s",
				sts->name, loc_name, loc_value);
	}


	/* if an invalid utf8_value is detected, we'd better ignore it.
	 * who knows which Pandora's box we'd open ... */
	if (0 == strcmp(utf8_name, propname_owner))
	{
		/* for user and group we try to find the user name, and fall back
		 * to the uid. */
		i=strtoul(utf8_value->data, &cp, 0);
		if (cp == utf8_value->data)
			STOPIF( wa__warn(WRN__META_USER_INVALID, EINVAL,
						"cannot read uid in %s", loc_value),
					NULL);
		else
		{
			while (*cp && isspace(*cp)) cp++;
			if (*cp)
			{
				status=hlp__get_uid(cp, &uid, pool);
				if (status == APR_SUCCESS)
					i=uid;
				/* If not found, return no error to upper levels */
				status=0;
			}

			if (sts->st.uid != i)
			{
				sts->remote_status |= FS_META_OWNER;
				if (!action->is_compare)
				{
					sts->st.uid = i;
					DEBUGP("marking owner %s to %d", 
							loc_value, sts->st.uid);
				}
			}
		}
	}
	else if (0 == strcmp(utf8_name, propname_group))
	{
		i=strtoul(utf8_value->data, &cp, 0);
		if (cp == utf8_value->data)
			STOPIF( wa__warn(WRN__META_USER_INVALID, EINVAL,
						"cannot read gid in %s", loc_value),
					NULL);
		else
		{

			while (*cp && isspace(*cp)) cp++;
			if (*cp)
			{
				status=hlp__get_gid(cp, &gid, pool);
				if (status == APR_SUCCESS)
					i=gid;
				status=0;
			}

			if (sts->st.gid != i)
			{
				sts->remote_status |= FS_META_GROUP;
				if (!action->is_compare)
				{
					sts->st.gid = i;
					DEBUGP("marking group %s to %d", 
							loc_value, sts->st.gid);
				}
			}
		}
	}
	else if (0 == strcmp(utf8_name, propname_mtime))
	{
		status_svn=svn_time_from_cstring(&at, utf8_value->data, pool);
		if (status_svn)
			STOPIF( wa__warn(WRN__META_MTIME_INVALID, EINVAL,
						"modification time string invalid: %s", loc_value),
					NULL);
		else
		{
			if (sts->st.mtim.tv_sec != apr_time_sec(at) ||
					sts->st.mtim.tv_nsec != apr_time_usec(at) * 1000)
			{
				sts->remote_status |= FS_META_MTIME;

				if (!action->is_compare)
				{
					/* Currently deactivated. Seems to make more problems than the  
					 * reverse behaviour.  
					 * -- Take the newer of the two timestamps.  */
					//				if (apr_time_sec(at) >= sts->st.mtim.tv_sec)
					{
						sts->st.mtim.tv_sec=apr_time_sec(at);
						//					if (apr_time_usec(at)*1000 > sts->st.mtim.tv_nsec)
						sts->st.mtim.tv_nsec=apr_time_usec(at) * 1000;
					}
					DEBUGP("marking mtime \"%s\" to %24.24s", 
							loc_value,
							ctime(& (sts->st.mtim.tv_sec) ));
				}
			}
		}
	}
	else if (0 == strcmp(utf8_name, propname_umode))
	{
		i=strtoul(utf8_value->data, &cp, 0);
		if (*cp || i>07777)
			STOPIF( wa__warn(WRN__META_UMASK_INVALID, EINVAL,
						"no valid permissions found in %s", loc_value),
					NULL);
		else
		{
			if ((sts->st.mode & 07777) != i)
			{
				sts->remote_status |= FS_META_UMODE;
				if (!action->is_compare)
				{
					sts->st.mode = (sts->st.mode & ~07777) | i;
					DEBUGP("marking mode \"%s\" to 0%o", 
							loc_value, sts->st.mode & 07777);
				}
			}
		}
	}
	else if (0 == strcmp(utf8_name, propname_special) &&
			0 == strcmp(utf8_value->data, propval_special))
	{
		if (S_ISANYSPECIAL(sts->st.mode))
		{
			DEBUGP("already marked as special");
		}
		else
		{
			/* Remove any S_IFDIR and similar bits. */
			if (! (S_ISLNK(sts->updated_mode) ||
						S_ISCHR(sts->updated_mode) ||
						S_ISBLK(sts->updated_mode)) )
				sts->updated_mode = sts->st.mode = 
					(sts->st.mode & 07777) | S_IFANYSPECIAL;
			DEBUGP("this is a special node");
		}
	}
	else if (0 == strcmp(utf8_name, propname_origmd5))
	{
		/* Depending on the order of the properties we might not know whether 
		 * this is a special node or a regular file; so we only disallow that 
		 * for directories. */
		BUG_ON(S_ISDIR(sts->updated_mode));
		STOPIF( cs__char2md5( utf8_value->data, sts->md5), NULL);
		DEBUGP("got a orig-md5: %s", cs__md52hex(sts->md5));
		sts->has_orig_md5=1;
	}
	else
	{
		if (strcmp(utf8_name, propval_updatepipe) == 0)
		{
			if (action->needs_decoder)
			{
				/* Currently we assume that programs (update- and commit-pipe) are 
				 * valid regardless of codeset; that wouldn't work as soon as the 
				 * programs' names includes UTF-8.
				 *
				 * \todo utf8->local??  */
				sts->decoder=strdup(utf8_value->data);
				STOPIF_ENOMEM( !sts->decoder );
				sts->decoder_is_correct=1;
				DEBUGP("got a decoder: %s", sts->decoder);
			}
		}

		/* Ignore svn:entry:* properties, but store the updatepipe, too. */
		if (!hlp__is_special_property_name(utf8_name))
		{
			sts->remote_status |= FS_PROPERTIES;

			DEBUGP("property %s: %s=%s", 
					sts->name,
					loc_name, loc_value);
			if (not_handled) *not_handled=1;
		}
	}


ex:
	return status;

#if 0
inval:
	/* possibly have a switch to just warn, but ignore invalid
	 * values, so that an emergency can be 99% handled */
	/* print path in case of errors? */
	status=ops__build_path(&cp, sts);
	STOPIF(EINVAL, "incorrect utf8_value for property %s ignored: "
			"file %s: \"%s\"",
			name, 
			status == 0 ? cp : sts->name, 
			utf8_value->data);
#endif
}


/** -.
 * Remove a (non-dir) file.
 * Must return errors silently. */
int up__unlink(struct estat *sts, char *filename)
{
	int status;


	status=0;
	if (!filename)
		STOPIF( ops__build_path(&filename, sts), NULL );

	/* If file has changed, we bail out */
	if (sts->entry_status & FS_CHANGED)
		STOPIF_CODE_ERR(1, EBUSY, 
				"File %s has been changed - won't remove", filename);

	if (unlink(filename) == -1)
	{
		status=errno;
		/* If it does not exist any more - should we warn?? */
		if (status == ENOENT) status=0;
	}
	else
	{
		STOPIF( waa__delete_byext(filename, WAA__FILE_MD5s_EXT, 1), NULL);
		STOPIF( waa__delete_byext(filename, WAA__PROP_EXT, 1), NULL);
	}

	DEBUGP("unlink(%s)", filename);

ex:
	return status;
}


/** Recursively delete a directory structure.
 * Only non-changed, known entries, so we don't
 * remove changed data.
 *
 * If an entry does not exist (ENOENT), it is ignored.
 *
 * Only entries that are registered from \a url are removed.
 *
 * If children that belong to other URLs are found we don't remove the 
 * directory.
 *
 * \todo conflict */
int up__rmdir(struct estat *sts, struct url_t *url)
{
	int status, i, has_others;
	struct estat *cur;
	char *path;


	status=0;
	has_others=0;

	/* Remove children */
	for(i=0; i<sts->entry_count; i++)
	{
		cur=sts->by_inode[i];

		if (url && cur->url != url)
			has_others++;
		else
		{
			/* TODO: is that true any more? */
			/* Checking the contents of sts here is not allowed any more -
			 * it may (eg. on update) already contain newer data, and that can be
			 * anything -- a file, a link, ... */
			/* Just trying the unlink is a single system call, like getting the 
			 * type of the entry with \c lstat(). */
			status=up__unlink(cur, NULL);
			if (status == EISDIR)
				status=up__rmdir(cur, url);

			STOPIF( status, "unlink of %s failed", cur->name);
		}
	}

	if (!has_others)
	{
		STOPIF( ops__build_path(&path, sts), NULL );
		status = rmdir(path) == -1 ? errno : 0;

		DEBUGP("removing %s: %d", path, status);
		if (status == ENOENT) status=0;
		STOPIF( status, "Cannot remove directory %s", path);
	}

ex:
	return status;
}


/* The file has current properties, which we'd like
 * to replace with the saved.
 * But all not-set properties should not be modified. 
 *
 * And all settings should be saved in the waa-area
 * _with the current values_, so that this entry won't
 * be seen as modified.
 *
 * The easy way:
 * 	 We set what we can, and do a stat() afterwards
 * 	 to capture the current setting.
 * 	 This has a small race condition. 
 * 	 If another process changes the meta-data _after_ setting and
 * 	 _before_ querying, we don't see that it was changed.
 *
 * How it was done:
 *   We store a copy of the wanted things, and copy what we set.
 *   So there's no race-condition, except that we change meta-data
 *   a process has just changed. */
/* Since svn 1.3.0 we no longer get all properties on an update,
 * only these that are different to the reported version.
 * That means that most times we'll get only the mtime as changed.
 *
 * Now, if the file has a unix-mode other than 0600 or an owner 
 * which is not equal to the current user, we wouldn't set that
 * because the change mask didn't tell to.
 * So the file would retain the values of the temporary file, which are
 * 0600 and the current user and group.
 *
 * The new strategy is: write all values.
 * If there are no properties set for a file, we'll just
 * write the values it currently has - so no problem. */
/* With one exception: the ctime will change, and so we'll believe that
 * it has changed next time. So fetch the *real* values afterwards. 
 *
 * Meta-data-only changes happen too often, see this case:
 * - We're at rev N.
 * - We commit an entry and get rev M *for this entry*. The directory still 
 *   has N, because there might be other new entries in-between.
 * - We want to update to T.
 * - We're sending to subversion "directory is at N",
 * - "file is at M",
 * - and we get back "file has changed, properties ...  
 *   [svn:entry:committed-rev = M]".
 *
 * So we're saying we have file at M, and get back "changed, last change 
 * happened at M".
 * (Will file a bug report.)
 *
 * So we get a meta-data change, update the meta-data (currently - will 
 * change that soon), and have another ctime (but don't update the entries' 
 * meta-data), so find the entry as changed ... 
 *
 * Current solution: read back the entries' meta-data after changing it. */
/* Another thought - if we have different meta-data locally, that's 
 * possibly something worth preserving. If the owner has changed in the 
 * repository *and* locally, we'd have to flag a conflict!
 * Furthermore the root entry gets no properties, so it gets set to owner 
 * 0.0, mode 0600 ... which is not right either. */
int up__set_meta_data(struct estat *sts,
		char *filename)
{
	struct timeval tv[2];
	int status;


	status=0;

	if (!filename)
		STOPIF( ops__build_path(&filename, sts), NULL );

	/* A chmod or utimes on a symlink changes the *target*, not
	 * the symlink itself. Don't do that. */
	if (!S_ISLNK(sts->updated_mode))
	{
		/* We have a small problem here, in that we cannot change *only* the user 
		 * or group. It doesn't matter much; the problem case is that the owner 
		 * has changed locally, the repository gives us another group, and we 
		 * overwrite the owner. But still: TODO */
		if (sts->remote_status & (FS_META_OWNER | FS_META_GROUP))
		{
			DEBUGP("setting %s to %d.%d",
					filename, sts->st.uid, sts->st.gid);
			status=chown(filename, sts->st.uid, sts->st.gid);
			if (status == -1)
			{
				STOPIF( wa__warn( errno==EPERM ? WRN__CHOWN_EPERM : WRN__CHOWN_OTHER,
							errno, "Cannot chown \"%s\" to %d:%d", 
							filename, sts->st.uid, sts->st.gid),
						NULL );
			}
		}

		if (sts->remote_status & FS_META_UMODE)
		{
			/* The mode must be set after user/group.
			 * If the entry has 07000 bits set (SGID, SUID, sticky),
			 * they'd disappear after chown(). */
			DEBUGP("setting %s's mode to 0%o", 
					filename, sts->st.mode);
			status=chmod(filename, sts->st.mode & 07777);
			if (status == -1)
			{
				STOPIF( wa__warn( errno == EPERM ? WRN__CHMOD_EPERM : WRN__CHMOD_OTHER, 
							errno, "Cannot chmod \"%s\" to 0%3o", 
							filename, sts->st.mode & 07777 ), 
						NULL );
			}
		}


		if (sts->remote_status & FS_META_MTIME)
		{
			/* index 1 is mtime */
			tv[1].tv_sec =sts->st.mtim.tv_sec;
			tv[1].tv_usec=sts->st.mtim.tv_nsec/1000;
			/* index 0 is atime.
			 * It's not entirely correct that we set atime to mtime here,
			 * but the atime is a volatile thing anyway ... */
			tv[0].tv_sec =sts->st.mtim.tv_sec;
			tv[0].tv_usec=sts->st.mtim.tv_nsec/1000;
			DEBUGP("setting %s's mtime %24.24s", 
					filename, ctime(& (sts->st.mtim.tv_sec) ));
			STOPIF_CODE_ERR( utimes(filename, tv) == -1,
					errno, "utimes(%s)", filename);
		}

		STOPIF( hlp__lstat(filename, & sts->st), NULL);
	}

ex:
	return status;
}


/** Handling non-file non-directory entries.
 * We know it's a special file, but not more; we have to take the filedata 
 * and retrieve the type. */
int up__handle_special(struct estat *sts, 
		char *path,
		char *data,
		apr_pool_t *pool)
{
	int status;
	char *cp;

	STOPIF( ops__string_to_dev(sts, data, &cp), NULL);
	STOPIF( hlp__utf82local(cp, &cp, -1), NULL);

	sts->stringbuf_tgt=NULL;
	DEBUGP("special %s has mode 0%o", path, sts->updated_mode);

	/* process */
	switch (sts->updated_mode & S_IFMT)
	{
		case S_IFBLK:
		case S_IFCHR:
			STOPIF_CODE_ERR( mknod(path, sts->st.mode, sts->st.rdev) == -1,
					errno, "mknod(%s)", path) ;
			break;

		case S_IFLNK:
			STOPIF_CODE_ERR( symlink(cp, path) == -1, 
					errno, "symlink(%s, %s)", cp, path);
			break;

		default:
			STOPIF_CODE_ERR(1, EINVAL, 
					"what kind of node is this??? (mode=0%o)", sts->updated_mode);
	}

ex:
	return status;
}


/* ---CUT--- here are the delta-editor functions */
svn_error_t *up__set_target_revision(void *edit_baton,
		svn_revnum_t rev,
		apr_pool_t *pool)
{
	struct estat *sts=edit_baton;
	int status;


	status=0;
	/* It makes no sense to set all members to the new revision - 
	 * we may get new ones, and they wouldn't be set.
	 * So do the whole tree at the end. */
	target_revision=rev;
	sts->repos_rev=rev;

	RETURN_SVNERR(status);
}


svn_error_t *up__open_root(void *edit_baton,
		svn_revnum_t base_revision,
		apr_pool_t *dir_pool UNUSED,
		void **root_baton)
{
	struct estat *sts=edit_baton;

	sts->repos_rev=base_revision;
	*root_baton=sts;

	return SVN_NO_ERROR; 
}


svn_error_t *up__add_directory(const char *utf8_path,
		void *parent_baton,
		const char *utf8_copy_path,
		svn_revnum_t copy_rev,
		apr_pool_t *dir_pool UNUSED,
		void **child_baton)
{
	struct estat *dir=parent_baton;
	struct estat *sts;
	int status;
	char* path;


	STOPIF( cb__add_entry(dir, utf8_path, &path, utf8_copy_path, 
				copy_rev, S_IFDIR, NULL, 1,
				child_baton), NULL );
	sts=(struct estat*)*child_baton;

	if (!action->is_compare)
	{
		/* this must be done immediately, because subsequent accesses may
		 * try to add sub-entries. */
		/* 0700 until overridden by property */
		STOPIF_CODE_ERR( mkdir(path, 0700) == -1, errno, 
				"mkdir(%s)", path);

		/* pre-fill data */
		STOPIF( hlp__lstat(path, &(sts->st)),
				"lstat(%s)", path); 
	}

	status=0;

ex:
	RETURN_SVNERR(status);
}



svn_error_t *up__change_dir_prop(void *dir_baton,
		const char *utf8_name,
		const svn_string_t *value,
		apr_pool_t *pool)
{
	struct estat *sts=dir_baton;
	int status;

	status=0;
	if (!sts->url || url__current_has_precedence(sts->url))
		STOPIF( up__parse_prop(sts, utf8_name, value, NULL, pool), NULL);

ex:
	RETURN_SVNERR(status);
}


svn_error_t *up__close_directory(
		void *dir_baton,
		apr_pool_t *pool)
{
	struct estat *sts=dir_baton;
	int status;


	STOPIF( ops__build_path(&filename, sts), NULL);
	/* set meta-data */
	STOPIF( up__set_meta_data(sts, filename), NULL);
	/* set correct values */
	STOPIF( hlp__lstat( filename, &(sts->st)),
			"Cannot lstat('%s')", filename);
	/* finished, report to user */
	STOPIF( st__status(sts), NULL);

	/* Mark this directory for being checked next time. */
	sts->flags |= RF_CHECK;

ex:
	RETURN_SVNERR(status);
}


/// FSVS GCOV MARK: up__absent_directory should not be executed
svn_error_t *up__absent_directory(const char *utf8_path,
		void *parent_baton,
		apr_pool_t *pool)
{
	struct estat *dir UNUSED =parent_baton;

	DEBUGP("in %s", __PRETTY_FUNCTION__);

	return SVN_NO_ERROR; 
}


svn_error_t *up__add_file(const char *utf8_path,
		void *parent_baton,
		const char *utf8_copy_path,
		svn_revnum_t copy_rev,
		apr_pool_t *file_pool,
		void **file_baton)
{
	struct estat *dir=parent_baton;
	int status;

	STOPIF( cb__add_entry(dir, utf8_path, NULL, utf8_copy_path, copy_rev,
				S_IFREG, NULL, 1, file_baton), NULL);

ex:
	RETURN_SVNERR(status);
}


svn_error_t *up__apply_textdelta(void *file_baton,
		const char *base_checksum,
		apr_pool_t *pool,
		svn_txdelta_window_handler_t *handler,
		void **handler_baton)
{
	struct estat *sts=file_baton;
	svn_stream_t *svn_s_src, *svn_s_tgt;
	int status;
	char *cp;
	char* fn_utf8;
	apr_file_t *source, *target;
	struct encoder_t *encoder;
	svn_stringbuf_t *stringbuf_src;


	stringbuf_src=NULL;
	encoder=NULL;
	STOPIF( ops__build_path(&filename, sts), NULL);

	if (action->is_compare) 
	{
		/* svn_stringbuf_create from a NULL pointer doesn't work - 
		 * we have to initialize it. */
		cp="";
		goto into_stringbufs;
	}

	STOPIF_CODE_ERR( sts->entry_status & FS_CHANGED, EBUSY, 
			"file '%s' was changed locally and cannot be updated", filename);
#if 0
	/* If an entry was removed, and an update is issued - 
	 * should we restore the entry, or should we not? 
	 * We do that, because the repository says it should be here. */
	STOPIF_CODE_ERR( sts->entry_status & FS_REMOVED, ENOENT, 
			"file '%s' was deleted locally", filename);
#endif

	status=strlen(filename)+10;
	if (status > tmp_len)
	{
		/* round to next kB */
		status= (status+1024) & ~(1024-1);
		filename_tmp=realloc(filename_tmp, status);
		STOPIF_ENOMEM(!filename_tmp);

		tmp_len=status;
	}

	strcpy(filename_tmp, filename);
	strcat(filename_tmp, ".up.tmp");

	DEBUGP("target is %s (0%o),", filename, sts->updated_mode);
	DEBUGP("  temp is %s", filename_tmp);

	if (!S_ISREG(sts->updated_mode))
	{
		/* special entries are taken into a svn_stringbuf_t */
		if (S_ISLNK(sts->updated_mode))
		{
			STOPIF( ops__link_to_string(sts, filename, &cp),
					NULL);
			STOPIF( hlp__local2utf8(cp, &cp, -1), NULL);
		}
		else
			cp=ops__dev_to_filedata(sts);

into_stringbufs:
		stringbuf_src=svn_stringbuf_create(cp, pool);
		sts->stringbuf_tgt=svn_stringbuf_create("", pool);

		svn_s_src=svn_stream_from_stringbuf(stringbuf_src, pool);
		svn_s_tgt=svn_stream_from_stringbuf(sts->stringbuf_tgt, pool);
		status=0;
	}
	else
	{
		/** \anchor FHP File handle pools.
		 *
		 * This is a bit complicated.
		 *
		 * With the file:/// protocol, the source and destination 
		 * filehandles are not closed by the subversion libraries; 
		 * with svn+ssh:/// they are.
		 *
		 * If we just do a apr_file_close(), we get the error EBADF 
		 * (bad filehandle), and would accordingly die.
		 *
		 * If we don't do it (and let apr_pool_cleanup close it), the
		 * close may just fall into the next second, and our 
		 * (in up__close_file) cached ctime is wrong - so we'd mark this
		 * entry as changed.
		 *
		 * One solution would be to do a apr_file_close(), and ignore EBADF;
		 * this is a bit unclean.
		 *
		 * So we go the other route: we simply define a subpool, where we 
		 * allocate the handles in, and clear that later.
		 * That has the additional advantage that the struct estat could 
		 * possibly be shrinked in the future. */
		/* Please note that for svn+ssh the pool given to this function cannot
		 * be used, as this is already destroyed by the time we get to
		 * up__close_file, and an apr_pool_clear() then results in a segfault.
		 * So we have to take the directories' pool. */
		/* We take a subpool of the global pool; that takes (tested) nearly  
		 * resources, as it's destroyed in close_file(). */
		STOPIF( apr_pool_create(&(sts->filehandle_pool), global_pool),
				"Creating the filehandle pool");

		/* If the file is new, has changed or is removed, 
		 * we should get full-text, ie. a delta against the empty file. */
		STOPIF( apr_file_open(&source, 
					(sts->remote_status & (FS_NEW|FS_CHANGED|FS_REMOVED)) ? 
					"/dev/null" : filename, 
					APR_READ, 0, sts->filehandle_pool),
				NULL);

		/* Mode, owner etc. will be done at file_close.
		 * We read if it's something special. */
		STOPIF( apr_file_open(&target, filename_tmp, 
					APR_WRITE | APR_CREATE | APR_TRUNCATE,
					APR_UREAD | APR_UWRITE, sts->filehandle_pool),
				NULL);

		svn_s_src=svn_stream_from_aprfile(source, sts->filehandle_pool);
		svn_s_tgt=svn_stream_from_aprfile(target, sts->filehandle_pool);

		/* How do we get the filesize here? */
		if (!action->is_import_export)
			STOPIF( cs__new_manber_filter(sts, svn_s_tgt, &svn_s_tgt, 
						sts->filehandle_pool),
					NULL);

		if (sts->decoder)
		{
			STOPIF( hlp__encode_filter(svn_s_tgt, sts->decoder, 1, 
						filename, &svn_s_tgt, &encoder, sts->filehandle_pool), NULL);
			/* If the file gets decoded, use the original MD5 for comparision. */
			encoder->output_md5= &(sts->md5);
		}
	}

	STOPIF( hlp__local2utf8(filename, &fn_utf8, -1), NULL );
	svn_txdelta_apply(svn_s_src, svn_s_tgt, 
			action->is_compare ? NULL : sts->md5, 
			fn_utf8, pool,
			handler, handler_baton);


	sts->remote_status |= FS_CHANGED;

ex:
	RETURN_SVNERR(status);
}


svn_error_t *up__change_file_prop(void *file_baton,
		const char *utf8_name,
		const svn_string_t *value,
		apr_pool_t *pool)
{
	struct estat *sts=file_baton;
	int status;

	status=0;
	if (!sts->url || url__current_has_precedence(sts->url))
		STOPIF( up__parse_prop(sts, utf8_name, value, NULL, pool), NULL);

	/* Ah yes, the famous "late property" sketch ... */
	BUG_ON(sts->remote_status & FS_CHANGED,
			"Entry has already been fetched, properties too late!");
ex:
	RETURN_SVNERR(status);
}


svn_error_t *up__close_file(void *file_baton,
		const char *text_checksum,
		apr_pool_t *pool)
{
	struct estat *sts=file_baton;
	int status;

	if (action->is_compare && text_checksum)
	{
		if (memcmp(text_checksum, sts->md5, sizeof(sts->md5)) != 0)
			sts->remote_status |= FS_CHANGED;
	}
	else
	{
		/* now we have a new md5 */
		DEBUGP("close file (0%o): md5=%s",
				sts->updated_mode, cs__md52hex(sts->md5));


		BUG_ON(!sts->updated_mode);

		if (S_ISREG(sts->updated_mode))
		{
			status=0;
			/* See the comment mark FHP. */
			/* This may be NULL if we got only property-changes, no file
			 * data changes. */
			if (sts->filehandle_pool)
				apr_pool_clear(sts->filehandle_pool);
			sts->filehandle_pool=NULL;
			/* Now the filehandles should be closed. */
			/* This close() before rename() is necessary to find out 
			 * if all data has been written (out of disk-space, etc).
			 * Sadly we can't check for errors. */
		}
		else
		{
			DEBUGP("closing special file");
			sts->stringbuf_tgt->data[ sts->stringbuf_tgt->len ]=0;
			STOPIF( up__handle_special(sts, filename_tmp, 
						sts->stringbuf_tgt->data, pool), NULL);
		}


		/* set meta-data */
		STOPIF( up__set_meta_data(sts, filename_tmp), NULL);

		/* rename to correct filename */
		STOPIF_CODE_ERR( rename(filename_tmp, filename)==-1, errno,
				"Cannot rename '%s' to '%s'", filename_tmp, filename);

		/* The rename changes the ctime. */
		STOPIF( hlp__lstat( filename, &(sts->st)),
				"Cannot lstat('%s')", filename);
	}

	/* finished, report to user */
	STOPIF( st__status(sts), NULL);

ex:
	RETURN_SVNERR(status);
}


/// FSVS GCOV MARK: up__absent_file should not be executed
svn_error_t *up__absent_file(const char *utf8_path,
		void *parent_baton,
		apr_pool_t *pool)
{
	struct estat *dir UNUSED=parent_baton;

	DEBUGP("in %s", __PRETTY_FUNCTION__);
	return SVN_NO_ERROR; 
}


svn_error_t *up__close_edit(void *edit_baton, 
		apr_pool_t *pool)
{
	struct estat *sts UNUSED=edit_baton;

	return SVN_NO_ERROR; 
}


/// FSVS GCOV MARK: up__abort_edit should not be executed
svn_error_t *up__abort_edit(void *edit_baton,
		apr_pool_t *pool)
{
	struct estat *sts UNUSED=edit_baton;

	return SVN_NO_ERROR; 
}

/* ---CUT--- end of delta-editor */


/* For locally changed files we have to tell the RA layer
 * that we don't have the original text, so that we get the full 
 * text instead of a delta. */
int ac___up_set_paths(struct estat *dir, 
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
		if (S_ISDIR(sts->st.mode))
			STOPIF( ac___up_set_paths(sts, reporter, report_baton, pool),
					NULL);
		else
			if (sts->entry_status & (FS_CHANGED | FS_REMOVED))
			{
				STOPIF( ops__build_path(&fn, sts), NULL );
				DEBUGP("  changed: %s", fn);
				/* Again, we have to cut the "./" in front ... */
				STOPIF_SVNERR( reporter->delete_path,
						(report_baton, fn+2, pool));
			}
	}

ex:
	return status;
}


/** Main update action.
 *
 * We do most of the setup before checking the whole tree. 
 *
 * Please note that this is not atomic - use unionfs. */
int up__work(struct estat *root, int argc, char *argv[])
{
	int status;
	svn_error_t *status_svn;
	svn_revnum_t rev;
	time_t delay_start;


	status=0;
	status_svn=NULL;
	STOPIF( waa__find_base(root, &argc, &argv), NULL);

	STOPIF( url__load_nonempty_list(NULL, 0), NULL);

	STOPIF_CODE_ERR(!urllist_count, EINVAL,
			"There's no URL defined");

	STOPIF( url__mark_todo(), NULL);

	STOPIF_CODE_ERR( argc != 0, EINVAL,
			"Cannot do partial updates!");


	opt__set_int(OPT__CHANGECHECK, PRIO_MUSTHAVE,
			opt__get_int(OPT__CHANGECHECK) | CHCHECK_FILE);

	only_check_status=1;
	/* Do that here - if some other checks fail, it won't take so long
	 * to notice the user */ 
	STOPIF( waa__read_or_build_tree(root, argc, argv, argv, NULL, 0), NULL);
	only_check_status=0;

	while ( ! ( status=url__iterator(&rev) ) )
	{
		STOPIF( cb__record_changes(root, rev, global_pool), NULL);

		if (action->is_compare)
		{
			/* This is for remote-status. Just nothing to be done. */
			printf("Remote-status against revision\t%ld.\n", rev);
		}
		else
		{
			/* set new revision */
			DEBUGP("setting revision to %llu", (t_ull)rev);
			STOPIF( ci__set_revision(root, rev), NULL);

			printf("Updating %s to revision\t%ld.\n", 
					current_url->url, rev);
		}
	}
	STOPIF_CODE_ERR( status != EOF, status, NULL);
	status=0;

	if (action->is_compare)
	{
	}
	else
	{
		DEBUGP("fetching from repository");
		STOPIF( rev__do_changed(root, global_pool), NULL);

		/* See the comment at the end of commit.c - atomicity for writing
		 * these files. */
		delay_start=time(NULL);
		STOPIF( waa__output_tree(root), NULL);
		STOPIF( url__output_list(), NULL);
		STOPIF( hlp__delay(delay_start, DELAY_UPDATE), NULL);
	}


ex:
	STOP_HANDLE_SVNERR(status_svn);
ex2:
	return status;
}


/* 
 * The problem with update is this.
 * - We need to check the working copy for changes.
 *   We have to do that to tell the svn layer which files to give us in full,
 *   as we won't do anything with a delta stream (we don't have the common 
 *   ancestor).
 * - We don't need to know about new local entries; if we stored them,
 *   we'd need to filter them out on waa__output_tree().
 *     (If we didn't filter them, they'd show up as already committed - so
 *     we'd loose them for the next commit.)
 *   And whether we do a getdents() while reading the directories or an
 *   lstat() before writing doesn't matter that much.
 * - If we just did the tree update without new local files and write that
 *   as current version in the WAA, we wouldn't find new entries that were
 *   done *before* the update - the parent directories' time stamp would
 *   be stored as the update time, and so we'd believe it to be unchanged.
 *
 * So what we do is
 * - we read the tree, but
 * - don't accept new local entries;
 * - directories that showed up as changed *before* the update get the
 *   RF_CHECK flag set on up__open_directory(), so that they get read 
 *   on the next operations, too.
 *
 */
