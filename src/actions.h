/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __ACTION_H__
#define __ACTION_H__

#include "global.h" 


/** \file
 * Action handling header file. */

/** \anchor callbacks \name callbacks Action callbacks. */
/** @{ */
/** Callback that gets called for each entry.
 *
 * Entries get read from the entry list in global [device, inode] order; in 
 * the normal action callback (\ref actionlist_t::local_callback and \ref 
 * actionlist_t::repos_feedback) the parent entries are handled \b after child 
 * entries (but the parent \c struct \ref estat "estats" exist, of course), 
 * so that the list of children is correct.
 *
 *
 * See also \ref waa__update_tree.
 *
 * The full (wc-based) path can be built as required by \ref 
 * ops__build_path().*/
/* unused, wrong doc
 * As in the entry list file (\ref dir) there is a callback \ref 
 * actionlist_t::early_entry that's done \b before the child entries;
 * Clearing \ref estat::do_this_entry and \ref estat::do_tree in this 
 * callback will skip calling \ref actionlist_t::local_callback for this and 
 * the child entries (see \ref ops__set_to_handle_bits()). */
typedef int (action_t)(struct estat *sts);
/** Callback for initializing the action. */
typedef int (work_t)(struct estat *root, int argc, char *argv[]);
/** One after all progress has been made. */
typedef int (action_uninit_t)(void);
/** @} */

/** The action wrapper. */
action_t ac__dispatch;


/** The always allowed action - printing general or specific help. */
work_t ac__Usage;
/** For convenience: general help, and help for the current action. */
#define ac__Usage_dflt() do { ac__Usage(NULL, 0, NULL); } while (0)
/** Print help for the current action. */
#define ac__Usage_this() do { ac__Usage(NULL, 1, (char**)action->name); } while (0)


/** Definition of an \c action. */
struct actionlist_t
{
	/** Array of names this action will be called on the command line. */
	const char** name;

	/** The function doing the setup, tear down, and in-between - the 
	 * worker main function.
	 *
	 * See \ref callbacks. */
	work_t *work;

	/** The output function for repository accesses.
	 * Currently only used in cb__record_changes().
	 *
	 * See \ref callbacks. */
	action_t *repos_feedback;

	/** The local callback.
	 * Called for each entry, just after it's been checked for changes.
	 * Should give the user feedback about individual entries and what 
	 * happens with them.
	 *
	 * For directories this gets called when they're finished; so immediately 
	 * for empty directories, or after all children are loaded.
	 * \note A removed directory is taken as empty (as no more elements are 
	 * here) - this is used in \ref revert so that revert gets called twice 
	 * (once for restoring the directory itself, and again after its 
	 * populated). 
	 *
	 * See \ref callbacks. */
	action_t *local_callback;
	/** The progress reporter needs a callback to clear the line after printing
	 * the progress. */
	action_uninit_t *local_uninit;

	/** A pointer to the verbose help text. */
	char const *help_text;

	/** Flag for usage in the action handler itself. */
	int i_val;

	/** Is this an import or export, ie do we need a WAA?
	 * We don't cache properties, manber-hashes, etc., if is_import_export 
	 * is set. */
	int is_import_export:1;
	/** This is set if it's a compare operation (remote-status).
	 * The properties are parsed, but instead of writing them into the 
	 * \c struct \c estat they are compared, and \c entry_status set
	 * accordingly. */
	int is_compare:1;
	/** Whether we need fsvs:update-pipe cached. 
	 * Do we install files from the repository locally? Then we need to know 
	 * how to decode them.
	 * We don't do that in every case, to avoid wasting memory. */
	int needs_decoder:1;
	/** Whether the entries should be filtered on opt_filter. */
	int only_opt_filter:1;
	/** Whether user properties should be stored in estat::user_prop while 
	 * running cb__record_changes(). */
	int keep_user_prop:1;
	/** Makes ops__update_single_entry() keep the children of removed 
	 * directories. */
	int keep_children:1;
	/** Says that we want the \c estat::st overwritten while looking for 
	 * local changes.  */
	int overwrite_sts_st:1;
	/** Whether waa__update_dir() may happen.
	 * (It must not for updates, as we'd store local changes as "from 
	 * repository"). */
	int do_update_dir:1;
	/** Says that this is a read-only operation (like "status"). */
	int is_readonly:1;
};


/** Find the action structure by name.
 *
 * Returns in \c * \a action_p the action matching (a prefix of) \a cmd.
 * */
int act__find_action_by_name(const char *cmd, 
		struct actionlist_t **action_p);


/** Array of all known actions. */
extern struct actionlist_t action_list[];
/** Gets set to the \e current action after commandline parsing. */
extern struct actionlist_t *action;
/** How many actions we know. */
extern const int action_list_count;


#endif
