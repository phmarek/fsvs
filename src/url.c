/************************************************************************
 * Copyright (C) 2006-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/select.h>

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
 * For a format definition of the URLs please see the 
 * chapter \ref url_format .
 *
 * \note
 * If there are already URLs defined, and use that command later again,
 * please note that as of 1.0.18 <b>the older URLs are not overwritten</b>
 * as before, but that the new URLs are \b appended to the given list!
 * If you want to start afresh, use something like
 * \code
 * echo "" | fsvs urls load
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
 * To see which URLs are in use for the current WC, you can use \c dump .
 *
 * As an optional parameter you can give a format statement; \c %p , \c %n 
 * , \c %r, \c %t and \c %u are substituted by the priority, name, current 
 * revision, target revision and URL.
 * Note: That's not a real \c printf()-format; only these and a few \\ 
 * sequences are recognized.
 *
 * Example:
 * \code
 * fsvs urls dump "  %u %n:%p\\n"
 *   http://svn/repos/installation/machine-1/trunk local:10
 *   http://svn/repos/installation/common/trunk common:50
 * \endcode 
 *
 * The default format is \c "N:%n,P:%p,D:%t,%u\\n" .
 * */

/** \defgroup url_format Format of URLs
 * \ingroup compat
 *
 * FSVS can now use more than 1 URL for update. The given URLs are 
 * "overlayed" according to their priority, and they get a name (to
 * ease updating only parts).
 *
 * Such an <i>extended URL</i> has the form
 * \code
 *   ["name:"{name},]["target:"{t-rev},]["prio:"{prio},]["target:"{rev},]URL
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
 * We need a name, so that the user can say 
 *   "<b>commit all outstanding changes to the repository at URL x</b>",
 * without having to remember the full URL.
 * After all, this URL should already be known, as there's a list of URLs to 
 * update from.
 *
 *
 * \section url_target What can I do with the target revision?
 *
 * Using the target revision you can tell fsvs that it should use the given 
 * revision number as destination revision - so update would go there, but 
 * not further.
 * Please note that the given revision number overrides the \c -r parameter 
 * - which sets the destination for all URLs.
 *
 * The default target is HEAD.
 *
 * \note In subversion you can enter \c URL\@revision - this syntax may be 
 * implemented in fsvs too. (But it has the problem, that as soon as you 
 * have a \c @ in the URL, you \b must give the target revision everytime!)
 *
 *
 * \section url_intnum There's an additional internal number - why that?
 *
 * This internal number is not for use by the user.
 * It is just used to have an unique identifier for an URL, without using
 * the full string.
 *
 * \note
 * On my system the package names are on average 12.3 characters long
 * (1024 packages with 12629 bytes, including newline):
 * \code
 *   COLUMNS=200 dpkg-query -l | cut -c5- | cut -f1 -d" " | wc
 * \endcode
 *
 * So if we store an \e id of the url instead of the name, we have
 * approx. 4 bytes per entry (length of strings of numbers from 1 to 1024).
 * Whereas we'd now use 12.3 characters, that's a difference of 8.3 
 * per entry.
 *
 * Multiplied with 150 000 entries we get about 1MB difference in filesize
 * of the dir-file. Not really small ...
 *
 * Currently we use about 92 bytes per entry. So we'd (unnecessarily) 
 * increase the size by about 10%.
 *
 * That's why there's an internal_number. 
 * */



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
		/* Using "!(->name | name)" might be faster, but this is clearer ... */
		if ( (!urllist[i]->name && !name) ||
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
int url__find_by_url(char *url, struct url_t **storage)
{
	int status;
	int i;


	status=EADDRNOTAVAIL;
	for(i=0; i<urllist_count; i++)
	{
		if (strcmp(urllist[i]->url, url) == 0)
		{
			if (storage) *storage=urllist[i];
			status=0;
			break;
		}
	}

	if (status)
		DEBUGP("url with url %s not found!", url);

	return status;
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
/** @} */


/** -.
 *
 * This function preserves it's input.
 * If storage is non- \c NULL, it's \c ->name member get's a copy of the given
 * (or a deduced) name.
 *
 * In \a def_parms the parameters found are flagged - see \ref url_flags.  
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
		value=strchr(cur, ':');
		STOPIF_CODE_ERR(!value, EINVAL,
				"!Specification '%s' is not a valid URL - ':' missing.", input);
		value++;

		end=strchr(value, ',');
		if (!end) end=value+strlen(value);

		nlen=value-cur;
		vlen=end-value;

		DEBUGP("cur=%s value=%s vlen=%d nlen=%d",
				cur, value, vlen, nlen);

		if (strncmp("name:", cur, nlen) == 0 ||
				strncmp("N:", cur, nlen) == 0)
		{
			STOPIF_CODE_ERR( have_seen & HAVE_NAME, EINVAL, 
					"!Found two names in URL '%s'; only one may be given.",
					input);
			/* "" == NULL == empty name? */
			if (vlen==0)
				DEBUGP("NULL name");
			else if (storage) 
			{
				/* If we need that name again, make a copy. */
				eurl.name=malloc(vlen+2);
				STOPIF_ENOMEM(!eurl.name);
				memcpy(eurl.name, value, vlen);
				eurl.name[vlen]=0;
			}
			DEBUGP("got a name '%s' (%d bytes), going on with '%s'",
					eurl.name, vlen, end);
			have_seen |= HAVE_NAME;
		}
		else if (strncmp("target:", cur, nlen) == 0 ||
				strncmp("T:", cur, nlen) == 0)
		{
			STOPIF_CODE_ERR( have_seen & HAVE_TARGET, EINVAL, 
					"!Already got a target revision in URL '%s'.",
					input);
			STOPIF( hlp__parse_rev( value, &cp, & eurl.target_rev), NULL);
			STOPIF_CODE_ERR( cp == value || *cp != ',', EINVAL,
					"The given target revision in '%s' is invalid.",
					input);
			DEBUGP("got target %s", hlp__rev_to_string(eurl.target_rev));
			have_seen |= HAVE_TARGET;
		}
		else if (strncmp("prio:", cur, nlen) == 0 ||
				strncmp("P:", cur, nlen) == 0)
		{
			STOPIF_CODE_ERR( have_seen & HAVE_PRIO, EINVAL, 
					"F!ound two priorities in URL '%s'; only one allowed.",
					input);
			eurl.priority=strtol(value, &cp, 0);
			STOPIF_CODE_ERR( cp == value || *cp != ',', EINVAL,
					"The given url '%s' is invalid; cannot parse the priority.",
					input);
			DEBUGP("got priority %d", eurl.priority);
			have_seen |= HAVE_PRIO;
		}
		else
		{
			if (strncmp("http:", cur, nlen) == 0 ||
					strncmp("https:", cur, nlen) == 0 ||
					strncmp("file:", cur, nlen) == 0 ||
					strncmp("svn:", cur, nlen) == 0 ||
					strncmp("svn+ssh:", cur, nlen) == 0)
				DEBUGP("known protocol found");
			else
				STOPIF_CODE_ERR(1, EINVAL, "!The given protocol is unknown!");

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

			/* We need the ":" and the "name" (protocol) too, but don't count the 
			 * \0 at the end.  */
			eurl.urllen=nlen + 1 + vlen - 1;
			eurl.url=strdup(cur);
			STOPIF_ENOMEM(!eurl.url);

			have_seen |= HAVE_URL;
		}

		while (*end == ',') end++;
		cur=end;
	}


	STOPIF_CODE_ERR( !eurl.url || !*eurl.url, EINVAL,
			"!No URL found in %s", input);

	if (storage) *storage=eurl;

	if (def_parms) *def_parms=have_seen;

ex:
	return status;
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
	struct url_t target, *dupl, *dest;


	status=0;
	dupl=NULL;
	STOPIF( url__parse(eurl, &target, &seen), NULL);
	/* shortcut */

	if (url__find_by_url(target.url, &dupl) == EADDRNOTAVAIL)
	{
		if (target.name)
		{
			/* The names must be unique. */
			status=url__find_by_name(target.name, NULL);
			if (status == EADDRNOTAVAIL)
			{
				/* Good, just what we want to hear. */
			}
			else if (status == 0)
				STOPIF( EADDRINUSE, 
						"!There's already an url named '%s'", target.name);
			else
				STOPIF( status, "Cannot look for url named '%s'", target.name);

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
			"Please contact dev@fsvs.tigris.org for help.");

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
	urllist=realloc(urllist, 
			sizeof(*urllist) * (urllist_count+1+reserve_space));
	STOPIF_ENOMEM(!urllist);
	url_mem=calloc(sizeof(*url_mem), reserve_space);
	STOPIF_ENOMEM(!url_mem);

	/* store url pointers */
	for(i=0; i<reserve_space; i++)
	{
		urllist[urllist_count+i]=url_mem+i;
	}
	urllist[urllist_count+i]=NULL;

ex:
	return status;
}


/** Comparing two URLs.
 *
 * They get sorted by \a priority ascending (lower numbers, so higher
 * priority, first), then by \a url ascending (sort URLs alphabetically). 
 *
 * This is necessary, as on update we walk the \a urllist in order, to
 * have lower priority entries appearing when higher priority entries are
 * removed. */
static int url___sorter(const void *a, const void *b)
{
	struct url_t *u1=*(struct url_t **)a,
							 *u2=*(struct url_t **)b;

	if (u1->priority == u2->priority)
		return strcmp(u1->url, u2->url);
	else
		return u1->priority - u2->priority;
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
 * \see waa_files. */
int url__load_list(char *dir, int reserve_space)
{
	int status, fh, l, i;
	struct stat64 st;
	char *urllist_mem;
	int inum, cnt, new_count;
	svn_revnum_t rev;
	struct url_t *target;


	fh=-1;
	urllist_mem=NULL;

	/* ENOENT must be possible without an error message. 
	 * The space must always be allocated. */
	status=waa__open_byext(dir, WAA__URLLIST_EXT, 0, &fh);
	if (status==ENOENT)
	{
		STOPIF( url__allocate(reserve_space), NULL);
		status=ENOENT;
		goto ex;
	}

	STOPIF_CODE_ERR(status, status, "Cannot read URL list");

	STOPIF_CODE_ERR( fstat64(fh, &st) == -1, errno,
			"fstat() of url-list");

	/* add 1 byte to ensure \0 */
	urllist_mem=malloc(st.st_size+1);
	STOPIF_ENOMEM(!urllist_mem);

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

	/* Sort list by priority */
	qsort(urllist, urllist_count, sizeof(*urllist), url___sorter);

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
 * \todo is 1024 bytes always enough? Maybe there's an RFC. Make that
 * dynamically - look how much we'll need. */
int url__output_list(void)
{
	int status, i, fh, l;
	char buffer[1024];
	struct url_t *url;


	fh=-1;

	STOPIF( url___set_internal_nums(), 
			"Setting the internal numbers failed.");

	/* Open for writing. */
	STOPIF( waa__open_byext(NULL, WAA__URLLIST_EXT, 1, &fh), NULL);
	for(i=0; i<urllist_count; i++)
	{
		url=urllist[i];

		l=snprintf(buffer, sizeof(buffer),
				"%d %ld T:%ld,N:%s,P:%d,%s",
				url->internal_number,
				url->current_rev,
				url->target_rev,
				url->name ? url->name : "",
				url->priority,
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

ex:
	if (fh != -1)
	{
		i=waa__close(fh, status);
		fh=-1;
		STOPIF(i, "Error closing the URL list");
	}

	return status;
}


/** -.
 * */
int url__open_session(svn_ra_session_t **session)
{
	int status;
	svn_error_t *status_svn;

	status=0;
	if (!current_url->pool)
	{
		STOPIF( apr_pool_create_ex(& current_url->pool, global_pool, 
					NULL, NULL), 
				"no pool");
	}

	/** Try svn_ra_reparent() */
	if (!current_url->session)
	{
		STOPIF_SVNERR_EXTRA( svn_ra_open,
				(& current_url->session, current_url->url,
				 &cb__cb_table, NULL,  /* cbtable, cbbaton, */
				 NULL,	/* config hash */
				 current_url->pool),
				"Opening URL '%s' brought an error:", current_url->url);

		if (session) 
			*session = current_url->session;

		DEBUGP("Opened url %s", current_url->url);
	}

ex:
	return status;
}


/** -.
 * */
int url__close_sessions(void)
{
  int status;
	int i;
	struct url_t *cur;

	status=0;
	for(i=0; i<urllist_count; i++)
	{
		cur=urllist[i];

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
	}

	return status;
}


/** -.
 * */
int url__current_has_precedence(struct url_t *to_compare)
{
	return (current_url->priority <= to_compare->priority);
}


/** Dumps the URLs to \c STDOUT . */
int url___dump(char *format)
{
	int status;
	int i;
	char *cp;
	FILE *output=stdout;
	struct url_t *url;


	if (!format) format="N:%n,P:%p,%u\\n";

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
							status= EOF == fputc('\\', output);
							break;
						case 'n':
							status= EOF == fputc('\n', output);
							break;
						case 'r':
							status= EOF == fputc('\r', output);
							break;
						case 't':
							status= EOF == fputc('\t', output);
							break;
						case 'f':
							status= EOF == fputc('\f', output);
							break;
						case 'x':
							status= cp[2] && cp[3] ? cs__two_ch2bin(cp+2) : -1;
							STOPIF_CODE_ERR(status <0, EINVAL, 
									"A \"\\x\" sequence must have 2 hex digits.");
							status= EOF == fputc(status, output);
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
							status= EOF == fputs(url->name ?: "", output);
							break;
						case 't':
							status= EOF == fputs(hlp__rev_to_string(url->target_rev), 
									output);
							break;
						case 'r':
							status= EOF == fputs( hlp__rev_to_string(url->current_rev), 
									output);
							break;
						case 'p':
							status= 0 > fprintf(output, "%u", url->priority);
							break;
						case 'u':
							status= EOF == fputs(url->url, output);
							break;
						case '%':
							status= EOF == fputc('%', output);
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
					status= EOF == fputc(*cp, output);
					cp++;
			}

			if (status)
			{
				status=errno;

				/* Quit silently. */
				if (status == EPIPE) 
					status=0;

				goto ex;

			}
		}
	}

ex:

	return status;
}


/** -.
 * The space for the output is allocated, and must not be freed. */
int urls__full_url(struct estat *sts, char *path, char **url)
{	
	const char none[]="(none)";
	static struct cache_t *cache=NULL;
	int status, len;
	char *data;

	status=0;

	if (sts->url)
	{
		len=sts->url->urllen + 1 + sts->path_len+1;
		STOPIF( cch__new_cache(&cache, 4), NULL);

		STOPIF( cch__add(cache, 0, NULL, len, &data), NULL);
    strcpy( data, sts->url->url);

		if (!path)
		  STOPIF( ops__build_path( &path, sts), NULL);
		if (path[0]=='.' && path[1]==0) 
		{
			/* Nothing to be done; just the base URL. */
		}
		else
		{
			/* Remove ./ at start. */
			if (path[0]=='.' && path[1]==PATH_SEPARATOR) path += 2;

			data[sts->url->urllen]='/';
			strcpy( data+sts->url->urllen+1, path);
		}

		*url=data;
	}
	else
		*url=(char*)none;

ex:
	return status;
}


/** -.
 * Writes the given URLs into the WAA. */
int urls__work(struct estat *root UNUSED, int argc, char *argv[])
{
	int status, fh, l, i, had_it;
	char *dir;
	char *cp;
	int have_space;
	struct url_t *target;


	dir=NULL;
	fh=-1;

	STOPIF( waa__given_or_current_wd(NULL, &dir), NULL );

	/* If there's \b no parameter given, we default to dump.
	 * - Use goto?
	 * - Set argv[0] to parm_dump? 
	 * - Test for argc? That's what we'll do. */
	if (argc>0 && strcmp(argv[0], parm_load) == 0)
	{
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

			status=hlp__string_from_filep(stdin, &cp, 1);
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
		}

		if (opt_verbose>=0)
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


		if (argc == 0 || strcmp(argv[0], parm_dump) == 0)
		{
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


	STOPIF( url__output_list(), NULL);
	/* Make an informational link to the base directory. */
	/* Should we ignore errors? */
	STOPIF( waa__make_info_link(dir, "_base", dir), NULL);

ex:
	return status;
}


