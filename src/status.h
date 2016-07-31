/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __STATUS_H__
#define __STATUS_H__

#include "actions.h"

/** \file
 * \ref status output header file. */

/** A function to show the local status of an entry. */
action_t st__status;
/** A function to show the remote status of an entry. */
action_t st__rm_status;
/** The \ref status worker function. */
work_t st__work;


/** Percentual display of progress. */
action_t st__progress;
/** Uninitializer for \ref st__progress. */
action_uninit_t st__progress_uninit;

/** Shows detailed information about the entry. */
int st__print_entry_info(struct estat *sts, int with_type);

/** Returns a string describing the \a entry_status bits of struct \a 
 * estat. */
volatile char* st__status_string(const struct estat * const sts);
/** Same as \ref st__status_string, but directly given an status. */
volatile char* st__status_string_fromint(int mask);
/** Return the string interpretation of the flags like \ref RF_CHECK. */
volatile char* st__flags_string_fromint(int mask);

#endif

