/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __HELPER_H__
#define __HELPER_H__


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

/** Reads a string from the given \c FILE* , removes \\r and/or \\n at the
 * end, and optionally removes \b all whitespace. */
int hlp__string_from_filep(FILE *input, char **string, int no_ws);

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
	/** Whether we're writing or reading. */
	int is_writer;
	/** Our datasource/sink. */
	svn_stream_t *orig;
	/** PID of child, for \c waitpid(). */
	pid_t child;
	/** STDIN filehandle for child. */
	int pipe_in;
	/** STDOUT filehandle for child. */
	int pipe_out;
	/** Whether we can get more data. */
	int eof;
	/** The un-encoded data digest (context). */ 
	apr_md5_ctx_t md5_ctx;
	/** How many bytes are left to send in this buffer. */
	apr_size_t bytes_left;
	/** Where unsent data starts. */
	int data_pos;
	/** The buffer. */
	char buffer[ENCODE_BLOCKSIZE];
	/** Where to put the final md5. */
	md5_digest_t *output_md5;
};


/** Encode \c svn_stream_t filter. */
int hlp__encode_filter(svn_stream_t *s_stream, const char *command, 
		int is_writer,
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

#endif
