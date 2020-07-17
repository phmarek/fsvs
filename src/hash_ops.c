/************************************************************************
 * Copyright (C) 2007-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "global.h"
#include "waa.h"
#include "helper.h"
#include "hash_ops.h"


/** \file
 * Hash operations for copy/move detection.
 *
 * The hash operations are binary clean; they don't care what kind of 
 * key/value data they store.
 * 
 * 
 * \section hsh_storage Storage considerations
 *
 * The basic question is ... do we need an unlimited amount of list entries 
 * in any hash bucket, or do we not?
 *
 * If we do, we have these possibilities:
 * \subsection hsh_store_pnt Extending the key, counting entries.
 * The first entry is written as now.
 * \dot
 *   digraph {
 *     node [shape=record];
 *     "key" -> "value1";
 *     }
 * \enddot
 * If we find a second entry with the same key,
 * - it is written as \c {key}1
 * - and the other one as \c {key}2;
 * - the \c {key} entry is set to the integer \c 2, (which can easily be 
 * distinguished because of the length)
 * \dot
 *   digraph {
 *     node [shape=record];
 *     "key" -> "2 (number of array elements)";
 *     "key.1" -> "value1";
 *     "key.2" -> "value2";
 *     }
 * \enddot
 * Every further entry with the same key would increment the counter, and 
 * get stored at that position. \n
 * That's not optimal for performance:
 * - It means reading a value (does the key exist?), and
 * - possibly writing that key with another value again, and
 * - storing a value.
 * Note that this would require some end-of-key marker (like \c \\0), or 
 * keys with a constant length.
 *
 *
 * \subsection hsh_store_list Storing a linked list in the hash.
 * Every entry is written with a header; the first for some given key gets 
 * the number \c 0, as an \e end-of-list marker.
 * \dot
 * digraph {
 *   node [shape=record];
 *   C0 [label ="<0>0 | value1" ];
 *   "key" -> C0;
 * }
 * \enddot
 * If this \c key has a collision, we
 * - increment the stored \c number,
 * - write the old value under the key \c {number}, with the header set 
 * to 0.
 * - The new value gets the current \c number prepended, and stored with 
 * the given \c key.
 * \dot
 * digraph {
 *   node [shape=record];
 *   {
 *     rank=same;
 *     C0 [label = "<p>0 | value2" ];
 *     C1 [label = "<p>1 | value1" ];
 *   }
 *   {
 *     rank=same;
 *     key;
 *     1;
 *   }
 *   key -> C1;
 *   1 -> C0;
 *
 *   edge [style=dotted];
 *   edge [arrowhead=none, arrowtail=normal];
 *   1 -> C1:p;
 *
 *   edge [style=invis, weight=20];
 *   key -> 1;
 * }
 * \enddot
 * After several insertions, the situation might be like this:
 * \dot
 * digraph {
 *   node [shape=record];
 *   {
 *     rank=same;
 *     Ca  [label = " { <p> 0 | value_A } " ];
 *     Cb  [label = " { <p> 0 | value_B2 } " ];
 *     Cb2 [label = " { <p> 1 | value_B1 } " ];
 *     Cc3 [label = " { <p> 3 | value_C3 } " ];
 *     Cc2 [label = " { <p> 2 | value_C2 } " ];
 *     Cc  [label = " { <p> 0 | value_C1 } " ];
 *   }
 *   {
 *     rank=same;
 *     key_A;
 *     key_B;
 *     key_C;
 *     1;
 *     2;
 *     3;
 *   }
 *
 *   "key_A" -> Ca;
 *
 *   "key_B" -> Cb2;
 *   "1" -> Cb;
 *
 *   "key_C" -> Cc3;
 *   "2" -> Cc;
 *   "3" -> Cc2;
 *
 *   edge [style=dotted];
 *   edge [arrowhead=none, arrowtail=normal];
 *   1 -> Cb2:p;
 *   3 -> Cc3:p;
 *   2 -> Cc2:p;
 *
 *   edge [style=invis, weight=20];
 *   key_A -> key_B;
 *   1 -> key_C;
 * }
 * \enddot
 *
 *
 * \subsection hsh_store_array Storing a verbatim array
 *
 * If there's a limited number of entries (with known length) to store, an 
 * array with a defined size might be easiest.  \n
 * A similar variant would be to simply concatenate the data in the hash 
 * buckets, with some suitable separator.
 * - memory intensive, slow for big buckets (many bytes to copy).
 * - For array iteration some special convention for the \c key would 
 * have to be used, like \c .dsize=0 and \c .dptr=array_def; the last 
 * returned index would have to be stored in the array structure.
 * - Big advantage: fast for reading, doesn't have to seek around.
 * \dot
 * digraph {
 *   node [shape=record];
 *   C1 [label = "num=4 | v1 | v2 | v3 | v4 | 0 | 0 | 0 | 0 | 0 | 0 | 0" ];
 *   "key" -> C1;
 *   }
 * \enddot
 *
 *
 * \subsection Conclusio
 *
 * Barring other (better) ideas, the array solution is currently 
 * implemented; the array is of fixed-size, can store only pointers, and 
 * the function for getting a list allows returning a set of elements. 
 *
 * <hr>
 * */


/** \name Simple hash functions.
 *
 * @{ */

/** Bare open function for internal use. 
 *
 * \a *fname_out, if not \c NULL, gets an allocated copy of the filename. 
 * */
int hsh___new_bare(char *wcfile, char *name, int gdbm_mode, 
		GDBM_FILE *output, 
		char **fname_out)
{
	int status;
	char *cp, *eos;
	GDBM_FILE db;


	status=0;
	db=NULL;

	if (gdbm_mode == HASH_TEMPORARY)
	{
		/* Replace our own constant with a public available value. */
		gdbm_mode = GDBM_NEWDB;
		cp=waa_tmp_path;
		/* Use this bit, so that the open filehandle says what it was. */
		eos=waa_tmp_fn;
	}
	else
		STOPIF( waa__get_waa_directory(wcfile, &cp, &eos, NULL,
					( (gdbm_mode == GDBM_READER) ? 0 : GWD_MKDIR)  
					| waa__get_gwd_flag(name)), NULL);
	strcpy(eos, name);

	if (gdbm_mode == GDBM_NEWDB)
	{
		/* libgdbm3=1.8.3-3 has a bug - with GDBM_NEWDB an existing database is 
		 * not truncated. Only the O_CREAT, not the O_TRUNC flag is used.
		 * debian #447981. */
		/** STOPIF_CODE_ERR\todo remove this bugfix sometime ... */
		/* No error, and ENOENT are both ok. */
		STOPIF_CODE_ERR( (unlink(cp) == -1) && (errno != ENOENT), errno,
				"Removing database file '%s'", cp);
		status=0;
	}

	db = gdbm_open(cp, 0, gdbm_mode, 0777, NULL);
	if (!db)
	{
		status=errno;
		if (status != ENOENT)
			STOPIF(status, "Cannot open database file %s", cp);
	}
	else
	{
		/* Temporary files can be removed immediately. */
		if (gdbm_mode == HASH_TEMPORARY)
			STOPIF_CODE_ERR( unlink(cp) == -1, errno,
					"Removing database file '%s'", cp);

		if (fname_out) 
			STOPIF( hlp__strdup( fname_out, cp), NULL);
	}

ex:
	if (status)
	{
		if (db) gdbm_close(db);
	}
	else
		*output=db;

	return status;
}


/** -.
 * If \a flags is \c GDBM_NEWDB, the file gets deleted immediately; there's 
 * no need to keep it around any longer, and it's not defined where it gets 
 * located.
 * If another open mode is used, the entry is always created in the WAA or 
 * CONF base directory for \a wcfile, ie.  the hashed path for the working 
 * copy root.
 */
int hsh__new(char *wcfile, char *name, int gdbm_mode, 
		hash_t *output)
{
	int status;
	hash_t hash;


	STOPIF( hlp__calloc( &hash, 1, sizeof(*hash)), NULL);

	/* Return errors silently. */
	status=hsh___new_bare(wcfile, name, 
			gdbm_mode & ~HASH_REMEMBER_FILENAME, 
			& (hash->db), 
			gdbm_mode & HASH_REMEMBER_FILENAME ? &(hash->filename) : NULL);

ex:
	if (status)
		IF_FREE(hash);
	else
		*output=hash;

	return status;
}


/** -.
 *
 * The previously marked keys in the hash table are removed; it is not 
 * checked for empty-ness nor reorganized.  */
int hsh__collect_garbage(hash_t db, int *did_remove)
{
	int status;
	int have_removed;
	datum key, next;

	status=0;
	have_removed=0;
	if (db && db->to_delete)
	{
		key=gdbm_firstkey(db->to_delete);
		while (key.dptr)
		{
			next=gdbm_nextkey(db->to_delete, key);
			STOPIF_CODE_ERR( gdbm_delete(db->db, key)!=0, gdbm_errno,
					"Removing entry");

			free(key.dptr);
			key=next;
			have_removed++;
		}

		DEBUGP("%d cleanups", have_removed);

		gdbm_close(db->to_delete);
		db->to_delete=NULL;
	}

	if (did_remove) *did_remove=have_removed;

ex:
	return status;
}


/** -.
 * 
 * If \a has_failed is set, some error has happened, and the registered 
 * keys are not used for deletion (like a \c ROLLBACK). */
int hsh__close(hash_t db, int has_failed)
{
	int status;
	int have_removed;
	datum key;


	status=0;
	if (!db) goto ex;

	have_removed=0;
	if (db->to_delete)
	{
		if (!has_failed)
			STOPIF( hsh__collect_garbage(db, &have_removed), NULL);
	}

	/* No more data in that hash? */
	if (hsh__first(db, &key) == ENOENT &&
			db->filename)
	{
		DEBUGP("nothing found, removing %s", db->filename);
		STOPIF( waa__delete_byext(db->filename, NULL, 0), 
				"Cleaning up the empty hash '%s'", db->filename);
	}
	else
	{
		DEBUGP("reorganize?");
		/* At least fewer space used? */
		if (have_removed)
			gdbm_reorganize(db->db);
	}

	DEBUGP("closing hash");
	/* It's possible that some hash was never opened, because there was no 
	 * need to. */
	if (db->db)
	{
		gdbm_close(db->db);
		db->db=NULL;
	}

ex:
	if (db)
		IF_FREE(db->filename);
	IF_FREE(db);

	return status;
}


/** -. */
int hsh__fetch(hash_t db, datum key, datum *value)
{
	static datum vl;

	if (!db) return ENOENT;

	vl=gdbm_fetch(db->db, key);

	if (value) *value=vl;
	else IF_FREE(vl.dptr);
	return (vl.dptr) ? 0 : ENOENT;
}


/** -.
 * */
int hsh__first(hash_t db, datum *key)
{
	datum k;

	if (!db) return ENOENT;

	k=gdbm_firstkey(db->db);
	if (key) *key=k;
	return (k.dptr) ? 0 : ENOENT;
}


/** -.
 * 
 * If \c oldkey==key the \c ->dptr is \c free()d; else the caller has to do 
 * that. */
int hsh__next(hash_t db, datum *key, const datum *oldkey)
{
	datum k;

	/* Get next key. */
	k=gdbm_nextkey(db->db, *oldkey);

	/* Ev. free old key-data. */
	if (oldkey == key) 
		/* Should be IF_FREE(oldkey->dptr) -- but oldkey==key, and oldkey is 
		 * read-only. */
		IF_FREE(key->dptr);

	/* Return new key. */
	*key=k;

	return (k.dptr) ? 0 : ENOENT;
}


/** -.
 * */
int hsh__store(hash_t db, datum key, datum value)
{
	int status;

	if (value.dsize == 0 || value.dptr == NULL)
		status=gdbm_delete(db->db, key);
	else
		status=gdbm_store(db->db, key, value, GDBM_REPLACE);

	STOPIF_CODE_ERR(status < 0, errno,
			"Error writing property %s", key.dptr);

ex:
	return status;
}


/** -.
 * The delimiting \\0 is stored, too. */
int hsh__store_charp(hash_t db, char *keyp, char *valuep)
{	
	datum key, value;

	key.dptr=keyp;
	value.dptr=valuep;

	key.dsize=strlen(key.dptr)+1;
	value.dsize=strlen(value.dptr)+1;

	return hsh__store(db, key, value);
}

/** @} */


/** \name Hash list manipulation.
 *
 * @{ */

/** Structure for storing a number of data packets of size sizeof(void*).
 * */
struct hsh___list
{
	/** Count of entries.
	 * 
	 * If we put the overflow flag/count at the end, the structure is not 
	 * runtime-resizeable. If we put it at the start, it will always need 
	 * alignment for the pointers behind.
	 * So we can just store the count ... a single bit wouldn't need fewer 
	 * bytes.
	 *
	 * If \c count>HASH__LIST_MAX, this entry is considered "overflown". */
	int count;
	/** Array of pointers. */
	void *entries[HASH__LIST_MAX];
};


/** -.
 * 
 *
 * */
int hsh__insert_pointer(hash_t hash, datum key, void *value)
{
	int status;
	datum listd;
	struct hsh___list list, *dst;

	status=hsh__fetch(hash, key, &listd);
	if (status == ENOENT)
	{
		/* None found. */
		/* Don't know why this is not a (void*) ... possibly because it 
		 * couldn't add up :-) */
		listd.dptr= (char*)&list;
		listd.dsize=sizeof(list);

		memset(&list, 0, sizeof(list));
		list.count=1;
		list.entries[0]=value;

		status=0;
	}
	else
	{
		/* Already something there. We can re-use the storage, as it was 
		 * malloc()ed anyway. */
		BUG_ON(listd.dsize != sizeof(list));

		/* status is still 0, so per default the list would get written. */
		dst=(struct hsh___list*)listd.dptr;
		if (dst->count == HASH__LIST_MAX)
		{
			/* On dst->count reaching MAX_DUPL_ENTRIES we have to write the 
			 * structure once again. */
			/* Later we could avoid it ... although it 
				 might be interesting how many there are. Maybe even resizing? */
			dst->count++;
		}
		else if (dst->count > HASH__LIST_MAX)
		{
			/* No more writes needed. */
			status=EFBIG;
		}
		else
		{
			/* Normal operation, still space available. */
			dst->entries[ dst->count ] = value;
			dst->count++;
		}
	}

	if (status != EFBIG)
		STOPIF( hsh__store(hash, key, listd), NULL);

ex:
	return status;
}


/** -.
 * If \a next_key is not \c NULL, it is set so that the next query can use 
 * this key for the next element.
 * If \c *next_key==NULL, no next element is there; everything has been 
 * returned.
 * If no (more) entry could be found, \c ENOENT is returned, and \c 
 * *found==0.
 *
 * In case of storage via an array hsh__list must store some internal 
 * state; therefore only a single loop is (currently) possible.
 * */
int hsh__list_get(hash_t hash, 
		datum current_key, datum *next_key, 
		struct estat **arr[], int *found)
{
	int status;
	/* We ensure at least a single NULL at the end. */
	static struct estat *data[HASH__LIST_MAX+1];
	datum value;
	struct hsh___list *list;
	int c;


	status=0;
	*found=0;
	*arr=NULL;
	/* No next key ... we return the array. */
	if (next_key) 
	{
		next_key->dsize=0;
		next_key->dptr=0;
	}

	status=hsh__fetch( hash, current_key, &value);
	/* Nothing here? Just report that. */
  if (status == ENOENT) goto ex;
	STOPIF(status, NULL);

	list=(struct hsh___list*)value.dptr;

	memset(data, 0, sizeof(data));

	c=list->count;
	/* Overflow? */
	if (c == HASH__LIST_MAX+1) c=HASH__LIST_MAX;

	BUG_ON( c<=0 || c>HASH__LIST_MAX, "number of entries=%d", c);
	memcpy(data, list->entries, c*sizeof(data[0]));

	*found=c;
	*arr=data;

ex:
	return status;
}


int hsh__register_delete(hash_t db, datum key)
{
	int status;
	const datum data= { .dsize=1, .dptr="\0", };

	status=0;

	if (!db->to_delete)
	{
		STOPIF( hsh___new_bare(NULL, "del", HASH_TEMPORARY,
					&(db->to_delete), NULL), 
				NULL);
	}

	DEBUGP("storing %s", key.dptr);

	status=gdbm_store(db->to_delete, key, data, GDBM_REPLACE);
	STOPIF_CODE_ERR(status < 0, errno, "Error writing key");

ex:
	return status;
}


/** @} */

