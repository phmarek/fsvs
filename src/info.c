/************************************************************************
 * Copyright (C) 2007-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

/** \file
 * \ref info action.
 *
 * Allows the user to display various information about his working copy
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
 * working copy. \n
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
 * directory; with another \c -R you'll get the whole (sub-)tree. */


#include <unistd.h>
#include <fcntl.h>

#include "global.h"
#include "est_ops.h"
#include "cp_mv.h"
#include "url.h"
#include "status.h"
#include "waa.h"
#include "warnings.h"


/** Utility function - prints the normal status and the extended 
 * information. */
int info__action(struct estat *sts)
{
	int status;

	/* Always print this entry. */
	sts->was_output=0;
	sts->flags |= RF_PRINT;

	/* TODO: entry_type is already overwritten by ops__stat_to_action() */
	STOPIF( st__status(sts), NULL);
	STOPIF( st__print_entry_info(sts), NULL);

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

	/* If verbose operation was wanted, we want return the copyfrom URL.
	 * We cannot simply look at opt__is_verbose() and set VERBOSITY_COPYFROM, 
	 * because with "-v" the OPT__VERBOSE priority is already at 
	 * PRIO_CMDLINE, so PRIO_PRE_CMDLINE doesn't work - and simply overriding 
	 * a specific wish (as given with "-o verbose=") isn't nice, too.
	 *
	 * So we check whether it seems that a single "-v" was given, and react 
	 * to that; perhaps we should resurrect the global opt_verbose variable, 
	 * and check what's the best verbosity default is in each worker. */
	if (opt__get_int(OPT__VERBOSE) == VERBOSITY_DEFAULT_v)
		opt__set_int( OPT__VERBOSE, PRIO_CMDLINE, 
				opt__get_int(OPT__VERBOSE) | VERBOSITY_COPYFROM);

	/* do not update the entries; print info based on *known* data. */

	status=waa__read_or_build_tree(root, argc, normalized, argv, NULL, 1);
	if (status == -ENOENT)
	{
	  printf("No tree information available. Did you commit?\n");
		status=0;
		goto ex;
	}
	STOPIF( status, "Cannot get tree information");

ex:
	return status;
}

