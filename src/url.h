/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __URL_H__
#define __URL_H__

#include "actions.h"
#include "global.h"

/** \file
 * Header file for the \ref urls action and URL-related functions. */

/** A \c NULL terminated array of url parameters. */
extern char **url__parm_list;
/** How many URLs were given as parameters. */ 
extern int url__parm_list_used;

/** URLs action. */
work_t url__work;


/** Loads the URLs for the given \a dir. */
int url__load_list(char *dir, int reserve_space);
/** Wrapper for url__load_list(); Cries for \c ENOENT . */
int url__load_nonempty_list(char *dir, int reserve_space);
/** Writes the URL list back. */
int url__output_list(void);

/** Returns a struct \a url_t matching the given string. */
int url__find_by_name(const char *name, struct url_t **storage);
/** Returns a struct \a url_t matching the given internal number. */
int url__find_by_intnum(int intnum, struct url_t **storage);
/** Returns a struct \a url_t matching the given url. */
int url__find_by_url(char *url, struct url_t **storage);

/** Returns the full URL for this entry. */
int url__full_url(struct estat *sts, char *path, char **url);

/** Parses the given string into the URL storage. */
int url__parse(char *input, struct url_t *storage, int *def_parms);
/** Opens a session to the \a current_url . */
int url__open_session(svn_ra_session_t **session);
/** Looks for an URL matching \a url, and returns its address. */
int url__find(char *url, struct url_t **output);

/** Returns whether \a current_url has a higher priority than the
 * URL to compare. */
int url__current_has_precedence(struct url_t *to_compare);
/** Insert or replace URL. */
int url__insert_or_replace(char *eurl, 
		struct url_t **storage, 
		int *existed);
/** Allocate additional space for the given number of URLs. */
int url__allocate(int reserve_space);

/** Closes all RA sessions, and frees up associated memory. */
int url__close_sessions(void);

/** Marks URLs for handling. */
int url__mark_todo(void);
/** Remember URL name parameter for later processing. */
int url__store_url_name(char *parm);
	 
	 
static inline int url__to_be_handled(const struct url_t *url)
{
  return (!url__parm_list_used) || url->to_be_handled;
}

#endif
