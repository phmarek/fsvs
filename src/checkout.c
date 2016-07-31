/************************************************************************
 * Copyright (C) 2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/select.h>

#include "url.h"
#include "waa.h"
#include "commit.h"
#include "export.h"


/** \file
 * \ref checkout action
 * */

/** \addtogroup cmds
 * 
 * \section checkout
 *
 * \code
 * fsvs checkout [path] URL [URLs...]
 * \endcode
 *
 * Sets one or more URLs for the current working directory (or the 
 * directory \c path), and does an \ref checkout of these URLs.
 *
 * Example:
 * \code
 * fsvs checkout . http://svn/repos/installation/machine-1/trunk
 * \endcode
 *
 * The distinction whether a directory is given or not is done based on the  
 * result of URL-parsing -- if it looks like an URL, it is used as an URL.  
 * \n Please mind that at most a single path is allowed; as soon as two 
 * non-URLs are found an error message is printed.
 *
 * If no directory is given, \c .  is used; this differs from the usual 
 * subversion usage, but might be better suited for usage as a recovery 
 * tool (where versioning \c / is common). Opinions welcome.
 *
 * The given \c path must exist, and \b should be empty -- \c fsvs will 
 * abort on conflicts, ie. if files that should be created already exist.
 * \n If there's a need to create that directory, please say so; patches 
 * for some parameter like \c -p are welcome.
 *
 * For a format definition of the URLs please see the chapter \ref 
 * url_format and the \ref urls and \ref update commands.
 *
 * Furthermore you might be interested in \ref o_softroot and \ref 
 * howto_backup_recovery.
 * */


/** -.
 * Writes the given URLs into the WAA, and gets the files from the 
 * repository. */
int co__work(struct estat *root, int argc, char *argv[])
{
	int status;
	int l;
	char *path;


	path=NULL;

	/* The allocation uses calloc(), so current_rev is initialized to 0. */
	STOPIF( url__allocate(argc+1), NULL);
	/* Append URLs. */
	for(l=0; l<argc; l++)
	{
		DEBUGP("parsing %s into %d", argv[l], urllist_count);
		/* Should we fail if the URL is given twice? */
		/* Don't print errors for now ... so we see whether it's an URL or not, 
		 * without alerting the user. */
		make_STOP_silent++;
		status=url__insert_or_replace(argv[l], NULL, NULL);
		make_STOP_silent--;
		if (status == EINVAL)
		{
			/* Invalid URL ... possibly path. */
			STOPIF_CODE_ERR(path, EINVAL,
					"!Two non-URLs were given:\n"
					"  %s\n"
					"and\n"
					"  %s\n"
					"but this action accepts only a single path.",
					path, argv[l]);

			path=argv[l];
		}
	}

	STOPIF_CODE_ERR( urllist_count==0, EINVAL,
			"!Need at least a single URL to checkout from.");

	if (path)
		STOPIF_CODE_ERR( chdir(path)==-1, errno,
				"!Cannot use the directory \"%s\";\nmaybe you meant to give an URL?", path);


	/* We don't use the loop above, because the user might give the same URL 
	 * twice - and we'd overwrite the fetched files. */
	for(l=0; l<urllist_count; l++)
	{
		STOPIF( exp__do(root, urllist[l]), NULL);

		urllist[l]->current_rev = target_revision;
		STOPIF( ci__set_revision(root, target_revision), NULL);
		printf("Checked out %s at revision\t%ld.\n", 
				urllist[l]->url, urllist[l]->current_rev);
	}

	/* Store where we are ... */
	STOPIF( url__output_list(), NULL);
	STOPIF( waa__output_tree(root), NULL);

ex:
	return status;
}


