/************************************************************************
 * Copyright (C) 2007-2009,2015 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
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
 * fsvs log [-v] [-r rev1[:rev2]] [-u name] [path]
 * \endcode
 * 
 * This command views the revision log information associated with the 
 * given \e path at its topmost URL, or, if none is given, the highest 
 * priority URL.
 *
 * The optional \e rev1 and \e rev2 can be used to restrict the 
 * revisions that are shown; if no values are given, the logs are given 
 * starting from \c HEAD downwards, and then a limit on the number of 
 * revisions is applied (but see the \ref o_logmax "limit" option).
 *
 * If you use the \ref glob_opt_verb "-v" -option, you get the files 
 * changed in each revision printed, too.
 *
 * There is an option controlling the output format; see the \ref 
 * o_logoutput "log_output option".
 *
 * Optionally the name of an URL can be given after \c -u; then the log of 
 * this URL, instead of the topmost one, is shown.
 *
 * TODOs: 
 * - \c --stop-on-copy
 * - Show revision for \b all URLs associated with a working copy?
 *   In which order?
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

static int log___path_prefix_len,
					 log___path_skip,
					 log___path_parm_len;
static char *log___path_prefix,
						*log___path_parm;


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
	static const char indent[]="  ";
	int status;
	int lines, len, cur, sol, i;
	const char *ccp;
	char *auth, *dat, *mess;
	FILE *output=stdout;

  apr_hash_index_t *hi;
	void const *name;
	apr_ssize_t namelen;
	char *local_name;
	int fn_count;
	char **filenames, *path_to_store;
	static const char ps[]={PATH_SEPARATOR, 0};


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
	STOPIF_CODE_EPIPE( fprintf(output, 
				"r%llu | %s | %s | %d line%s\n"
				"%s",
				(t_ull)revision, auth, dat, lines,
				lines == 1 ? "" : "s",
				(opt__get_int(OPT__LOG_OUTPUT) & LOG__OPT_COLOR) ? ANSI__NORMAL : ""),
			NULL);

	/* Print optionally the filenames */
	if (changed_paths)
	{
		STOPIF_CODE_EPIPE( fputs("Changed paths:\n", output), NULL);

		/* Prepare for sorting. */
		fn_count=apr_hash_count(changed_paths);
		STOPIF( hlp__alloc( &filenames, sizeof(*filenames)*fn_count), NULL);

		i=0;
		hi=apr_hash_first(pool, changed_paths);
		while (hi)
		{
			apr_hash_this(hi, &name, &namelen, NULL);

			STOPIF( hlp__utf82local( name, &local_name, namelen), NULL);

			BUG_ON(i>=fn_count,
					"too many filenames in hash - count was %d", fn_count);

			DEBUGP("got path %s", local_name);
			if (strncmp(local_name, log___path_prefix, log___path_prefix_len) == 0)
			{
				path_to_store=local_name + log___path_prefix_len;
				switch (*path_to_store)
				{
					case 0:
						/* Hack to make the ++ right. */
						path_to_store="x";

					case PATH_SEPARATOR:
						path_to_store++;
						STOPIF( hlp__strmnalloc(1 + log___path_parm_len + 
									strlen(path_to_store) + 1 + 3, 
									filenames+i, 
									log___path_parm, 
									(log___path_parm_len>1 && *path_to_store &&
									 log___path_parm[ log___path_parm_len-1 ] != PATH_SEPARATOR) ? 
									ps : "",
									path_to_store, NULL), NULL);

						i++;
				}
			}

			hi = apr_hash_next(hi);
		}

		BUG_ON(i>fn_count,
				"Wrong number of filenames in hash - count was %d", fn_count);
		fn_count=i;

		qsort(filenames, fn_count, sizeof(*filenames),
				hlp__compare_string_pointers);
		for(i=0; i<fn_count; i++)
		{
			STOPIF_CODE_EPIPE( fprintf(output, "  %s\n", filenames[i]), NULL);
			IF_FREE(filenames[i]);
		}
	}

	STOPIF_CODE_EPIPE( fputs("\n", output), NULL);

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
				cur--;
				/* No UTF-8 character (ie. 7bit ASCII) */
				if ((message[cur] & 0x80) == 0 ||
						/* or first character */
						(message[cur] & 0xc0) == 0xc0)
					break;
			}
			STOPIF_CODE_ERR(i < 0, EILSEQ, 
					"Invalid UTF8-sequence in log message for revision %llu found",
					(t_ull) revision);
			/* cur is now the index of the start character, and so equals the 
			 * number of bytes to process. */
		}
		DEBUGP("log output: %d bytes", cur);

		STOPIF( hlp__utf82local(message, &mess, cur), NULL);
		STOPIF_CODE_EPIPE( fputs(mess, output), NULL);

		message+=cur;
		len-=cur;
		/* If we found a newline, we need to indent.
		 * Is sol=!!ccp better? sol=ccp gives a warning, and sol=ccp!=NULL is 
		 * not nice, too. sol=(int)ccp gives warnings ... */
		sol= ccp!=NULL;
	}

	STOPIF_CODE_EPIPE( putc('\n', output), NULL);

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
	const char *base_url;


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
		STOPIF_CODE_ERR( argc>1, EINVAL,
				"!The \"log\" command currently handles only a single path.");
		STOPIF( ops__traverse(root, normalized[0], 0, 0, &sts), 
				"!The entry \"%s\" cannot be found.", normalized[0]);

		log___path_parm_len=strlen(argv[0]);
		STOPIF( hlp__strnalloc(log___path_parm_len+2, 
					&log___path_parm, argv[0]), NULL);
	}
	else
	{
		log___path_parm_len=0;
		log___path_parm="";
		sts=root;
	}

	current_url=NULL;
	if (url__parm_list_used)
	{
		STOPIF_CODE_ERR(url__parm_list_used>1, EINVAL,
				"!Only a single URL can be given.");
		STOPIF( url__find_by_name(url__parm_list[0], &current_url), 
				"!No URL with name \"%s\" found", url__parm_list[0]);
	}
	else
	{
		if (sts->url)
			current_url=sts->url;
		else
		{
			STOPIF_CODE_ERR(urllist_count>1, EINVAL,
					"!The given entry has no URL associated yet.");
		}
	}

	if (!current_url)
		current_url=urllist[0];


	DEBUGP("doing URL %s", current_url->url);
	STOPIF( url__open_session(NULL, NULL), NULL);


	if (argc)
	{
		paths=apr_array_make(global_pool, argc, sizeof(char*));

		STOPIF( ops__build_path(&path, sts), NULL);
		*(char **)apr_array_push(paths) = path+2;
	}
	else
	{
		paths=NULL;
		path=".";
	}

	/* Calculate the comparision string. */
	STOPIF_SVNERR( svn_ra_get_repos_root2,
			(current_url->session, &base_url, global_pool));
		/* |- current_url->url -|
		 * |- repos root-|
		 * http://base/url /trunk /relative/path/ cwd/entry...
		 *                 |-- log_path_skip ---|
		 *
		 * sts->path_len would be wrong for the WC root.
		 * */
	log___path_prefix_len=current_url->urllen - strlen(base_url) + strlen(path)-1;
	STOPIF( hlp__strmnalloc( log___path_prefix_len+1+5, 
				&log___path_prefix,
				current_url->url + strlen(base_url),
				/* Include the "/", but not the ".". */
				sts->parent ? path+1 : NULL, NULL), NULL);

	DEBUGP("got %d: %s - %s; filter %s(%d, %d)", 
			opt_target_revisions_given,
			hlp__rev_to_string(opt_target_revision),
			hlp__rev_to_string(opt_target_revision2),
			log___path_prefix, log___path_prefix_len, log___path_skip);


	/* To take the difference (for -rX:Y) we need to know HEAD. */
	STOPIF( url__canonical_rev(current_url, &opt_target_revision), NULL);
	STOPIF( url__canonical_rev(current_url, &opt_target_revision2), NULL);

	switch (opt_target_revisions_given)
	{
		case 0:
			opt_target_revision=SVN_INVALID_REVNUM;
			opt_target_revision2=1;

			STOPIF( url__canonical_rev(current_url, &opt_target_revision), NULL);
			opt__set_int(OPT__LOG_MAXREV, PRIO_DEFAULT, 100);
			break;
		case 1:
			opt_target_revision2 = 1;
			opt__set_int(OPT__LOG_MAXREV, PRIO_DEFAULT, 1);
			break;
		case 2:
			opt__set_int(OPT__LOG_MAXREV, 
					PRIO_DEFAULT,
					labs(opt_target_revision-opt_target_revision2)+1);
			break;
		default:
			BUG("how many");
	}
  limit=opt__get_int(OPT__LOG_MAXREV);

	DEBUGP("log limit at %d", limit);


	status_svn=svn_ra_get_log(current_url->session, paths,
			opt_target_revision, opt_target_revision2,
			limit,
			opt__is_verbose() > 0,
			0, // TODO: stop-on-copy,
			log__receiver, 
			NULL, global_pool);

	if (status_svn)
	{
		if (status_svn->apr_err == -EPIPE)
			goto ex;
		STOPIF_SVNERR( status_svn, );
	}


	STOPIF( log___divider(stdout, ANSI__NORMAL), NULL);

ex:
	STOP_HANDLE_SVNERR(status_svn);
ex2:
	return status;
}

