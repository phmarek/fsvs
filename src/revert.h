/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __REVERT_H__
#define __REVERT_H__

#include "actions.h"

/** \file
 * \ref revert command header file. */ 

/** \ref revert main action function. */
work_t rev__work;

/** \a Revert callback function. */
action_t rev__action;

/** Fetches a given entry from the repository. */
int rev__get_file(struct estat *sts, 
		svn_revnum_t revision,
		char *url_to_use,
		svn_revnum_t *fetched,
		char **only_tmp,
		apr_pool_t *pool);

/** Go through the tree, and fetch all changed entries (estimated
 * per \c remote_status). */
int rev__do_changed(svn_ra_session_t *session, 
		struct estat *dir, 
		apr_pool_t *pool);

/** Gets and writes the properties of the given \a sts into its \ref prop 
 * file. */
int rev__get_props(struct estat *sts, 
		char *utf8_path,
		svn_revnum_t revision,
		apr_pool_t *pool);


#endif

