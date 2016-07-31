/************************************************************************
 * Copyright (C) 2005-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __WAA_H__
#define __WAA_H__

#include <ctype.h>

#include "global.h"
#include "actions.h"


/** \file
 * WAA functions header file. */


/** Max macro.
 * Taken from gcc info manual. \c >? and \c <? are only for C++. */
#define max(a,b) 							\
	({ typeof (a) _a = (a); 				\
	   typeof (b) _b = (b);					\
	 _a > _b ? _a : _b; })


/** Entry list for disk-order update.
 * This structure is used to store a linked list of struct \c estat
 * in (mostly) ascending inode order. It is used in \c waa__update_tree()
 * to \c lstat() all (needed) entries in (hopefully) an order which minimizes
 * the backtracking of the storage media.
 * What this means is: The harddisk head should go straight in one direction,
 * and avoid seeking as much as possible. */
struct waa__entry_blocks_t {
/** Pointer to packed struct \c estat array. */
  struct estat *first;
	/** Number of entries in array */
	int count;
	/** Pointers for linked list. @{ */
	struct waa__entry_blocks_t *next, *prev;
	/** @} */
};


/** First block for to-be-updated pointers. */
extern struct waa__entry_blocks_t waa__entry_block;



/** \defgroup waa_files Files used by fsvs
 * \ingroup compat
 *
 * FSVS uses various files to store its configuration and informations 
 * about the system it is running on.
 *
 * Two file trees are used:
 * - \c /var/spool/fsvs (if not overridden by \ref WAA__PATH_ENV 
 *   "$FSVS_WAA").
 *   The WAA stores volatile data that should not be backed up; the files 
 *   have only lower-case letters.
 * - \c /etc/fsvs (or \ref CONF__PATH_ENV "$FSVS_CONF")
 *   This is used to store configuration data, eg.  for the working copies.  
 *   The names of files stored here have the first letter in upper-case.
 *   Having this data backed-up (eg. along with the rest of the filesystem) 
 *   allows easy recovery of the configuration.
 *   The single exception are the \ref dir Files; these are, strictly seen, 
 *   per working copy, but are stored in the spool directory, as they are 
 *   reconstructed on restore and would only give conflicts with old 
 *   versions.
 *
 * @{ */

/** \anchor waa_wc \name Per working copy
 * These are stored in a subdirectory of the WAA, named by the MD5-sum of 
 * the WC-path.
 * @{ */
/** \anchor urls List of URLs. */
#define WAA__URLLIST_EXT		"Urls"
/** \anchor dir List of files/subdirs/devices/symlinks in and below this
 * working copy directory. 
 * See also \a waa__output_tree().
 * This file is an small exception - it is stored in \c /etc/fsvs. */
#define WAA__DIR_EXT		"dir"
/** \anchor ign List of ignored patterns */
#define WAA__IGNORE_EXT		"Ign"
/** \anchor copy Hash of copyfrom relations.
 * The key is the destination-, the value is the source-path; they are 
 * stored relative to the wc root, without the leading \c "./", ie. as \c 
 * "dir/test". The \c \\0 is included in the data.  */
#define WAA__COPYFROM_EXT		"Copy"
/** @} */

/** \anchor waa_file \name Per file/directory
 * The cached informations (per-file) are located in the \c cache 
 * subdirectory of the WAA; two subdirectory levels are created below that.  
 *
 * @{ */
/** \anchor md5s List of MD5s of the manber blocks of a file -
 * for faster change detection on large files.
 *
 * Furthermore in the WAA directory of the working copy we store a 
 * (temporary) file as an index for all entries' MD5 checksums. */
#define WAA__FILE_MD5s_EXT	"md5s"
/** \anchor prop List of other properties.
 * These are properties not converted to meta-data. */
#define WAA__PROP_EXT		"prop"
/** @} */

/** \anchor ino
 * \name Temporary copy/move detection database
 * Entries are addressed by device and inode.
 * @{ */
/** For files */
#define WAA__FILE_INODE_EXT		"fino"
/** For directories */
#define WAA__DIR_INODE_EXT		"dino"
/** @} */

/** @} */



/* this should be optimized into a constant.
 * verified for gcc (debian 4.0.0-7ubuntu2) */
#define WAA__MAX_EXT_LENGTH max(																\
		max( max(strlen(WAA__DIR_EXT), strlen(WAA__PROP_EXT) ),			\
			strlen(WAA__FILE_MD5s_EXT) ),	\
		max(strlen(WAA__IGNORE_EXT), strlen(WAA__URLLIST_EXT) )			\
	)


/** Store the current working directory. */
int waa__save_cwd(char **where, int *len, int *buffer_size);
/** Initialize WAA operations. */
int waa__init(void);
/** Create a directory; ignore \c EEXIST. */
int waa__mkdir(char *dir);
/* Given an \a path and an \a extension, this function returns a 
 * \a filehandle that was opened for this entry in the WAA with \a flags. */
int waa__open(char *path,
		const char *extension,
		int mode,
		int *filehandle);
/** This function closes a writable filehandle that was opened in the
 * WAA via \c waa__open(). */
int waa__close(int filehandle, int has_failed);
/** Wrapper function. Opens a \c dir-file for the \a directory in the WAA.
 * \todo Should possibly be eliminated. */
int waa__open_dir(char *directory,
		int write,
		int *fh);
/** Creates the entries tree below \c root . */
int waa__build_tree(struct estat *root);
/** Write the \ref dir file for this \c root . */
int waa__output_tree(struct estat *root);
/** Read the \ref dir file for the current working directory. */
int waa__input_tree(struct estat *root,
		struct waa__entry_blocks_t **blocks,
		action_t *callback);
/** Wrapper function for \c waa__open(). */
int waa__open_byext(char *directory,
		char *extension,
		int write,
		int *fh);
/** Wrapper for \c waa__load_repos_urls_silent(). */
int waa__load_repos_urls(char *dir, int reserve_space);
/** Load the URLs associated with \a dir (or current working directory, if
 * \a dir is \c NULL). */
int waa__load_repos_urls_silent(char *dir, int reserve_space);
/** Returns the given directory or, if \c NULL , \c getcwd() . */
int waa__given_or_current_wd(char *directory, char **erg);
/** Creates a symlink in the WAA. */
int waa__make_info_link(char *directory, char *name, char *dest);
/** This function takes a \a path and an \a extension and tries to remove
 * the associated file in the WAA. */
int waa__delete_byext(char *path, 
		char *extension,
		int ignore_not_exist);
/** Reads the entry tree or, if none stored, builds one. */
int waa__read_or_build_tree(struct estat *root, 
		int argc, char *normalized[], char *orig[],
		action_t *callback,
		int return_ENOENT);


/** Given a list of path arguments the \a base path and relative paths
 * are returned. */
int waa__find_common_base(int argc, char *args[], 
		char ***normalized_paths);
/** Similar to \ref waa__find_common_base(), but allows only specification 
 * of a WC root. */
int waa__find_base(struct estat *root, int *argc, char ***args);


/** \name Building paths for FSVS's datafiles.
 * @{ */
/** The path should be in the WAA. */
#define GWD_WAA (1)
/** The path should be in the configuration area. */
#define GWD_CONF (2)
/** The intermediate directories should be created. */
#define GWD_MKDIR (4)
/** This function determines the directory used in the WAA area for the 
 * given \a path. */
int waa__get_waa_directory(char *path, 
		char **erg, char **eos, char **start_of_spec,
		int flags);
/** Function that returns the right flag for the wanted file.
 * To be used in calls of \ref waa__get_waa_directory(). */
static inline int waa__get_gwd_flag(const char *const extension)
{ return !extension || (isupper(extension[0])) ? GWD_CONF : GWD_WAA; }
/** @} */

/** This function traverses the tree and sets \c entry_status for the 
 * marked entries. */
int waa__update_tree(struct estat *root, 
		struct waa__entry_blocks_t *blocks);
/** Insert an \a entry block with \a count entries into the 
 * \a waa__entry_blocks list, to get it updated later 
 * by \a waa__update_tree(). */


/** The list of entries to be updated is registered after the given block.  
 * */
int waa__new_entry_block(struct estat *entry, int count, 
		struct waa__entry_blocks_t *previous);
/** Simple wrapper; inserts entries at the start of the list. */
static inline int waa__insert_entry_block(struct estat *entry, int count)
{ return waa__new_entry_block(entry, count, &waa__entry_block); }

/** The given paths are looked for in the entries tree, are marked
 * for update, and their parents are flagged. */
int waa__partial_update(struct estat *root, 
		int argc, char *normalized[], char *orig[],
		struct waa__entry_blocks_t *blocks);

/** This function traverses the tree and calls the handler function
 * for the marked entries. */
int waa__do_sorted_tree(struct estat *root, action_t handler);

/** A wrapper around dir__enumerator(), ignoring entries below \c 
 * $FSVS_WAA.  */
int waa__dir_enum(struct estat *this,
		int est_count,
		int by_name);


/** How many bytes the \ref dir file header has. */
#define HEADER_LEN (64)
/** Which version does the dir file have? */
#define WAA_VERSION (6)

/** Copy URL revision number.
 * The problem on commit is that we send a number of entries to the 
 * repository, and only afterwards we get to know which revision number
 * they got.
 * To avoid having to run through the whole tree again we use this special
 * marker, which gets set on the committed entries, to be corrected on
 * ops__save_1entry(). */
#define SET_REVNUM (-12)


/** Our current WC base. */
extern char *wc_path;
/** How much bytes the \ref wc_path has. */
extern int wc_path_len;

/** Buffers for temporary filename storage.
 * @{ */
extern char *waa_tmp_path, *waa_tmp_fn,
			 *conf_tmp_path, *conf_tmp_fn;
/** @} */


#endif

