/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __CHECKSUM_H__
#define  __CHECKSUM_H__

#include "global.h"
#include "interface.h"
#include <subversion-1/svn_delta.h>

/** \file
 * CRC, manber function header file. */


/** This structure is used for one big file.
 * It stores the CRCs and MD5s of the manber-blocks of this file. */
struct cs__manber_hashes 
{
	/** The manber-hashes */
	uint32_t *hash;
	/** The MD5-digests */
	md5_digest_t *md5;
	/** The position of the first byte of the next block, ie.
	 * N for a block which ends at byte N-1. */
	off_t *end;
	/** The index into the above arrays - sorted by manber-hash. */
	uint32_t *index;

	/** Number of manber-hash-entries stored */
	unsigned count;
};


/** Checks whether a file has changed. */
int cs__compare_file(struct estat *sts, char *fullpath, int *result);
/** Puts the hex string of \a md5 into \a dest, and returns \a dest. */
char* cs__md5tohex(const md5_digest_t md5, char *dest);
/** Converts an MD5 digest to an ASCII string in a self-managed buffer. */
char *cs__md5tohex_buffered(const md5_digest_t md5);
/** Converts an ASCII string to an MD5 digest. */
int cs__char2md5(const char *input, char **eos, md5_digest_t md5);

/** Callback for the checksum layer. */
int cs__set_file_committed(struct estat *sts);

/** Creates a \c svn_stream_t pipe, which writes the checksums of the 
 * manber hash blocks to the \ref md5s file. */
int cs__new_manber_filter(struct estat *sts,
		svn_stream_t *stream_input, 
		svn_stream_t **filter_stream,
		apr_pool_t *pool);

/** Reads the \ref md5s file into memory. */
int cs__read_manber_hashes(struct estat *sts, 
		struct cs__manber_hashes *data);

/** Hex-character pair to ascii. */
int cs__two_ch2bin(char *stg);

#endif

