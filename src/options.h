/************************************************************************
 * Copyright (C) 2007-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __SETTINGS_H__
#define __SETTINGS_H__


#include "global.h"


/** \file
 * Functions dealing with user settings. */


/** \name A list of supported settings/options.
 * @{ */
enum opt__settings_e {

	/* Output options */
	/** Option how paths should be shown.  See also \ref opt_paths_t and \ref 
	 * o_opt_path. */
	OPT__PATH=0,
	/** The maximum number of revisions on \c log output.
	 * See \ref o_logmax. */
	OPT__LOG_MAXREV,
	/** The option bits for log output.
	 * See \ref o_logoutput. */
	OPT__LOG_OUTPUT,
	/** Whether to pipe to colordiff.
	 * Currently yes/no/auto; possibly path/"auto"/"no"?
	 * See \ref o_colordiff. */
	OPT__COLORDIFF,
	/** Should directory listings be sorted?
	 * See \ref o_dir_sort. */
	OPT__DIR_SORT,
	/** Should the status output be colored?
	 * See \ref o_colordiff*/
	OPT__STATUS_COLOR,
	/** Stop on change.
	 * See \ref o_stop_change*/
	OPT__STOP_ON_CHANGE,
	/** The filter mask as given with \ref o_filter -f. */
	OPT__FILTER,

	/** Path for debug output.
	 * See \ref glob_opt_deb. */
	OPT__DEBUG_OUTPUT,

	/* merge/diff options */
	/** How conflicts on update should be handled.
	 * See \ref o_conflict. */
	OPT__CONFLICT,
	/** Default options for the merge program.
	 * See \ref o_merge. */
	OPT__MERGE_OPT,
	/** Name of the merge binary to be used.
	 * See \ref o_merge. */
	OPT__MERGE_PRG,

	/** Which program should be called.
	 * See \ref o_diff. */
	OPT__DIFF_PRG,
	/** Default options for the diff program.
	 * See \ref o_diff. */
	OPT__DIFF_OPT,
	/** Extra options for the diff program.
	 * See \ref o_diff. */
	OPT__DIFF_EXTRA,

	/** Set warning levels.
	 * See \ref o_warnings */
	OPT__WARNINGS,
	/** WAA root directory; per definition no PATH_SEPARATOR at the end.
	 * See \ref o_softroot. */
	OPT__SOFTROOT,

	/** Which URL to commit to.
	 * See \ref o_commit_to. */
	OPT__COMMIT_TO,

	/** The author for commit.
	 * See \ref o_author. */
	OPT__AUTHOR,
	/** Whether commits without changes should be done.
	 * See \ref o_empty_commit. */
	OPT__EMPTY_COMMIT,
	/** Should commit wait for the next full second?
	 * If shells would export \c $- we could do an \c auto value as well.
	 * See \ref o_delay. */
	OPT__DELAY,
	/** Do expensive copyfrom checks?
	 * See \ref o_copyfrom_exp */
	OPT__COPYFROM_EXP,

	/** The base path of the WAA.
	 * See \ref o_waa. */
	OPT__WAA_PATH,
	/** The base path of the configuration area.
	 * See \ref o_conf. */
	OPT__CONF_PATH,

	/** End of enum marker. */
	OPT__COUNT
};
/** @} */



/** \name List of priority levels for settings loading.
 * @{ */
enum opt__prio_e {
	/** Default value in program. */
	PRIO_DEFAULT=0,
	/** Value from \c /etc/fsvs/config, or at least from \c $FSVS_CONF/config.  
	 * */
	PRIO_ETC_FILE,
	/** Value read from ~/.fsvs/config. */
	PRIO_USER_FILE,
	/** Value read from \c $FSVS_CONF/$wc_dir/Config. */
	PRIO_ETC_WC,
	/** Value read from \c ~/$wc_dir/Config. */
	PRIO_USER_WC,
	/** Value read from environment variable. */
	PRIO_ENV,
	/** Value assumed from external state, but overrideable.
	 * Example: colors for log output; should not be printed when redirected 
	 * into a file, except if explicitly told so on the command line. */
	PRIO_PRE_CMDLINE,
	/** Value given on commandline. */
	PRIO_CMDLINE,
	/** Internal requirement. */
	PRIO_MUSTHAVE,
};


/* Pre-declaration. */
struct opt__list_t;

/** An option string parsing function. */
typedef int (opt___parse_t)(struct opt__list_t *, char *, enum opt__prio_e);

/** An option entry. 
 * */
struct opt__list_t {
	/** Name of the option. */
	char name[12];

	/** At which priority it has been written yet. */
	enum opt__prio_e prio;
	/** Function to convert the string into a value. */
	opt___parse_t *parse;
	/** Arbitrary parameter for the function. */
	const void *parm;

	/** Result, if it's a string. */
	const char *cp_val;
	/** Result, if it's an int. For a string its length is stored. */
	int i_val;
};

/** The list of all options.
 * Must be accessible. */
extern struct opt__list_t opt__list[OPT__COUNT];


/** Read the integer value of an option. */
	static inline __attribute__((const)) FASTCALL
int opt__get_int(enum opt__settings_e which) 
{
	return opt__list[which].i_val;
}

/** Read the string value of an option. */
	static inline __attribute__((const)) FASTCALL
const char* opt__get_string(enum opt__settings_e which) 
{
	return opt__list[which].cp_val;
}

/** Get the priority for an option. */
	static inline __attribute__((const)) FASTCALL
enum opt__prio_e opt__get_prio(enum opt__settings_e which) 
{
	return opt__list[which].prio;
}

/** Set the integer value of an option. */
	static inline FASTCALL 
void opt__set_int(enum opt__settings_e which, enum opt__prio_e prio, 
		int val)
{
	if (opt__list[which].prio <= prio) 
	{
		opt__list[which].i_val=val;
		opt__list[which].prio=prio;
	}
}

/** Set the string value of an option.
 * Will not get modified, unless a reader changes data.
 * */
	static inline FASTCALL 
void opt__set_string(enum opt__settings_e which, enum opt__prio_e prio, 
		char *stg)
{
	if (opt__list[which].prio <= prio) 
	{
		opt__list[which].cp_val=stg;
		opt__list[which].prio=prio;
	}
}


/** Parse the string for the option. */
int opt__parse_option(enum opt__settings_e which, enum opt__prio_e prio, 
		char *string);
/** Find the option, and parse the string. */
int opt__parse(char *key, char *value, enum opt__prio_e prio, 
		int quiet_errors);

/** Load options from the environment. */
int opt__load_env(char **env);

/** Maximum length of a line in a settings file. */
#define OPT__MAX_LINE_LEN (512)
/** Load options from a file.
 * Will use hlp__vpathcopy(), with parameters swapped (\a prio first). */
int opt__load_settings(char *path, char *name, enum opt__prio_e prio);

/** Returns \c 0 if the \a string is an \b off value (like \c off, \c 
 * false, or \c no). */
int opt__doesnt_say_off(const char *string);


/** \name Specific data for single options.
 * @{ */

/** \name Printing of paths.
 * See \ref o_opt_path.
 * @{ */
/** Path-printing enumeration */
enum opt_paths_t {
	/** \ref pd_wcroot */
	PATH_WCRELATIVE=0,
	/** \ref pd_parm */
	PATH_PARMRELATIVE,
	/** \ref pd_absolute*/
	PATH_ABSOLUTE,
	/** \ref pd_env */
	PATH_CACHEDENVIRON,
	/** \ref pd_env */
	PATH_FULLENVIRON,
};
/** @} */


/** \name List of constants for \ref o_delay option.
 * @{ */
enum opt__delay_e {
	DELAY_CHECKOUT	= 1 << 0,
  DELAY_COMMIT		= 1 << 1,
	DELAY_UPDATE		= 1 << 2,
	DELAY_REVERT		= 1 << 3,
};
/** @} */


/** \name List of constants for \ref o_conflict option.
 * @{ */
enum opt__conflict_e {
	CONFLICT_STOP=0,
	CONFLICT_LOCAL,
	CONFLICT_REMOTE,
	CONFLICT_BOTH,
	CONFLICT_MERGE,
};
/** @} */


/** Filter value to print \b all entries. */
#define FILTER__ALL (-1)

/** Generic yes/no/auto config values.
 * @{ */
#define OPT__YES (1)
#define OPT__NO (0)
#define OPT__AUTO (-1)
/** @} */

/** @} */

#endif

