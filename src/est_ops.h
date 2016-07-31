/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __EST_OPS_H__
#define __EST_OPS_H__


#include "global.h"
#include "waa.h"

/** \file
 * Functions for handling of indiviual <tt>struct estat</tt>s. */


/** Return the path of this entry. */
int ops__build_path(char **path, 
		struct estat *sts);
/** Calculate the length of the path for this entry. */
int ops__calc_path_len(struct estat *sts);
/** Get the filetype from \c st->mode. */
int ops___filetype(struct sstat_t *st);
/** Compare the \c struct \c sstat_t , and set the \c entry_status. */
int ops__stat_to_action(struct estat *sts, struct sstat_t *new);

/** \name Finding entries */
/** @{ */
/** Find an entry in the \a dir by \a bname. */
int ops__find_entry_byname(struct estat *dir, 
		const char *name, 
		struct estat **sts, int ignored_too);
/** Find an entry in \a dir by the inode. */
int ops__find_entry_byinode(struct estat *dir, 
		dev_t dev, 
		ino_t inode, 
		struct estat **sts);
/** Value for unknown indizes in \c ops__delete_entry(). */
#define UNKNOWN_INDEX (-1)
/** Delete an entry by either \a index_byinode, or \a index_byname, or \a 
 * sts. */
int ops__delete_entry(struct estat *dir, 
		struct estat *sts, 
		int index_byinode, 
		int index_byname);
/** @} */

/** This function returns blocks of (struct estat), possibly smaller than 
 * wanted by the caller. */
int ops__allocate(int needed, struct estat **where, int *count);
/** Frees the memory associated with this entry and all its children. */
int ops__free_entry(struct estat **sts_p);
/** Frees all "marked" entries in the given directory at once. */
int ops__free_marked(struct estat *dir, int fast_mode);
/** Appends the array of \a count \a new_entries as children to \a dir. */
int ops__new_entries(struct estat *dir,
		int count,
		struct estat **new_entries);

/** Writes a textual description of the given \a sts to the \a filehandle.  
 * */
int ops__save_1entry(struct estat *sts,
		ino_t parent_ino,
		int filehandle);
/** Fills \a sts from a buffer \a where. */
int ops__load_1entry(char **where, struct estat *sts, char **filename,
		ino_t *parent_i);
/** Does a \c lstat() on the given entry, and sets the \c entry_status. */
int ops__update_single_entry(struct estat *sts, char *fullpath);

/** Converts a string describing a special node to the \c struct \c sstat_t 
 * data. */
int ops__string_to_dev(struct estat *sts, char *data, char **info);
/** Converts a device entry into a string suitable for storage in the WAA 
 * area (using a \c : separator). */
char *ops__dev_to_waa_string(struct estat *sts);
/** See \c ops__dev_to_waa_string(), but uses a space character (\c \\x20 ) 
 * for subversion compatibility. */
char *ops__dev_to_filedata(struct estat *sts);
/** Reads a file. */
int ops__read_special_entry(apr_file_t *a_stream,
		char **data,
		int max, ssize_t *real_len,
		char *filename,
		apr_pool_t *pool);

/** Reads a symlink and returns a pointer to its destination. */
int ops__link_to_string(struct estat *sts, char *filename,
		char **erg);

/** Returns the filename. */
const char *ops__get_filename(const char *path);

/** Copies the data of a single struct estat. */
void ops__copy_single_entry(struct estat *src, struct estat *dest);

/** Create or find an entry below parent. */
int ops__traverse(struct estat *parent, char *relative_path, 
		int flags, int sts_flags,
		struct estat **ret);

/** Set the \ref estat::do_tree and \ref estat::do_this_entry attributes 
 * depending on \ref opt_recursive and the parent's bits. */
int ops__set_to_handle_bits(struct estat *sts);


/** Correlating entries from two directories \a dir_a and \a dir_B.
 * @{ */
/** Callback function type for A-only and B-only elements.
 * The first parameter is a pointer to the current struct estat; the other 
 * is the pointer to the pointer in the directory structure.
 * Not often needed ... could be done by var_args. */
typedef int (*ops__correlate_fn1_t)(struct estat *, struct estat **);
typedef int (*ops__correlate_fn2_t)(struct estat *, struct estat *);
/** The function to go through the lists. */
int ops__correlate_dirs(struct estat *dir_A, struct estat *dir_B,
		ops__correlate_fn1_t only_A,
		ops__correlate_fn2_t both,
		ops__correlate_fn1_t only_B,
		ops__correlate_fn2_t for_every);
/** @} */


/** Startstrings for links in the repository. */
extern const char link_spec[],
			cdev_spec[],
			bdev_spec[];

#define ops__mark_childchanged(start, field)           \
do {                                                    \
	register struct estat *_s=(start);                    \
  while (_s && !(_s->field & FS_CHILD_CHANGED))         \
	{                                                     \
	  _s->field |= FS_CHILD_CHANGED;                      \
		_s=_s->parent;                                      \
	}                                                     \
} while (0)

#define ops__mark_parent_cc(changed_entry, field)       \
   ops__mark_childchanged(changed_entry->parent, field)

#define ops__mark_changed_parentcc(changed_entry, field)     \
do {                                                    \
	changed_entry->field |= FS_CHANGED;                   \
  ops__mark_parent_cc(changed_entry, field);            \
} while (0)


#endif

