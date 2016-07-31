/************************************************************************
 * Copyright (C) 2005-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
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
int cb__record_changes(svn_ra_session_t *session,
		struct estat *root,
		svn_revnum_t target,
		apr_pool_t *pool);

#endif
