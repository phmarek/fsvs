/************************************************************************
 * Copyright (C) 2006-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <fcntl.h>
#include <string.h>

#include "warnings.h"
#include "interface.h"
#include "global.h"


/** \file
 * Functions, enumerations and other private parts of the warning subsystem.
 * */


/** The texts that are used for input/output of the warning actions. */
const char *wa__warn_action_text[_WA__LAST_INDEX] =
{
  [WA__WARN_ONCE] 	= "once",
  [WA__WARN_ALWAYS]	= "always",
  [WA__IGNORE] 			= "ignore",
  [WA__STOP] 				= "stop",
  [WA__COUNT] 			= "count",
};

/** Names for warnings; actions default to WARN_ONCE */
static struct wa__warnings wa___warn_options[_WRN__LAST_INDEX]=
{
  [WRN__META_MTIME_INVALID] 	= { "meta-mtime" },
  [WRN__META_USER_INVALID] 		= { "meta-user" },
  [WRN__META_GROUP_INVALID] 	= { "meta-group" },
  [WRN__META_UMASK_INVALID] 	= { "meta-umask" },

	[WRN__NO_URLLIST]						=	{ "no-urllist" },

	[WRN__CHARSET_INVALID]			=	{ "charset-invalid" },

	[WRN__CHMOD_EPERM]					=	{ "chmod-eperm", WA__WARN_ONCE},
	[WRN__CHOWN_EPERM]					=	{ "chown-eperm", WA__WARN_ONCE},
	[WRN__CHMOD_OTHER]					=	{ "chmod-other", WA__STOP },
	[WRN__CHOWN_OTHER]					=	{ "chown-other", WA__STOP },

	[WRN__MIXED_REV_WC]					=	{ "mixed-rev-wc", WA__WARN_ALWAYS },

	[WRN__PROP_NAME_RESERVED]		=	{ "propname-reserved", WA__STOP },

	[WRN__DIFF_EXIT_STATUS]			=	{ "diff-status", WA__IGNORE },

	[WRN__IGNPAT_WCBASE]				=	{ "ignpat-wcbase", WA__WARN_ALWAYS },

	[WRN__TEST_WARNING]					=	{ "_test-warning", WA__IGNORE },
};

/** The filehandle to print the warnings to.
 * Currently always \c stderr. */
static FILE *warn_out;


/** -.
 * */
int wa__split_process(char *warn, int prio)
{
	int status;
	char *input;

	status=0;
	input=warn;
	while (warn && *warn)
	{
		warn=strtok(input, ",; \r\t\n");
		if (!warn) break;

		/* As per the strtok() rules - input only set on first call. */
		input=NULL;

		STOPIF( wa__set_warn_option(warn, (enum opt__prio_e)prio),
				"In string %s", warn);
	}

ex:
	return status;
}


/**
 * -.
 * The given string is of the format \c warning=action.
 *
 * \a action can be any of the wa__warn_action_text[] strings; \a warning 
 * is the startstring of any of the \ref wa___warn_options.
 * If more than one warnings matches this string, all are set to the given
 * action.
 * The command line option looks like this: \c -Wmeta-mtime=ignore. */
int wa__set_warn_option(char *stg, enum opt__prio_e prio)
{
	char *delim;
	int status;
	int index, action, len;
	struct wa__warnings *warning;

	status=0;
	delim=strchr(stg, '=');
	STOPIF_CODE_ERR(!delim, EINVAL,
			"!The warning option '%s' is invalid",
			stg);

	/* Look for action. We do that first, so that
	 * multiple warnings can be switched at once:
	 *   -Wmeta=ignore */
	for(action=_WA__LAST_INDEX-1; action>=0; action--)
		if (strcmp(wa__warn_action_text[action], delim+1) == 0)
			break;
	STOPIF_CODE_ERR(action < 0, EINVAL,
			"The warning action specification '%s' is invalid",
			delim+1);

	/* Look for option(s).
	 * If we return ENOENT it looks to opt__load_env() as if it's simply no 
	 * valid key - and gets ignored. So we return EINVAL. */
	status=EINVAL;
	len=delim-stg;
	warning=wa___warn_options;
	for(index=0; index<_WRN__LAST_INDEX; index++, warning++)
	{
		if (strncmp(warning->text, stg, len) == 0)
		{
			if (warning->prio <= prio)
			{
				warning->action=action;
				warning->prio=prio;
				DEBUGP("warning option set: %s=%s, prio %d",
						warning->text, 
						wa__warn_action_text[warning->action],
						prio);
			}
			status=0;
		}
	}

	STOPIF_CODE_ERR(status, status,
			"The given warning option '%*s' matches no warnings",
			len, stg);

ex:
	return status;
}


/** -.
 * 
 * \param index The definition from \a warning_e.
 * \param stat The error code that could be used for stopping.
 * \param format \c printf()-style message.
 * */
int wa__warn(warning_e index, int stat, const char format[], ...)
{
	va_list va;
	int status, ret;

	if (!warn_out)
		warn_out=stderr;

	wa___warn_options[index].count++;
	status=0;
	switch (wa___warn_options[index].action)
	{
		case WA__IGNORE:
		case WA__COUNT:
			break;
		case WA__STOP:
			status=stat;
			if (!status) status=EAGAIN;

			/* Fall through. Modifying doesn't matter, we'll stop. */
		case WA__WARN_ONCE:
			/* Fall through, printing a warning */

		case WA__WARN_ALWAYS:
			/* Print a warning */
			va_start(va, format);
			ret=fprintf(warn_out, "\nWARNING");
			if (opt__is_verbose() > 0)
				ret|=fprintf(warn_out, "(%s)", wa___warn_options[index].text);
			ret|=fprintf(warn_out, ": ");
			ret|=vfprintf(warn_out, format, va);
			ret|=fprintf(warn_out, "\n\n");

			if (!status)
				/* Any negative value from above (error) will turn ret negative, too.
				 * We don't check if we're planning to stop. */
				STOPIF_CODE_ERR(ret<0, errno,
						"Error while printing warning");

			if (wa___warn_options[index].action == WA__WARN_ONCE)
				/* Switch to counting mode */
				wa___warn_options[index].action=WA__COUNT;
			break;
		default:
			BUG("Invalid warning action encountered");
	}

ex:
	return status;
}


/** -.
 *
 * Warnings set to \ref WA__IGNORE are not printed. */ 
int wa__summary(void)
{
	int i, status;
	int flag;


	status=0;
	/* Flush all streams, so that this warnings occur *after* 
	 * every other status output. */
	STOPIF_CODE_EPIPE( fflush(NULL), NULL);

	flag=0;
	for(i=0; i<_WRN__LAST_INDEX; i++)
	{
		DEBUGP("%d# %s: %dx",
				i,
				wa___warn_options[i].text,
				wa___warn_options[i].count);

		if (wa___warn_options[i].action != WA__IGNORE &&
				wa___warn_options[i].count)
		{
			/* This can only be called if there was at least a single warning,
			 * and then warn_out is set, too. */
			if (!flag++)
				STOPIF_CODE_ERR( fprintf(warn_out, "\nWarning summary:\n")<0, errno,
						"Error writing warning summary header");

			STOPIF_CODE_ERR( fprintf(warn_out, "   %s occurred %d time%s\n",
						wa___warn_options[i].text,
						wa___warn_options[i].count,
						wa___warn_options[i].count == 1 ? "" : "s") <0, errno,
					"Cannot write warning summary line");
		}
	}

ex:
	return status;
}


