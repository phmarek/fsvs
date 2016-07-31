/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <unistd.h>


#include "global.h"
#include "cache.h"


/** \file
 * Some small caching primitives.
 *
 * We have to do some caching - neither the APR-functions nor glibc caches 
 * results of \c getpwnam() and similar.
 * On update or commit we call them many, many times ... there it's good to 
 * have these values cached.
 *
 * It's not necessary for performance; but simply getting a \c char* back 
 * from some function and using it, knowing that it's valid for a few more 
 * calls of the same function, eases life tremendously.
 *
 * \todo Convert the other caches. 
 *
 * \todo Let the \c apr_uid_get() calls from \c update.c go into that -
 * but they need a hash or something like that. Maybe reverse the test and 
 * look whether the number (eg. uid) matches the string (username)?
 * */


/** -.
 * If a struct \ref cache_entry_t is used as a string, this might be 
 * useful.
 *
 * If memory should be allocated, but not copied, specify \a data as \c 
 * NULL.
 * For \a len \c ==-1 calls \c strlen().
 *
 * If \a copy_old_data is set, old value in this cache entry is kept.
 *
 * Please note that memory may have to be reallocated, causing \c *cache to 
 * change! */
inline int cch__entry_set(struct cache_entry_t **cache, 
		cache_value_t id, const char *data, int len,
		int copy_old_data,
		char **copy)
{
	int status;
	struct cache_entry_t *ce;
	int alloc_len;

	if (len == -1) len=strlen(data);

	status=0;
	ce=*cache;
	alloc_len=len+sizeof(struct cache_entry_t);
	if (!ce || 
			alloc_len > ce->len || 
			(ce->len - len) > 1024)
	{
		/* Round up a bit (including the struct). */
		alloc_len = (alloc_len + 96-1) & ~64;

		if (copy_old_data)
			ce=realloc(ce, alloc_len);
		else
		{
			/* Note: realloc() copies the old data to the new location, but most 
			 * of the time we'd overwrite it completely just afterwards. */
			free(*cache);
			ce=malloc(alloc_len);
		}

		STOPIF_ENOMEM(!ce);

		ce->len = alloc_len-sizeof(struct cache_entry_t)-1;
		*cache=ce;
	}

	ce->id = id;
	if (data) memcpy(ce->data, data, len);

	/* Just to be safe ... */
	ce->data[len]=0;

	if (copy) *copy=ce->data;

ex:
	return status;
}


/** -.
 * Can return \c ENOENT if not found. */
inline int cch__find(struct cache_t *cache, cache_value_t id, 
		int *index, char **data, int *len)
{
	int i;

	for(i=0; i<cache->used; i++)
		if (cache->entries[i]->id == id)
		{
			if (data) *data= cache->entries[i]->data;
			if (len) *len= cache->entries[i]->len;
			if (index) *index=i;

			return 0;
		}

	return ENOENT;
}


/** -.
 *
 * The given data is just inserted into the cache and marked as LRU.
 * An old entry is removed if necessary.
 */
int cch__add(struct cache_t *cache, 
		cache_value_t id, const char *data, int len,
		char **copy)
{
	int i;

	if ( cache->used >= cache->max) 
	{
		i=cache->lru+1;
		if (i >= cache->max) i=0;
	}
	else
		i= cache->used++;

	cache->lru=i;

	/* Set data */
	return cch__entry_set(cache->entries + i, id, data, len, 0, copy);
}


/** -.
 * \a id is a distinct numeric value for addressing this item.
 * The entry is set as LRU, eventually discarding older entries.
 * */
int cch__set_by_id(struct cache_t *cache, 
		cache_value_t id, const char *data, int len,
		int copy_old_data,
		char **copy)
{
	int i;

	/* Entry with same ID gets overwritten. */
	if (cch__find(cache, id, &i, NULL, NULL) == ENOENT)
	{
		return cch__add(cache, id, data, len, copy);
	}

		/* Found, move to LRU */
	cch__set_active(cache, i);

	/* Set data */
	return cch__entry_set(cache->entries + i, 
			id, data, len, 
			copy_old_data, copy);
}


/** -.
 * */
void cch__set_active(struct cache_t *cache, int i)
{
	struct cache_entry_t *tmp, **entries;


	entries=cache->entries;
	/* observe these 2 cases: */
	if (i < cache->lru)
	{
		/* from | 6 5 i 3 2 1 LRU 9 8 7 |
		 * to		| 6 5 3 2 1 LRU i 9 8 7 |
		 *	 -> move [i+1 to LRU] to i, i is the new LRU. */
		tmp=entries[i];
		memmove(entries+i, 
				entries+i+1, 
				(cache->lru-i) * sizeof(entries[0]));
		entries[cache->lru]=tmp;
	}
	else if (i > cache->lru)
	{
		/* from | 2 1 LRU 9 8 7 i 5 4 3 | 
		 * to		| 2 1 LRU i 9 8 7 5 4 3 |
		 *	 -> move [LRU+1 to i] to LRU+2; LRU++  */
		cache->lru++;
		BUG_ON(cache->lru >= cache->max);

		/* lru is already incremented */
		tmp=entries[i];
		memmove(entries+cache->lru+1, 
				entries+cache->lru, 
				(i-cache->lru) * sizeof(entries[0]));
		entries[cache->lru]=tmp;
	}
}

/** A simple hash.
 * Copies the significant bits ' ' .. 'Z' (or, really, \\x20 ..  \\x60) of 
 * at most 6 bytes of \a stg into a packed bitfield, so that 30bits are 
 * used.  */
inline cache_value_t cch___string_to_cv(const char *stg)
{
  union {
	  cache_value_t cv;
		struct {
			unsigned int c0:5;
			unsigned int c1:5;
			unsigned int c2:5;
			unsigned int c3:5;
			unsigned int c4:5;
			unsigned int c5:5;
			unsigned int ignore_me:2;
		};
	} __attribute__((packed)) result;

	result.cv=0;
	if (*stg) 
	{ result.c0 = *(stg++) - 0x20;
		if (*stg) 
		{ result.c1 = *(stg++) - 0x20;
			if (*stg) 
			{ result.c2 = *(stg++) - 0x20;
				if (*stg) 
				{ result.c3 = *(stg++) - 0x20;
					if (*stg) 
					{ result.c4 = *(stg++) - 0x20;
						if (*stg) 
						{ result.c5 = *(stg++) - 0x20;
						} } } } } }

	return result.cv;
}


/** -.
 * */
int cch__hash_find(struct cache_t *cache, const char *key, cache_value_t *data)
{
	int status;
	cache_value_t id;
	int i;

	id=cch___string_to_cv(key);

	DEBUGP("looking for %lX = %s", id, key);
	if (cch__find(cache, id, &i, NULL, NULL) == 0 && 
			strcmp(key, cache->entries[i]->data) == 0)
	{
		*data = cache->entries[i]->hash_data;
		DEBUGP("found %s=%ld", key, *data);
		status=0;
	}
	else
		status=ENOENT;

	return status;
}


/** -.
 * */
int cch__hash_add(struct cache_t *cache, const char *key, cache_value_t value)
{
	int status;
	cache_value_t id;


	id=cch___string_to_cv(key);
	STOPIF( cch__add(cache, id, key, strlen(key), NULL), NULL);
	cache->entries[ cache->lru ]->hash_data = value;

ex:
	return status;
}


