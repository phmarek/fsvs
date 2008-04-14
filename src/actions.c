/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "actions.h"
#include "est_ops.h"
#include "global.h"
#include "checksum.h"
#include "options.h"
#include "waa.h"


/** \file
 * Common functions for action (name) handling. */


/** This wrapper-callback for the current action callback calculates the \a 
 * path and fills in the \c entry_type for the current \a sts, if 
 * necessary.  */
int ac__dispatch(struct estat *sts)
{
	int status;

	status=0;
	if (!action->local_callback) goto ex;

	/* remove all variables */
	switch (sts->entry_type)
	{
		case FT_UNKNOWN:
		case FT_ANY:
		case FT_NONDIR:
			sts->entry_type=ops___filetype(&(sts->st));
	}

	/* If we're filtering, */
	/* We want all entries (eg. -v) */
	if (opt__get_int(OPT__FILTER) == FILTER__ALL ||
			/* or it's an interesting entry. */
			(sts->entry_status & opt__get_int(OPT__FILTER)))
		STOPIF( action->local_callback(sts), NULL);
	else 
	{
		DEBUGP("%s is not the entry you're looking for", sts->name);
		/* We have to keep the removed flag for directories, so that children 
		 * know that they don't exist anymore. */
		if (S_ISDIR(sts->st.mode) && (sts->entry_status & FS_REMOVED))
			sts->entry_status=FS_REMOVED;
		else
			sts->entry_status=0;
		/** \todo solve that cleaner. */
	}

ex:
	return status;
}


/** Given a string \a cmd, return the corresponding action entry.
 * Used by commandline parsing - finding the current action, and 
 * which help text to show. */
int act__find_action_by_name(const char *cmd, 
		struct actionlist_t **action_p)
{
	int len, i, status;
	struct actionlist_t *action;
	int match_nr;
	char const* const* cp;


	status=0;
	len=strlen(cmd);
	match_nr=0;
	action=action_list;

	for (i=action_list_count-1; i >=0; i--)
	{
		cp=action_list[i].name;
		while (*cp)
		{
			if (strncmp(*cp, cmd, len) == 0)
			{
				action=action_list+i;

				/* If it's am exact match, we're done.
				 * Needed for "co" (checkout) vs. "commit". */
				if (len == strlen(*cp)) goto done;

				match_nr++;
				break;
			}

			cp++;
		}
	}

	STOPIF_CODE_ERR( match_nr <1, ENOENT,
			"!Action \"%s\" not found. Try \"help\".", cmd);
	STOPIF_CODE_ERR( match_nr >=2, EINVAL,
			"!Action \"%s\" is ambiguous. Try \"help\".", cmd);

done:
	*action_p=action;

ex:
	return status;
}

