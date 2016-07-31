/************************************************************************
 * Copyright (C) 2006-2009 Philipp Marek.
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
/** Whether the URL list in FSVS_CONF must be written. */
extern int url__must_write_defs;

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
int url__full_url(struct estat *sts, char **url);
/** Returns the full URL for this entry for some other than the highest 
 * priority URL. */
int url__other_full_url(struct estat *sts, struct url_t *url, char **output); 

/** Parses the given string into the URL storage. */
int url__parse(char *input, struct url_t *storage, int *def_parms);
/** Opens a session to the \a current_url . */
int url__open_session(svn_ra_session_t **session, char **missing_dirs);
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

/** Closes given RA session and frees associated memory. */
int url__close_session(struct url_t *cur);
/** Closes all RA sessions. */
int url__close_sessions(void);

/** Marks URLs for handling. */
int url__mark_todo(void);
/** Remember URL name parameter for later processing. */
int url__store_url_name(char *parm);
/** Returns whether \a url should be handled. */
static inline int url__to_be_handled(const struct url_t *url)
{
  return (!url__parm_list_used) || url->to_be_handled;
}
	 

/** Simple function setting \c current_url, and returning whether there's 
 * something to do. */
int url__iterator2(svn_revnum_t *target_rev, 
		int only_if_count, char **missing);
static inline int url__iterator(svn_revnum_t *target_rev)
{
	return url__iterator2(target_rev, 0, NULL);
}


/** Comparing two URLs.
 *
 * They get sorted by \a priority ascending (lower numbers, so higher
 * priority, first), then by \a url ascending (sort URLs alphabetically). 
 *
 * This is necessary, as on update we walk the \a urllist in order, to
 * have lower priority entries appearing when higher priority entries are
 * removed.
 *
 * If the first URL has a higher priority, a negative value is returned. */
static inline int url__sorter(struct url_t *u1, struct url_t *u2)
{
	if (u1->priority == u2->priority)
		return strcmp(u1->url, u2->url);
	else
		return u1->priority - u2->priority;
}

/** For use in \c qsort(). */
int url__indir_sorter(const void *a, const void *b);


/** Changes the revision number, if \c SVN_INVALID_REVNUM, to the real 
 * value. */
int url__canonical_rev( struct url_t *url, svn_revnum_t *rev);

#endif
