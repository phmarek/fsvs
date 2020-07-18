/************************************************************************
 * Copyright (C) 2006-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __WARNINGS_H__
#define __WARNINGS_H__


#include "options.h"


/** \file
 * Declarations and public enumerations for the warning subsystem.
 * */


/** List of possible actions for warning messages. */
typedef enum {
	/** Warn only once. This has to be 0 to be the default! */
	WA__WARN_ONCE=0,
	/** Warn every time */
	WA__WARN_ALWAYS,
	/** Print an error, stop execution, and exit with an error code. */
	WA__STOP,
	/** Ignore this warning. */
	WA__IGNORE,
	/** Just count this warning. If we got an \ref WA__WARN_ONCE warning it's 
	 * set to this value; this way it is still incremented and printed 
	 * in a summary. Ignored warnings are completely ignored. */
	WA__COUNT,
	/** The maximum index. Keep this at the end! */
	_WA__LAST_INDEX
} warning_action_e;


/** Definitions for warnings. */
struct wa__warnings
{
	/** Short name for command line processing */
	char text[24];
	/** Action to take. When ONCE is reached, it gets changed to IGNORE. */
	warning_action_e action;
	/** How often this warning occured. Always incremented; 
	 * may be >1 for WARN_ONCE. */
	unsigned count;
	/** Whether the user set some value other than the default. */
	enum opt__prio_e prio;
};


/** List of defined warnings. */
typedef enum {
	/** Invalid mtime property. */
	WRN__META_MTIME_INVALID,
	/** Invalid user property. */
	WRN__META_USER_INVALID,
	/** Invalid group property. */
	WRN__META_GROUP_INVALID,
	/** Invalid unix-mode property. */
	WRN__META_UMASK_INVALID,

	/** No URL defined for entry. */
	WRN__NO_URLLIST,

	/** \c LC_CTYPE and/or \c LC_ALL are invalid. */
	WRN__CHARSET_INVALID,

	/** A normal user gets a \c EPERM on \c chmod(), if he is not owner.
	 * Could happen if file data is the same, but meta-data has changed. */
	WRN__CHMOD_EPERM,
	/** Other error codes of \c chmod() - not needed, as they should always
	 * lead to a stop? */
	WRN__CHMOD_OTHER,
	/** Normal users may not call \c chown(); they get an \c EPERM. */
	WRN__CHOWN_EPERM,
	/** Other error codes of \c chown() - not needed, as they should always
	 * lead to a stop? */
	WRN__CHOWN_OTHER,

	/** A property should be set with an reserved name. */
	WRN__PROP_NAME_RESERVED,

	/** Mixed revision working copies not allowed. */
	WRN__MIXED_REV_WC,

	/** Diff returned an exit status of 2 (means error).
	 * But as that is returned for diffing binary files, too,
	 * the exit status is normally ignored. */
	WRN__DIFF_EXIT_STATUS,

	/** Absolute ignore pattern doesn't match wc base. */
	WRN__IGNPAT_WCBASE,

	/** Test warning - for debugging and automated testing. */
	WRN__TEST_WARNING,
	/** Maximum index - keep this at the end! */
	_WRN__LAST_INDEX
} warning_e;


/** Possibly print a warning. */
int wa__warn(warning_e index, int status, const char format[], ...)
	__attribute__ ((format (printf, 3, 4) ));
/** Set the action of one or warnings. */
int wa__set_warn_option(char *stg, enum opt__prio_e prio);

/** Print the warning summary as debug messages.  */
int wa__summary(void);

/** Splits a string on whitespace, and sets warning options. */
int wa__split_process(char *warn, int prio);


#endif

