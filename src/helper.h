/************************************************************************
 * Copyright (C) 2005-2009,2015 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __HELPER_H__
#define __HELPER_H__

#include <ctype.h>

#include "global.h"
#include "options.h"

/** \file
 * Helper functions header file. */


static const char hex_chars[] = "0123456789abcdef";

#define _bin2hex(b, h) do { 				\
		*(h++) = hex_chars[*b >> 4];		\
		*(h++) = hex_chars[*b & 0x0f];		\
		b++;								\
	} while(0)

#define Mbin2hex(bin, hex, len) do { 		\
		switch(len)							\
		{									\
			case  4: _bin2hex(bin, hex);	\
			case  3: _bin2hex(bin, hex);	\
			case  2: _bin2hex(bin, hex);	\
			case  1: _bin2hex(bin, hex);	\
			case  0: break; 				\
			default: 						\
			 { int i=len;					\
				 while (i--)				\
					 _bin2hex(bin, hex); 	\
			 }								\
		}									\
   											\
		*hex='\0';  					\
	} while(0)


/** Converts a string with local encoding to \a UTF8, suitable for storage 
 * in the repository. */
int hlp__local2utf8(const char* local_string, char** utf8_string, int len);
/** Converts an \a UTF8 string to local encoding, to make it printable on 
 * the
 * current console. */
int hlp__utf82local(const char* utf8_string, char** local_string, int len);

void hlp__copy_stats(struct stat *src, struct sstat_t *dest);
int hlp__lstat(const char *fn, struct sstat_t *st);
int hlp__fstat(int fd, struct sstat_t *st);

/** A function like \a strcpy, but cleaning up paths. */
char *hlp__pathcopy (char *dst, int *len, ...) __attribute__((sentinel)) ;

/** Parses a string to a revision number. */
int hlp__parse_rev(char *stg, char **eos, svn_revnum_t *rev);

/** Reads a string from the given \c input into the (self-managed) buffer 
 * \a string, removes \\r and/or \\n at the end, and depending on \a flags 
 * strips whitespace or comments. */
int hlp__string_from_filep(FILE *input, char **string, char **eos, int flags);
/** If this bit is set, whitespace at the start and the end is removed. */
#define SFF_WHITESPACE (1)
/** With this flag comment lines (with \c '#' as first non-whitespace 
 * character) are ignored. */
#define SFF_COMMENT (2)
/** Can/should be used after opening the file. */
#define SFF_RESET_LINENUM (0x4000)
/** Get the line number of the last (current) \c FILE*, as return value. */
#define SFF_GET_LINENUM (0x8000)

/** Returns the name of the given user. */
const char *hlp__get_uname(uid_t uid, char *not_found);
/** Returns the name of the given group. */
const char *hlp__get_grname(gid_t gid, char *not_found);

/** Print the given data to \a output, safely converting special characters 
 * to codes like \c \\x1e. */
int hlp__safe_print(FILE *output, char *string, int maxlen);


/** \name f_encoder Encoder and decoder
 * @{ */
/** Blocksize for encoding pipes; we use a not too small value. */
#define ENCODE_BLOCKSIZE (32*1024)
/** Structure for an encoding process, with \c svn_stream_t input.
 *
 * When we shall give data, we have to feed data.
 * If not all data can be taken, we have to buffer the rest.
 * (We have to read some data, but don't know how much we can send 
 * further down the chain - so we have to buffer).*/
struct encoder_t {
	/** Our datasource/sink. */
	svn_stream_t *orig;

	/** Where to put the final md5. */
	md5_digest_t *output_md5;

	/** The un-encoded data digest (context). */ 
	apr_md5_ctx_t md5_ctx;
	/** How many bytes are left to send in this buffer. */
	apr_size_t bytes_left;

	/** PID of child, for \c waitpid(). */
	pid_t child;

	/** Whether we're writing or reading. */
	int is_writer;
	/** STDIN filehandle for child. */
	int pipe_in;
	/** STDOUT filehandle for child. */
	int pipe_out;
	/** Whether we can get more data. */
	int eof;
	/** Where unsent data starts. */
	int data_pos;

	/** The buffer. */
	char buffer[ENCODE_BLOCKSIZE];
};


/** Encode \c svn_stream_t filter. */
int hlp__encode_filter(svn_stream_t *s_stream, const char *command, 
		int is_writer,
		char *path,
		svn_stream_t **output, 
		struct encoder_t **encoder_out,
		apr_pool_t *pool);
/** @} */


/** Chroot helper function. */
int hlp__chrooter(void);

/** Distribute the environment variables on the loaded entries. */
int hlp__match_path_envs(struct estat *root);

/** Return a path that gets displayed for the user. */
int hlp__format_path(struct estat *sts, char *wc_relative_path,
  char **output);

/** Find a \c GID by group name, cached. */
int hlp__get_gid(char *group, gid_t *gid, apr_pool_t *pool);
/** Find a \c UID by user name, cached. */
int hlp__get_uid(char *user, uid_t *uid, apr_pool_t *pool);

/** Returns a string describing the revision number. */
char *hlp__rev_to_string(svn_revnum_t rev);

/** Function to compare two strings for \a max bytes, but treating \c '-' 
 * and \c '_' as equal. */
int hlp__strncmp_uline_eq_dash(char *always_ul, char *other, int max);


/** \a name is a subversion internal property. */
int hlp__is_special_property_name(const char *name);
/** Reads all data from \a stream and drops it. */
int hlp__stream_md5(svn_stream_t *stream, 
		unsigned char md5[APR_MD5_DIGESTSIZE]);

/** Delay until time wraps. */
int hlp__delay(time_t start, enum opt__delay_e which);

/** Renames a local file to something like .mine. */
int hlp__rename_to_unique(char *fn, char *extension, 
		const char **unique_name, 
		apr_pool_t *pool);


/* Some of the following functions use a (void*), but I'd rather use a 
 * (void**) ... Sadly that isn't convertible from eg. some (struct **) - 
 * and so I'd have to use casts everywhere, which wouldn't help the 
 * type-safety anyway. */
/** Allocates a buffer in \a *dest, and copies \a source. */
int hlp__strnalloc(int len, char **dest, const char * const source);
/** Like \c hlp__strnalloc, but concatenates strings until a \c NULL is 
 * found. */
int hlp__strmnalloc(int len, char **dest, 
		const char * source, ...) __attribute__((sentinel));
/** Own implementation of \c strdup(), possibly returning \c ENOMEM. */
inline static int hlp__strdup(char **dest, const char * const src)
{ return hlp__strnalloc(strlen(src), dest, src); }	
/** Error returning \c calloc(); uses \c (void**) \a output. */
int hlp__calloc(void *output, size_t nmemb, size_t count);
/** Reallocate the \c (void**) \a output. */
int hlp__realloc(void *output, size_t size);
/** Allocates a buffer of \a len bytes in \c (void**) \a *dest; can return 
 * \c ENOMEM.  */
inline static int hlp__alloc(void *dest, size_t len)
{
	*(void**)dest=NULL;
	return hlp__realloc(dest, len);
}


/** Stores the first non-whitespace character position from \a input in \a 
 * word_start, and returns the next whitespace position in \a word_end. */
char* hlp__get_word(char *input, char **word_start);
/** Skips all whitespace, returns first non-whitespace character. */
inline static char *hlp__skip_ws(char *input) 
{ 
	while (isspace(*input)) input++;
	return input;
}


/** Reads the subversion config file(s), found by \ref o_configdir. */
int hlp__get_svn_config(apr_hash_t **config);


/** Algorithm for finding the rightmost 0 bit.
 *    orig i: ... x 0 1 1 1
 *       i+1: ... x 1 0 0 0
 * XOR gives: ... x 1 1 1 1
 *   AND i+1: ... 0 1 0 0 0
 *
 * Maybe there's an easier way ... don't have "Numerical Recipes"
 * here with me. */
static inline int hlp__rightmost_0_bit(int i)
{
	return (i ^ (i+1)) & (i+1);
}

int hlp__compare_string_pointers(const void *a, const void *b);

int hlp__only_dir_mtime_changed(struct estat *sts);

#endif
