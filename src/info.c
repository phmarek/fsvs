/************************************************************************
 * Copyright (C) 2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

/** \file
 * \ref info action.
 *
 * Allows the user to display various informations about his working copy
 * entries - eg. URL, revision number, stored meta-data, etc. */

/** \addtogroup cmds
 *
 * \section info
 *
 * \code
 * fsvs info [-R [-R]] [PATH...]
 * \endcode
 * 
 * Use this command to show information regarding one or more entries in your
 * working copy.
 * Currently you must be at the working copy root; but that will change.
 * You can use \c -v to obtain slightly more information.
 * 
 * This may sometimes be helpful for locating bugs, or to obtain the
 * URL and revision a working copy is currently at.
 * 
 * Example:
 * \code
 *     $ fsvs info
 *     URL: file:///tmp/ram/fsvs-test-1000/tmp-repos-path/trunk
 *     ....       200  .
 *             Type:           directory
 *             Status:         0x0
 *             Flags:          0x100000
 *             Dev:            0
 *             Inode:          24521
 *             Mode:           040755
 *             UID/GID:        1000/1000
 *             MTime:          Thu Aug 17 16:34:24 2006
 *             CTime:          Thu Aug 17 16:34:24 2006
 *             Revision:       4
 *             Size:           200
 * \endcode
 *
 * The default is to print information about the given entry only.
 * With a single \c -R you'll get this data about \b all entries of a given 
 * directory; with a second \c -R you'll get the whole (sub-)tree. */


#include <unistd.h>
#include <fcntl.h>

#include "global.h"
#include "est_ops.h"
#include "url.h"
#include "status.h"
#include "waa.h"
#include "warnings.h"


/** Utility function - prints the normal status and the extended 
 * information. */
int info__action(struct estat *sts)
{
	int status;

/* Wird für gelöschte doppelt ausgegeben */
/* Kein errorlevel für gelöschte */
	/* Always print this entry. */
	sts->was_output=0;
	sts->flags |= RF_PRINT;

	STOPIF( st__status(sts, NULL), NULL);
	STOPIF( st__print_entry_info(sts, 1), NULL);

ex:
	return status;
}



int info__work(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;


	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);


	status=url__load_list(NULL, 0);
	if (status==ENOENT)
		STOPIF( wa__warn(WRN__NO_URLLIST, status, "No URLs defined"), NULL);
	else
		STOPIF_CODE_ERR( status, status, NULL);


	/* Default is single-element only. */
	opt_recursive-=2;

	only_check_status=1;

	status=waa__read_or_build_tree(root, argc, normalized, argv, NULL, 1);
	if (status == -ENOENT)
	{
	  printf("No tree information available. Did you commit?\n");
		status=0;
		goto ex;
	}
	STOPIF( status, "Cannot get tree information");

	only_check_status=0;


ex:
	return status;
}

