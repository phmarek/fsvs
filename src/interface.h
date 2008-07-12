/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __INTERFACE_H__
#define __INTERFACE_H__

/** \file
 * Interface to the outside.
 * */

/** \defgroup interface The interface to the outside world.
 * \ingroup compat
 *
 * Here the interfaces to the outside are defined - 
 * environment variables and similar. */
/** @{ */

/** If this variable has a numeric value other than 0, the debuglevel is 
 * set even before commandline parsing. */
#define FSVS_DEBUG_ENV "FSVS_DEBUGLEVEL"

/** The diff program to use.
 *
 * It's arguments are similar to
 * \code
 *   diff -u file1 --label1 file2 --label2
 * \endcode
 * If you use another program, expect these parameters.
 *
 * An exit status of 1 is ignored; the meaning "file has changed"
 * is assumed.
 * */
#define DIFF_ENV "FSVS_DIFF"
/** @} */


/** The default WAA path. */
#define DEFAULT_WAA_PATH "/var/spool/fsvs"
/** The default CONF path. */
#define DEFAULT_CONF_PATH "/etc/fsvs"


/** \name List of environment variables used for a chroot jail.
 * Note that these are not \c \#ifdef - marked, as we'd like to use 
 * off-the-shelf binaries from newer distributions without modifications!  
 * */
/** @{ */
/** The file descriptor number where \c fsvs can find the "original", 
 * "normal" root directory. */
#define CHROOTER_ROOT_ENV "FSVS_CHROOT_ROOT"
/** Which libraries should be preloaded? Space-separated list. */
#define CHROOTER_LIBS_ENV "FSVS_CHROOT_LIBS"
/** The old working directory <b>file descriptor</b> */
#define CHROOTER_CWD_ENV "FSVS_CHROOT_CWD"
/** @} */


/** \name Manber-parameters
 *
 * These should be written to a property for big files,
 * so that they can be easily read before fetching the file.
 * We need the same values for fetching as were used on storing -
 * else we cannot do some rsync-like update.
 *
 * \note Currently they are used only for checking whether a 
 * file has changed locally. Here they should be written into
 * the \a md5s file. */
/** @{ */
/** How many bits must be zero in the CRC to define that location
 * as a block border.
 * See checksum.c for details.
 *
 * 16 bits give blocks of 64kB (on average) ...
 * we use 17 for 128kB. */
#define CS__APPROX_BLOCKSIZE_BITS (17)
/** The bit mask for comparing. */
#define CS__MANBER_BITMASK ((1 << CS__APPROX_BLOCKSIZE_BITS)-1)
/** The modulus. Leave at 32bit. */
#define CS__MANBER_MODULUS (-1)
/** The prime number used for generation of the hash. */
#define CS__MANBER_PRIME (31)
/** The number of bytes for the block comparison.
 * Must be a power of 2 for performance reasons. */
#define CS__MANBER_BACKTRACK (2*1024)
#if (CS__MANBER_BACKTRACK-1) & CS__MANBER_BACKTRACK
#error CS__MANBER_BACKTRACK must be a power of 2!
#endif

/** The minimum filesize, at or above which files get
 * tested in blocks.
 * Makes sense to have block size as minimum, but is not needed. */
#define CS__MIN_FILE_SIZE (256*1024)
/** @} */


/** @} */


#endif

