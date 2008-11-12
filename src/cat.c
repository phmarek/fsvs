/************************************************************************
 * Copyright (C) 2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <unistd.h>


#include "global.h"
#include "waa.h"
#include "revert.h"
#include "url.h"
#include "est_ops.h"


/** \file
 * \ref cat action.
 *
 * \todo \code
 * fsvs cat [-r rev] [-u URLname] path
 * fsvs cat [-u URLname:rev] path
 * \endcode
 * */

/** 
 * \addtogroup cmds
 *
 * \section cat
 *
 * \code
 * fsvs cat [-r rev] path
 * \endcode
 *
 * Fetches a file with the specified revision or, if not given, BASE, from 
 * the repository, and outputs it to \c STDOUT.
 *
 * */

/** -.
 * Main function. */
int cat__work(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;
	struct estat *sts;
	struct svn_stream_t *output;
	svn_error_t *status_svn;


	status=0;
	STOPIF_CODE_ERR( argc != 1, EINVAL,
			"!Exactly a single path must be given.");

	STOPIF_CODE_ERR( opt_target_revisions_given > 1, EINVAL,
			"!At most a single revision is allowed.");

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);
	STOPIF( url__load_list(NULL, 0), NULL);
	STOPIF( waa__input_tree( root, NULL, NULL), NULL);

	STOPIF( ops__traverse(root, normalized[0], 
				OPS__FAIL_NOT_LIST, 0, &sts), NULL);

	current_url=sts->url;
	STOPIF_CODE_ERR( !current_url, EINVAL,
			"!For this entry no URL is known.");
			
	STOPIF( url__open_session(NULL), NULL);
	STOPIF_SVNERR( svn_stream_for_stdout,
			(&output, global_pool));

	STOPIF( rev__get_text_to_stream( normalized[0], 
				opt_target_revisions_given ? 
				opt_target_revision : sts->repos_rev,
				DECODER_UNKNOWN,
				output, NULL, NULL, NULL, global_pool), NULL);

ex:
	return status;
}
 
