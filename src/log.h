/************************************************************************
 * Copyright (C) 2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#ifndef __LOG_H__
#define __LOG_H__

#include "actions.h"

/** \file
 * \ref log action header file. */

/** Prints the given log messages. */
work_t log__work;


/** \name Log option bits.
 * The colorized cg-log output is nice!
 * @{ */
#define LOG__OPT_COLOR (1)
#define LOG__OPT_INDENT (2)
/** @} */

#define LOG__OPT_DEFAULT (LOG__OPT_COLOR | LOG__OPT_INDENT)


#endif
