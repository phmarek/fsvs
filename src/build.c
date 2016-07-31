/************************************************************************
 * Copyright (C) 2006-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include "global.h"
#include "waa.h"
#include "url.h"
#include "build.h"


/** \file
 * \ref _build_new_list action file.
 *
 * */

/** \addtogroup cmds
 * \section _build_new_list _build_new_list
 *
 * This is used mainly for debugging.
 * It traverses the filesystem and build a new entries file.
 * In production it should not be used - as the revision of the entries
 * is unknown, we can only use 0 - and loose information this way! */

int bld__work(struct estat *root, int argc, char *argv[])
{
	int status;

	STOPIF( waa__find_base(root, &argc, &argv), NULL);
	STOPIF( url__load_list(NULL, 0), NULL);
	/* If there are any URLs, we use the lowest-priority.
	 * Any sync-repos will correct that. */
	current_url=urllist[urllist_count-1];
	STOPIF( waa__build_tree(root), NULL);
	DEBUGP("build tree, now saving");
	STOPIF( waa__output_tree(root), NULL);

ex:
	return status;
}

