/************************************************************************
 * Copyright (C) 2006-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/


/** \file
 * \ref remote-status command.
 * */

/** \addtogroup cmds
 *
 * \section remote-status
 *
 * \code
 * fsvs remote-status PATH [-r rev]
 * \endcode
 *
 * This command looks into the repository and tells you which files would
 * get changed on an \ref update - it's a dry-run for \ref update .
 * 
 * Per default it compares to \c HEAD, but you can choose another 
 * revision with the \c -r parameter.
 * */

