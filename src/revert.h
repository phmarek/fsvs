/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __REVERT_H__
#define __REVERT_H__

#include "actions.h"

/** \file
 * \ref revert command header file. */ 

/** \ref revert main action function. */
work_t rev__work;

/** Has to fetch the decoder from the repository. */
#define DECODER_UNKNOWN ((char*)-1)

/** Gets a clean copy from the repository. */
int rev__install_file(struct estat *sts, svn_revnum_t revision,
		char *decoder,
		apr_pool_t *pool);

/** Go through the tree, and fetch all changed entries (estimated
 * per \c remote_status). */
int rev__do_changed(struct estat *dir, 
		apr_pool_t *pool);

/** Gets and writes the properties of the given \a sts into its \ref prop 
 * file. */
int rev__get_props(struct estat *sts, 
		char *utf8_path,
		svn_revnum_t revision,
		apr_pool_t *pool);

/** Gets the entry into a temporary file. */
int rev__get_text_to_tmpfile(char *loc_url, svn_revnum_t revision,
		char *encoder,
		char *filename_base, char **filename,
		struct estat *sts_for_manber, 
		struct estat *output_sts, apr_hash_t **props,
		apr_pool_t *pool);

/** Just a wrapper for rev__get_text_to_stream(). */
int rev__get_text_into_buffer(char *loc_url, svn_revnum_t revision,
		const char *decoder,
		svn_stringbuf_t **output,
		struct estat *sts_for_manber,
		struct estat *output_sts,
		apr_hash_t **props,
		apr_pool_t *pool);

/** General function to get a file into a stream. */
int rev__get_text_to_stream( char *loc_url, svn_revnum_t revision,
		const char *decoder,
		svn_stream_t *output,
		struct estat *sts_for_manber,
		struct estat *output_sts,
		apr_hash_t **props,
		apr_pool_t *pool);

#endif

