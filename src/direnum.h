/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __DIRENUM_H__
#define __DIRENUM_H__

/** \file
 * Directory enumerator header file. */

// for alphasort
#include <dirent.h>

#include "global.h"

/** This function reads a directory into a self-allocated memory area. */
int dir__enumerator(struct estat *this,
		int est_count,
		int by_name) ;

/** Sorts the entries of the directory \a sts by name into the
 * estat::by_name array, which is reallocated and NULL-terminated. */
int dir__sortbyname(struct estat *sts);
/** Sorts the existing estat::by_inode array afresh, by device/inode. */
int dir__sortbyinode(struct estat *sts);

int dir___f_sort_by_inode(struct estat **a, struct estat **b);
int dir___f_sort_by_inodePP(struct estat *a, struct estat *b);
int dir___f_sort_by_name(const void *a, const void *b);
int dir___f_sort_by_nameCC(const void *a, const void *b);
int dir___f_sort_by_nameCS(const void *a, const void *b);

/** How many bytes an average filename needs.
 * Measured on a debian system:
 * \code
 * find / -printf "%f\n" | wc
 * \endcode
 * */
#define ESTIMATED_ENTRY_LENGTH (15)

#endif

