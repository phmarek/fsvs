/************************************************************************
 * Copyright (C) 2007-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __REMOTE_H__
#define __REMOTE_H__

/** \file
 * Property handling header file - \ref prop-set, \ref prop-get, \ref 
 * prop-list.  */

#include <sys/types.h>

#include "global.h"
#include "hash_ops.h"

/** \name Opening a property file.
 *
 * The flags for this operations are defined in \c GDBM(3gdbm):
 * - GDBM_WRCREAT
 * - GDBM_READER
 * - GDBM_WRITER
 * - GDBM_NEWDB
 * */
/** @{ */
/** Open a property file, by WC-path. */
int prp__open_byname(char *wcfile, int gdbm_mode, hash_t *db);
/** Open a property file, by struct estat. */
int prp__open_byestat(struct estat *sts, int gdbm_mode, hash_t *db);
/** @} */

/** Set a property by name and data/datalen. */
int prp__set(hash_t db, const char *name, const char *data, int datalen);
/** Set a property by svn_string_t. */
inline int prp__set_svnstr(hash_t db, 
		const char *name, 
		const svn_string_t *utf8_value);

/** Writes the given set of properties of \a sts into its \ref prop file.  
 * */
int prp__set_from_aprhash(struct estat *sts, 
		apr_hash_t *props,
		apr_pool_t *pool);

/** Wrapper functions, if we need to have some compatibility layer. */
/** @{ */
/** Open a database, path specified through \a wcfile and \a name. */
int prp__db_open_byname(char *wcfile, int flags, char *name, 
		hash_t *db);
/** Get a value, addressed by a string; key length is calculated inclusive 
 * the \c \\0. */
int prp__get(hash_t db, char *keycp, datum *value);
/** Store the value; basic function. */
int prp__store(hash_t db, datum key, datum value);
/** Get first key. */
static inline int prp__first(hash_t db, datum *key)
{
  int status;
	status=hsh__first(db, key);
#ifdef ENABLE_DEBUG
	if (!status) 
		BUG_ON(key->dptr[key->dsize-1] != 0, "Not terminated!");
#endif
	return status;
}
/** Get next key. */
static inline int prp__next(hash_t db, datum *key, const datum *oldkey)
{
  int status;
	status=hsh__next(db, key, oldkey);
#ifdef ENABLE_DEBUG
	if (!status) 
		BUG_ON(key->dptr[key->dsize-1] != 0, "Not terminated!");
#endif
	return status;
}
/** Fetch a value. */
static inline int prp__fetch(hash_t db, datum key, datum *value)
{
	int status;

	if (!db) return ENOENT;

	status=hsh__fetch(db, key, value);
#ifdef ENABLE_DEBUG
	if (!status)
		BUG_ON(value->dptr[value->dsize-1] != 0, "Not terminated!");
#endif
	DEBUGP("read property %s=%s", key.dptr, value->dptr);

	return status;
}

/** Open, fetch, close a property hash corresponding to \a sts and \a name.  
 * */
int prp__open_get_close(struct estat *sts, char *name, 
		char **data, int *len);
/** @} */


/** Prop-get worker function. */
work_t prp__g_work;
/** Prop-set worker function. */
work_t prp__s_work;
/** Prop-list worker function. */
work_t prp__l_work;

#endif

