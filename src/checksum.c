/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <apr_md5.h>
#include <sys/mman.h>

#include "checksum.h"
#include "helper.h"
#include "global.h"
#include "est_ops.h"
#include "waa.h"


/** \file
 * CRC, manber functions. */

#define MAPSIZE (32*1024*1024)



/** CRC table.
 * We calculate it once, and reuse it. */
struct t_manber_parms
{
	AC_CV_C_UINT32_T values[256];
};

/** Everything needed to calculate manber-hashes out of a stream.
 * */
struct t_manber_data
{
	/** The entry this calculation is for. */
	struct estat *sts;
	/** The stream we're filtering. */
	svn_stream_t *input;
	/** Start of the current block. */
	off_t last_fpos;
	/** The current position in the file. Is always >= \a last_fpos. */ 
	off_t fpos;

	/** MD5-Context of full file. */
	apr_md5_ctx_t full_md5_ctx;
	/** MD5 of full file. */
	md5_digest_t full_md5;

	/** MD5-Context of current block. */
	apr_md5_ctx_t block_md5_ctx;
	/** MD5 of last block. */
	md5_digest_t block_md5;

	/** The file descriptor where the manber-block-MD5s will be written to. */
	int manber_fd;


	/** The internal manber-state. */
	AC_CV_C_UINT32_T state;
	/** The previous manber-state.  */
	AC_CV_C_UINT32_T last_state;
	/** Count of bytes in backtrack buffer. */
	int bktrk_bytes;
	/** The last byte in the rotating backtrack-buffer. */
	int bktrk_last;
	/** The backtrack buffer. */
	unsigned char backtrack[CS__MANBER_BACKTRACK];
	/** Flag to see whether we're in a zero-bytes block.
	 * If there are large blocks with only \c \\0 in them, we don't CRC
	 * or MD5 them - just output as zero blocks with a MD5 of \c \\0*16.
	 * Useful for sparse files. */
	int data_bits;
};

/** The precalculated CRC-table. */
struct t_manber_parms manber_parms;


/** \b The Manber-structure.
 * Currently only a single instance of manber-hashing runs at once,
 * so we simply use a static structure. */
static struct t_manber_data cs___manber;


/** The write format string for \ref md5s. */
const char cs___mb_wr_format[]= "%s %08x %10llu %10llu\n";
/** The read format string for \ref md5s. */
const char cs___mb_rd_format[]= "%*s%n %x %llu %llu\n";

/** The maximum line length in \ref md5s :
 * - MD5 as hex (constant-length), 
 * - state as hex (constant-length),
 * - offset of block, 
 * - length of block, 
 * - \\n,
 * - \\0 
 * */
#define MANBER_LINELEN (APR_MD5_DIGESTSIZE*2+1 + 8+1 + 10+1 +10+1 + 1)


/** Initializes a Manber-data structure from a struct \a estat. */
int cs___manber_data_init(struct t_manber_data *mbd, 
		struct estat *sts);
/** Returns the position of the last byte of a manber-block. */
int cs___end_of_block(const unsigned char *data, int maxlen, 
		int *eob, 
		struct t_manber_data *mb_f);


/** Hex-character to ascii. 
 * Faster than sscanf().
 * Returns -1 on error. */
static int cs__hex2val(char ch)
{
	/* I thought a bit whether I should store the values+1, ie. keep most of 
	 * the array as 0 - but that doesn't save any memory, it only takes more 
	 * time.
	 * Sadly the various is*() functions (like isxdigit()) don't seem to 
	 * include that information yet, and I couldn't find some table in the 
	 * glibc sources.
	 * (I couldn't find anything in that #define mess, TBH.) */
	static const signed char values[256]={
		/* 0x00 */
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		/* 0x20 = space ... */
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		/* 0x30 = "012345" */
		+0,  1,  2,  3,  4,  5,  6,  7,   8,  9, -1, -1, -1, -1, -1, -1,
		/* 0x40 = "@ABCD" ... */ 
		-1, 10, 11, 12, 13, 14, 15, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		/* 0x60 = "`abcd" ... */
		-1, 10, 11, 12, 13, 14, 15, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,

		/* 0x80 */
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,

		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,  -1, -1, -1, -1, -1, -1, -1, -1,
	};

	/* To avoid having problems with "negative" characters */
	return values[ ch & 0xff ];
}


/** -.
 * Faster than sscanf().
 * Returns -1 on error. */
inline int cs__two_ch2bin(char *stg)
{
	return 
		(cs__hex2val(stg[0]) << 4) |
		(cs__hex2val(stg[1]) << 0);
}


/** -.
 * Exactly the right number of characters must be present. */
int cs__char2md5(const char *input, char **eos, md5_digest_t md5)
{
	int i, status, x, y;


	status=0;
	for(i=0; i<APR_MD5_DIGESTSIZE; i++)
	{
		/* first check first character. If it's a \0, reading
		 * the next character would possibly result in a SEGV. */
		x=cs__hex2val(input[0]);
		y=cs__hex2val(input[1]);

		/* I briefly thought about moving this test out of the loop, but that 
		 * would possibly result in a SEGV if some (too short, but delimited) 
		 * buffer gets parsed. */
		STOPIF_CODE_ERR( (y|x) == -1, EINVAL,
				"Illegal hex characters in %c%c", input[0], input[1]);

		input+=2;
		md5[i]=(x<<4) | y;
	}

	/* This should return a (const char*), to be compatible with the input.
	 * But we cannot simply change all variables in ops__load_1entry() -
	 * strtoul() has the same signature (const char*, char**). */
	if (eos) *eos=(char*)input;

ex:
	return status;
}


/** -. */
char* cs__md5tohex(const md5_digest_t md5, char *dest)
{
	/* According to callgrind is this one of the most cpu-intensive
	 * places in *this* program (not counting libraries).
	 * Of course, it vanishes against apr_file_read and others ...
	 * But optimizing makes fun, and acta est fabula...
	 *
	 * If I read kcachegrind correctly, that shaved 1% runtime off on 
	 * a simple "fsvs up".
	 *
	 * If there was only something like #while .... */
	sprintf(dest, 
			"%02x" "%02x" "%02x" "%02x" 
			"%02x" "%02x" "%02x" "%02x" 
			"%02x" "%02x" "%02x" "%02x" 
			"%02x" "%02x" "%02x" "%02x" 
			,
			md5[4*0+0], md5[4*0+1], md5[4*0+2], md5[4*0+3], 
			md5[4*1+0], md5[4*1+1], md5[4*1+2], md5[4*1+3], 
			md5[4*2+0], md5[4*2+1], md5[4*2+2], md5[4*2+3], 
			md5[4*3+0], md5[4*3+1], md5[4*3+2], md5[4*3+3]
			);
#if 0
	int i;

	// Simple, slow variant
	for (i=0; i<APR_MD5_DIGESTSIZE; i++)
	{
		sprintf(cur+i*2, "%02x", md5[i]);
	}
#endif

	return dest;
}


/** -.
 * This function alternates between a few static buffers,
 * to allow for a printf("%s %s\n", cs__md5tohex_buffered(), cs__md5tohex_buffered()); 
 *
 * It doesn't use the string caches, because the exact length of the 
 * buffers is known ... so the alloc/realloc/strcpy overhead is 
 * unnecessary. */
char *cs__md5tohex_buffered(const md5_digest_t md5)
{
	static int last=0;
	static char stg[4][APR_MD5_DIGESTSIZE*2+1] = { { 0 } };
	char *cur;

	last++;
	if (last >= sizeof(stg)/sizeof(stg[0])) last=0;
	cur=stg[last];

	return cs__md5tohex(md5, cur);
}


/** Finish manber calculations.
 *
 * Calculates the full-file MD5 hash, and copies it into the associated
 * struct \a estat . - see comment at \a cs__new_manber_filter() . */
int cs___finish_manber(struct t_manber_data *mb_f)
{
	int status;

	status=0;
	STOPIF( apr_md5_final(mb_f->full_md5, & mb_f->full_md5_ctx), 
			"apr_md5_final failed");
	if (mb_f->sts)
		memcpy(mb_f->sts->md5, mb_f->full_md5, sizeof(mb_f->sts->md5));

	mb_f->sts=NULL;

ex:
	return status;
}


/** 
 * -.
 * \param sts Which entry to check
 * \param fullpath The path to the file (optionally, else \c NULL).  If the 
 * file has been checked already and fullpath is \c NULL, a debug message 
 * can write \c (null), as then even the name calculation
 * is skipped.
 * \param result is set to \c 0 for identical to old and \c &gt;0 for 
 * changed.
 * As a special case this function returns \c &lt;0 for <i>don't know</i> 
 * if the file is unreadable due to a \c EACCESS.
 *
 * \note Performance optimization
 * In normal circumstances not the whole file has to be read to get the 
 * result. On update a checksum is written for each manber-block of about 
 * 128k (but see \ref CS__APPROX_BLOCKSIZE_BITS); as soon as one is seen as 
 * changed the verification is stopped.
 * */
int cs__compare_file(struct estat *sts, char *fullpath, int *result)
{
	int i, status, fh;
	unsigned length_mapped, map_pos, hash_pos;
	off_t current_pos;
	struct cs__manber_hashes mbh_data;
	unsigned char *filedata;
	int do_manber;
	char *cp;
	struct sstat_t actual;
	md5_digest_t old_md5 = { 0 };
	static struct t_manber_data mb_dat;


	/* Default is "don't know". */
	if (result) *result = -1;
	/* It doesn't matter whether we test this or old_rev_mode_packed - if 
	 * they're different, this entry was replaced, and we never get here.  */
	if (S_ISDIR(sts->st.mode)) return 0;

	fh=-1;
	/* hash already done? */
	if (sts->change_flag != CF_UNKNOWN)
	{
		DEBUGP("change flag for %s: %d", fullpath, sts->change_flag);
		goto ret_result;
	}

	status=0;

	if (!fullpath)
		STOPIF( ops__build_path(&fullpath, sts), NULL);

	DEBUGP("checking for modification on %s", fullpath);

	DEBUGP("hashing %s",fullpath);
	memcpy(old_md5, sts->md5, sizeof(old_md5));

	/* We'll open and read the file now, so the additional lstat() doesn't 
	 * really hurt - and it makes sure that we see the current values (or at 
	 * least the _current_ ones :-). */
	STOPIF( hlp__lstat(fullpath, &actual), NULL);

	if (S_ISREG(actual.mode))
	{
		do_manber=1;
		/* Open the file and read the stream from there, comparing the blocks
		 * as necessary.
		 * If a difference is found, stop, and mark file as different. */
		/* If this call returns ENOENT, this entry simply has no md5s-file.
		 * We'll have to MD5 it completely. */
		if (actual.size < CS__MIN_FILE_SIZE)
			do_manber=0;
		else
		{
			status=cs__read_manber_hashes(sts, &mbh_data);
			if (status == ENOENT)
				do_manber=0;
			else 
				STOPIF(status, "reading manber-hash data for %s", fullpath);
		}

		hash_pos=0;
		STOPIF( cs___manber_data_init(&mb_dat, sts), NULL );

		/* We map windows of the file into main memory. Never more than 256MB. */
		current_pos=0;

		fh=open(fullpath, O_RDONLY);
		/* We allow a single special case on error handling: EACCES, which 
		 * could simply mean that the file has mode 000. */
		if (fh<0)
		{
			/* The debug statement might change errno, so we have to save the 
			 * value.  */
			status=errno;
			DEBUGP("File %s is unreadable: %d", fullpath, status);
			if (status == EACCES) 
			{
				status=0;
				goto ex;
			}

			/* Can that happen? */
			if (!status) status=EBUSY;
			STOPIF(status, "open(\"%s\", O_RDONLY) failed", fullpath);
		}

		status=0;
		while (current_pos < actual.size)
		{
			if (actual.size-current_pos < MAPSIZE)
				length_mapped=actual.size-current_pos;
			else
				length_mapped=MAPSIZE;
			DEBUGP("mapping %u bytes from %llu", 
					length_mapped, (t_ull)current_pos); 

			filedata=mmap(NULL, length_mapped, 
					PROT_READ, MAP_SHARED, 
					fh, current_pos);
			STOPIF_CODE_ERR( filedata == MAP_FAILED, errno,
					"comparing the file %s failed (mmap)",
					fullpath);

			map_pos=0;
			while (map_pos<length_mapped)
			{
				STOPIF( cs___end_of_block(filedata+map_pos,
							length_mapped-map_pos, 
							&i, &mb_dat ),
						NULL);

				if (i==-1) break;

				if (do_manber)
				{
					DEBUGP("  old hash=%08X  current hash=%08X", 
							mbh_data.hash[hash_pos], mb_dat.last_state);
					DEBUGP("  old end=%llu  current end=%llu", 
							(t_ull)mbh_data.end[hash_pos], 
							(t_ull)mb_dat.fpos);
					DEBUGP("  old md5=%s  current md5=%s", 
							cs__md5tohex_buffered(mbh_data.md5[hash_pos]),
							cs__md5tohex_buffered(mb_dat.block_md5));

					if (mb_dat.last_state != mbh_data.hash[hash_pos] ||
							mb_dat.fpos != mbh_data.end[hash_pos] ||
							memcmp(mb_dat.block_md5, 
								mbh_data.md5[hash_pos], 
								APR_MD5_DIGESTSIZE) != 0)
					{
						DEBUGP("found a different block before %llu:", 
								(t_ull)(current_pos+i));
changed:
						sts->md5[0] ^= 0x1;
						i=-2;
						break;
					}


					DEBUGP("block #%u ok...", hash_pos);
					hash_pos++;
					/* If this gets true (which it should never), we must not
					 * print the hash values etc. ... The index [hash_pos] is outside
					 * the array boundaries. */
					if (hash_pos > mbh_data.count)
						goto changed;
				}

				/* We have to reset the blocks even if we have no manber hashes ...  
				 * so the eg. data_bits value gets reset. */
				STOPIF( cs___end_of_block(NULL, 0, NULL, &mb_dat), NULL );

				map_pos+=i;
			}

			STOPIF_CODE_ERR( munmap((void*)filedata, length_mapped) == -1,
					errno, "unmapping of file failed");
			current_pos+=length_mapped;

			if (i==-2) break;
		}

		STOPIF( cs___finish_manber( &mb_dat), NULL);
	}
	else if (S_ISLNK(sts->st.mode))
	{
		STOPIF( ops__link_to_string(sts, fullpath, &cp), NULL);

		apr_md5(sts->md5, cp, strlen(cp));
	}
	else
	{
		DEBUGP("nothing to hash for %s", fullpath);
	}

	sts->change_flag = memcmp(old_md5, sts->md5, sizeof(sts->md5)) == 0 ?
		CF_NOTCHANGED : CF_CHANGED;
	DEBUGP("change flag for %s set to %d", fullpath, sts->change_flag);


ret_result:
	if (result)
		*result = sts->change_flag == CF_CHANGED;
	DEBUGP("comparing %s=%d: md5 %s", 
			fullpath, sts->change_flag == CF_CHANGED,
			cs__md5tohex_buffered(sts->md5));
	status=0;

ex:
	if (fh>=0) close(fh);

	return status;
}


/** -.
 * If a file has been committed, this is where various checksum-related
 * uninitializations can happen. */
int cs__set_file_committed(struct estat *sts) 
{
	int status;

	status=0;
	if (S_ISDIR(sts->st.mode)) goto ex;

	/* Now we can drop the check flag. */
	sts->flags &= ~(RF_CHECK | RF_PUSHPROPS);

	sts->repos_rev=SET_REVNUM;

ex:
	return status;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
 * Stream functions and callbacks
 * for manber-filtering
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int cs___manber_data_init(struct t_manber_data *mbd, 
		struct estat *sts)
{
	int status;

	memset(mbd, 0, sizeof(*mbd));
	mbd->manber_fd=-1;

	BUG_ON(mbd->sts, "manber structure already in use!");
	mbd->sts=sts;
	mbd->fpos= mbd->last_fpos= 0;
	apr_md5_init(& mbd->full_md5_ctx);
	STOPIF( cs___end_of_block(NULL, 0, NULL, mbd), NULL );

ex:
	return status;
}


void cs___manber_init(struct t_manber_parms *mb_d)
{
	int i;
	AC_CV_C_UINT32_T p;

	if (mb_d->values[0]) return;

	/* Calculate the CS__MANBER_BACKTRACK power of the prime */
	/* TODO: speedup like done in RSA - log2(power) */
	for(p=1,i=0; i<CS__MANBER_BACKTRACK; i++)
		p=(p * CS__MANBER_PRIME) & CS__MANBER_MODULUS;

	/* Precalculate for all 8bit values */
	for(i=0x00; i<0xff; i++)
		mb_d->values[i]=(i*p) & CS__MANBER_MODULUS;
}


/* This function finds the position which 
 *
 *   a b c d e f g h i j k l m n
 *   |..Block.1..| |..Block.2...
 * Here it would return h; ie. the number of characters
 * found in this data block belonging to the current block.
 *
 * If the whole data buffer belongs to the current block -1 is returned
 * in *eob.
 * */
int cs___end_of_block(const unsigned char *data, int maxlen, 
		int *eob, 
		struct t_manber_data *mb_f)
{
	int status;
	int i;


	status=0;
	if (!data)
	{
		DEBUGP("manber reinit");
		mb_f->state=0;
		mb_f->last_state=0;
		mb_f->bktrk_bytes=0;
		mb_f->bktrk_last=0;
		mb_f->data_bits=0;
		apr_md5_init(& mb_f->block_md5_ctx);
		memset(mb_f->block_md5, 0, sizeof(mb_f->block_md5));
		cs___manber_init(&manber_parms);
		goto ex;
	}


	*eob = -1;
	i=0;
	/* If we haven't had at least this many bytes in the current block,
	 * read up to this amount. */
	while (i<maxlen &&
			mb_f->bktrk_bytes < CS__MANBER_BACKTRACK)
	{
		/* In this initialization, we simply \c OR the bytes together.
		 * On block end detection we see if this is at least a
		 * \c CS__MANBER_BACKTRACK bytes long zero-byte block. */
		mb_f->data_bits |= data[i];

		mb_f->state = (mb_f->state * CS__MANBER_PRIME +
				data[i] ) % CS__MANBER_MODULUS;
		mb_f->backtrack[ mb_f->bktrk_last ] = data[i];
		/* The reason why CS__MANBER_BACKTRACK must be a power of 2:
		 * bitwise-AND is much faster than a modulo.
		 * In this loop the & is redundant - in a new block we should
		 * start from bktrk_last==0, but the AND is only 1 or 2 cycles,
		 * and we hope that gcc optimizes that. */
		mb_f->bktrk_last = ( mb_f->bktrk_last + 1 ) & 
			(CS__MANBER_BACKTRACK - 1);
		mb_f->bktrk_bytes++;
		i++;
	}

	if (!mb_f->data_bits)
	{
		/* No bits in the data set - only zeroes so far.
		 * Look for the next non-zero byte; there's a block border. */
		/* memchr is the exact opposite of what we need. */
		while (i<maxlen && !data[i]) i++;

		if (i < maxlen)
		{
			*eob=i;
			DEBUGP("zero block border at %d", i);
		}
	}
	else
	{
		while(i<maxlen)
		{
			/* ->last_state gets the previous CRC, and this gets stored.
			 * This is because the ->state has, on a block border, a lot of
			 * zeroes (per definition); so we store the previous value, which
			 * may be better suited for comparison. If the blocks are equal 
			 * up to byte N, they're equal up to N-1, too. */
			/* This need not be calculated in the previous loop, as we do no
			 * border-checking there. Only here, in this loop, 
			 * is the value needed. */
			mb_f->last_state=mb_f->state;
			mb_f->state = (mb_f->state*CS__MANBER_PRIME + data[i] -
					manber_parms.values[ mb_f->backtrack[ mb_f->bktrk_last ] ] ) 
				% CS__MANBER_MODULUS;
			mb_f->backtrack[ mb_f->bktrk_last ] = data[i];
			mb_f->bktrk_last = ( mb_f->bktrk_last + 1 ) & 
				(CS__MANBER_BACKTRACK - 1);
			/* This value has already been used. */
			i++;

			/* special value ? */
			if ( !(mb_f->state & CS__MANBER_BITMASK) )
			{
				*eob=i;
				apr_md5_update(& mb_f->block_md5_ctx, data, i);
				apr_md5_final( mb_f->block_md5, & mb_f->block_md5_ctx);
				DEBUGP("manber found a border: %u %08X %08X %s", 
						i, mb_f->last_state, mb_f->state, cs__md5tohex_buffered(mb_f->block_md5));
				break;
			}
		}

		/* Update md5 up to current byte. */
		if (*eob == -1)
			apr_md5_update(& mb_f->block_md5_ctx, data, i);
	}

	/* Update file global information */
	apr_md5_update(& mb_f->full_md5_ctx, data, i);
	mb_f->fpos += (*eob == -1) ? maxlen : *eob;

ex:
	DEBUGP("on return at fpos=%llu: %08X (databits=%2x)", 
			(t_ull)mb_f->fpos, mb_f->state, mb_f->data_bits);
	return status;
}


int cs___update_manber(struct t_manber_data *mb_f,
		const unsigned char *data, apr_size_t len)
{
	int status;
	int eob, i;
	/* MD5 as hex (constant-length), 
	 * state as hex (constant-length),
	 * offset of block, length of block, 
	 * \n, \0, reserve */
	char buffer[MANBER_LINELEN+10];
	char *filename;

	status=0;
	/* We tried to avoid doing this calculation for small files.
	 *
	 * But: this does not work.
	 * As *on update* we don't know how many bytes we'll
	 * have to process, and the buffer size is not specified,
	 * we might be legitimately called with single-byte values.
	 *
	 * On my machine I get for commit/update requests of 100kB 
	 * (yes, 102400 bytes), so I thought to have at least some chance.
	 * But: svn_ra_get_file uses 16k blocks ... 
	 *
	 * A full solution would either have to
	 *   - know how many bytes we get (update; on commit we know), or to
	 *   - buffer all to-be-written-data unless we have more bytes
	 *     than limited.
	 * The second variant might be easier.
	 *
	 * As we now check on end-of-stream for the size and remove the file if
	 * necessary, this is currently deactivated. */
	DEBUGP("got a block with %llu bytes", (t_ull)len);
	while (1)
	{
#if 0
		/* If first call, and buffer smaller than 32kB, avoid this calling ... */
		if (mb_f->fpos == 0 && len<32*1024)
			eob=-1;
		else
#endif
			STOPIF( cs___end_of_block(data, len, &eob, mb_f), NULL );

		if (eob == -1) 
		{
			DEBUGP("block continues after %lu.", (unsigned long)mb_f->fpos);
			break;
		}

		data += eob;
		len -= eob;
		DEBUGP("block ends after %lu; size %lu bytes (border=%u).", 
				(unsigned long)mb_f->fpos,
				(unsigned long)(mb_f->fpos - mb_f->last_fpos),
				eob);

		/* write new line to data file */
		i=sprintf(buffer, cs___mb_wr_format, 
				cs__md5tohex_buffered(mb_f->block_md5),
				mb_f->last_state,
				(t_ull)mb_f->last_fpos, 
				(t_ull)(mb_f->fpos - mb_f->last_fpos));
		BUG_ON(i > sizeof(buffer)-3, "Buffer too small - stack overrun");

		if (mb_f->manber_fd == -1)
		{
			/* The file has not been opened yet.
			 * Do it now.
			 * */
			STOPIF( ops__build_path(&filename, mb_f->sts), NULL);
			STOPIF( waa__open_byext(filename, WAA__FILE_MD5s_EXT, WAA__WRITE,
						&	cs___manber.manber_fd), NULL );
			DEBUGP("now doing manber-hashing for %s...", filename);
		}

		STOPIF_CODE_ERR( write( mb_f->manber_fd, buffer, i) != i,
				errno, "writing to manber hash file");

		/* re-init manber state */
		STOPIF( cs___end_of_block(NULL, 0, NULL, mb_f), NULL );
		mb_f->last_fpos = mb_f->fpos;
	}

ex:
	return status;
}


svn_error_t *cs___mnbs_close(void *baton);


svn_error_t *cs___mnbs_read(void *baton, 
		char *data, apr_size_t *len)
{
	int status;
	svn_error_t *status_svn;
	struct t_manber_data *mb_f=baton;

	status=0;
	/* Get the bytes, then process them. */
	STOPIF_SVNERR( svn_stream_read,
			(mb_f->input, data, len) );
	if (*len && data)
		STOPIF( cs___update_manber(mb_f, (unsigned char*)data, *len), NULL);
	else
		STOPIF_SVNERR( cs___mnbs_close, (baton));
ex:
	RETURN_SVNERR(status);
}


svn_error_t *cs___mnbs_write(void *baton, 
		const char *data, apr_size_t *len)
{
	int status;
	svn_error_t *status_svn;
	struct t_manber_data *mb_f=baton;

	status=0;
	/* We first write to the output stream, to know how many bytes could
	 * be processed. Then we use that bytes. 
	 * If we just processed the incoming bytes, then fewer would get written,
	 * and the remaining would be re-done we'd hash them twice. */
	STOPIF_SVNERR( svn_stream_write,
			(mb_f->input, data, len) );
	if (*len && data)
		STOPIF( cs___update_manber(mb_f, (unsigned char*)data, *len), NULL);
	else
		STOPIF_SVNERR( cs___mnbs_close, (baton));
ex:
	RETURN_SVNERR(status);
}


svn_error_t *cs___mnbs_close(void *baton)
{
	int status;
	svn_error_t *status_svn;
	struct t_manber_data *mb_f=baton;

	status=0;

	/* If there have been less than CS__MIN_FILE_SIZE bytes, we
	 * don't keep that file. */
	if (mb_f->manber_fd != -1)
	{
		STOPIF( waa__close(mb_f->manber_fd, 
					mb_f->fpos < CS__MIN_FILE_SIZE ? ECANCELED : 
					status != 0), NULL );
		mb_f->manber_fd=-1;
	}

	if (mb_f->input)
	{
		STOPIF_SVNERR( svn_stream_close,
				(mb_f->input) );
		mb_f->input=NULL;
	}

	STOPIF( cs___finish_manber(mb_f), NULL);

ex:
	RETURN_SVNERR(status);
}


/** -.
 * On commit and update we run the stream through a filter, to create the
 * manber-hash data (see \ref md5s) on the fly. 
 *
 * \note
 * We currently give the caller no chance to say whether he wants the
 * full MD5 or not.
 * If we ever need to let him decide, he must either 
 * - save the old MD5
 * - or (better!) says where the MD5 should be stored - this pointer
 *   would replace \c mb_f->full_md5 .
 *   */
int cs__new_manber_filter(struct estat *sts,
		svn_stream_t *stream_input, 
		svn_stream_t **filter_stream,
		apr_pool_t *pool)
{
	int status;
	svn_stream_t *new_str;
	char *filename;


	status=0;
	STOPIF( cs___manber_data_init(&cs___manber, sts),
			"manber-data-init failed");

	cs___manber.input=stream_input;

	new_str=svn_stream_create(&cs___manber, pool);
	STOPIF_ENOMEM( !new_str );

	svn_stream_set_read(new_str, cs___mnbs_read);
	svn_stream_set_write(new_str, cs___mnbs_write);
	svn_stream_set_close(new_str, cs___mnbs_close);

	STOPIF( ops__build_path(&filename, sts), NULL);
	DEBUGP("initiating MD5 streaming for %s", filename);

	*filter_stream=new_str;

	/* The file with the hashes for the blocks is not immediately opened.
	 * only when we detect that we have at least a minimum file size
	 * we do the whole calculation.*/

ex:
	return status;
}


/** \defgroup md5s_overview Overview
 * \ingroup perf
 *
 * When we compare a file with its last version, we read all the
 * manber-hashes into memory.
 * When we use them on commit for constructing a delta stream, we'll
 * have to have them sorted and/or indexed for fast access; then we
 * can't read them from disk or something like that.
 *
 *
 * \section md5s_perf Performance considerations
 *
 * \subsection md5s_count Count of records, memory requirements
 *
 * We need about 16+4+8 (28, with alignment 32) bytes per hash value,
 * and that's for approx. 128kB. So a file of
 *   1M needs  8*32 => 512 bytes, 
 *   1G needs 8k*32 => 512 kB,
 *   1T needs 8M*32 => 512 MB.
 * If this is too much, you'll have to increase CS__APPROX_BLOCKSIZE_BITS
 * and use bigger blocks.
 *
 * (Although, if you've got files greater than 1TB, you'll have other 
 * problems than getting >512MB RAM)
 * And still, there's (nearly) always a swap space ... 
 *
 *
 * \subsection md5s_perf Allocation
 *
 * To avoid the costs of the unused 4 bytes (which accumulate to 32MB
 * on a 1TB file) and to get the manber-hashes better into 
 * L2 cache (only the 32bit value - the rest is looked up after we
 * found the correct hash) we allocate 3 memory regions -
 * one for each data.
 *
 *
 * \subsection Reading for big files
 *
 * It's a tiny bit disturbing that we read the whole file at once and not
 * as-needed.
 * On the hypothetical 1TB file we'll be reading 512MB before seeing that
 * the first block had changed...
 * Perhaps this should be expanded later, to say something like "already
 * open, return next N entries" - file handle, last N, etc. should be stored 
 * in struct cs__manber_hashes. 
 * For now I'll just ignore that, as a (practical) 4GB file (a DVD)
 * will lead to 2MB read - and on average we'll find a difference after
 * 2GB more reading. 
 * 
 * A better way than read()/lseek() would be mmap of binary files. 
 * But that wouldn't allow to have the data compressed. 
 * I don't know if that matters; the 1TB file has
 * 8M lines * 60 Bytes => 480MB on ASCII-data.
 *
 * If we assume that the CRCs and lengths can be compressed away,
 * we still need the offsets and MD5s, so we would still end with 
 * 8M * 24 Bytes, ie. at least 196MB.
 * I don't think that's a necessity. 
 *
 *
 * \section Hash-collisions on big files
 *
 * Please note, that on 1TB files you'll have 8M of hash blocks, which
 * have a significant collision-chance on 32bit hash values!
 * (look up discussion about rsync and its bugs on block change detection).
 * We'd either have to use bigger blocks or a bigger hash value - 
 * the second might be easier and better, esp. as files this big should
 * be on a 64bit platform, where a 64bit hash won't be slow.
 *
 *
 * \section The last block
 *
 * The last block in a file ends per definition *not* on a manber-block-
 * border (or only per chance). This block is not written into the md5s file.
 * The data is verified by the full-file MD5 that we've been calculating.
 *
 * \todo When we do a rsync-copy from the repository, we'll have to look at
 * that again! Either we write the last block too, or we'll have to ask for
 * the last few bytes extra.
 * 
 * */
/** -.
 * \param sts The entry whose md5-data to load
 * \param data An allocated struct \c cs__manber_hashes; its arrays get
 * allocated, and, <b>on error</b>, deallocated.
 * If no error code is returned, freeing of the arrays has to be done by
 * the caller.
 * */
int cs__read_manber_hashes(struct estat *sts, struct cs__manber_hashes *data)
{
	int status;
	char *filename;
	int fh, i, spp;
	unsigned estimated, count;
	t_ull length, start;
	char buffer[MANBER_LINELEN+10], *cp;
	AC_CV_C_UINT32_T value;


	status=0;
	memset(data, 0, sizeof(*data));
	fh=-1;

	STOPIF( ops__build_path(&filename, sts), NULL);
	/* It's ok if there's no md5s file. simply return ENOENT. */
	status=waa__open_byext(filename, WAA__FILE_MD5s_EXT, WAA__READ, &fh);
	if (status == ENOENT) goto ex;
	STOPIF( status, "reading md5s-file for %s", filename);

	DEBUGP("reading manber-hashes for %s", filename);

	/* We don't know in advance how many lines (i.e. manber-hashes)
	 * there will be.
	 * So we just interpolate from the file size and the (near-constant)
	 * line-length and add a bit for good measure.
	 * The rest is freed as soon as we've got all entries. */
	length=lseek(fh, 0, SEEK_END);
	STOPIF_CODE_ERR( length==-1, errno, 
			"Cannot get length of file %s", filename);
	STOPIF_CODE_ERR( lseek(fh, 0, SEEK_SET), errno, 
			"Cannot seek in file %s", filename);
	/* We add 5%; due to integer arithmetic the factors have to be separated */
	estimated = length*21/(MANBER_LINELEN*20)+4;
	DEBUGP("estimated %u manber-hashes from filelen %llu", 
			estimated, length);

	/* Allocate memory ... */
	STOPIF( hlp__calloc( &data->hash, estimated, sizeof(*data->hash)), NULL);
	STOPIF( hlp__calloc( & data->md5, estimated, sizeof( *data->md5)), NULL);
	STOPIF( hlp__calloc( & data->end, estimated, sizeof( *data->end)), NULL);


	count=0;
	while (1)
	{
		i=read(fh, buffer, sizeof(buffer)-1);
		STOPIF_CODE_ERR( i==-1, errno,
				"reading manber-hash data");
		if (i==0) break;

		/* ensure strchr() stops */
		buffer[i]=0;
		cp=strchr(buffer, '\n');
		STOPIF_CODE_ERR(!cp, EINVAL, 
				"line %u is invalid", count+1 );

		/* reposition to start of next line */
		STOPIF_CODE_ERR( lseek(fh, 
					-i								// start of this line
					+ (cp-buffer)			// end of this line
					+ 1,							// over \n
					SEEK_CUR) == -1, errno, 
				"lseek back failed");
		*cp=0;

		i=sscanf(buffer, cs___mb_rd_format,
				&spp, &value, &start, &length);
		STOPIF_CODE_ERR( i != 3, EINVAL,
				"cannot parse line %u for %s", count+1, filename);

		data->hash[count]=value;
		data->end[count]=start+length;
		buffer[spp]=0;
		STOPIF( cs__char2md5(buffer, NULL, data->md5[count]), NULL);
		count++;
		BUG_ON(count > estimated, "lines should have syntax errors - bug in estimation.");
	}

	data->count=count;
	DEBUGP("read %u entry tuples.", count);

	if (estimated-count > 3)
	{
		DEBUGP("reallocating...");
		/* reallocate memory = free */
		STOPIF( hlp__realloc( &data->hash, count*sizeof(*data->hash)), NULL);
		STOPIF( hlp__realloc( & data->md5, count*sizeof(* data->md5)), NULL);
		STOPIF( hlp__realloc( & data->end, count*sizeof(* data->end)), NULL);
	}

	/* The index is not always needed. Don't generate now. */

ex:
	if (status)
	{
		IF_FREE(data->hash);
		IF_FREE(data->md5);
		IF_FREE(data->end);
	}

	if (fh != -1)
		STOPIF_CODE_ERR( close(fh) == -1, errno, 
				"Cannot close manber hash file (fd=%d)", fh);
	return status;
}

