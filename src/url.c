/************************************************************************
 * Copyright (C) 2006-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/select.h>
#include <subversion-1/svn_dirent_uri.h>


#include "url.h"
#include "waa.h"
#include "cache.h"
#include "helper.h"
#include "est_ops.h"
#include "checksum.h"
#include "racallback.h"


/** \file
 * \ref urls action, and functions for URLs.
 * */

/** \addtogroup cmds
 * 
 * \section urls
 *
 * \code
 * fsvs urls URL [URLs...]
 * fsvs urls dump
 * fsvs urls load
 * \endcode
 *
 * Initializes a working copy administrative area and connects 
 * \c the current working directory to \c REPOS_URL. All commits and
 * updates will be done to this directory and against the given URL.
 *
 * Example:
 * \code
 * fsvs urls http://svn/repos/installation/machine-1/trunk
 * \endcode
 *
 * For a format definition of the URLs please see the chapter \ref 
 * url_format.
 *
 * \note
 * If there are already URLs defined, and you use that command later again,
 * please note that as of 1.0.18 <b>the older URLs are not overwritten</b>
 * as before, but that the new URLs are \b appended to the given list!
 * If you want to start afresh, use something like
 * \code
 * true | fsvs urls load
 * \endcode
 *
 *
 * \subsection urls_load Loading URLs
 *
 * You can load a list of URLs from \c STDIN; use the \c load subcommand
 * for that.
 *
 * Example:
 * \code
 * ( echo 'N:local,prio:10,http://svn/repos/install/machine-1/trunk' ;
 *     echo 'P:50,name:common,http://svn/repos/install/common/trunk' ) |
 *   fsvs urls load
 * \endcode
 *
 * Empty lines are ignored.
 *
 * 
 * \subsection urls_dump Dumping the defined URLs
 *
 * To see which URLs are in use for the current WC, you can use \c dump.
 *
 * As an optional parameter you can give a format statement:
 * <table>
 * <tr><td>\c %p<td>priority
 * <tr><td>\c %n<td>name
 * <tr><td>\c %r<td>current revision
 * <tr><td>\c %t<td>target revision
 * <tr><td>\c %R<td>readonly-flag
 * <tr><td>\c %u<td>URL
 * <tr><td>\c %I<td>internal number for this URL
 * </table>
 *
 * \note That's not a real \c printf()-format; only these and a few \c \\ 
 * sequences are recognized.
 *
 * Example:
 * \code
 * fsvs urls dump "  %u %n:%p\\n"
 *   http://svn/repos/installation/machine-1/trunk local:10
 *   http://svn/repos/installation/common/trunk common:50
 * \endcode 
 *
 * The default format is \c "name:%n,prio:%p,target:%t,ro:%r,%u\\n"; for a 
 * more readable version you can use \ref glob_opt_verb "-v".
 *
 *
 * \subsection urls_modif Modifying URLs
 *
 * You can change the various parameters of the defined URLs like this:
 * \code
 * # Define an URL
 * fsvs urls name:url1,target:77,readonly:1,http://anything/...
 * # Change values
 * fsvs urls name:url1,target:HEAD
 * fsvs urls readonly:0,http://anything/...
 * fsvs urls name:url1,prio:88,target:32
 * \endcode 
 *
 * \note FSVS as yet doesn't store the whole tree structures of all URLs.  
 * So if you change the priority of an URL, and re-mix the directory trees 
 * that way, you'll need a \ref sync-repos and some \ref revert commands.  
 * I'd suggest to avoid this, until FSVS does handle that case better.
 *
 * */

/** \defgroup url_format Format of URLs
 * \ingroup userdoc
 *
 * FSVS can use more than one URL; the given URLs are \e overlaid according 
 * to their priority.
 *
 * For easier managing they get a name, and can optionally take a target 
 * revision.
 *
 * Such an <i>extended URL</i> has the form
 * \code
 *   ["name:"{name},]["target:"{t-rev},]["prio:"{prio},]URL
 * \endcode
 * where URL is a standard URL known by subversion -- 
 * something like <tt>http://....</tt>, <tt>svn://...</tt> or
 * <tt>svn+ssh://...</tt>.
 *
 * The arguments before the URL are optional and can be in any 
 * order; the URL must be last.
 *
 * Example:
 * \code
 *   name:perl,prio:5,svn://...
 * \endcode
 * or, using abbreviations,
 * \code
 *   N:perl,P:5,T:324,svn://...
 * \endcode
 *
 * Please mind that the full syntax is in lower case, whereas the 
 * abbreviations are capitalized! \n
 * Internally the \c : is looked for, and if the part before this character
 * is a known keyword, it is used. \n
 * As soon as we find an unknown keyword we treat it as an URL, ie. stop 
 * processing. 
 *
 * The priority is in reverse numeric order - the lower the number, the
 * higher the priority. (See \c url__current_has_precedence() )
 *
 *
 * \section url_prio Why a priority?
 *
 * When we have to overlay several URLs, we have to know \b which URL
 * takes precedence - in case the same entry is in more than one. <b>(Which
 * is \b not recommended!)</b>
 *
 *
 * \section url_name Why a name?
 *
 * We need a name, so that the user can say <b>"commit all outstanding 
 * changes to the repository at URL x"</b>, without having to remember the 
 * full URL.
 * After all, this URL should already be known, as there's a list of URLs to 
 * update from.
 *
 * You should only use alphanumeric characters and the underscore here; or, 
 * in other words, \c \\w or \c [a-zA-Z0-9_]. (Whitespace, comma and 
 * semicolon get used as separators.)
 *
 *
 * \section url_target What can I do with the target revision?
 *
 * Using the target revision you can tell fsvs that it should use the given 
 * revision number as destination revision - so update would go there, but 
 * not further.
 * Please note that the given revision number overrides the \c -r 
 * parameter; this sets the destination for all URLs.
 *
 * The default target is \c HEAD.
 *
 * \note In subversion you can enter \c URL\@revision - this syntax may be 
 * implemented in fsvs too. (But it has the problem, that as soon as you 
 * have a \c @ in the URL, you \b must give the target revision every 
 * time!)
 *
 *
 * \section url_intnum There's an additional internal number - why that?
 *
 * This internal number is not for use by the user. \n
 * It is just used to have an unique identifier for an URL, without using
 * the full string.
 *
 * On my system the package names are on average 12.3 characters long
 * (1024 packages with 12629 bytes, including newline):
 * \code
 *   COLUMNS=200 dpkg-query -l | cut -c5- | cut -f1 -d" " | wc
 * \endcode
 *
 * So if we store an \e id of the url instead of the name, we have
 * approx. 4 bytes per entry (length of strings of numbers from 1 to 1024).
 * Whereas using the needs name 12.3 characters, that's a difference of 8.3 
 * per entry.
 *
 * Multiplied with 150 000 entries we get about 1MB difference in filesize
 * of the dir-file. Not really small ... \n
 * And using the whole URL would inflate that much more.
 *
 * Currently we use about 92 bytes per entry. So we'd (unnecessarily) 
 * increase the size by about 10%.
 *
 * That's why there's an url_t::internal_number.  */


/** -.
 *
 * Does get \c free()d by url__close_sessions().
 *
 * See \ref glob_opt_urls "-u" for the specification. */
char **url__parm_list=NULL;
int url__parm_list_len=0,
		url__parm_list_used=0;

int url__must_write_defs=0;


/** -.
 *
 * Because this may be called below input_tree, returning \c ENOENT could 
 * be interpreted as <i>no dirlist found</i> - which has to be allowed in 
 * some cases.
 * So this returns \c EADDRNOTAVAIL. */
int url__find_by_name(const char *name, struct url_t **storage)
{
	int status;
	int i;

	/* Normalize */
	if (name && !*name) name=NULL;

	status=EADDRNOTAVAIL;
	for(i=0; i<urllist_count; i++)
	{
		/* Check for NULL name, and allow NULL === "", too. */
		if (!urllist[i]->name ?
				(!name || !*name) :
				(strcmp(urllist[i]->name, name) == 0) )
		{
			if (storage) *storage=urllist[i];
			status=0;
			break;
		}
	}

	if (status)
		DEBUGP("url with name %s not found!", name);

	return status;
}


/** -.
 *
 * Because this may be called below input_tree, returning \c ENOENT could 
 * be interpreted as <i>no dirlist found</i> - which has to be allowed in 
 * some cases.
 * So this returns \c EADDRNOTAVAIL. */
int url__find_by_url_in_list(char *url, 
		struct url_t **list, int count,
		struct url_t **storage)
{
	int status;
	int i;


	status=EADDRNOTAVAIL;
	for(i=0; i<count; i++)
	{
		if (strcmp(list[i]->url, url) == 0)
		{
			if (storage) *storage=list[i];
			status=0;
			break;
		}
	}

	if (status)
		DEBUGP("url with url %s not found!", url);

	return status;
}


/** Wrapper for url__find_by_url_in_list(). */
int url__find_by_url(char *url, struct url_t **storage)
{
	return url__find_by_url_in_list(url, urllist, urllist_count, storage);
}

/** -.
 * */
int url__find_by_intnum(int intnum, struct url_t **storage)
{
	int status;
	int i;

	/* We must not return ENOENT. Because this is called below input_tree,
	 * returning ENOENT could be interpreted as "no dirlist found" - which
	 * has to be allowed in some cases. */
	status=EADDRNOTAVAIL;
	for(i=0; i<urllist_count; i++)
	{
		if (urllist[i]->internal_number == intnum)
		{
			if (storage) *storage=urllist[i];
			status=0;
			break;
		}
	}

	if (status)
		DEBUGP("url with intnum %d not found!", intnum);
	else
		DEBUGP("url with intnum %d is %s", intnum, (*storage)->url);

	return status;
}


/** \anchor url_flags
 * \name Flags to store which attributes we already got for this URL.  */
/** @{ */
#define HAVE_NAME (1)
#define HAVE_PRIO (2)
#define HAVE_URL (4)
#define HAVE_TARGET (8)
#define HAVE_READONLY (16)
/** @} */


/** -.
 *
 * This function preserves it's input.
 * If storage is non- \c NULL, it's \c ->name member get's a copy of the given
 * (or a deduced) name.
 *
 * In \a def_parms the parameters found are flagged - see \ref url_flags; 
 * if \a def_parms is \c NULL, an URL \b must be present.
 * */
int url__parse(char *input, struct url_t *storage, int *def_parms)
{
	int status;
	char *cp, *value, *end, *cur;
	struct url_t eurl;
	int nlen, vlen, have_seen;


	status=0;

	have_seen=0;
	memset(&eurl, 0, sizeof(eurl));
	/* The internal number is initially unknown; we must not set one here,
	 * as a later read URL may have the number we've chosen.
	 * We have to give internal numbers in a second pass. */
	eurl.internal_number=INVALID_INTERNAL_NUMBER;
	eurl.current_rev=0;
	eurl.target_rev=SVN_INVALID_REVNUM;
	eurl.current_target_override=0;
	eurl.head_rev=SVN_INVALID_REVNUM;
	cur=input;

	DEBUGP("input: %s", input);
	while (! (have_seen & HAVE_URL))
	{
		/* Find first ':'.
		 * Variables are as follows:
		 *          nlen=5    vlen=3
		 *             [---][-]
		 *    name:xxx,prio:123,svn://xxxxxx 
		 *             ^    ^  ^
		 *             |    |  end
		 *             |    value
		 *             cur
		 *    */
		end=cur;
		value=NULL;
		while (*end)
		{
			/* There may be multiple ':' in a single value, eg. in the URL:
			 *   http://user:pass@host:port/.
			 * Set value to the first occurrence. */
			if (*end == ':' && !value)
				value = end+1;

			if (*end == ',') break;

			end++;
		}	

		/* Don't count the ':'. */
		nlen = (value ? value-1 : end) - cur;
		vlen =  value ? end - value : 0;

		DEBUGP("cur=%s value=%s end=%s vlen=%d nlen=%d",
				cur, value, end, vlen, nlen);

		if (strncmp("name", cur, nlen) == 0 ||
				strncmp("N", cur, nlen) == 0)
		{
			STOPIF_CODE_ERR( have_seen & HAVE_NAME, EINVAL, 
					"!Found two names in URL '%s'; only one may be given.",
					input);
			if (!value) goto need_value;

			/* "" == NULL == empty name? */
			if (vlen==0)
				DEBUGP("NULL name");
			else if (storage) 
			{
				/* If we need that name again, make a copy.
				 * We cannot simply use strdup(), because this is not 
				 * \0-terminated. */
				STOPIF( hlp__strnalloc(vlen, &eurl.name, value), NULL);

				DEBUGP("got a name '%s' (%d bytes), going on with '%s'",
						eurl.name, vlen, end);
				have_seen |= HAVE_NAME;
			}
		}
		else if (strncmp("target", cur, nlen) == 0 ||
				strncmp("T", cur, nlen) == 0)
		{
			STOPIF_CODE_ERR( have_seen & HAVE_TARGET, EINVAL, 
					"!Already got a target revision in URL '%s'.",
					input);
			if (!value) goto need_value;
			STOPIF( hlp__parse_rev( value, &cp, & eurl.target_rev), NULL);
			STOPIF_CODE_ERR( cp == value || cp != end, EINVAL,
					"The given target revision in '%s' is invalid.",
					input);
			DEBUGP("got target %s", hlp__rev_to_string(eurl.target_rev));
			have_seen |= HAVE_TARGET;
		}
		else if (strncmp("prio", cur, nlen) == 0 ||
				strncmp("P", cur, nlen) == 0)
		{
			STOPIF_CODE_ERR( have_seen & HAVE_PRIO, EINVAL, 
					"!Found two priorities in URL '%s'; only one allowed.",
					input);
			if (!value) goto need_value;
			eurl.priority=strtol(value, &cp, 0);
			STOPIF_CODE_ERR( cp == value || cp != end, EINVAL,
					"!The given url \"%s\" is invalid; cannot parse the priority.",
					input);
			DEBUGP("got priority %d", eurl.priority);
			have_seen |= HAVE_PRIO;
		}
		else if (strncmp("readonly", cur, nlen) == 0 ||
				strncmp("ro", cur, nlen) == 0)
		{
			STOPIF_CODE_ERR( have_seen & HAVE_READONLY, EINVAL, 
					"!Found two readonly flags in URL \"%s\"; only one allowed.",
					input);
			if (value) 
			{
				eurl.is_readonly=strtol(value, &cp, 0);
				STOPIF_CODE_ERR( cp == value || cp != end, EINVAL,
						"!Cannot parse the readonly flag in \"%s\".", input);
			}
			else
				eurl.is_readonly=1;

			have_seen |= HAVE_READONLY;
		}
		else
		{
			/* For URLs no abbreviation is allowed, so we check the length extra.  
			 * An exception is "svn+", which can have arbitrary tunnels after it; 
			 * see ~/.subversion/config for details.
			 *
			 * We cannot use strcmp(), as the URL has no '\0' at the given 
			 * position.
			 * We test for the ":", too, so that "http\0" isn't valid.
			 * */
			nlen++;
			if (strncmp("svn+", cur, 4) == 0)
			{
					/* At least a single character after the '+'; but nlen is already 
					 * incremented.  */
					STOPIF_CODE_ERR(nlen <= 5, EINVAL,
							"!No tunnel given after \"svn+\" in \"%s\".", cur);
			}
			else if (
					(nlen == 4 && strncmp("svn:", cur, nlen) == 0) ||
					(nlen == 5 &&
					 (strncmp("http:", cur, nlen) == 0 ||
						strncmp("file:", cur, nlen) == 0) ) ||
					(nlen == 6 && strncmp("https:", cur, nlen) == 0))
				DEBUGP("known protocol found");
			else
				STOPIF_CODE_ERR(1, EINVAL, 
						"!The protocol given in \"%s\" is unknown!", cur);

			/* The shortest URL is name="http:" and value="//a", or something 
			 * like that ;-) */
			if (!value || vlen<3 || strncmp(value, "//", 2)!=0)
				STOPIF_CODE_ERR(1, EINVAL, "!The URL in \"%s\" is invalid.", cur);

			/* Must be an URL */
			/* We remove any / at the end of the URL (which may have resulted from
			 * bash-completion), otherwise we'll get an error:
			 *   subversion/libsvn_subr/path.c:114: 
			 *     svn_path_join: Assertion `is_canonical (base, blen)' failed.
			 * That's not necessary. */
			/* Please note that URLs are defined to use a '/', not 
			 * (platform-dependent) PATH_SEPARATOR! */
			while (vlen>3 && value[vlen-1] == '/')
				value[--vlen] = 0;

			/* We need the ':' and the "name" (protocol) too, but don't count the 
			 * '\0' at the end.
			 * The ':' is already counted by the nlen++ above. */
			eurl.urllen=nlen + 0 + 1 + vlen - 1;
			STOPIF( hlp__strdup( &eurl.url, cur), NULL);

			have_seen |= HAVE_URL;
		}

		while (*end == ',') end++;
		if (!*end) break;
		cur=end;
	}


	if (def_parms) 
		*def_parms=have_seen;
	else
		STOPIF_CODE_ERR( !(have_seen & HAVE_URL), EINVAL,
				"!No URL found in %s", input);

	if (storage) *storage=eurl;

	/* Maybe not entirely correct here, because URLs might not be stored in 
	 * the URL list. */
	url__must_write_defs=1;

ex:
	return status;

need_value:
	STOPIF(EINVAL,
			"!Specification '%s' is not a valid URL - ':' missing.", input);
	goto ex;
}


/** -.
 * This functions returns 0 for success.
 * Error codes (eg \c EADDRNOTAVAIL ) are possible.
 *
 * If \a *existed is non- \c NULL, it is set to
 * 0 for a new URL or \c EEXIST if an existing URL was overwritten.
 * 
 * The URL is parsed into an empty space at the end of \a urllist ,
 * which must already exist!
 *
 * If the same URL was already used, the old entry gets overwritten. */
int url__insert_or_replace(char *eurl, 
		struct url_t **storage, 
		int *existed)
{
	int status;
	int seen;
	struct url_t target, *dupl, *dest, *by_name;


	status=0;
	STOPIF( url__parse(eurl, &target, &seen), NULL);


	by_name=NULL;
	/* No error checks necessary, pointer stays NULL if not found. */
	if (seen & HAVE_NAME)
		url__find_by_name(target.name, &by_name);

	dupl=NULL;
	/* If an URL is given, this is what is used for replacement. */
	if (seen & HAVE_URL)
		url__find_by_url(target.url, &dupl);
	else
	{
		/* If no URL, then try to find the name. */	
		dupl=by_name;
	}


	if (!dupl)
	{
		if (!(seen & HAVE_URL))
		{
			STOPIF( EINVAL,
					!(seen & HAVE_NAME) ? 
					"!No URL was given in \"%s\"." :
					"!Cannot find the name given in \"%s\", so cannot modify an URL.",
					eurl);
		}
		if (seen & HAVE_NAME)
		{
			/* The names must be unique. */
			STOPIF_CODE_ERR( by_name, EADDRINUSE, 
					"!There's already an url named \"%s\"", target.name);

			/* If we didn't find it, it's ok. */
			status=0;
		}

		/* Copy to destination */
		dest=urllist[urllist_count];
		*dest = target;
		urllist_count++;
	}
	else
	{
		/* \todo Currently it is not defined whether the strings are 
		 * heap-allocated or not, so it's not easy to free them.
		 *
		 * This should not happen so often, so we ignore that. */
		/* When it gets overwritten, we take care to not simply copy - 
		 * we just change the given values. */
		if (seen & HAVE_TARGET)
			dupl->target_rev = target.target_rev;
		if (seen & HAVE_PRIO)
			dupl->priority = target.priority;
		if (seen & HAVE_READONLY)
			dupl->is_readonly = target.is_readonly;
		if (seen & HAVE_NAME)
			dupl->name = target.name;
		/* The URL is the same, so the length is the same, and the internal 
		 * number is generated or already present. */
		dest=dupl;
	}

	if (existed)
		*existed = dupl ? EEXIST : 0;

	if (storage) 
		*storage=dest;

ex:
	return status;
}


/** Simple function to find an unused id.
 * Slow, but easy. I'd like to use the linux-kernel bitmap functions -
 * but they're not exported, and not available everywhere. */
int find_next_zero_bit(fd_set *fd, int from)
{
	while (FD_ISSET(from, fd)) from++;
	return from;
}


/** Set the internal number of all URLs which don't already have one. 
 *
 * I'm aware that a normal \c fd_set is normally limited to 
 * a few hundred bits (eg. for use with 1024 filehandles); but the low-level
 * ops don't know what we're doing, anyway. So we could just extend the 
 * bitmap, and it should work as before (although maybe 
 * there's be a sanity test).
 *
 * Sadly find_next_zero_bit and friends are not exported from the kernel,
 * so we have to use \c FD_ISSET and similar; there might be 
 * faster/better alternatives. Tell me if you know one. */
int url___set_internal_nums(void)
{
	int status;
	int i, j, bit;
	fd_set bitmap;


	/* We need to store only so many bits as we have URLs. 
	 * If URLs have higher inums there will be free lower inums. */

	STOPIF_CODE_ERR( sizeof(bitmap)*8 < urllist_count, EMFILE,
			"Your fd_set is too small for the number of urls.\n"
			"Please create a github issue.");

	status=0;
	FD_ZERO(&bitmap);
	/* Step 1: look which numbers are used. */
	for(i=0; i<urllist_count; i++)
	{
		if (urllist[i]->internal_number > urllist_count)
		{
			/* Note: For such values we still have to check whether two 
			 * internal numbers collide. */
			for(j=i+1; j<urllist_count; j++)
				STOPIF_CODE_ERR( 
						urllist[i]->internal_number == urllist[j]->internal_number,
						EINVAL, "The URLs %s and %s have identical internal numbers!",
						urllist[i]->url, urllist[j]->url);
		}
		else if (urllist[i]->internal_number != INVALID_INTERNAL_NUMBER)
		{
			STOPIF_CODE_ERR( FD_ISSET(urllist[i]->internal_number, &bitmap),
					EINVAL, 
					"The URL %s has a duplicate internal number!",
					urllist[i]->url);

			FD_SET(urllist[i]->internal_number, &bitmap);
		}
	}

	/* Step 2: Fill invalid. Start with internal number 1. */
	bit=1;
	for(i=0; i<urllist_count; i++)
	{
		DEBUGP("inum for %s is %d",
				urllist[i]->url, urllist[i]->internal_number);
		if (urllist[i]->internal_number == INVALID_INTERNAL_NUMBER)
		{
			/* Find a free bit */
			bit= find_next_zero_bit(&bitmap, bit);
			DEBUGP("found a free bit for %s: %d",
					urllist[i]->url, bit);

			urllist[i]->internal_number=bit;

			/* No need to set that bit here, just skip to the next. */
			bit++;
		}
	}

ex:
	return status;
}


/** -. */
int url__allocate(int reserve_space)
{
	int status;
	struct url_t *url_mem;
	int i;


	status=0;
	/* We put a terminating NULL pointer at the end. */
	STOPIF( hlp__realloc( &urllist, 
			sizeof(*urllist) * (urllist_count+1+reserve_space)), NULL);
	STOPIF( hlp__calloc( &url_mem, sizeof(*url_mem), reserve_space), NULL);

	/* store url pointers */
	for(i=0; i<reserve_space; i++)
	{
		urllist[urllist_count+i]=url_mem+i;
	}
	urllist[urllist_count+i]=NULL;

ex:
	return status;
}


/** -. */
int url__indir_sorter(const void *a, const void *b)
{
	struct url_t *u1=*(struct url_t **)a,
							 *u2=*(struct url_t **)b;

	return url__sorter(u1, u2);
}


/** -.
 * 
 * \a reserve_space says how much additional space should be allocated.
 *
 * This function sets \a urllist_mem to the string buffer allocated, and
 * the \a urllist pointers get set to the URLs.
 * If no \c dir file is found, \c ENOENT is returned without an error 
 * message.
 *
 * We read two sources; the one in \c FSVS_CONF normally holds
 * the URL, the target revision, the priority, the name, the internal 
 * number, and the readonly flag.
 *
 * The current revision is read from \c FSVS_WAA.
 *
 * \see waa_files. */
int url__load_list(char *dir, int reserve_space)
{
	int status, fh, l, i;
	struct stat st;
	char *urllist_mem;
	int inum, cnt, new_count;
	svn_revnum_t rev;
	int intnum;
	struct url_t *target;
	FILE *rev_in;
	char *buffer;


	fh=-1;
	urllist_mem=NULL;

	/* ENOENT must be possible without an error message. 
	 * The space must always be allocated. */
	status=waa__open_byext(dir, WAA__URLLIST_EXT, WAA__READ, &fh);
	if (status==ENOENT)
	{
		STOPIF( url__allocate(reserve_space), NULL);
		status=ENOENT;
		goto ex;
	}

	STOPIF_CODE_ERR(status, status, "Cannot read URL list");

	STOPIF_CODE_ERR( fstat(fh, &st) == -1, errno,
			"fstat() of url-list");

	/* add 1 byte to ensure \0 */
	STOPIF( hlp__alloc( &urllist_mem, st.st_size+1), NULL);

	status=read(fh, urllist_mem, st.st_size);
	STOPIF_CODE_ERR( status != st.st_size, errno, 
			"error reading url-list");

	urllist_mem[st.st_size]=0;

	/* count urls */
	new_count=0;
	for(l=0; l<st.st_size; )
	{
		while (isspace(urllist_mem[l])) l++;

		if (urllist_mem[l]) new_count++;
		l += strlen(urllist_mem+l)+1;
	}

	DEBUGP("found %d urls", new_count);
	STOPIF( url__allocate(reserve_space+new_count), NULL);

	/* store urls */
	for(l=i=0; i<new_count; )
	{
		/* Skip over \n and similar, mostly for the debug output. sscanf() 
		 * would ignore that anyway. */
		while (isspace(urllist_mem[l])) l++;

		DEBUGP("url %d of %d: %s",i, new_count, urllist_mem+l);
		if (urllist_mem[l]) 
		{
			STOPIF_CODE_ERR( 
					sscanf(urllist_mem+l, "%d %ld %n", 
					&inum, &rev, &cnt) != 2, 
					EINVAL,
					"Cannot parse urllist line '%s'", urllist_mem+l);

			STOPIF( url__insert_or_replace(urllist_mem+l+cnt, &target, NULL), NULL);
			target->internal_number=inum;
			target->current_rev=rev;

			i++;
			l += strlen(urllist_mem+l);
		}

		/* Skip over \0 */
		l++;
	}

	STOPIF_CODE_ERR( close(fh) == -1, errno, "closing the url-list");
	fh=-1;


	/* Read the current revisions from the WAA definition.
	 * If we got data before, we need this here too. */
	/* Exception for 1.1.18: Upgrade from 1.1.17. A non-existing file is 
	 * allowed, but will convert the data next time. */
	status=waa__open_byext(dir, WAA__URL_REVS, WAA__READ, &fh);
	if (status==ENOENT)
	{
		DEBUGP("No file; upgrading?");
		status=0;
	}
	else
	{
		/* Read the associated revisions. */
		rev_in=fdopen(fh, "r");
		while (1)
		{
			status=hlp__string_from_filep(rev_in, &buffer, NULL, 0);
			if (status == EOF) 
			{
				status=0;
				break;
			}
			STOPIF( status, "Failed to read copyfrom source");

			STOPIF_CODE_ERR( sscanf(buffer, "%d %lu 0 0 0 0\n", 
						&intnum, &rev) != 2, EINVAL,
					"Error parsing line \"%s\" from %s", buffer, WAA__URL_REVS);

			STOPIF( url__find_by_intnum(intnum, &target), 
					"URL number %d read from %s not found",
					intnum, WAA__URL_REVS);

			target->current_rev=rev;
		}
		STOPIF_CODE_ERR( fclose(rev_in)==-1, errno, 
			"error closing %s", WAA__URL_REVS);
		fh=-1;
	}


	/* Sort list by priority */
	qsort(urllist, urllist_count, sizeof(*urllist), url__indir_sorter);

	/* No writing would be necessary if nothing gets changed. */
	url__must_write_defs=0;

ex:
	/* urllist_mem must not be freed - our url-strings still live there! */
	if (fh!=-1) 
	{
		l=close(fh);
		STOPIF_CODE_ERR(l == -1 && !status, errno, "closing the url-list");
	}

	return status;
}


/** -.
 * 
 * This prints a message and stops if no URLs could be read. */
int url__load_nonempty_list(char *dir, int reserve_space)
{
	int status, load_st;

	status=0;
	if (!dir) dir=wc_path;

	load_st=url__load_list(dir, reserve_space);
	STOPIF_CODE_ERR( load_st==ENOENT || 
			urllist_count==0, ENOENT,
			"!No URLs have been defined for %s.", dir);

ex:
	return status;
}

	
/** -.
 * The data is written in two different locations.
 *
 * The internal number was chosen as combining key, because the URL might 
 * include strange characters, and there might not be a name.
 *
 * \todo is 1024 bytes always enough? Maybe there's an RFC. Make that
 * dynamically - look how much we'll need. */
int url__output_list(void)
{
	int status, i, fh, l, fh_revs;
	char buffer[1024];
	struct url_t *url;


	fh=-1;
	fh_revs=-1;

	STOPIF( url___set_internal_nums(), 
			"Setting the internal numbers failed.");

	if (url__must_write_defs)
		STOPIF( waa__open_byext(NULL, WAA__URLLIST_EXT, WAA__WRITE, &fh), NULL);

	STOPIF( waa__open_byext(NULL, WAA__URL_REVS, WAA__WRITE, &fh_revs), NULL);
	for(i=0; i<urllist_count; i++)
	{
		url=urllist[i];

		if (url->target_rev == 0 && url->current_rev == 0)
			continue;

		if (fh != -1)
		{
			l=snprintf(buffer, sizeof(buffer),
					"%d %d T:%ld,N:%s,P:%d,ro:%u,%s",
					url->internal_number,
					0, /* Previously the the current revision. */
					url->target_rev,
					url->name ? url->name : "",
					url->priority,
					url->is_readonly,
					url->url);

			STOPIF_CODE_ERR( l > sizeof(buffer)-4, E2BIG, 
					"You've got too long URLs; I'd need %d bytes. Sorry.", l);

			/* include the \0 */
			l++;
			/** \todo: writev */
			STOPIF_CODE_ERR( write(fh, buffer, l) != l, errno,
					"Error writing the URL list");
			STOPIF_CODE_ERR( write(fh, "\n", 1) != 1, errno,
					"Error writing the URL list delimiter");
			DEBUGP("writing line %s", buffer);
		}


		/* A few extra fields, to store some information later. */
		l=snprintf(buffer, sizeof(buffer),
				"%d %ld 0 0 0 0\n",
				url->internal_number,
				url->current_rev);
		/* This can never happen, apart from being caused by radiation. */
		BUG_ON( l > sizeof(buffer)-4);
		STOPIF_CODE_ERR( write(fh_revs, buffer, l) != l, errno,
				"Error writing the URL list");
	}

	url__must_write_defs=0;

ex:
	if (fh != -1)
	{
		i=waa__close(fh, status);
		fh=-1;
		STOPIF(i, "Error closing the URL list");
	}

	if (fh_revs != -1)
	{
		i=waa__close(fh_revs, status);
		fh_revs=-1;
		STOPIF(i, "Error closing the revisions list");
	}

	return status;
}


/** -.
 *
 * If \a missing_dirs is not \c NULL, this function returns in \c 
 * *missing_dirs a \b copied string with the missing path components from 
 * \c current_url->url (which should be freed later).
 *
 * \note The session is then registered at the \b existing part, so all 
 * accesses must include this relative part!
 *
 * If the URL is ok, a \c NULL is returned (not a pointer to a \c \\0 ).
 *
 * This is needed for the \c mkdir_base option; we cannot create the 
 * hierarchy here, because we need a commit editor for that, but in 
 * ci__directory() we cannot use a session based on an non-existing URL.
 * */
int url__open_session(svn_ra_session_t **session, char **missing_dirs)
{
	int status;
	svn_error_t *status_svn;
	apr_hash_t *cfg;
	char *buffer, *cp;
	int exists;
	svn_revnum_t head;


	status=0;
	if (!current_url->pool)
	{
		STOPIF( apr_pool_create_ex(& current_url->pool, global_pool, 
					NULL, NULL), 
				"no pool");
	}

	STOPIF( hlp__get_svn_config(&cfg), NULL);


	if (current_url->session) goto ex;


	/* We wouldn't need to allocate this memory if the URL was ok; but we
	 * don't know that here, and it doesn't hurt that much.
	 * Furthermore, only SVN knows what characters should be escaped - so
	 * lets get it done there. */
	buffer = (char*)svn_uri_canonicalize(current_url->url, global_pool);
	BUG_ON(!buffer);
	cp=buffer+strlen(buffer);
	BUG_ON(*cp);


	STOPIF_SVNERR_TEXT( svn_ra_open,
				(& current_url->session, buffer,
				 &cb__cb_table, NULL,  /* cbtable, cbbaton, */
				 cfg,	/* config hash */
				 current_url->pool),
				"svn_ra_open(\"%s\")", current_url->url);
	head=SVN_INVALID_REVNUM;
	STOPIF( url__canonical_rev( current_url, &head), NULL);

	DEBUGP("Trying url %s@%ld", buffer, head);
	while (1)
	{
		/* Is the caller interested in this check? If not, then just return. */
		if (!missing_dirs) break;


		/* Test whether the base directory exists; we need some lightweight 
		 * mechanism to detect that.
		 * Sadly we don't get a result when we open the session. */
		/* That's not entirely correct.
		 * In the time between this test and the commit running someone could 
		 * create or remove the base path; then we would have tested against 
		 * the wrong revision, and might fail nonetheless. */
		STOPIF( cb__does_path_exist(current_url->session, "", head, 
					&exists, current_url->pool), NULL);
		if (exists) break;


		/* Doesn't exist. Try with the last part removed. */
		/* We don't do URLs with less than a few characters. */
		while (cp > buffer+4 && *cp != '/') cp--;

		/* If we're before the hostname, signified by a "//", we abort. */
		STOPIF_CODE_ERR( cp[-1] == '/', EINVAL,
				"!Unsuccessfull svn_ra_stat() on every try for URL \"%s\".",
				current_url->url);

		/* We're at a slash, and try with a shortened URL. */
		*cp=0;

		DEBUGP("Reparent to %s", buffer);
		STOPIF_SVNERR( svn_ra_reparent,
				(current_url->session, buffer, current_url->pool));
	}


	/* See whether the original URL is valid. */
	if (missing_dirs) 
	{
		if (buffer + current_url->urllen == cp)
		{
			*missing_dirs=NULL;
		}
		else
		{
			/* Return just the missing parts:
			 *
			 *   url:    http://aaa/11/22/33/44
			 *   buffer: http://aaa/11/22
			 *   return: 33/44
			 *
			 * We return the characters that were cut off, without the '/'. */
			strcpy(buffer, current_url->url + 1 + (cp - buffer));

			DEBUGP("returning missing=%s", buffer);
			*missing_dirs=buffer;
		}
	}


	if (session) 
		*session = current_url->session;

ex:
	return status;
}


/** -.
 * */
int url__close_session(struct url_t *cur)
{
	/* There's no svn_ra_close() or suchlike.
	 * I hope it gets closed by freeing it's pool. */
	if (cur->pool)
	{
		DEBUGP("closing session and pool for %s", cur->url);

		BUG_ON(cur->pool == NULL && cur->session != NULL);
		apr_pool_destroy(cur->pool);
		cur->session=NULL;
		cur->pool=NULL;
	}

	return 0;
}


/** -.
 * */
int url__close_sessions(void)
{
  int status;
	int i;

	status=0;

	IF_FREE(url__parm_list);
	url__parm_list_len=url__parm_list_used=0;

	for(i=0; i<urllist_count; i++)
		STOPIF( url__close_session( urllist[i] ), NULL);

ex:
	return status;
}


/** -.
 * If an entry has \b no URL yet (is new), \a to_compare is \c NULL, and 
 * the \ref current_url has higher priority; this is common, and so done 
 * here too.  */
int url__current_has_precedence(struct url_t *to_compare)
{
	return to_compare==NULL ||
		(current_url->priority <= to_compare->priority);
}


/** Dumps the URLs to \c STDOUT . */
int url___dump(char *format)
{
	int status;
	int i;
	char *cp;
	FILE *output=stdout;
	struct url_t *url;


	if (!format) 
		format= opt__is_verbose()>0 ? 
			"%u\\n\tname: \"%n\"; priority: %p; current revision: %r; "
			"target: %t; readonly:%R\\n" :
			"name:%n,prio:%p,target:%t,ro:%R,%u\\n";

	status=0;
	for(i=0; i < urllist_count; i++)
	{
		url = urllist[i];
		cp=format;

		while (*cp)
		{
			switch (cp[0])
			{
				case '\\':
					switch (cp[1])
					{
						case '\\':
							STOPIF_CODE_EPIPE( fputc('\\', output), NULL);
							break;
						case 'n':
							STOPIF_CODE_EPIPE( fputc('\n', output), NULL);
							break;
						case 'r':
							STOPIF_CODE_EPIPE( fputc('\r', output), NULL);
							break;
						case 't':
							STOPIF_CODE_EPIPE( fputc('\t', output), NULL);
							break;
						case 'f':
							STOPIF_CODE_EPIPE( fputc('\f', output), NULL);
							break;
						case 'x':
							status= cp[2] && cp[3] ? cs__two_ch2bin(cp+2) : -1;
							STOPIF_CODE_ERR(status <0, EINVAL, 
									"A \"\\x\" sequence must have 2 hex digits.");
							STOPIF_CODE_EPIPE( fputc(status, output), NULL);
							/* There's a +2 below. */
							cp+=2;
							break;
						default:
							STOPIF_CODE_ERR(1, EINVAL, 
									"Unknown escape sequence '\\%c' in format.",
									cp[1]);
							break;
					}
					cp+=2;
					break;

				case '%':
					switch (cp[1])
					{
						/* Allow internal number, too? */
						case 'n':
							STOPIF_CODE_EPIPE( fputs(url->name ?: "", output), NULL);
							break;
						case 't':
							STOPIF_CODE_EPIPE( fputs(
										hlp__rev_to_string(url->target_rev), 
										output), NULL);
							break;
						case 'r':
							STOPIF_CODE_EPIPE( fputs(
										hlp__rev_to_string(url->current_rev), 
										output), NULL);
							break;
						case 'R':
							STOPIF_CODE_EPIPE( fprintf(output, "%u", 
										url->is_readonly), NULL);
							break;
						case 'I':
							STOPIF_CODE_EPIPE( fprintf(output, "%u", 
										url->internal_number), NULL);
							break;
						case 'p':
							STOPIF_CODE_EPIPE( fprintf(output, "%u", 
										url->priority), NULL);
							break;
						case 'u':
							STOPIF_CODE_EPIPE( fputs(url->url, output), NULL);
							break;
						case '%':
							STOPIF_CODE_EPIPE( fputc('%', output), NULL);
							break;
						default:
							STOPIF_CODE_ERR(1, EINVAL, 
									"Invalid placeholder '%%%c' in format.",
									cp[1]);
							break;
					}
					cp+=2;
					break;

				default:
					STOPIF_CODE_EPIPE( fputc(*cp, output), NULL);
					cp++;
			}
		}
	}

	status=0;

ex:
	return status;
}


/** -.
 * The space for the output is allocated, and must not be freed. */
int url__other_full_url(struct estat *sts, struct url_t *url, char **output)
{	
	static const char none[]="(none)";
	static struct cache_t *cache=NULL;
	int status, len;
	char *data, *path;

	status=0;

	if (url)
	{
		STOPIF( ops__build_path( &path, sts), NULL);
		len=url->urllen + 1 + sts->path_len+1;
		STOPIF( cch__new_cache(&cache, 4), NULL);

		STOPIF( cch__add(cache, 0, NULL, len, &data), NULL);
    strcpy( data, url->url);

		if (path[0]=='.' && path[1]==0) 
		{
			/* Nothing to be done; just the base URL. */
		}
		else
		{
			/* Remove ./ at start. */
			if (path[0]=='.' && path[1]==PATH_SEPARATOR) path += 2;

			data[url->urllen]='/';
			strcpy( data+url->urllen+1, path);
		}

		*output=data;
	}
	else
		*output=(char*)none;

ex:
	return status;
}


/** -. */
int url__full_url(struct estat *sts, char **url)
{	
	int status;

	STOPIF( url__other_full_url(sts, sts->url, url), NULL);

ex:
	return status;
}



/** -. */
int url__find(char *url, struct url_t **output)
{
	int i;
	struct url_t *cur;

	/* The URLs are in sorted order (by priority!), so just do a linear 
	 * search.  */
	for(i=0; i<urllist_count; i++)
	{
		cur=urllist[i];
		if (strncmp(cur->url, url, cur->urllen) == 0)
		{
			*output = cur;
			return 0;
		}
	}

	return ENOENT;
}


/** -.
 * Writes the given URLs into the WAA. */
int url__work(struct estat *root UNUSED, int argc, char *argv[])
{
	int status, l, i, had_it;
	char *dir;
	char *cp;
	int have_space;
	struct url_t *target;
	struct url_t *tmp;
	struct url_t **old_urllist;
	int old_urllist_count;


	dir=NULL;

	STOPIF( waa__given_or_current_wd(NULL, &dir), NULL );
	/* The current directory is the WC root. */
	STOPIF( waa__set_working_copy(dir), NULL);

	/* If there's \b no parameter given, we default to dump.
	 * - Use goto?
	 * - Set argv[0] to parm_dump? 
	 * - Test for argc? That's what we'll do. */
	if (argc>0 && strcmp(argv[0], parm_load) == 0)
	{
		/* In case the user had some URLs already defined and "load"s another 
		 * list, he would loose all URL internal numbers, so that a 
		 * "sync-repos" would be necessary.
		 *
		 * To avoid that we read the existing URLs (but ignore any errors, in 
		 * case the URLs are loaded again because the file is damaged). */
		status=url__load_list(NULL, argc+1);
		if (!status || status == ENOENT)
		{
			/* all right, just ignore that. */
		}
		else
		{
			/* Other errors are at least shown. */
			STOPIF_CODE_ERR_GOTO( 1, status, ignore_err,
					"!Got an error reading the old URL list, so the internal URL mappings\n"
					"cannot be kept; a \"sync-repos\" might be necessary.");
ignore_err:
			;
		}

		/* Don't remember the old values. */
		old_urllist_count=urllist_count;
		old_urllist=urllist;
		urllist=NULL;
		urllist_count=0;

		/* Surely write the list again. */
		url__must_write_defs=1;

		status=0;

		/* Load URLs. We do not know how many we'll get, possibly we'll have
		 * to allocate more memory. */
		i=0;
		have_space=0;
		while (1)
		{
			if (have_space < 1)
			{
				have_space=32;
				STOPIF( url__allocate(have_space), NULL);
			}

			status=hlp__string_from_filep(stdin, &cp, NULL, SFF_WHITESPACE);
			if (status == EOF) break;

			DEBUGP("parsing %s into %d", cp, urllist_count);
			STOPIF( url__insert_or_replace(cp, &target, &had_it), NULL);
			DEBUGP("had=%d", had_it);
			if (!had_it) 
			{
				have_space--;
				i++;
			}
			target->current_rev=0;

			/* Try to restore the internal number. */
			if (url__find_by_url_in_list(target->url, 
						old_urllist, old_urllist_count, &tmp) == 0)
				target->internal_number = tmp->internal_number;
		}

		IF_FREE(old_urllist);

		if (opt__is_verbose() >= 0)
			printf("%d URL%s loaded.\n", i, i==1 ? "" : "s");
	}
	else
	{
		/* Read URLs, reserving space. */
		status=url__load_list(NULL, argc+1);
		/* But ignore ENOENT */
		if (status == ENOENT)
			urllist_count=0;
		else
			STOPIF_CODE_ERR( status, status, NULL);

		/* Needs status still set from url__load_list()! */
		if (argc == 0 || strcmp(argv[0], parm_dump) == 0)
		{
			STOPIF_CODE_ERR( status==ENOENT, ENOENT,
				 "!No URLs defined for \"%s\".", dir);

			/* Dump */
			STOPIF( url___dump(argc ? argv[1] : NULL), NULL);
			goto ex;
		}


		/* Append/insert. */
		DEBUGP("%d to parse", argc);
		/* Parse URLs */
		for(l=0; l<argc; l++)
		{
			DEBUGP("parsing %s into %d", argv[l], urllist_count);
			STOPIF( url__insert_or_replace(argv[l], &target, &had_it), NULL);
			if (!had_it)
				target->current_rev=0;
		}
	} /* if load_from_stdin */

	STOPIF( waa__create_working_copy(dir), NULL);

	/* Write the URL list */
	STOPIF( url__output_list(), NULL);

ex:
	return status;
}


/** -.
 * This function takes a list of URL names (and optionally target 
 * revisions), and marks the URLs by setting url_t::to_be_handled.
 *
 * \ref url__parm_list gets destroyed. */
int url__mark_todo(void)
{
	int status;
	char *parm, *url_string, *rev_str, **list;
	static const char delim[]=",; \t\r\n\f";
	struct url_t *url;


	status=0;
	if (!url__parm_list_used) goto ex;

	/* Terminate the list */
	url__parm_list[url__parm_list_used] = NULL;
	list=url__parm_list;
	while (*list)
	{
		parm=*(list++);

		url_string=strtok(parm, delim);
		while (url_string && *url_string)
		{
			DEBUGP("marking URL %s", url_string);

			rev_str=strchr(url_string, '@');
			if (rev_str) *(rev_str++)=0;

			STOPIF( url__find_by_name(url_string, &url), 
					"!No URL with name \"%s\" found", url_string);

			if (url->to_be_handled)
				DEBUGP("URL %s mentioned multiple times", url->url);
			url->to_be_handled=1;

			if (rev_str) 
			{
				STOPIF( hlp__parse_rev(rev_str, NULL, 
							& url->current_target_rev), NULL);
				url->current_target_override=1;
			}

			url_string=strtok(NULL, delim);
		}	
	}

ex:
	return status;
}


/** -.
 *
 * We may have to reallocate. We don't want to allocate a pointer 
 * for each argument - we might be run with something like "find / 
 * -type f | xargs fsvs update". */
int url__store_url_name(char *parm)
{
	int status;

	status=0;
	/* The terminating NULL must be applied later, too. */
	if (url__parm_list_used+2 >= url__parm_list_len)
	{
		url__parm_list_len= url__parm_list_len ? url__parm_list_len*2 : 8;
		STOPIF( hlp__realloc( &url__parm_list, 
				url__parm_list_len*sizeof(*url__parm_list)), NULL);
	}

	url__parm_list[url__parm_list_used++] = parm;

ex:
	return status;
}


/** -.
 *
 * DAV (<tt>http://</tt> and <tt>https://</tt>) don't like getting \c 
 * SVN_INVALID_REVNUM on some operations; they throw an 175007 <i>"HTTP 
 * Path Not Found"</i>, and <i>"REPORT request failed on '...'"</i>.
 *
 * So we need the real \c HEAD.
 *
 * We try to be fast, and only fetch the value if we really need it. */
int url__canonical_rev( struct url_t *url, svn_revnum_t *rev)
{
	int status;
	svn_error_t *status_svn;


	status=0;
	status_svn=NULL;
	if (*rev == SVN_INVALID_REVNUM)
	{
		if (url->head_rev == SVN_INVALID_REVNUM)
		{
			BUG_ON( !url->session );
			/* As we ask at most once we just use the connection's pool - that 
			 * has to exist if there's a session. */
			STOPIF_SVNERR( svn_ra_get_latest_revnum,
					(url->session, & url->head_rev, url->pool));

			DEBUGP("HEAD of %s is at %ld", url->url, url->head_rev);
		}

		*rev=url->head_rev;
	}


ex:
	return status;
}


/** -.
 * Returns 0 as long as there's an URL to process; \c current_url is set, 
 * and opened. In \a target_rev the target revision (as per default of this 
 * URL, or as given by the user) is returned. \n
 * If \c current_url is not NULL upon entry the connection to this URL is 
 * closed, and its memory freed.
 *
 * If called with \a target_rev \c NULL, the internal index is reset, and 
 * no URL initialization is done.
 *
 * At the end of the list \c EOF is given.
 * */
int url__iterator2(svn_revnum_t *target_rev, int only_if_count,
		char **missing)
{
	int status;
	static int last_index=-1;
	svn_revnum_t rev;


	status=0;
	if (!target_rev) 
	{
		last_index=-1;
		goto ex;
	}


	while (1)
	{
		last_index++;
		if (last_index >= urllist_count) 
		{
			DEBUGP("no more URLs.");
			/* No more data. */
			status=EOF;
			goto ex;
		}

		current_url=urllist[last_index];

		if (only_if_count)
		{
			if (!current_url->entry_list_count)
			{
				DEBUGP("No changes for url %s.", current_url->url);
				continue;
			}
			DEBUGP("%d changes for url %s.", 
					current_url->entry_list_count, current_url->url);
		}

		if (url__to_be_handled(current_url)) break;
	}

	STOPIF( url__open_session(NULL, missing), NULL);


	if (current_url->current_target_override)
		rev=current_url->current_target_rev;
	else if (opt_target_revisions_given)
		rev=opt_target_revision;
	else
		rev=current_url->target_rev;
	DEBUGP("doing URL %s @ %s", current_url->url, 
			hlp__rev_to_string(rev));

	STOPIF( url__canonical_rev(current_url, &rev), NULL);
	*target_rev = rev;

ex:
	return status;
}

