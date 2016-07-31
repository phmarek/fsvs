/***********************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>

#include "global.h"
#include "add_unvers.h"
#include "status.h"
#include "ignore.h"
#include "warnings.h"
#include "est_ops.h"
#include "url.h"
#include "helper.h"
#include "waa.h"


/** \file
 * \ref add and \ref unversion action.
 * */

/**
 * \addtogroup cmds
 *
 * \section add
 *
 * \code
 * fsvs add [-u URLNAME] PATH [PATH...]
 * \endcode
 *
 * With this command you can explicitly define entries to be versioned,
 * even if they have a matching ignore pattern.
 * They will be sent to the repository on the next commit, just like
 * other new entries, and will therefore be reported as \e New .
 *
 * The \c -u option can be used if you're have more than one URL defined 
 * for this working copy and want to have the entries pinned to the this 
 * URL.
 *
 * \subsection add_ex Example
 * Say, you're versioning your home directory, and gave an ignore pattern
 * of <tt>./.*</tt> to ignore all <tt>.*</tt> entries in your home-directory.
 * Now you want <tt>.bashrc</tt>, <tt>.ssh/config</tt>, and your complete 
 * <tt>.kde3-tree</tt> saved, just like other data.
 * 
 * So you tell fsvs to not ignore these entries:
 * \code
 * 	fsvs add .bashrc .ssh/config .kde3
 * \endcode
 * Now the entries below <tt>.kde3</tt> would match your earlier 
 * <tt>./.*</tt> pattern (as a match at the beginning is sufficient), 
 * so you have to insert a negative ignore pattern (a \e take pattern):
 * \code
 * 	fsvs ignore prepend t./.kde3
 * \endcode
 * Now a <tt>fsvs st</tt> would show your entries as 
 * \e New , and the next commit will send them to the repository.
 * 
 * */

/**
 * \addtogroup cmds
 *
 * \section unversion
 *
 * \code 
 * fsvs unversion PATH [PATH...]
 * \endcode
 * 
 * This command flags the given paths locally as removed.
 * On the next commit they will be deleted in the repository, and the local
 * information of them will be removed, but not the entries themselves.
 * So they will show up as \e New again, and you get another chance 
 * at ignoring them.
 * 
 * \subsection unvers_ex Example
 * 
 * Say, you're versioning your home directory, and found that you no longer
 * want <tt>.bash_history</tt> and <tt>.sh_history</tt> versioned.
 * So you do
 * \code
 * 	fsvs unversion .bash_history .sh_history
 * \endcode
 * and these files will be reported as \c d (will be deleted, but only in the
 * repository).
 *
 * Then you do a 
 * \code
 * 	fsvs commit
 * \endcode
 *
 * Now fsvs would report these files as \c New , as it does no longer know 
 * anything about them; but that can be cured by 
 * \code
 * 	fsvs ignore "./.*sh_history"
 * \endcode
 * Now these two files won't be shown as \e New , either.
 * 
 * The example also shows why the given paths are not just entered as 
 * separate ignore patterns - they are just single cases of a 
 * (probably) much broader pattern.
 *
 * \note If you didn't use some kind of escaping for the pattern, the shell
 * would expand it to the actual filenames, which is (normally) not what you
 * want.
 * 
 * */

/** \defgroup howto_add_unv Semantics for an added/unversioned entry
 * \ingroup userdoc
 *
 * Here's a small overview for the \ref add and \ref unversion actions.
 *
 * - Unversion:
 *   The entry to-be-unversioned will be deleted from the repository and the 
 *   local waa cache, but not from disk. It should match an ignore pattern,
 *   so that it doesn't get committed the next time.
 * - Add:
 *   An added entry is required on commit - else the user told to store
 *   something which does not exist, and that's an error.
 *
 * \section add_unvers_status Status display
 *
 * <table>
 * <tr><th>Exists in fs ->       <th>YES        <th> NO
 * <tr><th>not seen before       <td>\c N         <td>\c -
 * <tr><th>committed             <td>\c C, \c R   <td>\c D
 * <tr><th>unversioned           <td>\c d         <td>\c d (or D?, or with !?)
 * <tr><th>added                 <td>\c n         <td>\c n (with !)
 * </table>
 * 
 *
 * If an entry is added, then unversioned, we remove it completely
 * from our list. We detect that by the RF_NOT_COMMITTED flag.
 * Likewise for an unversioned, then added, entry. 
 *
 * Please see also the \ref add command and the \ref unversion command.
 * */


/** General function for \ref add and \ref unversion actions.
 * This one really handles the entries. */
int au__action(struct estat *sts)
{
	int status;
	int old;
	int mask=RF_ADD | RF_UNVERSION;


	STOPIF_CODE_ERR(!sts->parent, EINVAL,
			"!Using %s on the working copy root doesn't make sense.",
			action->name[0]);

	status=0;

	/* This should only be done once ... but as it could be called by others, 
	 * without having action->i_val the correct value, we check on every 
	 * call.
	 * After all it's just two compares, and only for debugging ...  */
	BUG_ON( action->i_val != RF_UNVERSION && action->i_val != RF_ADD );

	old=sts->flags & mask;
	/* We set the new value for output, and possibly remove the entry 
	 * afterwards. */
	sts->flags = (sts->flags & ~mask) | action->i_val;
	DEBUGP("changing flags: has now %X", sts->flags);
	STOPIF( st__status(sts), NULL);

	/* If we have an entry which was added *and* unversioned (or vice versa), 
	 * but
	 * 1) has never been committed, we remove it from the list;
	 * 2) is a normal, used entry, we delete the flags. 
	 *
	 * Should we print "....." in case 2? Currently we show that it's being 
	 * added/unversioned again. */
	if (((sts->flags ^ old) & mask) == mask)
	{
		if (!sts->repos_rev) 
			STOPIF( ops__delete_entry(sts->parent, sts, 
						UNKNOWN_INDEX, UNKNOWN_INDEX), 
					NULL);
		else
			sts->flags &= ~mask;
	}

	if (sts->flags & RF_ADD)
	{
		/* Get the group. */
		STOPIF( ign__is_ignore(sts, &old), NULL);
		/* We don't want to know whether it's ignored, so we just discard the 
		 * ignore flag. */

		STOPIF( ops__apply_group(sts, NULL, NULL), NULL);

		/* And we don't want to ignore it, even if ops__apply_group() only 
		 * found an ignore pattern, thank you so much.  */
		sts->to_be_ignored=0;
	}

	if ((sts->flags & mask) == RF_ADD)
		sts->url=current_url;


ex:
	return status;
}


/** -.
 * */
int au__prepare_for_added(void)
{
	int status;

	STOPIF( url__load_list(NULL, 0), NULL);
	STOPIF( url__mark_todo(), NULL);
	STOPIF_CODE_ERR( url__parm_list_used>1, EINVAL,
			"!At most a single destination URL may be given.");

	if (url__parm_list_used)
	{
		STOPIF(url__find_by_name(url__parm_list[0], &current_url), 
				"!No URL with name \"%s\" defined.", url__parm_list[0]);
		DEBUGP("URL to add to: %s", current_url->url);
	}
	else
		current_url=NULL;

	/* We need the groups, to assign the auto-props. */
	STOPIF( ign__load_list(NULL), NULL);

ex:
	return status;
}


/** -.
 * */
int au__work(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;


	/* *Only* do the selected elements. */
	opt_recursive=-1;
	/* Would it make sense to do "-=2" instead, so that the user could override 
	 * that and really add/unversion more than single elements? */

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	STOPIF( au__prepare_for_added(), NULL);

	STOPIF( waa__read_or_build_tree(root, argc, normalized, argv, 
				NULL, 0), NULL);

	STOPIF( waa__output_tree(root), NULL);

ex:
	return status;
}

