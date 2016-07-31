/************************************************************************
 * Copyright (C) 2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __RESOLVE_H__
#define __RESOLVE_H__

/** \file
 * \ref resolve action header file. */

#include "global.h"
#include "actions.h"

/** This function takes a \c NULL terminated list of filenames, and appends
 * it to the conflict list of the given \a sts. */
int res__mark_conflict(struct estat *sts, ...) __attribute((sentinel));

/** Removes all stored files. */
int res__remove_aux_files(struct estat *sts);

/** Resolve command main function. */
work_t res__work;

/** Resolve command action function. */
action_t res__action;

#endif

