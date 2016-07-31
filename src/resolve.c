/************************************************************************
 * Copyright (C) 2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>
#include <sys/uio.h>
#include <sys/mman.h>


#include "global.h"
#include "est_ops.h"
#include "helper.h"
#include "url.h"
#include "status.h"
#include "resolve.h"
#include "waa.h"
#include "hash_ops.h"
#include "actions.h"


/** \file
 * The \ref resolve command source file.
 *
 * */

/** \addtogroup cmds
 * 
 * \section resolve
 *
 * \code
 * fsvs resolve PATH [PATH...]
 * \endcode
 * 
 * When FSVS tries to update local files which have been changed, a 
 * conflict might occur. (For various ways of handling these please see the 
 * \ref o_conflict "conflict" option.)
 *
 * This command lets you mark such conflicts as resolved.
 */


/** -.
 *
 * The conflict flag \b must be set by this function, so that it knows 
 * whether it has to purge any (wrongly) pre-existing \ref cflct file or to 
 * just append.
 * */
int res__mark_conflict(struct estat *sts, ...)
{
	int status;
	char *filename;
	va_list va;
	int filehdl;
	int len;
	struct iovec io[2] = { { 0 }, { .iov_base="\n", .iov_len=1 } };


	status=0;
	va_start(va, sts);
	filehdl=-1;

	STOPIF( ops__build_path(&filename, sts), NULL);
	STOPIF( waa__open_byext(filename, WAA__CONFLICT_EXT, 
				(sts->flags & RF_CONFLICT) ? WAA__APPEND : WAA__WRITE,
				&	filehdl), NULL );

	while ( (filename =va_arg(va, char*)) )
	{
		len=strlen(filename);

		io[0].iov_base=filename;
		/* Take the \0 */
		io[0].iov_len=len+1;
		STOPIF_CODE_ERR( writev(filehdl, io, sizeof(io)/sizeof(io[0])) != len+1+1,
				errno, "Writing the conflict list for %s", filename);
	}

	sts->flags |= RF_CONFLICT;

ex:
	if (filehdl != -1)
	{
		len=waa__close(filehdl, status);
		filehdl=-1;
		STOPIF_CODE_ERR( !status && len==-1, errno,
				"Closing the conflict list for %s", filename);
	}
	return status;
}


/** -.
 * */
int res__action(struct estat *sts)
{
	int status;

	status=0;
	if (sts->flags & RF_ISNEW)
	{
		/* We're not going recursively, so there's no need to process 
		 * sub-entries. */
		sts->entry_type=FT_IGNORE;
	}
	else
	{
		if ( sts->flags & RF_CONFLICT )
			STOPIF( res__remove_aux_files(sts), NULL);
		STOPIF( st__status(sts), NULL);
	}

ex:
	return status;
}


/** -.
 * */
int res__remove_aux_files(struct estat *sts)
{
	int status;
	char *filename, *to_remove;
	int filehdl;
	int len;
	char *mapped;
	struct sstat_t st;


	status=0;
	filehdl=-1;
	mapped=MAP_FAILED;

	STOPIF( ops__build_path(&filename, sts), NULL);
	STOPIF( waa__open_byext(filename, WAA__CONFLICT_EXT, 
				WAA__READ, &filehdl), NULL );

	STOPIF( hlp__fstat( filehdl, &st), NULL);

	mapped=mmap(NULL, st.size, PROT_READ, MAP_SHARED, filehdl, 0);
	STOPIF_CODE_ERR( mapped==MAP_FAILED, errno, 
			"Can't map handle %d", filehdl);

	to_remove=mapped;
	while ( to_remove - mapped != st.size )
	{
		BUG_ON(to_remove - mapped > st.size);

		if (unlink(to_remove) == -1)
			STOPIF_CODE_ERR( errno != ENOENT, errno,
					"Cannot remove conflict file \"%s\" (from \"%s\")",
					to_remove, filename);

		to_remove += strlen(to_remove)+1;
		if (*to_remove == '\n') to_remove++;
	}

	sts->flags &= ~RF_CONFLICT;

	STOPIF( waa__delete_byext(filename, WAA__CONFLICT_EXT, 0), NULL);

ex:
	if (filehdl != -1)
	{
		len=waa__close(filehdl, status);
		filehdl=-1;
		STOPIF_CODE_ERR( !status && len==-1, errno,
				"Closing the conflict list for %s", filename);
	}

	if (mapped != MAP_FAILED)
		STOPIF_CODE_ERR( munmap(mapped, st.size) == -1, errno,
				"Cannot munmap()");

	return status;
}


/** -.
 * */
int res__work(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;


	status=0;
	/* Don't recurse. */
	opt_recursive=-1;
	only_check_status=1;

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);
	if (argc == 0) ac__Usage_this();

	STOPIF( url__load_nonempty_list(NULL, 0), NULL);

	/* Maybe we should add a flag saying that we don't want unknown entries, 
	 * like it can easily happen with "fsvs resolve *".
	 * But then we'd get an error, and this is not so user-friendly like just 
	 * ignoring these entries in res__action(). */
	status=waa__read_or_build_tree(root, argc, normalized, argv, NULL, 1);
	if (status == -ENOENT)
		STOPIF(status, "!No data about current entries is available.");
	STOPIF(status, NULL);

	STOPIF( waa__output_tree(root), NULL);

ex:
	return status;
}

