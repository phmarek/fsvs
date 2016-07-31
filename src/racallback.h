/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __RACALLBACK_H__
#define __RACALLBACK_H__

#include <subversion-1/svn_ra.h>

/** \file
 * The cb__record_changes() and other callback functions header file.  */

/** The callback table for cb__record_changes(). */
extern struct svn_ra_callbacks_t cb__cb_table;

/** Initialize the callback functions.
 * \todo Authentication providers. */
svn_error_t *cb__init(apr_pool_t *pool);

/** A change-recording editor. */
int cb__record_changes(struct estat *root,
		svn_revnum_t target,
		apr_pool_t *pool);
/** Like cb__record_changes(), but allowing mixed reporting. */
int cb__record_changes_mixed(struct estat *root,
		svn_revnum_t target,
		char *other_paths[], svn_revnum_t other_revs,
		apr_pool_t *pool);


/** This function adds a new entry below dir, setting it to
 * \c FS_NEW or \c FS_REPLACED. */
int cb__add_entry(struct estat *dir, 
		const char *utf8_path, char **loc_path,
		const char *utf8_copy_path, 
		svn_revnum_t copy_rev,
		int mode,
		int *has_existed,
		int may_create,
		void **new);

#endif
