/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

/** \file
 * \ref export action.
 *
 * This is mostly the same as update; the difference is that
 * we export the given URL to the current directory, and don't use
 * a WAA area.
 * */

#include <subversion-1/svn_delta.h>
#include <subversion-1/svn_ra.h>
#include <subversion-1/svn_error.h>

#include "export.h"
#include "helper.h"
#include "url.h"
#include "racallback.h"


/**
 * \addtogroup cmds
 * \section export
 * 
 * \code
 * fsvs export REPOS_URL [-r rev]
 * \endcode
 * 
 * If you want to export a directory from your repository \b without
 * having to have an WAA-area, you can use this command.
 * This restores all meta-data - owner, group, access mask and 
 * modification time.
 * Its primary use is for data recovery.
 * 
 * The data gets written (in the correct directory structure) below the
 * current working directory; if entries already exist, the export will stop,
 * so this should be an empty directory.
 * */


/** \name Undefined functions
 *
 * There are some callbacks that should never get called.
 * Rather than setting their pointer in the \c export_editor structure
 * to \c NULL (and risk a \c SEGV if the ra layer definition changes), we use
 * some nearly empty functions to cry if something unexpected happens. */
/** @{ */
/// FSVS GCOV MARK: exp__invalid should not be executed
/** This function is used to write messages in case an undefined function
 * gets called by the ra layer. */
svn_error_t *exp__invalid(const char *name)
{
	int status;

	STOPIF_CODE_ERR(1, EINVAL,
			"The function %s got called "
			"during an export operation;\n"
			"this call is unexpected and won't be handled.\n"
			"This should not happen.\n",
			name);
ex:
	RETURN_SVNERR(status);
}


/// FSVS GCOV MARK: exp__delete should not be executed
svn_error_t *exp__delete(const char *path UNUSED,
		svn_revnum_t revision UNUSED,
		void *parent_baton UNUSED,
		apr_pool_t *pool UNUSED)
{
	return exp__invalid(__PRETTY_FUNCTION__);
}


/// FSVS GCOV MARK: exp__open_dir should not be executed
svn_error_t *exp__open_dir(const char *path UNUSED,
		void *parent_baton UNUSED,
		svn_revnum_t base_revision UNUSED,
		apr_pool_t *dir_pool UNUSED,
		void **child_baton UNUSED)
{
	return exp__invalid(__PRETTY_FUNCTION__);
}


/// FSVS GCOV MARK: exp__open_file should not be executed
svn_error_t *exp__open_file(const char *path UNUSED,
		void *parent_baton UNUSED,
		svn_revnum_t base_revision UNUSED,
		apr_pool_t *file_pool UNUSED,
		void **file_baton UNUSED)
{
	return exp__invalid(__PRETTY_FUNCTION__);
}
/** @} */



/** The export editor functions.
 * The functionality is the same as on update, so we simply use that
 * functions. */
const svn_delta_editor_t export_editor = 
{
	.set_target_revision 	= up__set_target_revision,

	.open_root 						= up__open_root,

	.delete_entry				 	= exp__delete,
	.add_directory 				= up__add_directory,
	.open_directory 			= exp__open_dir,
	.change_dir_prop 			= up__change_dir_prop,
	.close_directory 			= up__close_directory,
	.absent_directory 		= up__absent_directory,

	.add_file 						= up__add_file,
	.open_file 						= exp__open_file,
	.apply_textdelta 			= up__apply_textdelta,
	.change_file_prop 		= up__change_file_prop,
	.close_file 					= up__close_file,
	.absent_file 					= up__absent_file,

	.close_edit 					= up__close_edit,
	.abort_edit 					= up__abort_edit,
};


/** -.
 * \a root must already be initialized.
 *
 * The difference to update is that export expects an empty filesystem, ie.  
 * to fetch *all* nodes; it doesn't check whether some already exist 
 * locally.  */
int exp__do(struct estat *root, struct url_t *url)
{
	int status;
	svn_revnum_t rev;
	svn_error_t *status_svn;
	void *report_baton;
	const svn_ra_reporter2_t *reporter;


	status_svn=NULL;
	current_url=url;

	STOPIF( url__open_session(NULL), NULL);

	rev=url->target_rev;
	/* See the comment in update.c */
	STOPIF( url__canonical_rev(current_url, &rev), NULL);

	/* export files */
	STOPIF_SVNERR( svn_ra_do_update,
			(current_url->session,
			 &reporter,
			 &report_baton,
			 opt_target_revision,
			 "",
			 TRUE,
			 &export_editor,
			 root,
			 global_pool) );

	/* We always pretend to start empty. */
	STOPIF_SVNERR( reporter->set_path,
			(report_baton,
			 "", rev, TRUE,
			 NULL, global_pool));

	STOPIF_SVNERR( reporter->finish_report, 
			(report_baton, global_pool));


ex:
	STOP_HANDLE_SVNERR(status_svn);
ex2:
	return status;
}


/** -.
 *
 * This does a checkout of the given URL (using the various meta-data flags),
 * but without writing to the WAA.
 * */
int exp__work(struct estat *root, int argc, char *argv[])
{
	int status;
	struct url_t url;


	status=0;

	STOPIF_CODE_ERR(argc!=1, EINVAL,
			"1 parameter (URL) expected");

	STOPIF( url__parse(argv[0], &url, NULL), NULL);

	/* Initialize root structure */
	STOPIF( hlp__lstat(".", &root->st),
			"Cannot retrieve information about '.'");

	STOPIF( exp__do(root, &url), NULL);
	printf("Exported revision\t%ld.\n", target_revision);

ex:
	return status;
}

