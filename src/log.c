/************************************************************************
 * Copyright (C) 2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

/** \file
 * Fetch log information from repository - \ref log command.
 *
 * */

/** \addtogroup cmds
 * 
 * \section log
 *
 * \code
 * fsvs log [-v] [-r rev1[:rev2]] [path]
 * \endcode
 * 
 * This command views the log information associated with the given 
 * \e path, or, if none, the highest priority URL.
 *
 * The optional \e rev1 and \e rev2 can be used to restrict the 
 * revisions that are shown; if no values are given, the logs are 
 * given starting from HEAD downwards.
 *
 * If you use the \ref glob_opt_verb "-v"-option, you get the files changed 
 * in each revision printed, too.
 *
 * Currently at most 100 log messages are shown.
 *
 * There is an option controlling the output format; see \ref o_logoutput.
 *
 * TODOs: 
 * - \c --stop-on-copy
 * - Show revision for \b all URLs associated with a working copy?
 *   In which order?
 * - A URL-parameter, to specify the log URL. (Name)
 * - Limit number of revisions shown?
 * */


#include <apr_pools.h>
#include <subversion-1/svn_ra.h>
#include <subversion-1/svn_error.h>
#include <subversion-1/svn_string.h>
#include <subversion-1/svn_time.h>

#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>


#include "global.h"
#include "est_ops.h"
#include "waa.h"
#include "url.h"
#include "options.h"
#include "log.h"
#include "update.h"
#include "racallback.h"
#include "helper.h"


#define MAX_LOG_OUTPUT_LINE (1024)

//svn_log_message_receiver_t log__receiver;


int log___divider(FILE *output, char *color_after)
{
	return -1 == fprintf(output, "%s"
			"------------------------------------"
			"------------------------------------\n"
			"%s",
			(opt__get_int(OPT__LOG_OUTPUT) & LOG__OPT_COLOR) ? ANSI__BLUE : "",
			(opt__get_int(OPT__LOG_OUTPUT) & LOG__OPT_COLOR) ? color_after : "")
		? errno : 0;
}


/** The callback function for log messages.
 * The header and message body are printed in normal subversion format, 
 * possibly with indenting and/or colorizing.
 * The filehandle for output must be given as \a baton.
 *
 * The various strings are (?) in UTF-8, so we have to convert them. */
svn_error_t *log__receiver(void *baton, 
		apr_hash_t *changed_paths,
		svn_revnum_t revision,
		const char *author,
		const char *date,
		const char *message,
		apr_pool_t *pool)
{
	const char indent[]="  ";
	int status;
	int lines, len, cur, sol, i;
	const char *ccp;
	char *auth, *dat, *mess;
	FILE *output=baton;

  apr_hash_index_t *hi;
	void const *name;
	apr_ssize_t namelen;
	char *local_name;


	DEBUGP("got log for %llu", (t_ull)revision);

	/* It seems possible that message=NULL. */
	if (!message) message="(No message.)";

	/* count lines. */
	ccp=message;
	lines=1;
	while ( (ccp=strchr(ccp, '\n')) ) lines++, ccp++;
	len = ccp+strlen(message) - message;

	DEBUGP("got %d lines", lines);

	/* Are these always in UTF-8 ? */
	STOPIF( hlp__utf82local(author, &auth, -1), NULL);
	STOPIF( hlp__utf82local(date, &dat, -1), NULL);
	/* We don't do the message in a single piece, because that might be large. */

	STOPIF( log___divider(output, ANSI__GREEN), NULL);

	/* Taken from a svn commit message. */
	STOPIF_CODE_ERR( -1 == fprintf(output, 
			"r%llu | %s | %s | %d line%s\n"
			"%s",
			(t_ull)revision, auth, dat, lines,
			lines == 1 ? "" : "s",
			(opt__get_int(OPT__LOG_OUTPUT) & LOG__OPT_COLOR) ? ANSI__NORMAL : ""),
			errno, NULL);

	/* Print optionally the filenames */
	if (changed_paths)
	{
		STOPIF_CODE_ERR( -1 == fputs("Changed paths:\n", output), 
				errno, NULL);
		hi=apr_hash_first(pool, changed_paths);
		while (hi)
		{
			apr_hash_this(hi, &name, &namelen, NULL);

			STOPIF( hlp__utf82local( name, &local_name, namelen), NULL);

			STOPIF_CODE_ERR( -1 == fprintf(output, "  %s\n", local_name), 
					errno, NULL);
			hi = apr_hash_next(hi);
		}
	}

	STOPIF_CODE_ERR( -1 == fputs("\n", output), errno, NULL);

	/* Convert the message in parts;
	 * - so that not too big buffers are processed at once, and
	 * - so that we can do indenting, if wished. */
	len=strlen(message);
	sol=1;
	while(len >0)
	{
		DEBUGP("todo %d bytes, \\x%02X; sol=%d", len, *message & 0xff, sol);
		if (sol && (opt__get_int(OPT__LOG_OUTPUT) & LOG__OPT_INDENT))
			STOPIF_CODE_ERR( fputs(indent, output)==EOF, errno, NULL);

		cur= len <= MAX_LOG_OUTPUT_LINE ? len : MAX_LOG_OUTPUT_LINE;
		ccp=memchr(message, '\n', cur);
		if (ccp)
			cur=ccp-message+1;
		else if (cur == MAX_LOG_OUTPUT_LINE)
		{
		/* No newline, we need to split. */
			/* Find a position where we can split the stream into valid 
			 * characters.
			 * UTF-8 has defined that at most 4 bytes can be in a single 
			 * character, although up to 7 bytes could be used. We keep it 
			 * simple, and only look for a start character. */
			/* We limit the loop to find invalid sequences earlier. */
			for(i=8; i>=0 && cur > 0; i--)
			{
			  if ((message[cur-1] & 0x80) == 0 ||
						(message[cur-1] & 0xc0) == 0xc0)
					break;
				cur--;
			}
			STOPIF_CODE_ERR(i < 0, EILSEQ, 
					"Invalid UTF8-sequence in log message for revision %llu found",
					(t_ull) revision);
			/* cur is now the index of the start character, and so equals the 
			 * number of bytes to process. */
		}
		DEBUGP("log output: %d bytes", cur);

		STOPIF( hlp__utf82local(message, &mess, cur), NULL);
		STOPIF_CODE_ERR( fputs(mess, output) == EOF, errno, NULL);

		message+=cur;
		len-=cur;
		/* If we found a newline, we need to indent.
		 * Is sol=!!ccp better? sol=ccp gives a warning, and sol=ccp!=NULL is 
		 * not nice, too. sol=(int)ccp gives warnings ... */
		sol= ccp!=NULL;
	}

	putc('\n', output);

	STOPIF(status, NULL);

ex:
	RETURN_SVNERR(status);
}



/** -.
 *
 * */
int log__work(struct estat *root, int argc, char *argv[])
{
	struct estat *sts;
	int status;
	svn_error_t *status_svn;
	char *path;
	apr_array_header_t *paths;
	int limit;
	char **normalized;
	FILE *output=stdout;
	svn_revnum_t head;


	status_svn=NULL;
	STOPIF_CODE_ERR(argc>1, EINVAL,
			"!This command takes (currently) at most a single path.");

	/* Check for redirected STDOUT. */
	if (!isatty(STDOUT_FILENO))
		opt__set_int( OPT__LOG_OUTPUT, PRIO_PRE_CMDLINE,
				opt__get_int( OPT__LOG_OUTPUT) & ~LOG__OPT_COLOR);
	DEBUGP("options bits are %d", opt__get_int(OPT__LOG_OUTPUT));

	STOPIF( waa__find_common_base( argc, argv, &normalized), NULL);
	STOPIF( url__load_nonempty_list(NULL, 0), NULL);
	STOPIF( waa__input_tree(root, NULL, NULL), NULL);

	if (argc)
	{
		STOPIF( ops__traverse(root, normalized[0], 0, 0, &sts), 
				"This entry is unknown.");
		if (!sts->url)
		{
			STOPIF_CODE_ERR(urllist_count>1, EINVAL,
					"!The given entry has no URL associated yet.");
			DEBUGP("Taking default URL.");
		}

		current_url=sts->url;
	}
	else
	{
		sts=root;
		argc=1;
		current_url=urllist[0];
	}

	DEBUGP("doing URL %s", current_url->url);
	STOPIF( url__open_session(&session), NULL);


	if (argc)
	{
		paths=apr_array_make(global_pool, argc, sizeof(char*));

		STOPIF( ops__build_path(&path, sts), NULL);
		*(char **)apr_array_push(paths) = path+2;
	}
	else
		paths=NULL;

	DEBUGP("got %d: %s - %s", 
			opt_target_revisions_given,
			hlp__rev_to_string(opt_target_revision),
			hlp__rev_to_string(opt_target_revision2));


	limit=100;
	switch (opt_target_revisions_given)
	{
		case 0:
			opt_target_revision=SVN_INVALID_REVNUM;
			opt_target_revision2=1;
			break;
		case 1:
			opt_target_revision2 = 1;
			limit=1;
			break;
		case 2:
			break;
		default:
			BUG("how many");
	}

  /* DAV (http:// and https://) don't like getting SVN_INVALID_REVNUM - 
	 * they throw an 175007 "HTTP Path Not Found", and "REPORT request 
	 * failed on '...'". Get the real number. */
	STOPIF_SVNERR( svn_ra_get_latest_revnum,
			(session, &head, global_pool));
	DEBUGP("HEAD is at %ld", head);
	if (opt_target_revision == SVN_INVALID_REVNUM)
		opt_target_revision=head;
	if (opt_target_revision2 == SVN_INVALID_REVNUM)
		opt_target_revision2=head;


	STOPIF_SVNERR( svn_ra_get_log,
			(session,
			 paths,
			 opt_target_revision,
			 opt_target_revision2,
			 limit,
			 opt_verbose>0,
			 0, // TODO: stop-on-copy,
			 log__receiver,
			 output,
			 global_pool) );

	STOPIF( log___divider(output, ANSI__NORMAL), NULL);

ex:
	STOP_HANDLE_SVNERR(status_svn);
ex2:
	return status;
}

