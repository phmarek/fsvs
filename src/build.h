/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __BUILD_H__
#define __BUILD_H__

#include "actions.h"
#include "global.h"

/** \file
 * \ref _build_new_list action header file.
 *
 * This action is not normally used; as it throws away data from the WAA
 * it is dangerous if simply called without \b exactly knowing the 
 * implications.
 *
 * The only use is for debugging - all other disaster recovery is better done
 * via \c sync-repos.
 * */

/** Build action. */
work_t bld__work;
/** Delay action. */
work_t delay__work;


#endif

