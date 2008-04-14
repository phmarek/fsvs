/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __IGNORE_H__
#define __IGNORE_H__

/** \file
 * \ref ignore patterns header file. */

#include "global.h"
#include "actions.h"

/** \name Where a new pattern should be inserted. */
/** @{ */
/** At the front. */
#define PATTERN_POSITION_START (0)
/** Behind all other patterns (appended). */
#define PATTERN_POSITION_END (-1)
/** @} */


/** Ignore command main function. */
work_t ign__work;

/** Adds a list of new ignore patterns to the internal list. */
int ign__new_pattern(unsigned count, 
		char *pattern[], char *ends,
		int user_pattern, int position);
/** Tells whether the given entry is to be ignored. */
int ign__is_ignore(struct estat *sts, int *is_ignored);
/** Loads the ignore list from the WAA. */
int ign__load_list(char *dir);


#endif

