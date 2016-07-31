/************************************************************************
 * Copyright (C) 2007-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/
#include <ctype.h>
#include <stdlib.h>

#include "global.h"
#include "log.h"
#include "interface.h"
#include "options.h"
#include "helper.h"
#include "warnings.h"


/** \file
 * Functions dealing with user settings. */

#define ENV_PREFIX "FSVS_"

/* \addtogroup cmds
 * 
 * \section options
 *
 * \code
 * fsvs options dump
 * fsvs options help
 * \endcode
 *
 * This command 
 * 
 * There's no \c load, as FSVS wouldn't know where to store the settings.
 *
 * Please see \ref options for the list of options, their values and 
 * meanings.
 *
 * */


/** A structure to associate a string with an integer. */
struct opt___val_str_t 
{
	const char *string;
	int val;
};

/** We don't use the value INT_MIN directly, because then we couldn't use 
 * -1 for "everything." */
#define BITMAP_CLEAR	(( (unsigned)INT_MIN) >> 1)
#define BITMAP_CLEAR_MASK	(BITMAP_CLEAR ^ (BITMAP_CLEAR >> 2))

/** Associate the path options with the enumerated value.
 * See also \ref o_opt_path */
const struct opt___val_str_t opt___path_strings[]= {
	{ .val=PATH_PARMRELATIVE,		.string="parameter"},
	{ .val=PATH_ABSOLUTE,				.string="absolute"},
	{ .val=PATH_WCRELATIVE,			.string="wcroot"},
	{ .val=PATH_CACHEDENVIRON,	.string="environment"},
	{ .val=PATH_FULLENVIRON,		.string="full-environment"},
	{ .string=NULL, }
};

/** Log output strings and bits. */
const struct opt___val_str_t opt___log_output_strings[]= {
	{ .val=LOG__OPT_COLOR,				 		.string="color" },
	{ .val=LOG__OPT_INDENT,				 		.string="indent" },
	{ .val=0, 												.string="normal" },
	{ .string=NULL, }
};

/** Strings for auto/yes/no settings.
 *
 * Don't change the order without changing all users! */
const struct opt___val_str_t opt___yes_no[]= {
	{ .val=OPT__YES,				 					.string="yes" },
	{ .val=OPT__YES,				 					.string="true" },
	{ .val=OPT__YES,				 					.string="on" },
	{ .val=OPT__NO,				 						.string="no" },
	{ .val=OPT__NO,				 						.string="off" },
	{ .val=OPT__NO,				 						.string="false" },
	{ .string=NULL, }
};

/* Why doesn't this work?? */
const struct opt___val_str_t *opt___no_words = opt___yes_no+3;


/** Filter strings and bits.
 * \ref glob_opt_filter. */
const struct opt___val_str_t opt___filter_strings[]= {
	{ .val=FILTER__ALL,				 								.string="any" },
	{ .val=FS_CHANGED | FS_NEW | FS_REMOVED, 	.string="text" },
	{ .val=FS_META_CHANGED, 									.string="meta" }, 
	{ .val=FS_META_MTIME, 										.string="mtime" }, 
	{ .val=FS_META_OWNER, 										.string="owner" },
	{ .val=FS_META_UMODE,											.string="mode" },
	{ .val=FS_META_GROUP, 										.string="group" },
	{ .val=FS_NEW, 														.string="new" },
	{ .val=FS_CHANGED, 												.string="changed" },
	{ .val=FS_REMOVED, 												.string="deleted" },
	{ .val=FS_REMOVED, 												.string="removed" },
	{ .val=FS__CHANGE_MASK,										.string="default" },
	{ .val=FS__CHANGE_MASK,										.string="def" },
	{ .val=0, 																.string="none" },
	{ .string=NULL, }
};


/** Change detection strings.
 * \ref o_chcheck. */
const struct opt___val_str_t opt___chcheck_strings[]= {
	{ .val=0,																	.string="none" },
	{ .val=CHCHECK_FILE,											.string="file_mtime" },
	{ .val=CHCHECK_DIRS,			 								.string="dir" },
	{ .val=CHCHECK_ALLFILES,	 								.string="allfiles" },
	{ .val=-1,	 															.string="full" },
};


/** Verbosity strings
 * \ref o_verbose. */
const struct opt___val_str_t opt___verbosity_strings[]= {
	{ .val=VERBOSITY_VERYQUIET | BITMAP_CLEAR,			.string="none" },
	{ .val=VERBOSITY_VERYQUIET | BITMAP_CLEAR,			.string="veryquiet" },
	{ .val=VERBOSITY_QUIET 		 | BITMAP_CLEAR,			.string="quiet" },
	{ .val=VERBOSITY_SHOWCHG,												.string="changes" }, 
	{ .val=VERBOSITY_SHOWCHG,												.string="status" }, 
	{ .val=VERBOSITY_SHOWSIZE,											.string="size" },
	{ .val=VERBOSITY_SHOWNAME,											.string="path" },
	{ .val=VERBOSITY_SHOWNAME,											.string="name" },
	{ .val=VERBOSITY_SHOWTIME,											.string="time" },
	{ .val=VERBOSITY_DEFAULT,												.string="default" },
	{ .val=VERBOSITY_TOP_URL,		 										.string="url" },
	{ .val=VERBOSITY_ALL_URLS | VERBOSITY_TOP_URL,	.string="urls" },
	{ .val=VERBOSITY_COPYFROM,	 										.string="copyfrom" },
	{ .val=VERBOSITY_GROUP,		 											.string="group" },
	{ .val=VERBOSITY_STACKTRACE,										.string="stack" },
	{ .val=VERBOSITY_STACKTRACE,										.string="backtrace" },
	{ .val=VERBOSITY_STACKTRACE,										.string="stacktrace" },
	{ .val=-1,																			.string="all" },
};


/** Delay action names.
 * See \ref o_delay. */
const struct opt___val_str_t opt___delay_strings[]= {
	{ .val=DELAY_COMMIT,				 	.string="commit" },
	{ .val=DELAY_UPDATE,	 				.string="update" },
	{ .val=DELAY_REVERT, 					.string="revert" }, 
	{ .val=DELAY_CHECKOUT, 				.string="checkout" }, 
	{ .val=-1,										.string="yes" },
	{ .val=0, 										.string="no" },
	{ .string=NULL, }
};


/** Conflict resolution options.
 * See \ref o_conflict. */
const struct opt___val_str_t opt___conflict_strings[]= {
	{ .val=CONFLICT_STOP,				 	.string="stop" },
	{ .val=CONFLICT_LOCAL,				.string="local" },
	{ .val=CONFLICT_REMOTE,				.string="remote" }, 
	{ .val=CONFLICT_BOTH,  				.string="both" }, 
	{ .val=CONFLICT_MERGE, 				.string="merge" }, 
	{ .string=NULL, }
};



/** \name Predeclare some functions.
 * @{ */
opt___parse_t opt___string2val;
opt___parse_t opt___strings2bitmap;
opt___parse_t opt___strings2empty_bm;
opt___parse_t opt___store_string;
opt___parse_t opt___store_env_noempty;
opt___parse_t opt___normalized_path;
opt___parse_t opt___parse_warnings;
opt___parse_t opt___atoi;
opt___parse_t opt___debug_buffer;
/** @} */


/**
 * Must be visible, so that the inline function have direct accecss.
 *
 * As delimiter should \c '_' be used; as the comparision is done via 
 * hlp__strncmp_uline_eq_dash(), the user can also use '-'.  */
struct opt__list_t opt__list[OPT__COUNT]=
{
	[OPT__PATH] = {
		.name="path", .i_val=PATH_PARMRELATIVE, 
		.parse=opt___string2val, .parm=opt___path_strings, 
	},
	[OPT__LOG_MAXREV] = {
		.name="limit", .i_val=0, .parse=opt___atoi,
	},
	[OPT__LOG_OUTPUT] = {
		.name="log_output", .i_val=LOG__OPT_DEFAULT,
		.parse=opt___strings2empty_bm, .parm=opt___log_output_strings,
	},
	[OPT__COLORDIFF] = {
		.name="colordiff", .cp_val=NULL, .parse=opt___store_string,
	},
	[OPT__DIR_SORT] = {
		.name="dir_sort", .i_val=OPT__NO, 
		.parse=opt___string2val, .parm=opt___yes_no,
	},
	[OPT__STATUS_COLOR] = {
		.name="stat_color", .i_val=OPT__NO, 
		.parse=opt___string2val, .parm=opt___yes_no,
	},
	[OPT__STOP_ON_CHANGE] = {
		.name="stop_change", .i_val=OPT__NO, 
		.parse=opt___string2val, .parm=opt___yes_no,
	},
	[OPT__DIR_EXCLUDE_MTIME] = {
		.name="dir_exclude_mtime", .i_val=OPT__NO,
		.parse=opt___string2val, .parm=opt___yes_no,
	},
	[OPT__FILTER] = {
		.name="filter", .i_val=0, 
		.parse=opt___strings2bitmap, .parm=opt___filter_strings,
	},
	[OPT__CHANGECHECK] = {
		.name="change_check", .i_val=CHCHECK_FILE, 
		.parse=opt___strings2bitmap, .parm=opt___chcheck_strings,
	},
	[OPT__ALL_REMOVED] = {
		.name="all_removed", .i_val=OPT__YES, 
		.parse=opt___string2val, .parm=opt___yes_no,
	},
	[OPT__VERBOSE] = {
		.name="verbose", .i_val=VERBOSITY_DEFAULT, 
		.parse=opt___strings2bitmap, .parm=opt___verbosity_strings,
	},

	[OPT__DEBUG_OUTPUT] = {
		.name="debug_output", .cp_val=NULL, .parse=opt___store_string, 
	},
	[OPT__DEBUG_BUFFER] = {
		.name="debug_buffer", .i_val=0, .parse=opt___debug_buffer, 
	},
	[OPT__GROUP_STATS] = {
		.name="group_stats", .i_val=OPT__NO,
		.parse=opt___string2val, .parm=opt___yes_no,
	},

	[OPT__CONFLICT] = {
		.name="conflict", .i_val=CONFLICT_MERGE,
		.parse=opt___string2val, .parm=opt___conflict_strings,
	},
	[OPT__MERGE_PRG] = {
		.name="merge_prg", .cp_val="diff3", .parse=opt___store_string, 
	},
	[OPT__MERGE_OPT] = {
		.name="merge_opt", .cp_val="-m", .parse=opt___store_string,
	},
	[OPT__DIFF_PRG] = {
		.name="diff_prg", .cp_val="diff", .parse=opt___store_string, 
	},
	[OPT__DIFF_OPT] = {
		.name="diff_opt", .cp_val="-pu", .parse=opt___store_string,
	},
	[OPT__DIFF_EXTRA] = {
		.name="diff_extra", .cp_val=NULL, .parse=opt___store_string,
	},

	[OPT__WARNINGS] = {
		.name="warning", .parse=opt___parse_warnings,
	},
	[OPT__SOFTROOT] = {
		.name="softroot", .cp_val=NULL, .parse=opt___normalized_path,
	},

	[OPT__MKDIR_BASE] = {
		.name="mkdir_base", .i_val=OPT__NO,
		.parse=opt___string2val, .parm=opt___yes_no,
	},
	[OPT__COMMIT_TO] = {
		.name="commit_to", .cp_val=NULL, .parse=opt___store_string,
	},
	[OPT__AUTHOR] = {
		.name="author", .cp_val="", .parse=opt___store_env_noempty,
	},
	[OPT__PASSWD] = {
		.name="password", .cp_val="", .parse=opt___store_string,
	},

	/* I thought about using opt___normalized_path() for these two; but that 
	 * would be a change in behaviour. */
	[OPT__WAA_PATH] = {
		.name="waa", .parse=opt___store_string,
		/* Doing that here gives a warning "initializer not constant".
			 .cp_val=DEFAULT_WAA_PATH, .i_val=strlen(DEFAULT_WAA_PATH), */
		.cp_val=NULL, .i_val=0,
	},
	[OPT__CONF_PATH] = {
		.name="conf", .parse=opt___store_string,
		/* Doing that here gives a warning "initializer not constant".
			 .cp_val=DEFAULT_CONF_PATH, .i_val=strlen(DEFAULT_CONF_PATH), */
		.cp_val=NULL, .i_val=0,
	},
	[OPT__CONFIG_DIR] = {
		.name="config_dir", .parse=opt___store_string,
		.cp_val=NULL, .i_val=0,
	},


	[OPT__EMPTY_COMMIT] = {
		.name="empty_commit", .i_val=OPT__YES, 
		.parse=opt___string2val, .parm=opt___yes_no,
	},
	[OPT__EMPTY_MESSAGE] = {
		.name="empty_message", .i_val=OPT__YES, 
		.parse=opt___string2val, .parm=opt___yes_no,
	},
	[OPT__DELAY] = {
		.name="delay", .i_val=OPT__NO,
		.parse=opt___strings2empty_bm, .parm=opt___delay_strings,
	},
	[OPT__COPYFROM_EXP] = {
		.name="copyfrom_exp", .i_val=OPT__YES,
		.parse=opt___string2val, .parm=opt___yes_no,
	},
};


/** Get the debugbuffer size, round and test for minimum size.
 * The value is in KB; we round up to a 4kB size, and make it at least 8k.  
 * A value of \c 0 means \b off.
 * */
int opt___debug_buffer(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio UNUSED)
{
#ifndef ENABLE_DEBUGBUFFER
	int status;
	STOPIF(EINVAL, "!The debugbuffer option is not available, because\n"
			"fmemopen() was not found during compilation.");
ex:
	return status;
#else
	char *l;
	int i;

	i=strtol(string, &l, 0);
	if (*l) return EINVAL;

	if (i)
	{
		if (i<4) 
			i=4;
		else
			/* Round up. */
			i = (i+3) & ~3;

		i *= 1024;
	}

	ent->i_val=i;
 
	return 0;
#endif
}


/** Get an integer value directly. */
int opt___atoi(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio UNUSED)
{
	char *l;

	ent->i_val=strtol(string, &l, 0);

	if (*l) return EINVAL;
	return 0;
}


/** Find an integer value by comparing with predefined strings. */
inline int opt___find_string(const struct opt___val_str_t *list, 
		const char *string, 
		int *result)
{
	for(; list->string; list++)
	{
		if (strcmp(string, list->string) == 0)
		{
			*result = list->val;
			return 0;
		}
	}

	return EINVAL;
}


/** Set an integer value by comparing with some strings. */
int opt___string2val(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio UNUSED)
{
	int i;
	int status;

	STOPIF( opt___find_string(ent->parm, string, &i), NULL);

	ent->i_val=i;

ex:
	return status;
}


/** Convert a string into a list of words, and \c OR their associated 
 * values together.
 * With an association of \c 0, or if BITMAP_CLEAR is set, the value is 
 * resetted.  */
int opt___strings2bitmap(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio UNUSED)
{
	static const char delim[]=";,:/";
	int status;
	int val, i;
	char buffer[strlen(string)+1];
	char *cp;

	status=0;
	/* We make a local copy, so we can use strsep(). */
	strcpy(buffer, string);
	string=buffer;

	val=ent->i_val;
	DEBUGP("Bitmap starting with 0x%X, from %s", val, string);

	while ( (cp=strsep(&string, delim)) )
	{
		/* Return errors quietly. */
		status=opt___find_string(ent->parm, cp, &i);
		if (status) goto ex;

		if (i == 0 || ((i & BITMAP_CLEAR_MASK) == BITMAP_CLEAR))
			val=0;
		else val |= i;
	}

	DEBUGP("New bitmap is 0x%X", val);

	ent->i_val=val;

ex:
	return status;
}


/** The same as opt___strings2bitmap(), but starting with a zero value on 
 * each parsed value.
 * */
int opt___strings2empty_bm(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio)
{
	ent->i_val=0;
	return opt___strings2bitmap(ent, string, prio);
}


/** Simple store a copy of the string. */
int opt___store_string(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio UNUSED)
{
	int status;

	ent->i_val=strlen(string);
	/* strdup() would work, but count again. */

	/* This initial write has to be done, so cast the const away. */
	STOPIF( hlp__strnalloc(ent->i_val, (char**)&ent->cp_val, string), NULL);

ex:
	return status;
}


/** Store a string, or expand a (non-empty) environment variable. */
int opt___store_env_noempty(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio)
{
	/* Not ideal - makes a copy of an environment variable, that 
	 * wouldn't be needed. */
	if (string[0] == '$') string=getenv(string+1);

	if (!string || !*string)
		return 0;

	return opt___store_string(ent, string, prio);
}


/** Parse warning settings. */
int opt___parse_warnings(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio)
{
	int status;

	STOPIF( wa__split_process(string, prio), NULL);
ex:
	return status;
}


/** -.
 * If the given priority is at least equal to the current value, parse the 
 * strng and set the value. */
int opt__parse_option(enum opt__settings_e which, enum opt__prio_e prio, 
		char *string)
{
	int status;
	struct opt__list_t *ent;

	status=0;

	string=hlp__skip_ws(string);
	ent=opt__list+which;
	if (ent->prio <= prio) 
	{
		STOPIF( ent->parse(ent, string, prio), 
				"!Parsing value '%s' for option '%s' failed.", 
				string, ent->name);
		ent->prio=prio;
	}

ex:
	return status;
}


/** -.
 * If the \a value is \c NULL, try to split the \a key on a \c =.
 * Then find the matching option, and set its value (depending on the given 
 * priority). */
int opt__parse(char *key, char *value, enum opt__prio_e prio,
		int quiet_errors)
{
	int status;
	int klen;
	int i;


	status=0;

	/* Normalize. */
	key=hlp__skip_ws(key);

	/* If no value given ... */
	if (!value)
	{
		value=strchr(key, '=');
		STOPIF_CODE_ERR(!value, EINVAL,
				"!Cannot find value in string '%s'.", key);
		klen=value-key;
		value++;
	}
	else
		klen=strlen(key);

	while (klen && isspace(key[klen])) klen--;

	value=hlp__skip_ws(value);

	//	DEBUGP("Got %*s=%s", klen, key, value);

	/* Find option. */
	for(i=0; i<OPT__COUNT; i++)
	{
		if (hlp__strncmp_uline_eq_dash(opt__list[i].name, key, klen) == 0 &&
				opt__list[i].name[klen] == 0)
		{
			DEBUGP("parsing %s[%i] = %s", opt__list[i].name, i, value);
			STOPIF( opt__parse_option( i, prio, value), NULL);
			goto ex;
		}
	}

	status=ENOENT;
	if (!quiet_errors)
		STOPIF( status, "!Option name '%s' unknown.", key);

ex:
	return status;
}


/** -.
 * Ignores empty lines; comment lines are defined by an \c # as first 
 * non-whitespace character. */
int opt__load_settings(char *path, char *name, enum opt__prio_e prio)
{
	int status;
	char fn[strlen(path) + 1 + (name ? strlen(name) : 0) + 1];
	char *buffer;
	FILE *fp;


	status=0;
	strcpy(fn, path);
	if (name)
	{
		strcat(fn, "/");
		strcat(fn, name);
	}

	DEBUGP("reading settings from %s, with prio %d", 
			fn, prio);
	fp=fopen(fn, "rt");
	if (!fp)
	{
		status=errno;
		if (status == ENOENT)
		{
			/* Ignore that. */
			status=0;
			goto ex;
		}
		STOPIF( status, "Open file '%s'", fn);
	}

	hlp__string_from_filep(NULL, NULL, NULL, SFF_RESET_LINENUM);
	while (!feof(fp))
	{
		status=hlp__string_from_filep(fp, &buffer, NULL,
				SFF_WHITESPACE | SFF_COMMENT);
		if (status == EOF) break;
		STOPIF( status, NULL);

		if (*buffer == '#') continue;

		STOPIF( opt__parse(buffer, NULL, prio, 0),
				"In file '%s' on line %u", fn, 
				hlp__string_from_filep(NULL, NULL, NULL, SFF_GET_LINENUM));
	}

	/* status == EOF from the loop above. */
	status=0;

ex:
	if (fp) fclose(fp);

	return status;
}


/** -.
 * Looks for environment variables with the given \c ENV_PREFIX, and tries to 
 * parse them as options.
 *
 * Invalid names are ignored, invalid values not. */
int opt__load_env(char **env)
{
	int status;
	char *cur;
	char buffer[32];
	int i;

	status=0;

	while ( (cur=*(env++)) )
	{
		if (strncmp(cur, ENV_PREFIX, strlen(ENV_PREFIX)) == 0)
		{
			DEBUGP("found env %s", cur);
			cur += strlen(ENV_PREFIX);

			for(i=0; cur[i] != '=' && i<sizeof(buffer)-1; i++)
				buffer[i]=tolower(cur[i]);
			buffer[i]=0;

			/* Should we print warnings or errors for invalid environment 
			 * variables?  */
			if (cur[i] != '=')
			{
				DEBUGP("rejected - key too long.");
				continue;
			}

			status=opt__parse(buffer, cur+i+1, PRIO_ENV, 1);
			if (status == ENOENT)
			{
				DEBUGP("key not known.");
			}
			else STOPIF(status, NULL);
		}
	}

	status=0;

ex:
	return status;
}


int opt___normalized_path(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio)
{
	char path[strlen(string)+1];
	int p;

	hlp__pathcopy(path, &p, string, NULL);

	p--;
	while (p>0 && path[p] == PATH_SEPARATOR) path[p--]=0;

	if (p > 0)
		return opt___store_string(ent, path, prio);
	else
		return EINVAL;
}

/** -.
 * */
int opt__help(struct estat *root, int argc, char *argv[])
{
	return EBUSY;
}

/** -.
 * Invalid values are handled by returning \c 1, ie. they don't say \c off.
 * */
int opt__doesnt_say_off(const char *string)
{
	int i;

	i=OPT__YES;
	if (opt___find_string(opt___yes_no+3, string, &i)) return 1;
	return i;
}


/** -.
 *
 * \todo Maybe the variable reading should be changed to use this code, and
 * loop via \c getenv() over all options? */
char *opt__variable_from_option(enum opt__settings_e which)
{
	static char buffer[ sizeof(ENV_PREFIX) + 
		sizeof(opt__list[0].name) + 1] = ENV_PREFIX;
	char * const target=buffer+strlen(ENV_PREFIX);
	int i;

	i=0;
	while ( (target[i] = toupper(opt__list[which].name[i])) )
		i++;

	return buffer;
}
