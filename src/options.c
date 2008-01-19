/************************************************************************
 * Copyright (C) 2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/
#include <ctype.h>
#include <stdlib.h>

#include "global.h"
#include "log.h"
#include "options.h"
#include "helper.h"
#include "warnings.h"


/** \file
 * Functions dealing with user settings. */


/** A structure to associate a string with an integer. */
struct opt___val_str_t 
{
  int val;
	const char *string;
};


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

/** Strings for auto/yes/no settings. */
const struct opt___val_str_t opt___yes_no_auto[]= {
	{ .val=OPT__AUTO, 								.string="auto" },
	{ .val=OPT__YES,				 					.string="yes" },
	{ .val=OPT__YES,				 					.string="true" },
	{ .val=OPT__YES,				 					.string="on" },
	{ .val=OPT__NO,				 						.string="no" },
	{ .val=OPT__NO,				 						.string="off" },
	{ .val=OPT__NO,				 						.string="false" },
	{ .string=NULL, }
};

/* Why doesn't this work?? */
// const struct opt___val_str_t opt___yes_no[] = &opt___yes_no_auto[1];


/** Filter strings and bits.
 * \ref glob_opt_filter. */
const struct opt___val_str_t opt___filter_strings[]= {
	{ .val=FILTER__ALL,				 		.string="any" },
	{ .val=FS_CHANGED | FS_NEW, 	.string="text" },
	{ .val=FS_META_CHANGED, 			.string="meta" }, 
	{ .val=FS_META_MTIME, 				.string="mtime" }, 
	{ .val=FS_META_OWNER, 				.string="owner" },
	{ .val=FS_META_GROUP, 				.string="group" },
	{ .val=FS_NEW, 								.string="new" },
	{ .val=FS_REMOVED, 						.string="deleted" },
	{ .val=0, 										.string="none" },
	{ .string=NULL, }
};


/** \name Predeclare some functions.
 * @{ */
opt___parse_t opt___string2val;
opt___parse_t opt___strings2bitmap;
opt___parse_t opt___strings2empty_bm;
opt___parse_t opt___store_string;
opt___parse_t opt___normalized_path;
opt___parse_t opt___parse_warnings;
opt___parse_t opt___atoi;
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
	[OPT__DIFF_PRG] = {
		.name="diff_prg", .cp_val="diff", .parse=opt___store_string, 
	},
	[OPT__DIFF_OPT] = {
		.name="diff_opt", .cp_val="-pu", .parse=opt___store_string,
	},
	[OPT__DIFF_EXTRA] = {
		.name="diff_extra", .cp_val=NULL, .parse=opt___store_string,
	},
	[OPT__DEBUG_OUTPUT] = {
		.name="debug_output", .cp_val=NULL, .parse=opt___store_string, 
	},
	[OPT__LOG_OUTPUT] = {
		.name="log_output", .i_val=LOG__OPT_DEFAULT,
		.parse=opt___strings2empty_bm, .parm=opt___log_output_strings,
	},
	[OPT__FILTER] = {
		.name="filter", .i_val=0, 
		.parse=opt___strings2bitmap, .parm=opt___filter_strings,
	},
	[OPT__WARNINGS] = {
		.name="warning", .parse=opt___parse_warnings,
	},
	[OPT__SOFTROOT] = {
		.name="softroot", .cp_val=NULL, .parse=opt___normalized_path,
	},
	[OPT__COLORDIFF] = {
		.name="colordiff", .i_val=OPT__AUTO, 
		.parse=opt___string2val, .parm=opt___yes_no_auto, 
	},
	[OPT__DIR_SORT] = {
		.name="dir_sort", .i_val=OPT__NO, 
		.parse=opt___string2val, .parm=opt___yes_no_auto+1,
	},
	[OPT__COMMIT_TO] = {
		.name="commit_to", .cp_val=NULL, .parse=opt___store_string,
	},
	[OPT__STATUS_COLOR] = {
		.name="stat_color", .i_val=OPT__NO, 
		.parse=opt___string2val, .parm=opt___yes_no_auto+1,
	},
};


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
inline int opt___find_string(const struct opt___val_str_t *list, char *string, 
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
 * With an association of \c 0 the value is resetted. */
int opt___strings2bitmap(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio UNUSED)
{
	const char delim[]=";,:/";
	int status;
	int val, i;
	char buffer[strlen(string)+1];
	char *cp;

	/* We make a local copy, so we can use strsep(). */
	strcpy(buffer, string);
	string=buffer;

	val=ent->i_val;
	DEBUGP("Bitmap starting with 0x%X, from %s", val, string);

	while ( (cp=strsep(&string, delim)) )
	{
		STOPIF( opt___find_string(ent->parm, cp, &i), NULL);
		if (i == 0) val=0;
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
	ent->i_val=strlen(string);
	/* strdup() would work, but count again. */

	ent->cp_val=malloc(ent->i_val+1);
	if (!ent->cp_val) return ENOMEM;

	/* This initial write has to be done, so cast the const away. */
	memcpy((char*)ent->cp_val, string, ent->i_val+1);
	return 0;
}


/** Parse warning settings. */
int opt___parse_warnings(struct opt__list_t *ent, char *string, 
		enum opt__prio_e prio)
{
	return wa__set_warn_option(string, prio);
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

	while (isspace(*string)) string++;
	ent=opt__list+which;
	if (ent->prio <= prio) 
	{
		STOPIF( ent->parse(ent, string, prio), 
				"!Parsing value '%s' for option '%s' failed", 
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
int opt__parse(char *key, char *value, enum opt__prio_e prio)
{
	int status;
	int klen;
	int i;


	status=0;

	/* Normalize. */
	while (isspace(*key)) key++;

	/* If no value given ... */
	if (!value)
	{
		value=strchr(key, '=');
		STOPIF_CODE_ERR(!value, EINVAL,
				"!Cannot find value in string '%s'", key);
		klen=value-key;
		value++;
	}
	else
		klen=strlen(key);

	while (klen && isspace(key[klen])) klen--;

	while (isspace(*value)) value++;

	//	DEBUGP("Got %*s=%s", klen, key, value);

	/* Find option. */
	for(i=0; i<OPT__COUNT; i++)
	{
		if (hlp__strncmp_uline_eq_dash(opt__list[i].name, key, klen) == 0 &&
				opt__list[i].name[klen] == 0)
		{
			DEBUGP("parsing %s[%i] = %s", opt__list[i].name, i, value);
			STOPIF(opt__parse_option( i, prio, value), NULL);
			goto ex;
		}
	}

	status=ENOENT;

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
	char buffer[OPT__MAX_LINE_LEN];
	int line;
	FILE *fp;
	char *sol, *eol;


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
		STOPIF( status, "Open file '%s'", buffer);
	}

	line=0;
	while (!feof(fp))
	{
		line++;
		sol=fgets( buffer, sizeof(buffer), fp);
		if (!sol) continue;

		while (isspace(*sol)) sol++;
		if (*sol == '#') continue;

		if (*buffer)
		{
			eol=sol+strlen(sol)-1;
			/* That doesn't work for options with a 1-character name and without 
			 * a value -- we'd need ">=" . Who cares? */
			while (eol >= buffer && isspace(*eol)) eol--;

			/* Character *after* non-whitespace; could be at buffer[0] (if we'd 
			 * use ">=" above).  */
			eol[1]=0;

			/* Only non-empty lines. */
			if (*sol)
			{
				STOPIF( opt__parse(sol, NULL, prio),
						"In file '%s' on line %d", fn, line);
			}
		}
	}

ex:
	if (fp) fclose(fp);

	return status;
}


/** -.
 * Looks for environment variables with the given \c prefix, and tries to 
 * parse them as options.
 *
 * Invalid names are ignored, invalid values not. */
int opt__load_env(char **env)
{
	const char prefix[]="FSVS_";
	int status;
	char *cur;
	char buffer[32];
	int i;

	status=0;

	while ( (cur=*(env++)) )
	{
		if (strncmp(cur, prefix, strlen(prefix)) == 0)
		{
			DEBUGP("found env %s", cur);
			cur += strlen(prefix);

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

			status=opt__parse(buffer, cur+i+1, PRIO_ENV);
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
