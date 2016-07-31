/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __UPDATE_H__
#define __UPDATE_H__

#include "actions.h"

/** \file
 * \ref update action header file. */

/** Main \ref update worker function. */
work_t up__work;


/** Parse subversion properties for the given entry. */
int up__parse_prop(struct estat *sts, 
		const char *utf8_name, 
		const svn_string_t *utf8_value,
		int *not_handled,
		apr_pool_t *pool);

/** Set the meta-data for this entry. */
int up__set_meta_data(struct estat *sts,
		const char *filename);

/** \name The delta-editor functions.
 * These are being used for remote-status. */
/** @{ */
svn_error_t *up__set_target_revision(void *edit_baton,
		svn_revnum_t rev,
		apr_pool_t *pool);
svn_error_t *up__open_root(void *edit_baton,
		svn_revnum_t base_revision,
		apr_pool_t *dir_pool UNUSED,
		void **root_baton);
svn_error_t *up__add_directory(const char *path,
		void *parent_baton,
		const char *copy_path,
		svn_revnum_t copy_rev,
		apr_pool_t *dir_pool UNUSED,
		void **child_baton);
svn_error_t *up__change_dir_prop(void *dir_baton,
		const char *name,
		const svn_string_t *value,
		apr_pool_t *pool UNUSED);
svn_error_t *up__close_directory(
		void *dir_baton,
		apr_pool_t *pool);
svn_error_t *up__absent_directory(const char *path,
		void *parent_baton,
		apr_pool_t *pool);
svn_error_t *up__add_file(const char *path,
		void *parent_baton,
		const char *copy_path,
		svn_revnum_t copy_rev,
		apr_pool_t *file_pool,
		void **file_baton);
svn_error_t *up__apply_textdelta(void *file_baton,
		const char *base_checksum,
		apr_pool_t *pool,
		svn_txdelta_window_handler_t *handler,
		void **handler_baton);
svn_error_t *up__change_file_prop(void *file_baton,
		const char *name,
		const svn_string_t *value,
		apr_pool_t *pool);
svn_error_t *up__close_file(void *file_baton,
		const char *text_checksum,
		apr_pool_t *pool UNUSED);
svn_error_t *up__absent_file(const char *path,
		void *parent_baton,
		apr_pool_t *pool);
svn_error_t *up__close_edit(void *edit_baton, 
		apr_pool_t *pool);
svn_error_t *up__abort_edit(void *edit_baton,
		apr_pool_t *pool);
/** @} */

int up__handle_special(struct estat *sts, 
		char *path,
		char *data,
		apr_pool_t *pool);

int up__unlink(struct estat *sts, char *filename);
int up__rmdir(struct estat *sts, struct url_t *url);
int up__fetch_decoder(struct estat *sts);

#endif


