/************************************************************************
 * Copyright (C) 2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/
#ifndef __PREPROC_H__
#define __PREPROC_H__

/** \file
 * Preprocessor macros for global use.
 * */

#include <fcntl.h>
#include <sys/stat.h>


/** Macros for counting the bits set.
* The interested party is referred to "<i>Numerical Recipes</i>".
* @{ */
#define _BITCOUNTx(x, m, s) ( ((x) & m) + (((x) & (m << s)) >> s) )
#define _BITCOUNT6(x) (_BITCOUNTx(          (x), 0x5555555555555555ULL,  1))
#define _BITCOUNT5(x) (_BITCOUNTx(_BITCOUNT6(x), 0x3333333333333333ULL,  2))
#define _BITCOUNT4(x) (_BITCOUNTx(_BITCOUNT5(x), 0x0f0f0f0f0f0f0f0fULL,  4))
#define _BITCOUNT3(x) (_BITCOUNTx(_BITCOUNT4(x), 0x00ff00ff00ff00ffULL,  8))
#define _BITCOUNT2(x) (_BITCOUNTx(_BITCOUNT3(x), 0x0000ffff0000ffffULL, 16))
#define _BITCOUNT1(x) (_BITCOUNTx(_BITCOUNT2(x), 0x00000000ffffffffULL, 32))
#define _BITCOUNT(x) ( (int)_BITCOUNT1(x) )
/** @} */


/** How many bits a \c mode_t must be shifted to get the packed 
 * representation.
 * */
#define MODE_T_SHIFT_BITS (_BITCOUNT(S_IFMT ^ (S_IFMT-1)) -1)

/** The number of bits needed for storage. */
#define PACKED_MODE_T_NEEDED_BITS (_BITCOUNT(S_IFMT))


/** How to convert from \c mode_t to the packed representation (in struct 
 * \ref estat) and back.
 * @{ */
#define MODE_T_to_PACKED(mode) ((mode) >> MODE_T_SHIFT_BITS)
#define PACKED_to_MODE_T(p) ((p) << MODE_T_SHIFT_BITS)
/** @} */
/** Simplification for testing packed modes.
 * Used with S_ISDIR etc. */
#define TEST_PACKED(test, val) test(PACKED_to_MODE_T(val))


#endif
