/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __CACHE_H__
#define __CACHE_H__


/** \file
 * Cache header file. */

/** Type of data we're caching; must be size-compatible with a pointer, as  
 * such is stored in some cases (eg ops__build_path()).
 * */
typedef long cache_value_t;


/** What an internal cache entry looks like.
 * Is more or less a buffer with (allocated) length; the real length is 
 * normally specified via some \\0 byte, by the caller. (A string.) */
struct cache_entry_t {
	/** ID of entry */
	cache_value_t id;
	/** User-data for hashes */
	cache_value_t hash_data;
	/** Length of data */
	int len;
#if 0
	/** Measurement of accesses */
	short accessed;
#endif
	/** Copy of data. */
	char data[1];
};


#define CACHE_DEFAULT (4)

/** Cache structure.
 * The more \b active an entry is, the more at the start of the array.
 * 
 * If a \c struct \ref cache_t is allocated, its \c .max member should be 
 * set to the default \ref CACHE_DEFAULT value.
 *
 * For a \c struct \ref cache_t* the function \ref cch__new_cache() must be 
 * used. */
struct cache_t {
	/** For how many entries is space allocated? */
	int max;
	/** How many entries are used. */
	int used;
	/** Which entry was the last accessed.
	 *
	 * If the array of entries looked like this, with \c B accessed after \c 
	 * C after \c D:
	 * \dot
	 * digraph {
	 *   rank=same;
	 *   D -> C -> B -> Z -> Y -> ppp -> E;
	 *   ppp [label="..."];
	 *   B [label="B=LRU", style=bold];
	 * }
	 * \enddot
	 * After setting a new entry \c A it looks like that:
	 * \dot
	 * digraph {
	 *   rank=same;
	 *   D -> C -> B -> A -> Y -> ppp -> E;
	 *   ppp [label="..."];
	 *   A [label="A=LRU", style=bold];
	 * }
	 * \enddot	 * */
	int lru;

	/** Cache entries, \c NULL terminated. */
  struct cache_entry_t *entries[CACHE_DEFAULT+1];
};


/** Adds a copy of the given data (\a id, \a data with \a len) to the \a 
 * cache; return the new allocated data pointer in \a copy.
 * */
int cch__add(struct cache_t *cache, 
		cache_value_t id, const char *data, int len,
		char **copy);

/** Find an entry, return index and/or others. */
int cch__find(struct cache_t *cache, cache_value_t id, 
		int *index, char **data, int *len);

/** Copy the given data into the given cache entry. */
inline int cch__entry_set(struct cache_entry_t **cache, 
		cache_value_t id, const char *data, int len,
		int copy_old_data,
		char **copy);
	
/** Look for the same \a id in the \a cache, and overwrite or append the 
 * given data. */
int cch__set_by_id(struct cache_t *cache, 
		cache_value_t id, const char *data, int len,
		int copy_old_data,
		char **copy);

/** Makes the given index the head of the LRU list. */
void cch__set_active(struct cache_t *cache, int index);


/** Create a new \a cache, with a user-defined size.
 *
 * I'd liked to do something like
 * \code
 *   static struct cache_t *cache=cch__new_cache(32);
 * \endcode
 * but that couldn't return error codes (eg. \c ENOMEM).
 * We'd need something like exceptions ....
 *
 * So I take the easy route with an inline function. Additional cost: a 
 * single "test if zero". 
 *
 * \note Another way could have been:
 * \code
 *   static struct cache_t *cache;
 *   static int status2=cch__new_cache(&cache, 32);
 *
 *   if (status2) { status=status2; goto ex; }
 *
 * ex:
 *   return status;
 * \endcode
 * But that's not exactly "better", and still does a "test if zero" on each 
 * run.
 *
 * */
static inline int cch__new_cache(struct cache_t **cache, int max)
{
	int status, len;

	status=0;
	if (!*cache)
	{
		len= sizeof(struct cache_entry_t*)*(max-CACHE_DEFAULT)+
			sizeof(struct cache_t);

		*cache=malloc(len);
		STOPIF_ENOMEM(!*cache);

		memset(*cache, 0, len);
		(*cache)->max=max;
	}

ex:
	return status;
}


/** Interpret the \a cache as a hash, look for the \a key, returning the 
 * \ref cache_entry_t::hash_data or \c ENOENT. */
int cch__hash_find(struct cache_t *cache, 
		const char *key, cache_value_t *data);
/** Interpret the \a cache as a hash and store the given \a value to the \a 
 * key. */
int cch__hash_add(struct cache_t *cache, 
		const char *key, cache_value_t value);

#endif

