/************************************************************************
 * Copyright (C) 2007-2009,2015 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/


/** \file
 * Properties handling - \ref prop-get, \ref prop-set, \ref prop-list.
 *
 * Deleted properties are marked locally be a special value; callers need 
 * to check by using prp__prop_will_be_removed().
 *
 * \todo --xml, --raw, --dump switches?
 *
 * \todo should \ref prop-get and \ref prop-list use UTF-8 or local 
 * encoding? Currently the names and values are dumped as-is, ie. UTF-8.
 * */

#include <sys/types.h>
#include <limits.h>
#include <gdbm.h>
#include <fcntl.h>
#include <subversion-1/svn_props.h>

#include "global.h"
#include "waa.h"
#include "url.h"
#include "helper.h"
#include "actions.h"
#include "hash_ops.h"
#include "props.h"
#include "update.h"
#include "ignore.h"
#include "est_ops.h"
#include "add_unvers.h"
#include "warnings.h"


/** \defgroup props Property handling
 * \ingroup compati
 *
 * We take and give arbitrary properties to the subversion layers; a few 
 * are used directly, see \ref s_p_n "special property names" for details.
 *
 * All other properties are stored <b>with a terminating NUL</b> by the 
 * hash layer; see hsh__new().
 *
 * */


/** \addtogroup cmds
 *
 * \section prop-get
 *
 * \code
 * fsvs prop-get PROPERTY-NAME PATH...
 * \endcode
 *
 * Prints the data of the given property to \c STDOUT.
 *
 * \note Be careful! This command will dump the property <b>as it is</b>,
 * ie. with any special characters! If there are escape sequences or binary 
 * data in the property, your terminal might get messed up! \n
 * If you want a safe way to look at the properties, use prop-list with the 
 * \c -v parameter.
 */

 /* TODO: If you use \c - as the value, the value is read from \c STDIN. */
/** \addtogroup cmds
 *
 * \section prop-set
 *
 * \code
 * fsvs prop-set [-u URLNAME] PROPERTY-NAME VALUE PATH...
 * \endcode
 *
 * This command sets an arbitrary property value for the given path(s).
 *
 * \note Some property prefixes are reserved; currently everything starting 
 * with <tt>svn:</tt> throws a (fatal) warning, and <tt>fsvs:</tt> is 
 * already used, too. See \ref s_p_n.
 *
 * If you're using a multi-URL setup, and the entry you'd like to work on 
 * should be pinned to a specific URL, you can use the \c -u parameter; 
 * this is like the \ref add "add" command, see there for more details.
 * 
 * */

/** \addtogroup cmds
 *
 * \section prop-del
 *
 * \code
 * fsvs prop-del PROPERTY-NAME PATH...
 * \endcode
 *
 * This command removes a property for the given path(s).
 *
 * See also \ref prop-set.
 *
 * */

/** \addtogroup cmds
 *
 * \section prop-list
 *
 * \code
 * fsvs prop-list [-v] PATH...
 * \endcode
 *
 * Lists the names of all properties for the given entry. \n
 * With \c -v, the value is printed as well; special characters will be
 * translated, as arbitrary binary sequences could interfere with your 
 * terminal settings.
 *
 * If you need raw output, post a patch for \c --raw, or write a loop with  
 * \ref prop-get "prop-get".
 *
 * */


/** \defgroup s_p_n Special property names
 * \ingroup userdoc
 *
 * \section fsvs_props Special FSVS properties. 
 *
 * These are used \b only by \c FSVS; \c subversion doesn't know them.
 * @{ */
/** The common prefix. */
#define FSVS_PROP_PREFIX "fsvs:"

/** The name for the commit-pipe property. 
 *
 * If this property is set for a file, this file gets filtered by the given 
 * command on its way to the repository. This is mostly used for backups, 
 * to protect data.
 * 
 * To make that easier to understand, here's an example.
 * You're versioning your \c etc:
 * \code
 *     cd /etc
 *     fsvs urls <your repository url>
 * \endcode
 * That means that \c /etc/shadow, \c /etc/ssh/ssh_host_key and so on would  
 * all get transmitted to the repository.
 * Now we could say that if the machine crashes hard, a changed ssh-key is 
 * the least of our worries - so we simply exclude it from backup.
 * \code
 *     fsvs ignore './ssh/ssh_host_*key'
 * \endcode
 * But the users' passwords and similar should not be lost - so we use \c 
 * gpg to encrypt them on backup. You generate a key, whose private key 
 * <b>gets kept in a secure place, but not (only) on this machine</b>; 
 * because if the machine gets damaged, the backups could no longer be 
 * decrypted.
 * \note If the key is on this machine, and it gets hacked, your backups 
 * might be read!
 * \code
 *     gpg --import-key  .....
 *     fsvs propset fsvs:commit-pipe 'gpg -er <backup-key>' shadow
 * \endcode
 * You might want/need to set an update-pipe, too; see \ref 
 * FSVS_PROP_UPDATE_PIPE for details.
 *
 * The only thing left is to take the first backup:
 * \code
 *     fsvs commit
 * \endcode
 *
 * \note Currently only files can use this property. Would it make sense 
 * for devices or symlinks too? Currently not, as the only way to send 
 * these into the repository is changing the major/minor number - which is 
 * not possible with normal files.\n If we instead sent the whole data, we 
 * could encrypt a filesystem into the repository - but that would get no 
 * delta-transfers, and deltification only if not CBC ...\n Sending the 
 * fsvs generated string "cdev:x:x" for encryption wouldn't help; so if 
 * such special files must be processed, we'd might need to make a \e raw 
 * pipe - which does no interpreting.\n Ideas welcome.
 *
 * \note Encrypted data cannot be deltified, so the few marked files will 
 * take their full space in the repository. (Although \c gpg compresses the 
 * files before encryption, so it won't be \b that bad.)
 *
 * You might be interested in
 * \ref exp_env "exported environment variables", too.
 *
 * \note Another idea is to ignore files that are not readable by everyone; 
 * see \ref ign_mod "ignore pattern modifiers" for details.
 * */
#define FSVS_PROP_COMMIT_PIPE FSVS_PROP_PREFIX "commit-pipe"

/** The name of the update-pipe property.
 *
 * This is the reverse thing to \ref FSVS_PROP_COMMIT_PIPE; it's used in 
 * the same way.
 *
 * Extending the example before:
 * \code
 *     fsvs propset fsvs:commit-pipe 'gpg -er <backup-key>' shadow
 *     fsvs propset fsvs:update-pipe 'gpg -d' shadow
 * \endcode
 *
 * \note This command is used for \ref revert, \ref diff, and \ref export, 
 * too.
 *
 * */
#define FSVS_PROP_UPDATE_PIPE FSVS_PROP_PREFIX "update-pipe"

/** Local install commandline.
 *
 * \note This is not yet implemented. This list is more or less just a kind 
 * of brainstorming. If you need this feature, tell me.
 *
 * This is used after the temporary file (which had possibly used \ref 
 * FSVS_PROP_UPDATE_PIPE) has been written; the normal, internal fsvs 
 * operation is approximately this:
 * - File gets piped through \ref FSVS_PROP_UPDATE_PIPE into a temporary 
 *   file, which was created with mask \c 0700.
 * - <tt>chmod $m $tmp</tt> <i> - set the stored access mode</i>.
 * - <tt>chown $u.$g $tmp || chown $U.$G $tmp</tt> <i> - set user and group 
 *   by the stored strings, and if that fails, by the uid and gid</i>.
 * - <tt>touch -t$t $tmp</tt> <i> - set the stored access mode</i>.
 * - <tt>mv $tmp $dest</tt> <i>rename to destination name</i>.
 *
 * You could get a more or less equivalent operation by using
 * \code
 *     fsvs propset fsvs:update-pipe \
 *       '/usr/bin/install -g$g -o$u -m$m  $tmp $dest' \
 *       [paths]
 * \endcode
 *
 * The environment gets prepared as outlined above - you get the variables
 * - \c $g and \c $G (group name and gid),
 * - \c $u and \c $U (owner name and uid),
 * - \c $m (octal mode, like \c 0777),
 * - \c $t (mtime in form yyyymmddHHMM.SS - like used with GNU touch(1)),
 * - \c $tmp (name of temporary file) and
 * - \c $dest (destination name)
 * set.
 *
 * After the given program completed 
 * - $tmp gets deleted (\c ENOENT is not seen as an error, in case your 
 *   install program moved the file), and
 * - the destination path gets queried to store the meta-data of the (now 
 *   assumed to be \e non-modified) node.
 * */
#define FSVS_PROP_INSTALL_CMD FSVS_PROP_PREFIX "install"


/** The MD5 of the original (un-encoded) data.
 *
 * Used for encoded entries, see \ref FSVS_PROP_COMMIT_PIPE.
 *
 * If we do a sync-repos (or update), we need the cleartext-MD5 to know 
 * whether the entry has changed; this entry holds it.
 *
 * \todo Do we need some kind of SALT here, to avoid plaintext guessing? */
#define FSVS_PROP_ORIG_MD5 FSVS_PROP_PREFIX "original-md5"
/** @} */

/** \name propNames Meta-data property names.
 *
 * Depending on the subversion sources there may be some of these already
 * defined - especially if the header files are from the meta-data branches.
 * These would override the defaults - but lets hope that they'll always
 * be compatible! */
/** @{ */
#ifndef SVN_PROP_TEXT_TIME
#define SVN_PROP_TEXT_TIME  SVN_PROP_PREFIX "text-time"
#endif
#ifndef SVN_PROP_OWNER
#define SVN_PROP_OWNER SVN_PROP_PREFIX "owner"
#endif
#ifndef SVN_PROP_GROUP
#define SVN_PROP_GROUP  SVN_PROP_PREFIX "group"
#endif
#ifndef SVN_PROP_UNIX_MODE
#define SVN_PROP_UNIX_MODE  SVN_PROP_PREFIX "unix-mode"
#endif
/** @} */


/** \section svn_props Property names from the subversion name-space
 *
 * \c FSVS has a number of reserved property names, where it stores the 
 * meta-data and other per-entry data in the repository.
 *
 * \section svn_data Meta-data of entries
 *
 * Such names are already in use in the \e mtime and \e meta-data branches
 * of subversion; we use the values defined in \c svn_props.h (if any), or 
 * use the originally used values to be compatible. 
 *
 * These start all with the string defined in \c SVN_PROP_PREFIX, which is 
 * \c svn: . */
/** @{ */

/** Modification time - \c svn:text-time. */
const char propname_mtime[]=SVN_PROP_TEXT_TIME,
		 /** -. */
		 propname_owner[]=SVN_PROP_OWNER,
		 /** -. */
		 propname_group[]=SVN_PROP_GROUP,
		 /** -. */
		 propname_origmd5[]=FSVS_PROP_ORIG_MD5,
		 /** -. */
		 propname_umode[]=SVN_PROP_UNIX_MODE,
		 /** -. Subversion defines that for symlinks; we use that for devices, 
			* too.  */
		 propname_special[]=SVN_PROP_SPECIAL,
		 /** -. */
		 propval_special []=SVN_PROP_SPECIAL_VALUE,

		 /** -. This will get the local file as \c STDIN, and its output goes to the 
			* repository.
			* See \ref FSVS_PROP_COMMIT_PIPE. */
		 propval_commitpipe[]=FSVS_PROP_COMMIT_PIPE,
		 /** -. This will get the repository file as \c STDIN, and its output goes 
			* to a local temporary file, which gets installed.  See \ref 
			* FSVS_PROP_UPDATE_PIPE.
			* */
		 propval_updatepipe[]=FSVS_PROP_UPDATE_PIPE,
		 /** -. */
		 propval_orig_md5  []=FSVS_PROP_ORIG_MD5;
/** @} */


/** -.
 * 
 * I thought about using "constant prefix.$random" => "$propertyname" for 
 * them - but it's more work than simply ignoring them before listing. 
 *
 * And as they're not widely used, it's easier this way. */
const char prp___to_be_removed_value[]=
	"FSVS:INTERNAL-to-be-removed-- 91b88fdf-c285-4b73-a988-32d333c7548";



/** -.
 * Just a wrapper for the normal property operation.
 *
 * Must be silent for \c ENOENT, so that <tt>fsvs pl *</tt> doesn't give an 
 * error.  */
int prp__open_byname(char *wcfile, int gdbm_mode, hash_t *db)
{
  int status;
	
	status=hsh__new(wcfile, WAA__PROP_EXT, gdbm_mode, db);
	if (status != ENOENT)
		STOPIF(status, "Opening property file for %s", wcfile);

ex:
	return status;
}


/** -.
 * Returns ENOENT silently.
 * */
int prp__open_byestat(struct estat *sts, int gdbm_mode, hash_t *db)
{
	int status;
	char *fn;

	STOPIF( ops__build_path(&fn, sts), NULL);
	status=prp__open_byname(fn, gdbm_mode, db);
	if (status != ENOENT) STOPIF(status, NULL);

ex:
	return status;
}


/** -.
 *
 * If \a datalen is -1, \c strlen(data) is used. */
int prp__set(hash_t db, 
		const char *name, 
		const char *data, int datalen)
{
	int status;
	datum key, value;

	key.dptr=(char*)name;
	key.dsize=strlen(name)+1;

	if (data)
	{
		value.dptr=(char*)data;
		value.dsize=datalen;

		if (datalen == -1)
			value.dsize= *data ? strlen(data)+1 : 0;
#ifdef ENABLE_DEBUG
		else
			BUG_ON(value.dptr[value.dsize-1] != 0, "Not terminated!");
#endif
	}
	else
	{
		value.dptr=NULL;
		value.dsize=0;
	}

	STOPIF( prp__store(db, key, value), NULL);

ex:
	return status;
}


/** -.
 * Convenience function.
 * The svn_string_t has the number of characters used, whereas we store the 
 * \c \\0 at the end, too. */
int prp__set_svnstr(hash_t db, 
		const char *name, 
		const svn_string_t *utf8_value)
{
	return prp__set(db, name, utf8_value->data, utf8_value->len+1);
}


/** -.
 * */
int prp__store(hash_t db, datum key, datum value)
{
	int status;

	DEBUGP("storing property %s=%s", key.dptr, value.dptr);
	STOPIF( hsh__store(db, key, value), NULL);
ex:
	return status;
}


/** -.
 * Wrapper for prp__fetch(). */
int prp__get(hash_t db, const char * const keycp, datum *value)
{
  static datum key;

	key.dptr=(char*)keycp;
	key.dsize=strlen(keycp)+1;
	return prp__fetch(db, key, value);
}


/** -.
 * The meta-data of the entry is overwritten with the data coming from the 
 * repository; its \ref estat::remote_status is set.
 * If \a props_db is not NULL, the still opened property database is 
 * returned.
 * */
int prp__set_from_aprhash(struct estat *sts, 
		apr_hash_t *props, 
		enum prp__set_from_aprhash_e flags,
		hash_t *props_db,
		apr_pool_t *pool)
{
	int status;
	apr_hash_index_t *hi;
	char *prop_key;
	apr_ssize_t prop_key_len;
	
	svn_string_t *prop_val;
	hash_t db;
	int to_store, count;
	void *k, *v;


	status=0;
	count=0;

	/* The old behaviour was to always open the database file. If no 
	 * user-specified properties are given, old properties were removed that 
	 * way.
	 * But gdbm has the problem that on gdbm_close() a fsync() is done - even 
	 * if nothing was written; this means that for every updated file we 
	 * create a new property database, write nothing in it, do a fsync(), 
	 * close it, and delete it again - which costs some time.
	 *
	 * Debian bug #514704.
	 *
	 * (Removing of old properties is needed because we'd only know in 
	 * cb__record_changes() that properties get removed; in revert we only 
	 * have the new list.
	 * TODO: Merge local and remote changes.) */
	/* We remember the filename, so that empty hashes get removed on close.  
	 * */
	db=NULL;
	hi=apr_hash_first(pool, props);

	if (flags & STORE_IN_FS)
	{
		/* If we want to write the data to disk, but there is nothing to write 
		 * (and the caller doesn't need the DB), just remove the file. See 
		 * above.  */
		if (!hi && !props_db)
		{
			STOPIF( prp__unlink_db_for_estat(sts), NULL);
			goto ex;
		}

		STOPIF( prp__open_byestat(sts, 
					GDBM_NEWDB | HASH_REMEMBER_FILENAME, &db), NULL);
	}

	for (; hi; hi = apr_hash_next(hi)) 
	{
		/* As the name/key is a (char*), we don't need its length. */
		/* Is there a cleaner way than this (cast or compiler warning)??
		 * subversion defines a "const void * key" and casts that to 
		 * whatever needed in subsequent calls - which isn't pretty, too. */
		k=&prop_key;
		v=&prop_val;
		apr_hash_this(hi, k, &prop_key_len, v);

		to_store=0;
		STOPIF( up__parse_prop(sts, prop_key, prop_val, 
					&to_store, pool), NULL);

		if (to_store)
		{
			if (db)
			{
				/** \todo - store in utf-8? local encoding?
				 * What if it's binary???  Better do no translation, ie store as 
				 * UTF-8. */
				STOPIF( prp__set_svnstr(db, prop_key, prop_val), NULL);
			}
			count++;
		}
		else
		{
			/* If already used it's no longer needed. */
			if (flags & ONLY_KEEP_USERDEF)
				apr_hash_set(props, prop_key, prop_key_len, NULL);
		}
	}

	DEBUGP("%d properties stored", count);
	if (props_db)
		*props_db=db;
	else
		STOPIF( hsh__close(db, status), NULL);

ex:
	return status;
}


/** -.
 * */
int prp__g_work(struct estat *root, int argc, char *argv[])
{
	int status, st2;
	datum key, value;
	hash_t db;
	FILE *output;
	char **normalized;


	db=NULL;
	status=0;
	output=stdout;
	if (argc<2) ac__Usage_this();


	key.dptr=*(argv++);
	key.dsize=strlen(key.dptr)+1;
	argc--;


	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);


	for(; *argv; argv++)
	{
		db=NULL;
		status=prp__open_byname( *normalized, GDBM_READER, &db);
		if (!status)
			status=prp__fetch(db, key, &value);

		if (status == ENOENT)
		{
			DEBUGP("No such property");
		}
		else if (status)
		{
			/* Any other error means trouble. */
			STOPIF( status, NULL);
		}
		else if (value.dptr && !prp__prop_will_be_removed(value))
		{
			STOPIF_CODE_EPIPE( fputs(value.dptr, output), NULL);
			STOPIF_CODE_EPIPE( fputc('\n', output), NULL);
		}

		if (db)
			STOPIF( hsh__close(db, status), NULL);
		db=NULL;
	}

	status=0;

ex:
	if (db)
	{
		st2=hsh__close(db, status);
		db=NULL;
		if (!status && st2)
			STOPIF( st2, NULL);
	}
	return status;
}


/** -.
 *
 * Depending on action->i_val properties are removed or added.
 * */
int prp__s_work(struct estat *root, int argc, char *argv[])
{
	int status, st2;
	datum key, value, rv;
	hash_t db;
	char **normalized;
	struct estat *sts;
	int change;


	status=0;
	db=NULL;
	if (argc<2) ac__Usage_this();

	/* Check name for special values. */
	if (svn_prop_is_svn_prop(*argv))
		STOPIF( wa__warn( WRN__PROP_NAME_RESERVED, EINVAL, 
					"This is a reserved property name and should not be used." ), 
				NULL );


	key.dptr=*(argv++);
	key.dsize=strlen(key.dptr)+1;
	argc--;

	if (action->i_val == FS_REMOVED)
	{
		value.dptr=(char*)prp___to_be_removed_value;
		/* + \0 */
		value.dsize=strlen(prp___to_be_removed_value)+1;
	}
	else
	{
		value.dptr=*(argv++);
		value.dsize=strlen(value.dptr)+1;
		argc--;
		if (argc<1) ac__Usage_this();
	}


	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	STOPIF( au__prepare_for_added(), NULL);

	STOPIF( waa__input_tree(root, NULL, NULL), NULL);


	for(; *normalized; normalized++)
	{
		STOPIF( ops__traverse(root, *normalized, 
					OPS__CREATE | OPS__FAIL_NOT_LIST, RF_ADD, &sts), NULL);

		if (sts->flags & RF_ISNEW)
		{
			/* Get group. */
			STOPIF( ign__is_ignore(sts, &change), NULL);
			STOPIF( ops__apply_group(sts, &db, NULL), NULL);

			if (!sts->url)
				sts->url=current_url;

			STOPIF( hlp__lstat( *normalized, & sts->st),
					"!'%s' can not be queried", *normalized);
			/* Such entries must be set as added, if needed - else they wouldn't be 
			 * seen as new.  */
			sts->flags |= RF_ADD;
		}
		else
			STOPIF( prp__open_byestat(sts, GDBM_WRCREAT, &db), NULL);


		/* Check if modified. */
		change=0;
		status=prp__fetch(db, key, &rv);
		if (action->i_val == FS_REMOVED)
		{
			if (status == ENOENT) 
				DEBUGP("%s on %s didn't exist anyway", key.dptr, *normalized);
			else
				change++;
		}
		else
		{
			if (status == ENOENT)
				change++;
			else
			{
				change = (rv.dsize != value.dsize) ||
					memcmp(rv.dptr, value.dptr, value.dsize) != 0;
				DEBUGP("%s on %s change? %d", key.dptr, *normalized, change);
			}
		}

		if (change)
		{
			STOPIF( prp__store(db, key, value), NULL);
			sts->flags |= RF_PUSHPROPS;
		}

		STOPIF( hsh__close(db, status), NULL);
		db=NULL;
	}

	STOPIF( waa__output_tree(root), NULL);

ex:
	if (db)
	{
		st2=hsh__close(db, status);
		db=NULL;
		if (!status && st2)
			STOPIF( st2, NULL);
	}
	return status;
}


/**  -.
 * \a data and \a len are optional output parameters, ie. may be \c NULL.
 * if \a data is used, it \b must be free()d.
 *
 * Returns \c ENOENT silently. */
int prp__open_get_close(struct estat *sts, char *name, 
		char **data, int *len)
{
	int status;
	hash_t props;
	datum value;

	props=NULL;

	status=prp__open_byestat(sts, GDBM_READER, &props);
	if (status == ENOENT) goto ex;
	STOPIF(status, NULL);

	status=prp__get(props, name, &value);
	if (status == ENOENT) goto ex;
	STOPIF(status, NULL);

	if (len) *len=value.dsize;
	if (data) *data=value.dptr;
	else IF_FREE(value.dptr);

ex:
	if (props)
		hsh__close(props, status);
	return status;
}


/** -.
 * */
int prp__l_work(struct estat *root, int argc, char *argv[])
{
	int status, count;
	int many_files;
	char indent[5]="    ";
	hash_t db;
	FILE *output;
	datum key, data;
	char **normalized;


	status=0;
	db=NULL;
	if (!argc) ac__Usage_this();


	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);


	output=stdout;
	many_files= argc>1;
	if (!many_files) *indent=0;

	for(; *normalized; normalized++)
	{
		status=prp__open_byname( *normalized, GDBM_READER, &db);
		if (status == ENOENT) goto noprops;

		if (status)
			STOPIF(status, "Cannot open properties file for '%s'",
					*normalized);


		count=0;
		status=prp__first(db, &key);
		while (status == 0)
		{
			DEBUGP("got key with len=%d: %.30s", key.dsize, key.dptr);

			STOPIF( prp__fetch(db, key, &data), NULL);
			if (prp__prop_will_be_removed(data))
			{
				/* This property will be removed on next commit. */
			}
			else
			{
				count++;

				if (count==1 && many_files)
					STOPIF_CODE_EPIPE( printf("Properties of %s:\n", *normalized), NULL);

				STOPIF_CODE_EPIPE( fputs(indent, output), NULL);
				/* The key and value are defined to have a \0 at the end.
				 * This should not be printed. */
				STOPIF( hlp__safe_print(output, key.dptr, key.dsize-1), NULL);

				if (opt__is_verbose() > 0)
				{
					STOPIF_CODE_EPIPE( fputc('=',output), NULL);
					STOPIF( hlp__safe_print(output, data.dptr, data.dsize-1), NULL);

					free(data.dptr);
				}

				STOPIF_CODE_EPIPE( fputc('\n', output), NULL);
			}

			status=prp__next(db, &key, &key);
		}

		if (count == 0)
		{
noprops:
			printf("%s has no properties.\n", *normalized);
			status=0;
			continue;
		}


		STOPIF( hsh__close(db, status), NULL);
		db=NULL;
	}

ex:
	hsh__close(db, status);
	return status;
}


/** -. */
int prp__unlink_db_for_estat(struct estat *sts)
{
	int status;
	char *cp, *eos, *path;

	STOPIF( ops__build_path(&path, sts), NULL);

	STOPIF( waa__get_waa_directory(path, &cp, &eos, NULL,
				waa__get_gwd_flag(WAA__PROP_EXT)), NULL);
	strcpy(eos, WAA__PROP_EXT);

	status= unlink(cp) == -1 ? errno : 0;
	if (status == ENOENT)
		status=0;
	else STOPIF(status, "deleting properties of %s (%s)", path, cp);

ex:
	return status;
}

/** -. */
int prp__sts_has_no_properties(struct estat *sts, int *result)
{
	hash_t db;
	int status, rv;
	datum key;

	status=0;
	rv = prp__open_byestat( sts, GDBM_READER, &db);
	if (rv == ENOENT)
		goto done;
	STOPIF(rv, NULL);

	rv = prp__first(db, &key);
	STOPIF( hsh__close(db, 0), NULL);

done:
	*result = (rv == ENOENT);
ex:
	return status;
}
