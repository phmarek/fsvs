/************************************************************************
 * Copyright (C) 2007-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __HASH_OPS_H
#define __HASH_OPS_H

#include "global.h"
#include <db.h>

/** \file
 * Hash operations header file.
 *
 * Similar to the property operations; but these here work on a hash of 
 * lists, to be able to store multiple files with the same name.
 * */



/** A convenience type. */
typedef struct hash_s *hash_t;
/** The abstract hash type. */
struct hash_s
{
/** We use a GDBM file as a hash, so we don't have to have all data in 
 * memory.  */
	GDBM_FILE db;
	/** Storage for transactional \c DELETE.
	 * 
	 * Eg on commit only when everything was ok we may remove the used 
	 * copyfrom entries; here we store the keys to remove. */
	/* Should that simply be a hash_t? We'd get a bit cleaner code, but would 
	 * waste a few bytes. */
	GDBM_FILE to_delete;
	/** Allocated copy of the filename, if HASH_REMEMBER_FILENAME was set. */
	char *filename;
};



/** Create a new hash for \a wcfile with the given \a name.
 */
int hsh__new(char *wcfile, char *name, int gdbm_mode, 
		hash_t *hash);
/** Only a temporary hash; not available in \c gdbm.
 * Unless the predefined constants include the value \c 0, and ORed 
 * together give -1, this is a distinct value. */
#define HASH_TEMPORARY ((GDBM_NEWDB | GDBM_READER | \
			GDBM_WRCREAT | GDBM_WRITER) +1)
/** This flag tells hsh__new() to remember the filename, for later 
 * cleaning-up. */
#define HASH_REMEMBER_FILENAME (0x40000000)

/** \section hsh__lists Lists addressed by some hash.
 * @{ */
/** Number of slots reserved. */
#define HASH__LIST_MAX (32)

/** For short-time storage (single program run): Insert the pointer \a 
 * value into the \a hash at \a key. */
int hsh__insert_pointer(hash_t hash, datum key, void* value);


/** Get an list of \a found entries from \a hash addressed by \a 
 * current_key into the (statically allocated) \a arr.  */
int hsh__list_get(hash_t hash, 
		datum current_key, datum *next_key, 
		struct estat **arr[], int *found);
/** @} */



/** \section hsh_simple Simple hash operations.
 * These are just wrappers, and are incompatible to the other hash 
 * functions - they don't store any links to other elements.
 * @{ */
/** Store character strings in the hash table. */
int hsh__store_charp(hash_t db, char *key, char *value);
/** Store some value in the hash table. */
int hsh__store(hash_t db, datum key, datum value);
/** Read \a value associated with some \a key in \a db.
 * Memory of datum::dptr is malloc()ed. */
int hsh__fetch(hash_t db, datum key, datum *value);

/** Find first \a key. */
int hsh__first(hash_t db, datum *key);
/** Find next \a key. */
int hsh__next(hash_t db, datum *key, const datum *oldkey);

/** Registers some key for deletion on database close. */
int hsh__register_delete(hash_t db, datum key);

/** Close a property file. */
int hsh__close(hash_t db, int has_failed);
/** Collect garbage in the hash table. */
int hsh__collect_garbage(hash_t db, int *did_remove);
/** @} */


#endif

