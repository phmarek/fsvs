/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/time.h>
#include <time.h>
#include <stdarg.h>
#include <langinfo.h>
#include <locale.h>

#include <apr_pools.h>

#include <subversion-1/svn_error.h>

#include "global.h"
#include "interface.h"
#include "ignore.h"
#include "checksum.h"
#include "helper.h"
#include "waa.h"
#include "options.h"
#include "cp_mv.h"
#include "status.h"
#include "url.h"
#include "warnings.h"
#include "options.h"
#include "actions.h"
#include "racallback.h"

/** \file
 * The central parts of fsvs (main).
 * */


/** \defgroup add_unv_ign Adding and removing entries from versioning
 *
 * Normally all new entries are taken for versioning.
 * The following chapters show you how to get finer control. 
 *
 * Furthermore please take a look at the \ref ignore , and \ref add 
 * and \ref unversion commands. */

/** \defgroup dev Hints and documentation for developers
 * Some description of data structures, and similar.
 * */

/** \defgroup userdoc Documentation for users
 *
 * Here you find the basic documentations for FSVS. */


/** \defgroup cmds Commands and command line parameters
 * \ingroup userdoc
 *
 * fsvs is a client for subversion repositories; it is designed
 * for fast versioning of big directory trees.
 *
 * \section cmds_synopsis SYNOPSIS
 *
 * <tt>fsvs command [options] [args]</tt>
 *
 *
 * The following commands are understood by FSVS:
 *
 * \section cmds_local Local configuration and information:
 * <dl>
 *   <dt>\ref urls<dd><tt>Define working copy base 
 *   directories by their URL(s)</tt>
 *   <dt>\ref status<dd><tt>Get a list of changed entries</tt>
 *   <dt>\ref info<dd><tt>Display detailed information about 
 *   single entries</tt>
 *   <dt>\ref log<dd><tt>Fetch the log messages from the repository</tt>
 *   <dt>\ref diff<dd><tt>Get differences between files (local and 
 *   remote)</tt>
 *   <dt>\ref cpfd "copyfrom-detect"<dd><tt>Ask FSVS about probably 
 *   copied/moved/renamed entries; see \ref cp</tt>
 * </dl>
 *
 * \section cmds_au Defining which entries to take:
 * <dl>
 *   <dt>\ref ignore and \ref rign<dd><tt>Define ignore patterns</tt>
 *   <dt>\ref unversion<dd><tt>Remove entries from versioning</tt>
 *   <dt>\ref add<dd><tt>Add entries that would be ignored</tt>
 *   <dt>\ref cp, \ref mv<dd><tt>Tell FSVS that entries were 
 *   copied</tt>
 * </dl>
 *
 * \section cmds_rep Commands working with the repository:
 * <dl>
 *   <dt>\ref commit<dd><tt>Send changed data to the repository</tt>
 *   <dt>\ref update<dd><tt>Get updates from the repository</tt>
 *   <dt>\ref checkout<dd><tt>Fetch some part of the repository, and 
 *     register it as working copy</tt>
 *   <dt>\ref cat<dd><tt>Get a file from the directory
 *   <dt>\ref revert and \ref uncp<dd><tt>Undo local changes and 
 *     entry markings</tt>
 *   <dt>\ref remote-status<dd><tt>Ask what an \ref update 
 *   would bring</tt>
 * </dl>
 *
 * \section cmds_prop Property handling:
 * <dl>
 *   <dt>\ref prop-set<dd><tt>Set user-defined properties</tt>
 *   <dt>\ref prop-get<dd><tt>Ask value of user-defined properties</tt>
 *   <dt>\ref prop-list<dd><tt>Get a list of user-defined properties</tt>
 * </dl>
 *
 * \section cmds_rec Additional commands used for recovery and debugging:
 * <dl>
 *   <dt>\ref export<dd><tt>Fetch some part of the repository</tt>
 *   <dt>\ref sync-repos<dd><tt>Drop local information about the entries, 
 *     and fetch the current list from the repository.</tt>
 * </dl>
 *
 * \note Multi-url-operations are relatively new; there might be rough edges.
 *
 *
 * The <b>return code</b> is \c 0 for success, or \c 2 for an error.
 * \c 1 is returned if the option \ref o_stop_change is used, and 
 * changes are found; see also \ref o_filter.
 * 
 *
 * \section glob_opt Universal options
 *
 *
 * \subsection glob_opt_version -V -- show version
 * \c -V makes FSVS print the version and a copyright notice, and exit.
 *
 *
 * \subsection glob_opt_deb -d and -D -- debugging
 * If FSVS was compiled using \c --enable-debug you can enable printing 
 * of debug messages (to \c STDOUT) with \c -d.
 * Per default all messages are printed; if you're only interested in a 
 * subset, you can use \c -D \e start-of-function-name.
 * \code
 *      fsvs -d -D waa_ status
 * \endcode
 * would call the \a status action, printing all debug messages of all WAA 
 * functions - \c waa__init, \c waa__open, etc.
 *
 * For more details on the other debugging options \ref o_debug_output 
 * "debug_output" and \ref o_debug_buffer "debug_buffer" please see the 
 * options list.
 *
 *
 * \subsection glob_opt_rec -N, -R -- recursion
 * The \c -N and \c -R switches in effect just decrement/increment a  
 * counter; the behaviour is chosen depending on that. So a command line of 
 * <tt>-N -N -N -R -R</tt> is equivalent to <tt>-3 +2 = -1</tt>, this 
 * results in \c -N. 
 *
 *
 * \subsection glob_opt_verb -q, -v -- verbose/quiet
 * <tt>-v<tt>/<tt>-q<tt> set/clear verbosity flags, and so give more/less 
 * output.
 *
 * Please see \ref o_verbose "the verbose option" for more details.
 *
 *
 * \subsection glob_opt_chksum -C -- checksum
 * \c -C chooses to use more change detection checks; please see \ref 
 * o_chcheck "the change_check option" for more details.
 *
 * 
 * \subsection glob_opt_filter -f -- filter entries
 * This parameter allows to do a bit of filtering of entries, or, for some 
 * operations, modification of the work done on given entries.
 *
 * It requires a specification at the end, which can be any combination of 
 * \c any, \c text, \c new, \c deleted (or \c removed), \c meta, \c mtime, \c group, \c mode,
 * \c changed or \c owner; \c default or \c def use the default value.
 *
 * By giving eg. the value \c text, with a \ref status action only entries 
 * that are new or changed are shown; with \c mtime,group only entries 
 * whose group or modification time has changed are printed.
 *
 * \note Please see \ref o_chcheck for some more information.
 *
 * \note If an entry gets replaced with an entry of a different type (eg. a 
 * directory gets replaced by a file), that counts as \c deleted \b and \c 
 * new.
 *
 * If you use \c -v, it's used as a \c any internally.
 *
 * If you use the string \c none, it resets the bitmask to \b no entries 
 * shown; then you can built a new mask.
 * So \c owner,none,any,none,delete would show deleted entries.
 * If the value after all commandline parsing is \c none, it is reset to 
 * the default.
 *
 * 
 * \subsection glob_opt_warnings -W warning=action -- set warnings
 *
 * Here you can define the behaviour for certain situations that should not 
 * normally happen, but which you might encounter.
 *
 * The general format here is \e specification = \e action, where \e 
 * specification is a string matching the start of at least one of the 
 * defined situations, and \e action is one of these:
 * - \e once to print only a single warning,
 * - \e always to print a warning message \b every time,
 * - \e stop to abort the program,
 * - \e ignore to simply ignore this situation, or
 * - \e count to just count the number of occurrences.
 *
 * If \e specification matches more than one situation, all of them are 
 * set; eg. for \e meta=ignore all of \e meta-mtime, \e meta-user etc. are 
 * ignored.
 *
 * If at least a single warning that is \b not ignored is encountered 
 * during the program run, a list of warnings along with the number of 
 * messages it would have printed with the setting \e always is displayed, 
 * to inform the user of possible problems.
 *
 * The following situations can be handled with this:
 * <table>
 * <tr><td>\e meta-mtime, \e meta-user, \e meta-group, \e meta-umask
 * <td>These warnings are issued if a meta-data property that was fetched 
 * from the repository couldn't be parsed. This can only happen if some 
 * other program or a user changes properties on entries.<br>
 * In this case you can use \c -Wmeta=always or \c -Wmeta=count, until the 
 * repository is clean again.
 *
 * <tr><td>\e no-urllist<td>
 * This warning is issued if a \ref info action is executed, but no URLs 
 * have been defined yet.
 *
 * <tr><td>\e charset-invalid<td>
 * If the function \c nl_langinfo(3) couldn't return the name of the current 
 * character encoding, a default of UTF-8 is used.
 * You might need that for a minimal system installation, eg. on recovery.
 *
 * <tr><td>\e chmod-eperm, \e chown-eperm<td>
 * If you update a working copy as normal user, and get to update a file 
 * which has another owner but which you may modify, you'll get errors 
 * because neither the user, group, nor mode can be set. \n
 * This way you can make the errors non-fatal.
 *
 * <tr><td>\e chmod-other, \e chown-other<td>
 * If you get another error than \c EPERM in the situation above, you might 
 * find these useful.
 *
 * <tr><td>\e mixed-rev-wc<td>
 * If you specify some revision number on a \ref revert, it will complain 
 * that mixed-revision working copies are not allowed. \n
 * While you cannot enable mixed-revision working copies (I'm working on 
 * that) you can avoid being told every time.
 *
 * <tr><td>\e propname-reserved<td>
 * It is normally not allowed to set a property with the \ref prop-set 
 * action with a name matching some reserved prefixes.
 *
 * <tr><td>\anchor warn_ign_abs_not_base \e ignpat-wcbase<td>
 * This warning is issued if an \ref ignpat_shell_abs "absolute ignore 
 * pattern" does not match the working copy base directory. \n
 * See \ref ignpat_shell_abs "absolute shell patterns" for more details.
 *
 * <tr><td>\e diff-status<td>
 * GNU diff has defined that it returns an exit code 2 in case of an error; 
 * sadly it returns that also for binary files, so that a simply <tt>fsvs 
 * diff some-binary-file text-file</tt> would abort without printing the 
 * diff for the second file. \n
 * Because of this FSVS currently ignores the exit status of diff per 
 * default, but this can be changed by setting this option to eg. \e stop.
 *
 * </table>
 *
 * Also an environment variable FSVS_WARNINGS is used and parsed; it is 
 * simply a whitespace-separated list of option specifications.
 *
 *
 * \subsection glob_opt_urls -u URLname[@revision[:revision]] -- select URLs
 *
 * Some commands can be reduced to a subset of defined URLs; 
 * the \ref update command is a example.
 * 
 * If you have more than a single URL in use for your working copy, \c 
 * update normally updates \b all entries from \b all URLs. By using 
 * this parameter you can tell FSVS to update only the specified URLs.
 *
 * The parameter can be used repeatedly; the value can have multiple URLs, 
 * separated by whitespace or one of \c ",;".
 *
 * \code
 *   fsvs up -u base_install,boot@32 -u gcc
 * \endcode
 *
 * This would get \c HEAD of \c base_install and \c gcc, and set the target 
 * revision of the \c boot URL <b>for this command</b> at 32.
 *
 *
 * \subsection glob_options -o [name[=value]] -- other options
 * This is used for setting some seldom used option, for which default can 
 * be set in a configuration file (to be implemented, currently only 
 * command-line).
 *
 * For a list of these please see \ref options.
 *
 *
 * \section Signals
 * 
 * If you have a running FSVS, and you want to change its verbosity, you can send the process either
 * \c SIGUSR1 (to make it more verbose) or \c SIGUSR2 (more quiet). 
 *
 */


/** -.
 * */
char parm_dump[]="dump",
		 parm_test[]="test",
		 parm_load[]="load";


int debuglevel=0,
		/** -. We start with recursive by default. */
		opt_recursive=1;

svn_revnum_t target_revision;
svn_revnum_t opt_target_revision=SVN_INVALID_REVNUM;
svn_revnum_t opt_target_revision2=SVN_INVALID_REVNUM;
int opt_target_revisions_given=0;

char *opt_commitmsg,
		 *opt_debugprefix,
		 *opt_commitmsgfile;

/** -.
 * Is there some better way? And I don't want to hear about using C++ 
 * templates and generating each function twice - once with output and once  
 * without!
 * Maybe with some call that encapsulates this functionality, and uses some 
 * stack? Although we can simply increment/decrement this value. */
int make_STOP_silent=0;

/** Remember how the program was called. */
static char *program_name;
/** -. */
char *start_path=NULL;
/** -. */
int start_path_len=0;

#ifdef HAVE_LOCALES
char *local_codeset;
#endif

apr_pool_t *global_pool;

struct url_t *current_url;

/* For Solaris, which doesn't have one ... */
char **environ=NULL;


/** Opens the debug output file or pipe, as specified.
 *
 * If a debug buffer is given, this is filled first; and only in case of a 
 * buffer flush the given file or pipe is opened, to receive the buffer 
 * contents.
 *
 * This function cannot return errors. */
void _DEBUGP_open_output(FILE **output, int *was_popened)
{
	const char *fn;
	FILE *tmp;


	*output=stdout;
	*was_popened=0;

	fn=opt__get_string(OPT__DEBUG_OUTPUT);
	if (fn)
	{
		*was_popened= (fn[0] == '|');
		if (*was_popened)
			tmp=popen(fn+1, "w");
		else
			tmp=fopen(fn, "w");

		if (tmp) *output=tmp;
		else DEBUGP("'%s' cannot be opened: %d=%s", 
				opt__get_string(OPT__DEBUG_OUTPUT),
				errno, strerror(errno)); 
	}
}

/** This constant is used to determine when to rotate the debug output 
 * buffer. */
#define MAX_DEBUG_LINE_LEN (1024)

/** -.
 * Never called directly, used only via the macro DEBUGP().
 *
 * For uninitializing in the use case \c debug_buffer the \c line value is 
 * misused to show whether an error occured. */
void _DEBUGP(const char *file, int line, 
		const char *func, 
		char *format, ...)
{
	static struct timeval tv;
	static struct timezone tz;
	struct tm *tm;
	va_list va;
	static FILE *debug_out=NULL;
	static int was_popened=0;
	int ms;
	const char *fn;
	static char *buffer_start=NULL;
	static int did_wrap=0;
	FILE *real_out;
	long mem_pos;

	/* Uninit? */
	if (!file)
	{
		if (line && opt__get_int(OPT__DEBUG_BUFFER) && debug_out)
		{
			/* Error in program, do output. */
			_DEBUGP_open_output(&real_out, &was_popened);

			mem_pos=ftell(debug_out);
			if (mem_pos>=0 && did_wrap)
			{
				buffer_start[mem_pos]=0;

				/* Look for the start of a line. */
				fn=strchr(buffer_start+mem_pos,'\n');
				if (fn)
					fputs(fn+1, real_out);
			}
			fputs(buffer_start, real_out);

			/* This is just the mem stream */
			fclose(debug_out);
			/* The "real" stream might be a pipe. */
			debug_out=real_out;
		}
		/* Error checking makes not much sense ... */
		if (debug_out)
		{
			if (was_popened)
				pclose(debug_out);
			else
				if (debug_out != stdout)
					fclose(debug_out);
			debug_out=NULL;
		}
		return;
	}

	if (!debuglevel) return;

	/* look if matching prefix */
	if (opt_debugprefix &&
			strncmp(opt_debugprefix, func, strlen(opt_debugprefix)))
		return;

	if (!debug_out)
	{
		/* Default to STDOUT. */
		debug_out=stdout;

#ifdef ENABLE_DEBUGBUFFER
		if (opt__get_int(OPT__DEBUG_BUFFER))
		{
			buffer_start=malloc(opt__get_int(OPT__DEBUG_BUFFER));
			if (buffer_start)
				debug_out=fmemopen(buffer_start, 
						opt__get_int(OPT__DEBUG_BUFFER), "w+");

			if (buffer_start && debug_out)
			{
				DEBUGP("using a buffer of %d bytes.", opt__get_int(OPT__DEBUG_BUFFER));
			}
			else
			{
				opt__set_int(OPT__DEBUG_BUFFER, PRIO_MUSTHAVE, 0);
				debug_out=stdout;
				DEBUGP("cannot use memory buffer for debug");
			}
		}
		else
		{
			_DEBUGP_open_output(&debug_out, &was_popened);
		}
#else
		_DEBUGP_open_output(&debug_out, &was_popened);
#endif
	}

	gettimeofday(&tv, &tz);
	tm=localtime(&tv.tv_sec);
	/* Just round down, else we'd have to increment the other fields for 
	 * >= 999500 us. */
	ms=tv.tv_usec/1000;


#ifdef ENABLE_DEBUGBUFFER
	if (opt__get_int(OPT__DEBUG_BUFFER))
	{
		/* Check whether we should rotate. */
		mem_pos=ftell(debug_out);
		if (mem_pos+MAX_DEBUG_LINE_LEN >= opt__get_int(OPT__DEBUG_BUFFER))
		{
			/* What can possibly go wrong ;-/ */
			fseek(debug_out, 0, SEEK_SET);
			did_wrap++;
		}
	}
#endif

	fprintf(debug_out, "%02d:%02d:%02d.%03d %s[%s:%d] ",
			tm->tm_hour, tm->tm_min, tm->tm_sec, ms,
			func,
			file, line);

	va_start(va, format);
	vfprintf(debug_out, format, va);

	fputc('\n', debug_out);
	fflush(debug_out);
}


/** -.
 * It checks the given status code, and (depending on the command line flag
 * \ref glob_opt_verb "-v") prints only the first error or the whole call 
 * stack.
 * If \ref debuglevel is set, prints some more details - time, file and 
 * line.
 * Never called directly; only via some macros.
 *
 * In case the first character of the \a format is a <tt>"!"</tt>, it's a 
 * user error - here we normally print only the message, without the error 
 * code line. The full details are available via \c -d and \c -v.
 *
 * \c -EPIPE is handled specially, in that it is passed up, but no message  
 * is printed. */
int _STOP(const char *file, int line, const char *function,
		int errl, const char *format, ...)
{
	static int already_stopping=0;
	static int error_number;
	int is_usererror;
	struct timeval tv;
	struct timezone tz;
	struct tm *tm;
	va_list va;
	FILE *stop_out=stderr;
	char errormsg[256];


	if (make_STOP_silent) return errl;
	if (errl==-EPIPE) return errl;

	is_usererror= format && *format == '!';
	if (is_usererror) format++;


	/* With verbose all lines are printed; else only the first non-empty. */
	if ( (already_stopping || !format) &&
			!(opt__get_int(OPT__VERBOSE) & VERBOSITY_STACKTRACE))
		return error_number;

	if (! (already_stopping++))
	{
		/* flush STDOUT and others */
		fflush(NULL);

		if (is_usererror)
		{
			va_start(va, format);
			vfprintf(stop_out, format, va);
			if (!(debuglevel || opt__is_verbose()>0))
				goto eol;
		}


		fputs("\n\nAn error occurred", stop_out);

		if (debuglevel || opt__is_verbose()>0)
		{
			gettimeofday(&tv, &tz);
			tm=localtime(&tv.tv_sec);
			fprintf(stop_out, " at %02d:%02d:%02d.%03d",
					tm->tm_hour, tm->tm_min, tm->tm_sec,  (int)(tv.tv_usec+500)/1000);
		}

		errormsg[0]=0;
		svn_strerror (errl, errormsg, sizeof(errormsg));
		fprintf(stop_out, ": %s (%d)\n",
				errormsg[0] ? errormsg : strerror(abs(errl)), errl);
	}

	/* Stacktrace */
	fputs("  in ", stop_out);
	fputs(function, stop_out);
	if (debuglevel)
		fprintf(stop_out, " [%s:%d]", file, line);

	if (format)
	{
		fputs(": ", stop_out);

		va_start(va, format);
		vfprintf(stop_out, format, va);
	}

eol:
	fputc('\n', stop_out);
	fflush(stop_out);

	error_number=errl;
	already_stopping=1;
	return errl;
}


#define _STRINGIFY(x) #x
#define STRINGIFY(x) " " #x "=" _STRINGIFY(x)

/** For keyword expansion - the version string. */
const char* Version(FILE *output)
{
	static const char Id[] ="$Id: fsvs.c 2420 2010-01-25 09:29:49Z pmarek $";

	fprintf(output, "FSVS (licensed under the GPLv3), (C) by Ph. Marek;"
			" version " FSVS_VERSION "\n");
	if (opt__is_verbose()>0)
	{
		fprintf(output, "compiled on " __DATE__ " " __TIME__ 
				", with options:\n\t"
#ifdef HAVE_VALGRIND
				STRINGIFY(HAVE_VALGRIND)
#endif
#ifdef HAVE_VALGRIND_VALGRIND_H
				STRINGIFY(HAVE_VALGRIND_VALGRIND_H)
#endif
#ifdef ENABLE_DEBUG
				STRINGIFY(ENABLE_DEBUG)
#endif
#ifdef ENABLE_GCOV
				STRINGIFY(ENABLE_GCOV)
#endif
#ifdef ENABLE_RELEASE
				STRINGIFY(ENABLE_RELEASE)
#endif
#ifdef HAVE_LOCALES
				STRINGIFY(HAVE_LOCALES)
#endif
#ifdef HAVE_UINT32_T
				STRINGIFY(HAVE_UINT32_T)
#endif
#ifdef AC_CV_C_UINT32_T
				STRINGIFY(AC_CV_C_UINT32_T)
#endif
#ifdef HAVE_LINUX_TYPES_H
				STRINGIFY(HAVE_LINUX_TYPES_H)
#endif
#ifdef HAVE_LINUX_UNISTD_H
				STRINGIFY(HAVE_LINUX_UNISTD_H)
#endif
#ifdef HAVE_DIRFD
				STRINGIFY(HAVE_DIRFD)
#endif
#ifdef HAVE_STRUCT_STAT_ST_MTIM
				STRINGIFY(HAVE_STRUCT_STAT_ST_MTIM)
#endif
#ifdef CHROOTER_JAIL
				STRINGIFY(CHROOTER_JAIL)
#endif
#ifdef HAVE_COMPARISON_FN_T
				STRINGIFY(HAVE_COMPARISON_FN_T)
#endif
#ifdef HAVE_O_DIRECTORY
				STRINGIFY(HAVE_O_DIRECTORY)
#endif
#ifdef O_DIRECTORY
				STRINGIFY(O_DIRECTORY)
#endif
#ifdef HAVE_LINUX_KDEV_T_H
				STRINGIFY(HAVE_LINUX_KDEV_T_H)
#endif
#ifdef ENABLE_DEV_FAKE
				STRINGIFY(ENABLE_DEV_FAKE)
#endif
#ifdef DEVICE_NODES_DISABLED
				STRINGIFY(DEVICE_NODES_DISABLED)
#endif
#ifdef HAVE_STRSEP
				STRINGIFY(HAVE_STRSEP)
#endif
#ifdef HAVE_LUTIMES
				STRINGIFY(HAVE_LUTIMES)
#endif
#ifdef HAVE_LCHOWN
				STRINGIFY(HAVE_LCHOWN)
#endif
#ifdef WAA_WC_MD5_CHARS
				STRINGIFY(WAA_WC_MD5_CHARS)
#endif
#ifdef HAVE_FMEMOPEN
				STRINGIFY(HAVE_FMEMOPEN)
#endif
#ifdef ENABLE_DEBUGBUFFER
				STRINGIFY(ENABLE_DEBUGBUFFER)
#endif
				STRINGIFY(NAME_MAX)
				"\n");
	}
	return Id;
}


/** \addtogroup cmds
 *
 * \section help
 *
 * \code
 * help [command]
 * \endcode
 *
 * This command shows general or specific \ref help (for the given 
 * command). A similar function is available by using \c -h or \c -? after 
 * a command.
 * */
/** -.
 * Prints help for the given action.
 * */
int ac__Usage(struct estat *root UNUSED, 
		int argc UNUSED, char *argv[])
{
	int status;
	int i, hpos, len;
	char const* const*names;


	status=0;
	Version(stdout);

	/* Show help for a specific command? */
	if (argv && *argv)
	{
		STOPIF( act__find_action_by_name(*argv, &action), NULL);
		printf("\n"
				"Help for command \"%s\".\n", action->name[0]);
		names=action->name+1;
		if (*names)
		{
			printf("Aliases: ");
			while (*names)
			{
				printf("%s%s",
						names[0],
						names[1] ? ", " : "\n");
				names++;
			}
		}

		puts("");

		puts(action->help_text);
	}
	else
	{
		/* Print generic help text: list of commands, parameters. */
		printf(
				"\n"
				"Known commands:\n"
				"\n  ");
		hpos=2;
		for(i=0; i<action_list_count; i++)
		{
			len = strlen(action_list[i].name[0]);

			/* I know, that could be dependent on terminal width. */
			if (hpos+2+len >= 75) 
			{
				printf("\n  ");
				hpos=2;
			}

			printf("%s%s", action_list[i].name[0], 
					i+1 == action_list_count ? "\n" : ", ");
			hpos += 2 + len;
		}

		puts(
				"\n"
				"Parameters:\n"
				"\n"
				"-v     increase verbosity\n"
				"-q     decrease verbosity (quiet)\n"
				"\n"
				"-C     checksum possibly changed files;\n"
				"       if given twice checksum *all* files.\n"
				"\n"
				"-V     show version\n"
				"\n"
				"Environment variables:\n"
				"\n"
				"$FSVS_CONF  defines the location of the FSVS Configuration area\n"
				"            Default is " DEFAULT_CONF_PATH ", but any writeable directory is allowed.\n"
				"$FSVS_WAA   defines the location of the Working copy Administrative Area\n"
				"            Default is " DEFAULT_WAA_PATH ", but any writeable directory is allowed.\n"
				);
	}

ex:
	exit(status);

	/* Never done */
	return 0;
}

/** USR1 increases FSVS' verbosity. */
void sigUSR1(int num)
{
	if (opt__verbosity() < VERBOSITY_DEFAULT)
		opt__set_int(OPT__VERBOSE, PRIO_MUSTHAVE, VERBOSITY_DEFAULT);
	else if (debuglevel < 3) 
	{
		debuglevel++;
		DEBUGP("more debugging via SIGUSR1");
	}
}
/** USR2 decreases FSVS' verbosity. */
void sigUSR2(int num)
{
	if (debuglevel)
	{
		DEBUGP("less debugging via SIGUSR2");
		debuglevel--;
	}
	else if (opt__verbosity() >= VERBOSITY_DEFAULT)
		opt__set_int(OPT__VERBOSE, PRIO_MUSTHAVE, VERBOSITY_QUIET);
}


/** Handler for SIGPIPE.
 * We give the running action a single chance to catch an \c EPIPE, to 
 * clean up on open files and similar; if it doesn't take this chance, the 
 * next \c SIGPIPE kills FSVS. */
void sigPipe(int num)
{
	DEBUGP("got SIGPIPE");
	signal(SIGPIPE, SIG_DFL);
}


/** Signal handler for debug binaries.
 * If the \c configure run included \c --enable-debug, we intercept 
 * \c SIGSEGV and try to start \c gdb. 
 *
 * We use a pipe to stop the parent process; debugging within gdb normally
 * starts with a \c bt (backtrace), followed by <tt>up 3</tt> to skip over
 * the signal handler and its fallout. */
#ifdef ENABLE_DEBUG
/// FSVS GCOV MARK: sigDebug should not be executed
void sigDebug(int num)
{
	char ppid_str[20];
	int pid;
	int pipes[2];


	/* if already tried to debug, dump core on next try. */
	signal(SIGSEGV, SIG_DFL);

	/* Try to spew the debug buffer. */
	_DEBUGP(NULL, EBUSY, NULL, NULL);

	/* We use a pipe here for stopping/continuing the active process.
	 *
	 * The child tries to read from the pipe. That blocks.
	 * The parent tries to start gdb.
	 *   - If the exec() returns with an error, we simply close
	 *     the pipe, and the child re-runs into its SEGV.
	 *   - If the exec() works, the child will be debugged.
	 *     When gdb exits, the pipe end is closed, so the child
	 *     will no longer be blocked. */
	pipes[0]=pipes[1]=-1;
	if ( pipe(pipes) == -1) goto ex;

	pid=fork();
	if (pid == -1) return;

	if (pid)
	{
		/* Parent tries to start gdb for child.
		 * We already have the correct pid. */
		close(pipes[0]);

		sprintf(ppid_str, "%d", pid);
		execlp("gdb", "gdb", program_name, ppid_str, NULL);

		close(pipes[1]);
		exit(1);
	}
	else
	{
		/* Parent.
		 * Either gdb attaches, or the child interrupts the read with
		 * a signal. */
		close(pipes[1]);
		pipes[1]=-1;
		read(pipes[0], &pid, 1);
	}

ex:
	if (pipes[0] != -1) close(pipes[0]);
	if (pipes[1] != -1) close(pipes[1]);
}


/** This function is used for the component tests.
 * When FSVS is run with "-d" as parameter, a call to fileno() is 
 * guaranteed to happen here; a single "up" gets into this stack frame, and 
 * allows easy setting/quering of some values. */
void *_do_component_tests(int a)
{
	/* How not to have them optimized out? */
	static int int_array[10];
	static void *voidp_array[10];
	static char *charp_array_1[10];
	static char *charp_array_2[10];
	static char **charpp;
	static char buffer[1024];
	static struct estat *estat_array[10];

	int_array[0]=fileno(stdin);
	voidp_array[0]=stdin+fileno(stdout);
	buffer[0]=fileno(stderr);
	charpp=charp_array_2+4;

	switch(a)
	{
		case 4: return int_array;
		case 9: return voidp_array;
		case 6: return buffer;
		case 2: return charp_array_1;
		case 3: return estat_array;
		case 7: return charpp;
		case 8: return charp_array_2;
	}
	return NULL;
}
#endif


/** The main function. 
 *
 * It does the following things (not in that order):
 * - Initializes the various parts 
 *   - APR (apr_initialize()), 
 *   - WAA (waa__init()), 
 *   - RA (svn_ra_initialize()), 
 *   - Callback functions (cb__init()) ...
 *   - Local charset (\c LC_CTYPE)
 * - Processes the command line. In glibc the options are reordered to the
 *   front; on BSD systems this is not done, so there's an extra loop to do
 *   that manually.
 *   We want all options processed, and then the paths (or other parameters)
 *   in a clean list.
 * - And calls the main action.
 *
 * <h1>How the parameters get mungled - example</h1>
 *
 * On entry we have eg. this:
 * \dot
 * digraph {
 *   rankdir=LR;
 *   node [shape=rectangle, fontsize=10];
 *
 *   parm [shape=record,
 *     label="{ { <0>fsvs | <1>update | <2>-u | <3>baseinstall | <4>/bin }}"]
 *
 *   list [shape=record,
 *     label="{ args | { <0>0 | <1>1 | <2>2 | <3>3 | <4>4 | NULL } }" ];
 *
 *   list:0:e -> parm:0:w;
 *   list:1:e -> parm:1:w;
 *   list:2:e -> parm:2:w;
 *   list:3:e -> parm:3:w;
 *   list:4:e -> parm:4:w;
 *
 *   N1 [label="NULL", shape=plaintext];
 *   N2 [label="NULL", shape=plaintext];
 *   url__parm_list -> N1;
 *
 *   program_name -> N2;
 * }
 * \enddot
 *
 * After command line parsing we have:
 * \dot
 * digraph {
 *   rankdir=LR;
 *   node [shape=rectangle, fontsize=10];
 *
 *   parm [shape=record,
 *     label="{ { <0>fsvs | <1>update | <2>-u | <3>baseinstall | <4>/bin }}"]
 *
 *   list [shape=record,
 *     label="{ args | { <0>0 | <1>1 | NULL | <3>3 | <4>4 | NULL }}" ];
 *
 *   list:0:e -> parm:1:w;
 *   list:1:e -> parm:4:w;
 *   list:3:e -> parm:3:w;
 *   list:4:e -> parm:4:w;
 *
 *   program_name -> parm:0:w;
 *
 *   ulist [shape=record,
 *     label="{ url__parm_list | { <0>0 | NULL } }" ];
 *   ulist:0:e -> parm:3:w;
 * }
 * \enddot
 *
 *
 * <h2>Argumentation for parsing the urllist</h2>
*
* I'd have liked to keep the \ref url__parm_list in the original \a args 
* as well; but we cannot easily let it start at the end, and putting it 
* just after the non-parameter arguments
* - might run out of space before some argument (because two extra \c NULL 
		*   pointers are needed, and only a single one is provided on startup),
	* - and we'd have to move the pointers around every time we find a 
	*   non-option argument.
	*
	* Consider the case <tt>[fsvs, update, /bin/, -uURLa, -uURLb, /lib, 
	* NULL]</tt>.
	* That would be transformed to
	* -# <tt>[update, NULL, /bin/, -uURLa, -uURLb, /lib, NULL]</tt>
	* -# <tt>[update, /bin/, NULL, -uURLa, -uURLb, /lib, NULL]</tt>
	* -# And now we'd have to do <tt>[update, /bin/, NULL, -uURLa, NULL, 
	*  -uURLb, /lib, NULL]</tt>; this is too long.
	*   We could reuse the \c NULL at the end ... but that's not that fine, 
	*   either -- the \ref url__parm_list wouldn't be terminated.
	*
	* So we go the simple route - allocate an array of pointers, and store 
	* them there.
	*
	* */
int main(int argc, char *args[], char *env[])
{
	struct estat root = { };
	int status, help;
	char *cmd;
	svn_error_t *status_svn;
	int eo_args, i;
	void *mem_start, *mem_end;


	help=0;
	eo_args=1;
	environ=env;
	program_name=args[0];
#ifdef ENABLE_DEBUG
	/* If STDOUT and STDIN are on a terminal, we could possibly be 
	 * debugged. Depends on "configure --enable-debug". */
	if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
		signal(SIGSEGV, sigDebug);

	/* Very early debugging */
	cmd=getenv(FSVS_DEBUG_ENV);
	if (cmd)
		debuglevel = atoi(cmd);
#endif

	signal(SIGPIPE, sigPipe);
	signal(SIGUSR1, sigUSR1);
	signal(SIGUSR2, sigUSR2);
	mem_start=sbrk(0);


#ifdef HAVE_LOCALES
	/* Set the locale from the environment variables, so that we get the 
	 * correct codeset told.
	 * Do that while still in the chroot jail. */
	cmd=setlocale(LC_ALL, "");
	DEBUGP("LC_ALL gives %s", cmd);
	/* The second call is in case that the above fails.
	 * Sometimes eg. LC_PAPER is set to an invalid value; then the first
	 * call fails, but the seconds succeeds.
	 * See also the fsvs dev@ mailing list (April 2006), where a post
	 * to dev@subversion is referenced */
	cmd=setlocale(LC_CTYPE, "");
	DEBUGP("LC_CTYPE gives %s", cmd);

	local_codeset=nl_langinfo(CODESET);
	if (!local_codeset)
	{
		STOPIF( wa__warn(WRN__CHARSET_INVALID, EINVAL,
					"Could not retrieve the current character set - assuming UTF-8."),
				"nl_langinfo(CODESET) failed - check locale configuration.");
	}
	else
	{
		DEBUGP("codeset found to be %s", local_codeset);
		if (strcmp(local_codeset, "UTF-8")==0)
			/* man page says "This pointer MAY point to a static buffer ..."
			 * so no freeing. */
			local_codeset=NULL;
	}

	if (!local_codeset)
		DEBUGP("codeset: using identity");
#else
	DEBUGP("build without locales");
#endif


	/* Are we running in a chroot jail? Try to preload libraries, and escape.  
	 * 
	 * Originally there was an "#ifdef CHROOTER_JAIL" around this line.
	 * But if this is compiled in unconditionally a precompiled binary of 
	 * your favourite distribution should work, too! (So there could be a 
	 * script that fetches all needed libraries out of eg. debian-testing and 
	 * prepares a chroot for you. Any volunteers ;-?
	 * 
	 * As this function keeps quiet if \b no needed environment variable is 
	 * set it's just a small bit of additional work. */
	STOPIF( hlp__chrooter(), NULL);

	/* Load options from environment variables. */
	STOPIF( opt__load_env(environ), NULL);
	STOPIF( waa__save_cwd(&start_path, &start_path_len, 0), NULL);


	if (!isatty(STDOUT_FILENO))
		opt__set_int( OPT__STATUS_COLOR, PRIO_PRE_CMDLINE, 0);

	/* direct initialization doesn't work because
	 * of the anonymous structures */
	root.repos_rev=0;
	root.name=root.strings=".";
	root.st.size=0;
	root.st.mode=S_IFDIR | 0700;
	root.entry_count=0;
	/* The problem is that the root entry is never done explicitly; so we 
	 * have to hard-code that here; but it is the default for all entries 
	 * anyway. */
	root.do_filter_allows=1;
	root.do_filter_allows_done=1;


	while (1)
	{
		/* The GNU version of getopt re-orders the parameters and looks
		 * after non-options too; the BSD versions do not.
		 * So we have to look ourselves whether there's a -- or 
		 * end-of-arguments.
		 * And we reorder to the front of the array, so that the various
		 * parts can parse their data. */
		status=getopt(argc, args, "+a:VhdvCm:F:D:qf:r:W:NRo:u:?");
		if (status == -1) 
		{
			DEBUGP("no argument at optind=%d of %d",optind, argc);
			/* End of array or -- found? */
			if (optind == argc) break;
			/* Note that this is safe - the program name is always at
			 * front (optind starts with 1), so it's never negative */
			if (strcmp("--", args[optind-1])==0) 
			{
				/* Copy the rest of the arguments and stop processing */
				while (optind < argc) args[eo_args++] = args[optind++];
				break;
			}

			/* Normal argument (without "-") found, put to front. */
			args[eo_args++]=args[optind++];
			continue;
		}

		switch (status)
		{
			case '?':
			case 'h':
			default:
				help=1;
				break;

			case 'W':
				STOPIF( wa__set_warn_option(optarg, PRIO_CMDLINE),
						"Warning option '%s' is invalid", optarg);
				break;

			case 'C':
				i = hlp__rightmost_0_bit(opt__get_int(OPT__CHANGECHECK));
				opt__set_int(OPT__CHANGECHECK, PRIO_CMDLINE,
						opt__get_int(OPT__CHANGECHECK) | i);
				break;

			case 'o':
				STOPIF( opt__parse( optarg, NULL, PRIO_CMDLINE, 0), 
						"!Cannot parse option string '%s'.", optarg);
				break;

			case 'f':
				STOPIF( opt__parse_option(OPT__FILTER, PRIO_CMDLINE, optarg), NULL);
				break;

			case 'u':
				/* Some functions want URLs \b and some filename parameter (eg.  
				 * update, to define the wc base), and we have to separate them.
				 *
				 * As update will take an arbitrary number of URLs and filenames, 
				 * there's no easy way to tell them apart; and we have to do that 
				 * _before_ we know the wc base, so we cannot just try to lstat() 
				 * them; and we know the URLs (and their names) only _after- we 
				 * know the wc root. So we would have to guess here.  */
				STOPIF( url__store_url_name(optarg), NULL);
				break;

				/* Maybe we should warn if -R or -N are used for commands that 
				 * don't use them? Or, better yet, use them everywhere (where 
				 * applicable).  */
			case 'R':
				opt_recursive++;
				break;
			case 'N':
				opt_recursive--;
				break;

			case 'F':
				if (opt_commitmsg) ac__Usage_this();
				opt_commitmsgfile=optarg;
				break;
			case 'm':
				if (opt_commitmsgfile) ac__Usage_this();
				opt_commitmsg=optarg;
				break;

			case 'r':
				/* strchrnul is GNU only - won't work on BSD */
				cmd=strchr(optarg, ':');
				if (cmd) *(cmd++)=0;

				STOPIF( hlp__parse_rev(optarg, NULL, &opt_target_revision), NULL);
				opt_target_revisions_given=1;
				/* Don't try to parse further if user said "-r 21:" */
				if (cmd && *cmd)
				{
					STOPIF( hlp__parse_rev(cmd, NULL, &opt_target_revision2), NULL);
					opt_target_revisions_given=2;
				}
				break;

#if ENABLE_RELEASE
			case 'D':
			case 'd':
				fprintf(stderr, "This image was compiled as a release "
						"(without debugging support).\n"
						"-d and -D are not available.\n\n");
				exit(1);
#else
			case 'D':
				opt_debugprefix=optarg;
				if (!debuglevel) debuglevel++;
				/* Yes, that could be merged with the next lines ... :*/
				break;
			case 'd':
				/* Debugging wanted.
				 * If given twice, any debugbuffer and debug_output settings are 
				 * overridden, to get clear, find debug outputs directly to the 
				 * console. */
				if (debuglevel == 1)
				{
					/* Close any redirection or memory buffer that might already be 
					 * used. */
					_DEBUGP(NULL, 0, NULL, NULL);

					/* Manual override. */
					opt__set_string(OPT__DEBUG_OUTPUT, PRIO_MUSTHAVE, NULL);
					opt__set_int(OPT__DEBUG_BUFFER, PRIO_MUSTHAVE, 0);

					/* Now we're going directly. */
					DEBUGP("Debugging set to unfiltered console");
				}
				debuglevel++;
				break;
#endif
			case 'q':
				/* -q gives a default level, or, if already there, goes one level 
				 * down.
				 * If we would like to remove one bit after another from the 
				 * bitmask, we could use
				 *   i=opt__get_int(OPT__VERBOSE);
				 *   if (i < VERBOSITY_DEFAULT) i=0;
				 *   else i &= ~hlp__rightmost_0_bit(~i);
				 *   opt__set_int(OPT__VERBOSE, PRIO_CMDLINE, i);
				 * (similar to the 'v' case) */
				opt__set_int(OPT__VERBOSE, PRIO_CMDLINE, 
						opt__verbosity() <= VERBOSITY_QUIET ?
						VERBOSITY_VERYQUIET : VERBOSITY_QUIET);
				break;

			case 'v':
				/* A bit more details. */
				i=opt__get_int(OPT__VERBOSE);
				if (i == VERBOSITY_QUIET)
					i=VERBOSITY_DEFAULT;
				else
					i |= hlp__rightmost_0_bit(i);

				opt__set_int(OPT__VERBOSE, PRIO_CMDLINE, i);

				/* If not overridden by the commandline explicitly */
				opt__set_int(OPT__FILTER, PRIO_PRE_CMDLINE, FILTER__ALL);
				break;
				
			case 'V':
				Version(stdout);
				exit(0);
		}
	}

	/* Now limit the number of arguments to the ones really left */
	argc=eo_args;
	/* Set delimiter */
	args[argc]=NULL;
	/* Say we're starting with index 1, to not pass the program name. */
	optind=1;


	/* Special case: debug buffer means capture, but don't print normally. */
	if (opt__get_int(OPT__DEBUG_BUFFER) && 
			opt__get_prio(OPT__DEBUG_BUFFER)==PRIO_CMDLINE &&
			!debuglevel)
	{
		debuglevel++;
		DEBUGP("debug capturing started by the debug_buffer option.");
	}


	/* first non-argument is action */
	if (args[optind])
	{
		cmd=args[optind];
		optind++;

		STOPIF( act__find_action_by_name(cmd, &action), NULL);
		if (help) ac__Usage_this();
	}
	else
	{
		if (help) ac__Usage_dflt();
		action=action_list+0;
	}

	DEBUGP("optind=%d per_sts=%d action=%s rec=%d filter=%s verb=0x%x", 
			optind, 
			(int)sizeof(root),
			action->name[0],
			opt_recursive,
			st__status_string_fromint( opt__get_int(OPT__FILTER)),
			opt__verbosity());

	for(eo_args=1; eo_args<argc; eo_args++)
		DEBUGP("argument %d: %s", eo_args, args[eo_args]);


	/* waa__init() depends on some global settings, so do here.
	 * Must happen before loading values, as the WAA and CONF paths get 
	 * fixed. */
	STOPIF( waa__init(), NULL);

	/* Load options from config file, whose path is specified eg. via 
	 * environment. */
	strcpy(conf_tmp_fn, "config");
	STOPIF( opt__load_settings(conf_tmp_path, NULL, PRIO_ETC_FILE ), NULL);


#ifdef ENABLE_DEBUG
	/* A warning, which is ignored per default. Just to allow more testing 
	 * coverage. */
	STOPIF( wa__warn( WRN__TEST_WARNING, 0, "test warning" ), NULL );

	/* We allocate some stack usage in _do_component_tests(), to allow the 
	 * component tests to work. */
	if (debuglevel) _do_component_tests(optind);
#endif

	/* Do some initializations. Some of them won't always be needed - 
	 * maybe we should do another flag in the action list, if that turns
	 * out to be slow sometimes (DNS failure/misconfiguration, loading
	 * delay, ...) */
	STOPIF( apr_initialize(), "apr_initialize");
	STOPIF( apr_pool_create_ex(&global_pool, NULL, NULL, NULL), 
			"create an apr_pool");
	STOPIF_SVNERR( svn_ra_initialize, (global_pool));
	STOPIF_SVNERR( cb__init, (global_pool));


	STOPIF( action->work(&root, argc-optind, args+optind), 
			"action %s failed", action->name[0]);

	/* Remove copyfrom records in the database, if any to do. */
	STOPIF( cm__get_source(NULL, NULL, NULL, NULL, status), 
			NULL);

	/* Maybe we should try that even if we failed? 
	 * Would make sense in that the warnings might be helpful in determining
	 * the cause of a problem.
	 * But the error report would scroll away, so we don't do that. */
	STOPIF( wa__summary(), NULL);

	STOPIF( url__close_sessions(), NULL);

ex:
	mem_end=sbrk(0);
	DEBUGP("memory stats: %p to %p, %llu KB", 
			mem_start, mem_end, (t_ull)(mem_end-mem_start)/1024);
	if (status == -EPIPE)
	{
		DEBUGP("got EPIPE, ignoring.");
		status=0;
	}

	_DEBUGP(NULL, status, NULL, NULL);

	if (status) return 2;

	return 0;
}

/* vim: set cinoptions=*200 fo-=a : */
