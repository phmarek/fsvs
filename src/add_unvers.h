/************************************************************************
 * Copyright (C) 2006-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __ADD_UNVERS_H__
#define __ADD_UNVERS_H__

#include "actions.h"

/** \file
 * \ref add and \ref unversion action header file.
 * */

/** For adding/unversioning files. */
work_t au__work;

/** Worker function. */
action_t au__action;

/** In case we need to handle new entries we might have to assign an URL to 
 * them. */
int au__prepare_for_added(void);


#endif


