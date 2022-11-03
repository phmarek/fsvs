/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
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

extern int ign__max_group_name_len;

/** Group structure.
 * Needed by commit, too. */
struct grouping_t {
	char *group_name;
	apr_hash_t *auto_props;
	struct url_t *url;
	int is_ignore:1;
	int is_take:1;
};



/** Ignore command main function. */
work_t ign__work;
/** Rel-ignore command main function. */
work_t ign__rign;

/** Adds a list of new ignore patterns to the internal list. */
int ign__new_pattern(unsigned count, 
		char *pattern[], char *ends,
		int user_pattern, int position);
/** Tells whether the given entry is to be ignored. */
int ign__is_ignore(struct estat *sts, int *is_ignored);
/** Loads the ignore list from the WAA. */
int ign__load_list(char *dir);

/** Print the grouping statistics. */
int ign__print_group_stats(FILE *output);

enum {
	FORCE_IGNORE=0,
	ALLOW_GROUPS,
};

/** List of bits for pattern definitions.
 * @{ */
#define HAVE_DIR 1
#define HAVE_CASE 2
#define HAVE_GROUP 4
#define HAVE_MODE 8
#define HAVE_PATTERN 16
#define HAVE_PATTERN_SUBST 32
/** @} */


/** For the help text. */
#define hlp_ignore hlp_groups

#endif

