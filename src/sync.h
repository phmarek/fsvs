/************************************************************************
 * Copyright (C) 2005-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __SYNC_H__
#define __SYNC_H__

#include "actions.h"

/** \file
 * \ref sync-repos action header file. */

/** Loads the directory structure from the repository. */
work_t sync__work;
/** Prints the synchronization status and stats the (maybe existing) 
 * local entries. */
action_t sync__progress;

#endif


