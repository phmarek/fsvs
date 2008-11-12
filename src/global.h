/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __GLOBAL_H__
#define __GLOBAL_H__

#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <gdbm.h>
#include <stddef.h>
#include <sys/types.h>
#include <apr_md5.h>
#include <apr_file_io.h>
#include <subversion-1/svn_ra.h>
#include <subversion-1/svn_string.h>
#include <pcre.h>


/** \file
 * Global definitions.
 * Here are things defined that are needed in almost every action. */

/** If this pointer is non-\c NULL, free() it and set it 
 * to \c NULL, to avoid repeated free()ing. */
#define IF_FREE(x) do { if (x) free(x); x=NULL; } while (0)

/* \addtogroup compati */
/* @{ */
/** The system-specific character to delimit directories.
 * Surely there's something like that in APR somewhere. */
#define PATH_SEPARATOR ('/')
/** The system-specific character to be used before environment variables.
 * For DOS-compatibility printing that behind the name would be necessary, 
 * too - therefore it's not automatic per \c configure. */
#define ENVIRONMENT_START ('$')
/** @} */

/** We have some functions which don't use all parameters. */
#define UNUSED __attribute__  ((__unused__))


/** A type for holding an MD5 digest.
 * Only the digest, not the working data set. */
typedef unsigned char md5_digest_t[APR_MD5_DIGESTSIZE];



/** \addtogroup PrintfTypes Types for consistent printf() output.
 * \ingroup compati
 *
 * These types are used to get consistent stack usage for \c printf() 
 * and friends.
 * The types for eg. \a ino_t vary in different architectures -- they are
 * 32 or 64bit. If we just pushed the arch-dependent size on the stack,
 * but used always the same format string, \c printf() would (on some
 * architectures) take the wrong argument off the stack, which results
 * in corrupted output of later data.  */
/** @{ */
typedef unsigned long long t_ull;
typedef long long t_ll;
typedef unsigned long t_ul;
/** @} */


/** \anchor PatTypes
 * \name Pattern types.
 *
 * The ignore/take specifications can be in several forms; please
 * see the doc/IGNORING documentation.  */
/** @{ */
#define PT_SHELL (1)
#define PT_PCRE (2)
#define PT_DEVICE (3)
#define PT_INODE (4)
#define PT_SHELL_ABS (5)

/**  Data storage for ignore patterns. */
struct ignore_t {
	/** The pattern string as given by the user, including flags. */
	char *pattern;

	/** The calculated pattern string.
	 * Does no longer include the flags (like \e take), and shell syntax 
	 * is converted to PCRE. */
	char *compare_string;

	union {
		/* for shell and pcre */
		struct {
			/** PCRE main data storage */
			pcre *compiled;
			/** PCRE extra data storage. Currently nearly unused. */
			pcre_extra *extra;
			/** This member should be used to know at which levels in the
			 * path hierarchy this pattern applies, to avoid trying to match
			 * at certainly not-matching strings.
			 *
			 * In case of something like \verbatim
			 * 		dir/a*§/b*§/§*.old
			 * \endverbatim the directory \c dir and below should get this pattern, 
			 * but on the subdir_list.
			 * In case of a pattern with ** we have to give that to everyone
			 * below. 
			 * \todo currently unused. */
			unsigned short path_level;
			/** Flag telling whether this shell pattern has a \c ** in it.
			 * \todo Would be used with ignore_t::path_level. */
			unsigned int has_wildwildcard:1;
		};

		/** For device compares */
		struct {
			/** The major number */
			int major;
			/** The minor number */
			int minor;
			/** \see PatDevComp */
			char compare;
			/** Flag saying whether a minor number was given, or if only the major
			 * number was told. */
			char has_minor;
		};

		/** Inode compares are easy: just an inode number and a device. */
		struct {
			/** Inode number */
			ino_t inode;
			/** Device */
			dev_t dev;
		};
	};

	/** AND-value for mode matching, or \c 0 for not chosen. */
	unsigned short mode_match_and;
	/** CMP-value for mode matching. */
	unsigned short mode_match_cmp;

	/** Should this match only directories? */
	unsigned int dir_only:1;
	/** Is this an ignore or take pattern?  \a 0 = take, \a 1 = ignore */
	unsigned int is_ignore:1;
	/** Ignore case for comparing? */
	unsigned int is_icase:1;
	/** Is it an \e internally generated pattern (for the WAA area)?
	 * Internal patterns are not saved and not printed. */
	unsigned int is_user_pat:1;

	/** Which type is this pattern? See \ref PatTypes. */
	/* This is at the end because of alignment issues. */
	unsigned int type:3;
};

/** Whether the device compare should be 
 * for \e equal, \e lesser or \e higher devices.
 *
 * If the device numbers get completely randomized (was discussed
 * on \c linux-kernel some time ago) this will be useless;
 * we'll have to add another pattern type like eg. 
 * <em>all devices with a major number like the device node 
 * \c /dev/ram0</em> to make sense again.  */
#define PAT_DEV__UNSPECIFIED (0)
#define PAT_DEV__LESS (1)
#define PAT_DEV__EQUAL (2)
#define PAT_DEV__GREATER (4)
#define PAT_DEV___INVALID_MASK (PAT_DEV__LESS | PAT_DEV__GREATER)
#define PAT_DEV__HAVE_MINOR (0x80)
/** @} */


/** The special value used for a not-yet-valid url_t::internal_number. */
#define INVALID_INTERNAL_NUMBER (-1)
/** All the data FSVS must know about an URL. */
struct url_t {
	/** The URL itself (http:// or svn:// or similar) */
	char *url;
	/** The user-given priority; need not be unique. 
	 * The lower the number, the higher the priority. */
	int priority; 
	/** The length of the URL, not counting the \c \\0. */
	int urllen;
	/** The revision we'd like that URL to be at - normally HEAD. */
	svn_revnum_t target_rev;
	/** The revision the user gave <B>for this command</b> for this URL.
	 * Normally equals \c target_rev. */
	svn_revnum_t current_target_rev;
	/** The revision number this URL is currently at. */
	svn_revnum_t current_rev;
	/** The \c HEAD revision, or \c SVN_INVALID_REVNUM if not yet known. */
	svn_revnum_t head_rev;
	/** The user-given symbolic name */
	char *name;
	/** The number which is used in the dir-lists to reference this url_t.
	 * Must be unique in the URL-list. 
	 *
	 * This is a different thing as the priority - the user must be able
	 * to change the priority of the URLs, without changing our internal
	 * references! */
	int internal_number;
	/** A count of entries using this url. Used for determining if it's still
	 * needed. */
	unsigned count;
	/** A session connected to this URL. 
	 * \todo Session sharing for URLs in the same repository. */
	svn_ra_session_t *session;
	/** The pool this session was allocated in. */
	apr_pool_t *pool;
	/** Flag saying whether this URL should be done.
	 * Should not be queried directly, but by using url__to_be_handled().  */
	int to_be_handled:1;
	/** Whether the user gave a specific override revision number. */
	int current_target_override:1;
};


/** \addtogroup Entries Entry data storage.
 * \ingroup dev
 *
 * The basic data structure for entries; an entry can be a file, directory, 
 * or special node (symlink or device).
 * */
/** @{ */
/** A shortened struct stat64. 
 * The glibc version needs 96 bytes, this version only 52 bytes.
 *
 * We save 44 bytes; multiplied with 150 000 entries this makes a difference
 * of 6.6MB. */
/* We may not use the st_ names, as some have #defined names. */
/* On my system these are all either 64bit or 32bit types, so there's
 * no alignment problem (which would arise if there was eg. a 16bit type,
 * then space would be wasted) */
struct sstat_t {
	/* For easier comparison, we overlay an 64bit type. */
	union {
		/** The modification time as \c seconds, \c microseconds. */
		struct timespec mtim;
		/** The same value in a single integer value.
		 * \deprecated Currently unused. */
		unsigned long long _mtime;
	};
	union {
		/** The creation time as \c seconds, \c microseconds. */
		struct timespec ctim;
		/** The same value in a single integer value.
		 * \deprecated Currently unused. */
		unsigned long long _ctime;
	};

	union {
		/** The size in bytes (for files, symlinks and directories). */
		off_t	size;
		/** The device number (for devices). */
		dev_t rdev;
	};

	/** Device number of \b host filesystem. */
	dev_t dev;
	/** Inode */
	ino_t ino;

	/** The access mode (like \c 0700, \c 0755) with all other (non-mode) 
	 * bits, ie S_IFDIR. */
	mode_t mode;

	/** The owner's id. */
	uid_t uid;
	/** The group number. */
	gid_t gid;
};



/** The central structure for data storage.
 *
 * The name comes from <em>extended struct stat</em>.
 * This structure is used to build the tree of entries that we're processing.
 *
 * We need both a local and a remote status, to see on update when there 
 * might be a conflict. \todo Single status, and check entries on-time?
 */
struct estat {
	/** The parent of this entry, used for tree walking.
	 * Must be \c NULL for the root entry and the root entry alone. */
	struct estat *parent;
	/** Name of this entry. */
	char *name;

	/** Meta-data of this entry. */
	struct sstat_t st;

	/** Revision of this entry. Currently only the value in the root entry is 
	 * used; this will be moved to \c * \ref url and removed from here. */
	svn_revnum_t repos_rev;
	/** The revision number before updating. */
	svn_revnum_t old_rev;

	/** The URL this entry is from.
	 * Will be used for multi-url updates. */
	struct url_t *url;

	/** If an entry gets removed, the old version is remembered (if needed) 
	 * via the \c old pointer (eg to know which children were known and may 
	 * be safely removed).  */
	struct estat *old;

	/** Data about this entry. */
	union {
		/** For files */
		struct {
			/** The decoder string from fsvs:update-pipe. Only gets set if 
			 * action->needs_decoder != 0. */
			char *decoder;
			/** MD5-hash of the repository version.  While committing it is set 
			 * to the \e new MD5, and saved with \a waa__output_tree(). */
			md5_digest_t md5;
			/** Whether we got an "original" MD5 from the repository to compare.  
			 * */
			unsigned int has_orig_md5:1;
			/** Flag whether this entry has changed or not changed (as per 
			 * MD5/manber-compare), or if this is unknown yet.
			 * See \ref ChgFlag. */
			unsigned int change_flag:2;
		};
		/** For directories.
		 * The by_inode and by_name members are positioned so that they collide 
		 * with the \c md5 file member above - in case of incorrect file types 
		 * that's safer, as they'll contain invalid pointers instead of (the 
		 * valid) \c decoder.  */
		struct {
			/** Name storage space for sub- (and sub-sub- etc.) entries.
			 * Mainly used in the root inode, but is used in newly found directories
			 * too. \c NULL for most directory entries. */
			char *strings;
			/** List of child entries.
			 * Sorted by inode number, NULL-terminated.
			 * Always valid. */
			struct estat **by_inode;
			/** List of child entries.
			 * Sorted by name, NULL-terminated. 
			 * May be NULL if not currently used; can be (re-)generated by calling
			 * dir__sortbyname(). */
			struct estat **by_name;

			/** How many entries this directory has. */
			AC_CV_C_UINT32_T entry_count;

			/** Used to know when this directories' children are finished.
			 * Counts the number of unfinished subdirectories.
			 * This is volatile and should be in the union below (with \ref 
			 * estat::child_index), but as it's only used for directories it 
			 * conserves memory to keep it here. */
			AC_CV_C_UINT32_T unfinished;

			/** This flag is set if any child is *not* at the same revision,
			 * so this directory has to be descended on reporting. */
			unsigned int other_revs:1;
			/** If this bit is set, the directory has to be re-sorted before
			 * being written out -- it may have new entries, which are not in
			 * the correct order. */
			unsigned int to_be_sorted:1;
			/* Currently unused - see ignore.c. */
#if 0
			struct ignore_t **active_ign;
			struct ignore_t **subdir_ign;
#endif
		};
	};

	/** These are for temporary use. */
	union {
		/** For commit. */
		struct {
			/** This entries' baton. */
			void *baton;
		};
		/** Export for a file. */
		struct {
			/** The pool used for the filehandles; for a discussion see \ref FHP. */
			apr_pool_t *filehandle_pool;
		};
		/** Export of a special entry. */
		struct {
			/** String-buffers for special entries.
			 * While a file is \b always streamed to disk, special entries are
			 * \b always done in memory. */
			svn_stringbuf_t *stringbuf_tgt;
		};
		struct {
			/** Used in waa__input_tree() and waa__update_tree(). */
			AC_CV_C_UINT32_T child_index;
		};
		/* output_tree */
		struct {
			AC_CV_C_UINT32_T file_index;
		};
	};


	/** \name Common variables for all types of entries. */
	/** Which argument causes this path to be done. */
	char *arg;

	/** Stored user-defined properties as \c name=>svn_string_t, if \c 
	 * action->keep_user_prop is set.
	 * Allocated in a subpool of \c estat::url->pool, so that it's still 
	 * available after cb__record_changes() returns.
	 * The subpool is available from a hash lookup with key "" (len=0). */
  apr_hash_t *user_prop;

	/** Updated unix mode from ops__update_single_entry().
	 * See the special \ref fsvsS_constants below.
	 * \todo Strip that to only the needed bits, ie. the ones (used) in \c 
	 * S_IFMT, and make estat compact, by putting between the bitfields.
	 * Convention: has \b always to be set according to the current type of 
	 * the entry (to account for the shared members, eg. by_inode); where 
	 * estat::st.mode has the \e original value, as seen by the repository. */
	mode_t updated_mode;

	/** Flags for this entry. See \ref EntFlags for constant definitions. */
	AC_CV_C_UINT32_T flags;

	/** Local status of this entry - \ref fs_bits. */
	unsigned int entry_status:10;

	/** Remote status of this entry. \ref fs_bits. */
	unsigned int remote_status:10;

	/** Cache index number +1 of this entries' path.
	 * \c 0 (and \c >MAX_CACHED_PATHS) is used as \e uninitialized; so the 
	 * value here has range of <tt>[1 .. MAX_CACHED_PATHS]</tt> instead of 
	 * the usual <tt>[0 .. MAX_CACHED_PATHS-1]</tt>. */
	unsigned int cache_index:6;

	/** Length of path up to here. Does not include the \c \\0. See \ref 
	 * ops__calc_path_len. */
	unsigned short path_len:16;

	/** At which level is this path? The wc root is at level 0, its children 
	 * at 1, and so on. */
	unsigned short path_level:9;

	/** Whether this entry was already printed. \todo Remove by changing the 
	 * logic. */
	unsigned int was_output:1;
	/** This flag tells whether the string for the decoder is surely correct.
	 * It is currently used for updates; after we parse the properties in 
	 * cb__record_changes(), we'll have the correct value. */
	unsigned int decoder_is_correct:1;

	/** Flag saying whether this entry was specified by the user on the 
	 * command line. */
	unsigned int do_userselected:1;
	/** Says that a child of this entry was given by the user on the 
	 * commandline.
	 * Unlike \a FS_CHILD_CHANGED, which is set if some child has \e actually 
	 * changed, this just says that we have to check. */
	unsigned int do_child_wanted:1;
	/** Flag derived from parents' \ref estat::do_userselected.
	 * Set for \b all entries which should be handled. */
	unsigned int do_this_entry:1;
	/** Flag saying whether the \c "-f" filter condition applies.
	 * Normally set in \ref ops__set_todo_bits(), can be cleared in \ref 
	 * ops__update_filter_set_bits(). */
	unsigned int do_filter_allows:1;
	/** Flag used for debugging. If estat::do_filter_allows is queried 
	 * without being defined earlier, we trigger a \ref BUG().
	 * Was conditionalized on \c ENABLE_DEBUG - but that got ugly. */
	unsigned int do_filter_allows_done:1;

	/** Whether this entry should not be written into the \ref dir "entry 
	 * list", and/or ignored otherwise. */
	unsigned int to_be_ignored:1;
};

/** \anchor fsvsS_constants Special FSVS file type constants.
 * @{ */
#define S_IFUNDEF (0)
/** All sockets get filtered out when the directory gets read, so we can 
 * safely reuse that value for the case where we don't know \b what kind of 
 * special entry that is (eg when receiving \c "svn:special" from the 
 * repository). */
#define S_IFANYSPECIAL S_IFSOCK
#define S_ISANYSPECIAL S_ISSOCK
/** @} */

/** \anchor EntFlags Various flags for entries.
 *
 * The RF means repos-flags, as these flags have a meaning when talking
 * to the repository.  */
/** This item will be unversioned, ie remotely deleted and locally
 * purged from the \b tree, but not from the filesystem. */
#define RF_UNVERSION (1)
/** Such an entry will be sent to the repository as a new item.
 * Used if this entry would get ignored by some pattern, but the user
 * has specifically told to take it, too. */
#define RF_ADD (2)
/** This entry should be checked for modifications. 
 * Is currently used for directories; if they are stored in the WAA with
 * their current mtime they wouldn't get checked for modifications.
 * Using this flag it's possibly to specify that they should be read. 
 * \note Persistent until commit! */
#define RF_CHECK (4)
/** Properties have changed locally, must be committed.
 * Needed in case this is the \b only change - else we would not commit 
 * this entry. */
#define RF_PUSHPROPS (8)
/** Set if this entry was marked as copy. 
 * If it is a directory, the children will have the \c RF_COPY_SUB flag, 
 * unless the copy attribute is not inherited, but they're themselves 
 * copies from other entries. */
#define RF_COPY_BASE (16)
/** Set if this entry got implicitly copied (sub-entry). */
#define RF_COPY_SUB (32)
/** Has this entry a conflict? */
#define RF_CONFLICT (64)
/** Whether this entry was just created by \a ops__traverse(). */
#define RF_ISNEW (1<<19)
/** Print this entry, even if not changed.
 * More or less the same as opt_verbose, but per entry. */
#define RF_PRINT (1 << 20)

/** Which of the flags above should be stored in the WAA. */
#define RF___SAVE_MASK (RF_UNVERSION | RF_ADD | RF_CHECK | \
		RF_COPY_BASE | RF_COPY_SUB | RF_PUSHPROPS | RF_CONFLICT)
/** Mask for commit-relevant flags.
 * An entry with \c RF_COPY_BASE must (per definition) marked as \c RF_ADD; 
 * and RF_PUSHPROPS gets folded into FS_PROPERTIES. */
#define RF___COMMIT_MASK (RF_UNVERSION | RF_ADD | RF_COPY_BASE | RF_PUSHPROPS)
#define RF___IS_COPY (RF_COPY_BASE | RF_COPY_SUB)



/** \name File statii.
 * \anchor fs_bits
 * */
/** @{ */
#define FS_NO_CHANGE (0)
#define FS_NEW (1 << 0)
#define FS_REMOVED (1 << 1)
#define FS_CHANGED (1 << 2)
/** This flag says that it's an "approximate" answer, - no hashing has been 
 * done. */
#define FS_LIKELY (1 << 3)
#define FS_REPLACED (FS_NEW | FS_REMOVED)

/** Flag for update/commit. Note that this doesn't normally get set when a  
 * property has been changed locally - for that the persistent flag 
 * RF_PUSHPROPS is used.
 * */
#define FS_PROPERTIES (1 << 4)


/** Meta-data flags. */
#define FS_META_MTIME (1 << 5)
#define FS_META_OWNER (1 << 6)
#define FS_META_GROUP (1 << 7)
#define FS_META_UMODE (1 << 8)

#define FS_META_CHANGED (FS_META_MTIME | FS_META_OWNER | \
		FS_META_GROUP | FS_META_UMODE)

/** This flag on a directory entry says that the directory itself was
 * not changed, but some child, so the children of this directory
 * have to be checked for modifications. */
#define FS_CHILD_CHANGED (1 << 9)

#define FS__CHANGE_MASK (FS_NEW | FS_REMOVED | FS_CHANGED | \
		FS_META_CHANGED | FS_PROPERTIES)
/** @} */


/** \anchor ChgFlag Change detection flags. */
#define CF_UNKNOWN (0)
#define CF_CHANGED (1)
#define CF_NOTCHANGED (2)
/** @} */


/** \section TravFlags Flags defining the behaviour of \ref ops__traverse() 
 * and sub-calls. */
/** Non-existing paths should be created */
#define OPS__CREATE (1)
/** Newly created entries should be put on the update list */
#define OPS__ON_UPD_LIST (2)
/** Whether this entry \b has to exist in the cached tree -- normally that 
 * means it must not be created.  */
#define OPS__FAIL_NOT_LIST (4)
/** With this flag \a ops__traverse returns \c ENOENT if the entry does not 
 * exist in the local filesystem. */
#define OPS__FAIL_NOT_FS (16)



/** \addtogroup Debug Debugging and error checking.
 * \ingroup dev
 *
 * Depending on configure options (\c --enable-debug, \c --enable-release)
 * and system state (valgrind found) various debug and error check functions
 * are compiled out.
 * Makes the binary much smaller, but leaves no chance for debugging. */
/** @{ */
#ifdef ENABLE_RELEASE
#define DEBUGP(...) do { } while (0)
#else
/** Declaration of the debug function. */
extern void _DEBUGP(const char *file, int line, const char *func, char *format, ...)
	__attribute__ ((format (printf, 4, 5) ));
	/** The macro used for printing debug messages.
	 * Includes time, file, line number and function name. 
	 * Allows filtering via opt_debugprefix.
	 * \note Check for \ref PrintfTypes "argument sizes". */
#define DEBUGP(...) _DEBUGP(__FILE__, __LINE__, __PRETTY_FUNCTION__, __VA_ARGS__)
#endif


	/** \name Error-printing and -handling functions. 
	 *
	 * Except for the subversion-library wrapper macros they need exactly this
	 * function layout:
	 *
	 * \code
	 *   int some_function( ... some args ... )
	 *   {
	 *     int status;
	 *
	 *     STOPIF( function_call(1, 2, "a"), 
	 *       "String describing the error situation with %s",
	 *       "parameters as needed");
	 *
	 *   ex:
	 *     cleanups();
	 *     return status;
	 *   }
	 * \endcode
	 *
	 * It works by checking the return value; if it is not zero, a 
	 * <tt>goto ex</tt> is done. At this mark some cleanup is possible. */
	/** @{ */
	/** A flag to turn error printing temporarily off.
	 * This is useful where entire calltrees would have to be equipped with 
	 * some \c silent parameter. */
	extern int make_STOP_silent;
	/** Master error function. */
extern int _STOP(const char *file, int line, const char *function,
		int errl, const char *format, ...)
__attribute__ ((format (printf, 5, 6) ));
/** Completely customizable error call macro. 
 * Seldom used, as all things are parametrized.
 * \note This is like SVN_ERR(), but has one difference: The function 
 * is not simply ended (via return), cleanup is still possible.
 * \note Putting the \c make_STOP_silent check here enlarges the \c .text 
 * section of \c fsvs for about 3kByte! */
#define STOPIF_FULLPARM(cond, status, code, go, ... ) \
	do                                                  \
{                                                   \
	if (cond)                                         \
	{                                                 \
		status=code;																		\
		_STOP(__FILE__, __LINE__, __PRETTY_FUNCTION__, code, __VA_ARGS__);   \
		goto go;                                        \
	}                                                 \
} while (0)
/** Another error call macro. 
 * Normally not used. */
#define STOPIF_CODE_ERR_GOTO(cond, code, ex, ... ) STOPIF_FULLPARM(cond, status, code, ex, __VA_ARGS__)
/** Error call macro for system functions.
 * \param cond The condition; TRUE means an error happened.
 * \param code The error code to return. Normally an \c E* value.
 * This error macro is used mainly for system calls, where a certain value
 * specifies that an error has happened and some other data (mostly \c errno) 
 * stores the detailed error code. 
 * \code
 *   STOPIF_CODE_ERR( fork("ls -la") == -1, errno, 
 *     "Fork() failed!");
 * \endcode */
#define STOPIF_CODE_ERR(cond, code, ... ) STOPIF_CODE_ERR_GOTO(cond, code, ex, __VA_ARGS__)
/* Main error call macro.
 * This is the macro that should be used for all internal function calls.
 * \param code The status code to check.
 * All other things are hardcoded. */
#define STOPIF(code, ... ) \
	do                                                \
{                                                   \
	status=(code);                                    \
	if (status)                     									\
	{																									\
		_STOP(__FILE__, __LINE__, __PRETTY_FUNCTION__, status, __VA_ARGS__);   \
		goto ex;                                        \
	}																									\
} while (0)
/** A simplified error call macro for returning ENOMEM.
 * \code
 *   void *x;
 *
 *   x=malloc(1024);
 *   STOPIF_ENOMEM(!x);
 * \endcode
 * */
#define STOPIF_ENOMEM(cond) STOPIF_CODE_ERR(cond, ENOMEM, NULL)
/** An error return macro that is used for user output - special handling 
 * \c EPIPE to get a silent return.
 * If \c code returns something negative (like printf, puts, putc ... do; 
 * \c EOF is defined as \c -1), and \a error is \c EPIPE, go on with \c 
 * -EPIPE. */
#define STOPIF_CODE_EPIPE(code, ...) \
	do                                                   \
{                                                      \
	if ((code) < 0)                                      \
	{                                                    \
		status=errno;                                      \
		if (status == EPIPE) status= -EPIPE;               \
		STOPIF(status, "Error writing output");          \
	}                                                    \
} while (0)

/** \page svnlibwrap Subversion library calls wrapper.
 * If this is used in some function, an additional variable is needed:
 * \code
 *   int some_function( <em> ... some args ... </em> )
 *   {
 *     int status;
 *     svn_error_t *status_svn;
 *
 *     STOPIF_SVNERR( <em>svn_function</em>, 
 *       (<em>parameters ...</em>) );
 *
 *   ex:
 *     STOP_HANDLE_SVNERR(status_svn);
 *   ex2:
 *     <em> ... cleanups here ... </em>
 *     return status;
 *   }
 * \endcode
 */ 
/** The master error macro for calling subversion functions. */
#define STOPIF_SVNERR_TEXT(func, parm, fmt, ...)          \
	do                                                       \
{                                                          \
	status_svn=func parm;                                    \
	STOPIF_CODE_ERR( status_svn, status_svn->apr_err,   \
			fmt ": %s", ## __VA_ARGS__, status_svn->message);  \
} while (0)
/* The mainly used function wrapper.
 * \param func Name of the subversion function
 * \param parm A parenthesized list of arguments.
 * \code
 *   STOPIF_SVNERR( svn_ra_initialize, (global_pool));
 * \endcode
 */
#define STOPIF_SVNERR(func, parm) STOPIF_SVNERR_TEXT(func, parm, #func)
/** Convert the svn_error_t into a message and a returnable integer. */
#define STOP_HANDLE_SVNERR(svnerr) STOPIF_CODE_ERR_GOTO(svnerr, svnerr->apr_err, ex2, (const char*)svnerr->message)

/** The opposite to STOP_HANDLE_SVNERR(); this converts an status
 * to the svn_error_t.
 * Needed for all callbacks (eg. editor functions) which have to 
 * return a svn_error_t. */
#define RETURN_SVNERR(status) return status ? 				\
	svn_error_create (status, NULL, 									\
			__PRETTY_FUNCTION__) : SVN_NO_ERROR;


/** \name Runtime check macros */
/** @{ */
/** Makes the program abort.
 * If the configure had --enable-debug and \c gdb is in the path, try
 * to use \c gdb to debug this problem (only if STDIN and STDOUT are ttys). */
#define BUG(...) do { fflush(NULL); debuglevel=1; DEBUGP(__VA_ARGS__); *(int*)42=__LINE__; } while (0)
/** The same as BUG(), but conditionalized. 
 * \code
 *   BUG_ON(a == b, "HELP")
 * \endcode 
 * would print <tt>
 *   INTERNAL BUG
 *     a == b
 *     HELP
 * </tt> and try to start gdb or abort. */
#define BUG_ON(cond, ...) do { if (cond) BUG( "INTERNAL BUG\n  " #cond "\n  " __VA_ARGS__); } while (0)
/** @} */


/** \name Valgrind memory addressing checking.
 *
 * These are copied from \c valgrind/memcheck.h; they will be overridden
 * by the correct valgrind definitions if the valgrind headers are found
 * and fsvs is configured with \c --enable-debug. */
/** @{ */
#define VALGRIND_MAKE_MEM_NOACCESS(_qzz_addr,_qzz_len) do { } while(0)
#define VALGRIND_MAKE_MEM_UNDEFINED(_qzz_addr,_qzz_len) do { } while(0)
#define VALGRIND_MAKE_MEM_DEFINED(_qzz_addr,_qzz_len) do { } while(0)

#ifdef ENABLE_DEBUG
#ifdef HAVE_VALGRIND
#undef VALGRIND_MAKE_MEM_DEFINED
#undef VALGRIND_MAKE_MEM_UNDEFINED
#undef VALGRIND_MAKE_MEM_NOACCESS
#include <valgrind/memcheck.h>
#endif
#endif
/** @} */
/** @} */


/** \addtogroup Globals Global options.
 * \ingroup dev
 *
 * A list of variables that can be set by commandline parameters or 
 * environment variables; these are used in nearly every action. */
/** @{ */
/** Greater than zero if additional details are wanted, or negative for 
 * extra quiet operation. */
extern int opt_verbose, 
			 /** Flag for recursive/non-recursive behaviour.
				* Starting with 0, gets incremented with \c -R and decremented with \c 
				* -N. Different actions have different default levels. */
			 opt_recursive,
			 /** If this is an import/export command (eg restoration after harddisk 
				* crash), we don't use the WAA for data storage. */
			 is_import_export,
			 /** Flag saying whether the local update should only set the entry_status
				* of existing entries and not check for new ones. Needed for update. */
			 only_check_status,
			 /** Whether debug messages are wanted. */
			 debuglevel;

			 /** A pointer to the commit message; possibly a mmap()ped file. */
			 extern char *opt_commitmsg,
			 /** The file name of the commit message file. */
			 *opt_commitmsgfile;

			 /** The revision we're getting from the repository. */
			 extern svn_revnum_t target_revision;
			 /** The revision the user wants to get at (\c -r parameter).
				* \c HEAD is represented by \c SVN_INVALID_REVNUM.
				* Has to be splitted per-URL when we're going to multi-url operation. */
			 extern svn_revnum_t opt_target_revision;
			 /** The second revision number the user specified. */
			 extern svn_revnum_t opt_target_revision2;
			 /** How many revisions the user specified on the commandline (0, 1 or 2).
				* For multi-update operations it's possibly to update the urls to different
				* revisions; then we need to know for which urls the user specified a
				* revision number. Per default we go to \c HEAD.
				* */
			 extern int opt_target_revisions_given;

			 /** The local character encoding, according to \a LC_ALL or \a LC_CTYPE) */
#ifdef HAVE_LOCALES
			 extern char *local_codeset;
#endif

			 /** The session handle for RA operations. */
			 extern svn_ra_session_t *session;

			 /** The first allocated APR pool. All others are derived from it and its
				* children. */
			 extern apr_pool_t *global_pool;

			 /** The array of URLs. */
			 extern struct url_t **urllist;
			 /** Number of URLs we have. */
			 extern int urllist_count;
			 /** Pointer to \b current URL. */
			 extern struct url_t *current_url;

			 extern unsigned approx_entry_count;
			 /** @} */


			 extern char propname_mtime[],
			 /** Modification time - \c svn:owner */
			 propname_owner[],
			 /** Modification time - \c svn:group */
			 propname_group[],
			 /** Modification time - \c svn:unix-mode */
			 propname_umode[],
			 /** Original MD5 for encoded entries. */
			 propname_origmd5[],
			 /** Flag for special entry. */
			 propname_special[],
			 /** The value for the special property; normally \c "*". */
			 propval_special[],

			 /** Commit-pipe program.  */
			 propval_commitpipe[],
			 /** Update-pipe program. */
			 propval_updatepipe[];


			 /** \addtogroup cmds_strings Common command line strings
				* \ingroup compat
				*
				* These strings may have to be localized some time, that's why they're
				* defined in this place. */
			 /** @{ */
			 extern char parm_dump[],
			 parm_load[];
/** @} */

/** Remember where we started. */
extern char *start_path;
/** How much bytes the \ref start_path has. */
extern int start_path_len;

/** ANSI color sequences, for colorized outputs.
 * I tried using ncurses - but that messes with the terminal upon 
 * initialization. I couldn't find a sane way to make that work. Anybody?
 *
 * \todo We assume light text on dark background here.
 * @{ */
#define ANSI__BLUE "\x1b[1;34m"
#define ANSI__GREEN "\x1b[1;32m"
#define ANSI__RED "\x1b[1;31m"
#define ANSI__WHITE "\x1b[1;37m"
#define ANSI__NORMAL "\x1b[0;0m"
/** @} */

/** For Solaris */
extern char **environ;

#endif
