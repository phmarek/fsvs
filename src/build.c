/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <time.h>

#include "global.h"
#include "waa.h"
#include "helper.h"
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

/** Traverse the filesystem, build a tree, and store it as WC.
 * Doesn't do anything with the repository. */
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


/** \addtogroup cmds
 * \section delay
 *
 * This command delays execution until the time has passed at least to the 
 * next second after writing the \ref dir "dir" and \ref urls "urls" files.
 * So, where previously the \ref delay "delay" option was used, this can be 
 * substituted by the given command followed by the \c delay command.
 *
 * The advantage is over the \ref o_delay option is, that read-only 
 * commands can be used in the meantime.
 *
 * An example:
 * \code
 *   fsvs commit /etc/X11 -m "Backup of X11"
 *   ... read-only commands, like "status"
 *   fsvs delay /etc/X11
 *   ... read-write commands, like "commit"
 * \endcode
 *
 * In the testing framework it is used to save a bit of time; in normal 
 * operation, where \c fsvs commands are not so tightly packed, it is 
 * normally preferable to use the \ref o_delay "delay" option. */
/** Waits until the \c dir and \c Urls files have been modified in the 
 * past, ie their timestamp is lower than the current time (rounded to 
 * seconds.) */
int delay__work(struct estat *root, int argc, char *argv[])
{
	int status;
	int i;
	time_t last;
	struct sstat_t st;
	char *filename, *eos;
	char *list[]= { WAA__DIR_EXT, WAA__URLLIST_EXT };


	STOPIF( waa__find_base(root, &argc, &argv), NULL);
	if (opt_verbose)
		printf("Waiting on WC root \"%s\"\n", wc_path);

	last=0;
	for(i=0; i<sizeof(list)/sizeof(list[0]); i++)
	{
		STOPIF( waa__get_waa_directory(wc_path, &filename, &eos, NULL,
					waa__get_gwd_flag(list[i])), NULL);
		strcpy(eos, list[i]);

		status=hlp__lstat(filename, &st);
		DEBUGP("stat(%s) returns status %d; %llu=%26.26s",
				filename, status,
				(t_ull)st.mtim.tv_sec,
				ctime(&st.mtim.tv_sec));

		if (status == ENOENT)
		{
			/* Ok, never mind. */
		}
		else
		{
			STOPIF( status, NULL);
			if (st.mtim.tv_sec > last) last=st.mtim.tv_sec;
		}
	}

	DEBUGP("waiting until %llu", (t_ull)last);
	opt__set_int(OPT__DELAY, PRIO_MUSTHAVE, -1);
	STOPIF( hlp__delay(last, 1), NULL);

ex:
	return status;
}

