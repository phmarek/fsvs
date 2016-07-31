/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __COMMIT_H__
#define __COMMIT_H__

#include <subversion-1/svn_delta.h>

#include "actions.h"

/** \file
 * \ref commit action header file. */


/** Mark entries' parents as to-be-traversed. */
action_t ci__action;
/* Main commit function. */
work_t ci__work;

/** Sets the given revision \a rev recursive on all entries correlating to 
 * \a current_url.  */
int ci__set_revision(struct estat *this, svn_revnum_t rev);

#endif


