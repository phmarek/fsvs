/************************************************************************
 * Copyright (C) 2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>

#include "global.h"
#include "status.h"
#include "est_ops.h"
#include "url.h"
#include "checksum.h"
#include "props.h"
#include "cache.h"
#include "helper.h"
#include "waa.h"


/** \file
 * \ref cp and \ref mv actions.
 *
 * Various thoughts ...
 * - Can we construct relations between 2 new files?
 *   We'd just have to write the MD5 of the new files into the hash, then 
 *   we'd find the first file on commit of the 2nd file ... and we see that 
 *   the other one is new, too. \n
 *   But see \ref commit_2_revs "creating 2 revisions on commit".
 *
 *   */

/**
 * \addtogroup cmds
 *
 * \anchor mv
 * \section cp
 *
 * \code
 * fsvs cp SRC DEST
 * fsvs cp dump
 * fsvs cp load
 * \endcode
 *
 * This command marks \c DEST as a copy of \c SRC, so that on the next 
 * commit of \c DEST the corresponding source path is sent as copy source.
 *
 * Please note that this command works \b always on a directory structure - 
 * if you mark a directory as copied, the whole structure is \b marked as 
 * copy. That means that if some entries below the copy are missing, they 
 * are mentioned as removed on the next commit.
 *
 * For performance reasons no checks regarding the existance of paths is 
 * made; as the given relations are used only on the next commit, they 
 * don't need to be valid as they are recorded.
 *
 * If this command are used without parameters, the currently defined
 * relations are printed; please keep in mind that the \b key is the 
 * destination name, ie. the 2nd line of each pair!
 * 
 * The input format for \c load is newline-separated - first a \c SRC line, 
 * followed by a \c DEST line, then an line with just a dot ("<tt>.</tt>") 
 * as delimiter.  If you've got filenames with newlines or other special 
 * characters, you have to give the paths as arguments. 
 *
 * Internally the paths are stored relative to the working copy base 
 * directory, and they're printed that way, too.
 *
 * \todo -0 like for xargs?
 *
 * \note As subversion currently treats a rename as copy+delete, the \ref 
 * mv command is an alias to \ref cp.
 *
 * If you have a need to give the filenames \c dump or \c load as first 
 * parameter for copyfrom relations, give some path, too, as in 
 * <tt>./dump</tt>.
 *
 * \todo Filter for dump (patterns?), \c replace or \c append modes for 
 * load. */


/**
 * \addtogroup cmds
 *
 * \section copyfrom-detect
 *
 * \code
 * fsvs copyfrom-detect [paths...]
 * \endcode
 *
 * This command tells \c fsvs to look through the new entries, and see 
 * whether it can find some that seem to be copied from others already 
 * known. \n
 * It will output a list with source and destination path and why it could 
 * match. 
 *
 * This is just for information purposes and doesn't change any FSVS state, 
 * <i>unless some option/parameter is set. (TODO)</i>
 *
 * The list format is <b>on purpose</b> incompatible with the \c load 
 * syntax, as the best match normally has to be taken manually. 
 * 
 * \todo some parameter that just prints the "best" match, and outputs the 
 * correct format.
 *
 * If \ref glob_opt_verb "verbose" is used, an additional value giving the 
 * percentage of matching blocks, and the count of possibly copied entries 
 * is printed.
 *
 * Example:
 * \code
 *   $ fsvs copyfrom-list -v
 *   newfile1
 *     md5:oldfileA
 *   newfile2
 *     md5:oldfileB
 *     md5:oldfileC
 *     md5:oldfileD
 *   newfile3
 *     inode:oldfileI
 *     manber=82.6:oldfileF
 *     manber=74.2:oldfileG
 *     manber=53.3:oldfileH
 *     ...
 *   3 copyfrom relations found.
 * \endcode
 *
 * The abbreviations are:
 * <table>
 *
 * <TR><td>\e md5<td>
 * The \b MD5 of the new file is identical to that of one or more already 
 * committed files; there is no percentage.
 *
 * <TR><td>\e inode<td>
 * The \b device/inode number is identical to the given known entry; this 
 * could mean that the old entry has been renamed or hardlinked.
 * \b Note: Not all filesystems have persistent inode numbers (eg. NFS) - 
 * so depending on your filesystems this might not be a good indicator!
 *
 * <TR><td>\e manber<td>
 * Analysing files of similar size shows some percentage of 
 * (variable-sized) <b>common blocks</b> (ignoring the order of the 
 * blocks).
 *
 * <TR><td>\e dirlist<td>
 * The new directory has similar files to the old directory.\n
 * The percentage is (number_of_common_entries)/(files_in_dir1 + 
 * files_in_dir2 - number_of_common_entries)
 *
 * </table>
 *
 * \note Only \b md5 and \b inode is currently done.
 *
 * \note If too many possible matches are found, not all may be printed; 
 * only the indicator <tt>...</tt> is shown at the end.
 * 
 * */

/* for internal use only, not visible
 *
 * \section cp_mv_data Storing and reading the needed data
 *
 * For copy/move detection we need fast access to other files with the same 
 * or similar data.
 * - To find identical files we just take a GDBM database, indexed by the 
 *   MD5, and having a (\c \\0 separated) list of filenames.
 *   Why GDBM?
 *   - For partial commits we need to remove/use parts of the data; a 
 *   textfile would have to be completely re-written.
 * - We need the list of copies/moves identified by the user.
 *   - This should be quickly editable
 *   - No big blobs to stay after a commit (empty db structure?)
 *   - Using an file/symlink in the WAA for the new entry seems bad. We'd 
 *   have to try to find such an file/symlink for each committed entry.
 *     - But might be fast by just doing readlink()? = a single syscall
 *	   - The number of copy-entries is typically small.
 *	   - Easy to remove
 *	   - Uses inodes, though
 *	   - Easily readable - just points to source
 *	 - GDBM?
 *	   - easy to update and delete
 *	   - might uses some space
 *	   - a single entry
 *	   - not as easy to read
 *	 - Text-file is out, because of random access for partial commits.
 * - Maybe we should write the manber hash of the first two blocks there, 
 *   too? -- No, manber-hashes would be done on all files at once.
 *
 * */

/* As the databases are good only for a single run, we can easily store the 
 * address directly. For the real copy-from db we have to use the path.
 *
 * Temporary storage:
 *   value.dptr=sts;
 *   value.dsize=sizeof(sts);
 * Persistent:
 *   value.dptr=path;
 *   value.dsize=sts->path_len;
 * */


/** Maximum number of entries that are stored.
 * The -1 is for overflow detection \c "...". */
#define MAX_DUPL_ENTRIES (HASH__LIST_MAX -1)

#if 0
/** Files smaller than this are not automatically bound to some ancestor; 
 * their data is not unique enough. */
#define MIN_FILE_SIZE (32)
#endif


/** How many entries could be correlated. */
int copydetect_count;


/** Function and type declarations.
 * @{ */
typedef datum (cm___to_datum_t)(const struct estat *sts);
cm___to_datum_t cm___md5_datum;
cm___to_datum_t cm___inode_datum;
/** @} */

enum cm___simple_match_e {
  CM___MD5=0,
	CM___INODE_F,
	CM___INODE_D,
};

/** Structure for storing ways for simple file matching. */
struct cm___simple_match_t {
	char name[7];
  cm___to_datum_t *to_key;
	int entry_types;
	hash_t db;
	char filename[8];
};

/** Array with ways for simple matches.
 *
 * We keep file and directory inode matching separated; a file cannot be 
 * the copyfrom source of a directory, and vice-versa. */
struct cm___simple_match_t cm___simple_match_array[]=
{
  [CM___MD5] = { .name="md5", .to_key=cm___md5_datum, 
		.entry_types=FT_FILE, .filename=WAA__FILE_MD5s_EXT},
  [CM___INODE_F] = { .name="inode", .to_key=cm___inode_datum, 
		.entry_types=FT_FILE,  .filename=WAA__FILE_INODE_EXT},
  [CM___INODE_D] = { .name="inode", .to_key=cm___inode_datum, 
		.entry_types=FT_DIR,  .filename=WAA__DIR_INODE_EXT},
};
#define CM___SIMPLE_MATCH_NUM (sizeof(cm___simple_match_array)/sizeof(cm___simple_match_array[0]))



/** Gets a \a datum from a struct estat::md5. */
datum cm___md5_datum(const struct estat *sts)
{
	datum d;

	d.dsize=APR_MD5_DIGESTSIZE*2+1;
	d.dptr=cs__md52hex(sts->md5);
	return d;
}


/** Gets a \a datum from the filesystem addressing - device and inode. */
datum cm___inode_datum(const struct estat *sts)
{
	static struct { ino_t ino; dev_t dev; } tmp;
	datum d;

	tmp.ino=sts->st.ino;
	tmp.dev=sts->st.dev;
	d.dptr=(char*)&tmp;
	d.dsize=sizeof(tmp);
	return d;
}


int cm___register_entry(struct estat *sts, char *path)
{
	int status;
	int i;
	struct cm___simple_match_t *match;


	status=0;
	if (!(sts->entry_status & FS_NEW))
	{
		for(i=0; i<CM___SIMPLE_MATCH_NUM; i++)
		{
			match=cm___simple_match_array+i;
			if (sts->entry_type & match->entry_types)
			{
				DEBUGP("inserting %s for %s", sts->name, match->name);
				STOPIF( hsh__insert_pointer( match->db, 
							(match->to_key)(sts), sts), NULL);
			}
		}
	}

ex:
	return status;
}


int cm___simple_match(struct estat *entry)
{	
	int status;
	char *path, *formatted;
	int i, count, have_match, j, overflows;
	datum key;
	struct estat *sts, **list;
	struct cm___simple_match_t *match;


	formatted=NULL;
	status=0;
	have_match=0;

	/* Down below status will get the value ENOENT from the hsh__list_get() 
	 * lookups; we change it back to 0 shortly before leaving. */

	for(i=0; i<CM___SIMPLE_MATCH_NUM; i++)
	{
		match=cm___simple_match_array+i;

		key=(match->to_key)(entry);
		status=hsh__list_get(match->db, key, &key, &list, &count);
		/* ENOENT = nothing to see */
		if (status != ENOENT)
		{
			STOPIF(status, NULL);

			overflows=0;
			/* Print header line for this file. */
			if (!have_match)
			{
				STOPIF( ops__build_path(&path, entry), NULL);
				STOPIF( hlp__format_path(entry, path, &formatted), NULL);
				printf("%s\n", formatted);
				have_match=1;
			}

			do
			{
				for(j=0; j<count; j++)
				{
					if (count > MAX_DUPL_ENTRIES)
					{
						overflows++;
						/* We show one less than we store, so we have the overflow 
						 * information.  */
						break;
					}

					sts=list[j];
					STOPIF( ops__build_path(&path, sts), NULL);
					STOPIF( hlp__format_path(sts, path, &formatted), NULL);
					printf("  %s:%s\n", match->name, formatted);
				}

				status=hsh__list_get(match->db, key, &key, &list, &count);
			} while (status == 0);

			if (overflows)
				printf("    ...\n");
		}
	}

	/* *************** conclusion */
	if (have_match)
		copydetect_count++;
	else
	{
		if (opt_verbose>0)
		{
			/* cache might be overwritten again when we're here. */
			STOPIF( ops__build_path(&path, entry), NULL);
			STOPIF( hlp__format_path(entry, path, &formatted), NULL);
			printf("- No copyfrom relation found for %s\n", formatted);
		}
		else
			DEBUGP("No source found for %s = %s", path, key.dptr);
	}

	/* If we get here, we're ok. */
	status=0;


ex:
	return status;
}


int cm__find_dir_source(struct estat *dir)
{
	int status;
	status=0;

	STOPIF( cm___simple_match( dir ), NULL);

ex:
	return status;
}


int cm__find_file_source(struct estat *file)
{
	int status;
	char *path;


	status=0;
	STOPIF( ops__build_path(&path, file), NULL);
	DEBUGP("finding source of %s", file->name);

	STOPIF( cs__compare_file(file, path, NULL), NULL);
	/* TODO: EPIPE and similar for output */

	STOPIF( cm___simple_match( file ), NULL);

ex:
	return status;
}



/** After loading known entries try to find some match for every new entry.  
 * */
int cm__find_copied(struct estat *root)
{
	int status;
	struct estat *sts, **child;


	status=0;
	child=root->by_inode;
	if (!child) goto ex;

	while (*child)
	{
		sts=*child;
		/* Should we try to associate the directory after all children have been 
		 * done? We could simply take a look which parent the children's sources 
		 * point to ...
		 *
		 * Otherwise, if there's some easy way to see the source of a directory, 
		 * we could maybe save some searching for all children.... */
		if (sts->entry_status & FS_NEW)
		{
			switch(sts->entry_type)
			{
				case FT_DIR:
					STOPIF(cm__find_dir_source(sts), NULL);
					break;
				case FT_SYMLINK:
				case FT_FILE:
					STOPIF(cm__find_file_source(sts), NULL);
					break;
				default:
					DEBUGP("Don't handle entry %s", sts->name);
			}
		}

		if (sts->entry_type == FT_DIR && 
				(sts->entry_status & FS_CHILD_CHANGED) )
			STOPIF( cm__find_copied(sts), NULL);

		child++;
	}

ex:
	return status;
}


/*
	 bei commit: wenn ein zu kopierendes verzeichnis erreicht wird, quelle der 
	 kopie feststellen, und dessen elemente mit aktuellem zustand vergleichen??

	 oder einfacher, zum zeitpunkt der kopie die vorhandenen einträge als "soll" vermerken.
	 */


/** -.
 * */
int cm__detect(struct estat *root, int argc, char *argv[])
{
	int status, st2;
	char **normalized;
	int i;
	struct cm___simple_match_t *match;
	hash_t hash;


	/* Operate recursively. */
	opt_recursive++;
	/* But do not allow to get current MD5s - we need the data from the 
	 * repository.
	 * TODO? */
	opt_checksum=0;

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	/** \todo Do we really need to load the URLs here? They're needed for 
	 * associating the entries - but maybe we should do that two-way:
	 * - just read \c intnum , and store it again
	 * - or process to <tt>(struct url_t*)</tt>.
	 *
	 * Well, reading the URLs doesn't cost that much ... */
	STOPIF( url__load_list(NULL, 0), NULL);


	for(i=0; i<CM___SIMPLE_MATCH_NUM; i++)
	{
		match=cm___simple_match_array+i;

		DEBUGP("open hash for %s", match->name);

		/* Create database file for WC root. */
		STOPIF( hsh__new(wc_path, match->filename, 
					HASH_TEMPORARY, & match->db), NULL);
	}


	/* We read all entries, and show some progress. */
	status=waa__read_or_build_tree(root, argc, normalized, argv, 
				cm___register_entry, 1);
	if (status == ENOENT)
		STOPIF(status, "!No committed working copy found.");
	STOPIF(status, NULL);


	copydetect_count=0;

	STOPIF( cm__find_copied(root), NULL);

	if (!copydetect_count)
		printf("No copyfrom relations found.\n");
	else if (opt_verbose>0)
		printf("%d copyfrom relation%s found.\n",
				copydetect_count, copydetect_count == 1 ? "" : "s");

ex:
	for(i=0; i<CM___SIMPLE_MATCH_NUM; i++)
	{
		hash=cm___simple_match_array[i].db;
		cm___simple_match_array[i].db=NULL;
		if (hash)
		{
			st2=hsh__close(hash);
			STOPIF_CODE_ERR( st2 && !status, st2, NULL);
		}
	}

	return status;
}


/** Normalizes a path.
 * */ 
int cm___normalize_path(char *path, char **output)
{
	static struct cache_t *cache;
	int status, len;
	char *cp;

	STOPIF( cch__new_cache(&cache, 8), NULL);
	STOPIF( cch__add(cache, 0, NULL, 
				wc_path_len + 1 + strlen(path) + 1, &cp), NULL);
	DEBUGP("norm from: %s", path);
	hlp__pathcopy(cp, &len, path, NULL);
	DEBUGP("norm to: %s", cp);

	*output=cp;

ex:
	return status;
}


/** Checks whether a path is below \c wc_path, and returns the relative 
 * part.
 *
 * If that isn't possible (because \a path is not below \c wc_path),
 * \c EINVAL is returned.
 * The case \c path==wc_path is not allowed, either. */
inline int cm___not_below_wcpath(char *path, char **out)
{
	if (strncmp(path, wc_path, wc_path_len) != 0 ||
			path[wc_path_len] != PATH_SEPARATOR)
		return EINVAL;

	*out=path+wc_path_len+1;
	return 0;
}


/** Wrapper around hsh__store_charp(), that normalizes the paths.
 *
 * We take the same order of arguments - \c key, \c value. */
int cm___store_norm_paths(hash_t db, char *dest, char *src,
		int are_normalized)
{
	int status;
	const char err[]="!The %s path \"%s\" is not below the wc base.";

	if (!are_normalized)
	{
		STOPIF( cm___normalize_path(dest, &dest), NULL);
		STOPIF( cm___normalize_path(src, &src), NULL);
	}

	STOPIF( cm___not_below_wcpath(dest, &dest),
			err, "destination", dest);
	STOPIF( cm___not_below_wcpath(src, &src),
			err, "source", src);

	STOPIF( hsh__store_charp(db, dest, src), NULL);

ex:
	return status;
}


/** Dump a list of copyfrom relations to the stream. 
 *
 * TODO: filter by wildcards (?)
 */
int cm___dump_list(FILE *output, int argc, char *normalized[])
{
	int status;
	hash_t db;
	datum key, value;
	int have;


	/* TODO: Use some filter, eg. by pathnames. */
	db=NULL;

	/* Create database file for WC root. */
	status=hsh__new(wc_path, WAA__COPYFROM_EXT, GDBM_READER, &db);
	if (status==ENOENT)
	{
		status=0;
		goto no_copyfrom;
	}

	have=0;
	status=hsh__first(db, &key);
	while (status == 0)
	{
		STOPIF( hsh__fetch(db, key, &value), NULL);

		/* The . at the end is suppressed; therefore we print it from the 
		 * second dataset onwards.  */
		if (have)
			status=fputs(".\n", output);

		status |= fprintf(output, "%s\n%s\n", value.dptr, key.dptr);
		if (status < 0)
		{
			status=errno;
			if (status == EPIPE)
			{
				status=0;
				break;
			}

			STOPIF(status, "output error");
		}

		status=hsh__next(db, &key, key);
		have++;
	}

	if (!have)
	{
no_copyfrom:
		fprintf(output, "No copyfrom information was written.\n");
	}
	else
		if (opt_verbose>0)
			fprintf(output, "%d copyfrom relation%s.\n", 
					have, have == 1 ? "" : "s");

ex:
	if (db)
		STOPIF( hsh__close(db), NULL);

	return status;
}


/** -.
 * */
int cm__work(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;
	hash_t db;
	int count;
	FILE *input=stdin;
	char *src, *dest, *cp;
	int is_dump, is_load;


	status=0;
	is_load=is_dump=0;
	db=NULL;

/* We have to do the parameter checking in two halfs, because we must not 
 * use "dump" or "load" as working copy path. So we first check what to do, 
 * eventually remove these strings from the parameters, and then look for 
 * the wc base. */
	/* If there's \b no parameter given, we default to dump. */
	if (argc==0) 
		is_dump=1;
	else if (strcmp(argv[0], parm_dump) == 0)
	{
		is_dump=1;
		argv++;
		argc--;
	}
	else if (strcmp(argv[0], parm_load) == 0)
	{
		is_load=1;
		argv++;
		argc--;
	}

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	if (is_dump)
	{
		STOPIF( cm___dump_list(stdout, argc, normalized), NULL);
		/* To avoid the indentation */
		goto ex;
	}

	if (is_load)
	{
		/* Load copyfrom data. */
		STOPIF( hsh__new(wc_path, WAA__COPYFROM_EXT, GDBM_NEWDB, &db), NULL);
		count=0;

		while (1)
		{
			status=hlp__string_from_filep(input, &cp, 0);
			if (status == EOF) 
			{
				status=0;
				break;
			}
			STOPIF( status, "Failed to read copyfrom source");

			STOPIF_CODE_ERR( !*cp, EINVAL, 
					"!Copyfrom source must not be empty.");
			src=strdup(cp);
			STOPIF_ENOMEM(!src);


			status=hlp__string_from_filep(input, &cp, 0);
			STOPIF_CODE_ERR( status == EOF, EINVAL,
					"!Expected a target specification, got EOF!");
			STOPIF( status, "Failed to read copyfrom destination");

			dest=strdup(cp);
			STOPIF_ENOMEM(!dest);


			/* Get the empty line */
			status=hlp__string_from_filep(input, &cp, 1);
			if (status == EOF)
				DEBUGP("delimiter line missing - EOF");
			else if (status == 0 && 
					cp[0] == '.' && cp[1] == 0)
				DEBUGP("delimiter line ok");
			else
			{
				STOPIF(status, "Cannot read delimiter line");
				/* status == 0 ? not empty. */
				STOPIF(EINVAL, "Expected delimiter line - got %s", cp);
			}

			DEBUGP("read %s => %s", src, dest);

			STOPIF( cm___store_norm_paths(db, dest, src, 0), NULL);
			count++;

			free(dest);
			free(src);
		}

		if (opt_verbose>=0)
			printf("%d copyfrom relation%s loaded.\n", count, count==1 ? "" : "s");
	}
	else
	{
		STOPIF_CODE_ERR(argc != 2, EINVAL, 
				"!At least source and destination, "
				"or \"dump\" resp. \"load\" must be given.");

		/* Create database file for WC root. */
		STOPIF( hsh__new(wc_path, WAA__COPYFROM_EXT, GDBM_WRCREAT, &db), NULL);

		/* Set copyfrom relation; the paths are already normalized. */
		STOPIF( hsh__store_charp(db, normalized[1], normalized[0]), NULL);
	}

ex:
	if (db)
	{
		count=hsh__close(db);
		STOPIF_CODE_ERR( count && !status, count, NULL);
	}

	return status;
}


/** -.
 * If \a name is not given, it has to be calculated.
 *
 * All of \a source, \a src_name and \a src_rev are optional; if \a root is 
 * given, it helps the searching performance a bit.
 *
 * If the \a src_name is returned, it must be \c free()ed after use.
 *
 * Uninitializing should be done via calling with \c sts==NULL.
 *
 * */
int cm__get_source(struct estat *sts, char *name,
		struct estat *root,
		struct estat **src_sts, 
		char **src_name, svn_revnum_t *src_rev)
{
	int status;
	static hash_t hash;
	datum key, value;
	struct estat *src;


	status=0;
	value.dptr=NULL;

	if (!sts)
	{
		/* uninit */
		STOPIF( hsh__close(hash), NULL);
		goto ex;
	}

	if (!hash)
	{
		/* init */
		status=hsh__new(wc_path, WAA__COPYFROM_EXT, GDBM_READER, &hash);
		if (status == ENOENT)
			goto ex;

		STOPIF( status, NULL);
	}

	if (src_name) *src_name=NULL;
	if (src_rev) *src_rev=SVN_INVALID_REVNUM;
	if (src_sts) *src_sts=NULL;

	/* Normal proceedings */
	if (!name)
		STOPIF( ops__build_path( &name, sts), NULL);

	if (name[0]=='.' && name[1]==PATH_SEPARATOR)
		name+=2;

	key.dptr=name;
	key.dsize=strlen(name)+1;
	status=hsh__fetch(hash, key, &value);
	if (status) 
	{
		DEBUGP("no source for %s found",
				name);
		goto ex;
	}

	if (!root)
	{
		root=sts;
		while (root->parent) root=root->parent;
	}

	status=ops__traverse( root, value.dptr, 0, 0, &src);
	if (status == ENOENT)
	{
		DEBUGP("Source entry %s for %s doesn't exist; might be stale.",
				value.dptr, name);
	}
	else
	{
		STOPIF( status, NULL);

		if (src_sts) *src_sts=src;
		if (src_rev) *src_rev=src->url->current_rev;
		if (src_name)
		{
			*src_name=strdup(value.dptr);
			STOPIF_ENOMEM(!src_name);
		}

		DEBUGP("source of %s found as %s",
				name, value.dptr);
	}

ex:
	IF_FREE( value.dptr );

	return status;
}


