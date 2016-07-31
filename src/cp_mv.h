/************************************************************************
 * Copyright (C) 2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __CP_MV_H
#define __CP_MV_H

#include "actions.h"

/** \file
 * \ref cp and \ref mv actions header file.
 * */

/** For defining copyfrom relations. */
work_t cm__work;
/** For automatically finding relations. */
work_t cm__detect;

/** Worker function. */
action_t cm__action;

/** Returns the source element of the given entry. */
int cm__get_source(struct estat *sts, char *name,
		struct estat *root, 
		struct estat **source, 
		char **src_name, svn_revnum_t *src_rev);

#endif


