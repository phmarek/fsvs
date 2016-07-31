/************************************************************************
 * Copyright (C) 2006-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __EXPORT_H__
#define __EXPORT_H__

#include "actions.h"
#include "update.h"

/** \file
 * \ref export action header file */

/** The \ref export action. */
work_t exp__work;

/** This function exports \a url into the current working directory. */
int exp__do(struct estat *root, struct url_t *url);

#endif


