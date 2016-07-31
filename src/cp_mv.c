/************************************************************************
 * Copyright (C) 2007-2008 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>
#include <search.h>

#include "global.h"
#include "cp_mv.h"
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
 * Please note that this command works \b always on a directory \b 
 * structure - if you say to copy a directory, the \b whole structure is 
 * marked as copy. That means that if some entries below the copy are 
 * missing, they are reported as removed from the copy on the next commit.
 * \n (Of course it is possible to mark files as copied, too; non-recursive 
 * copies are not possible.)
 *
 * \note Or TODO: There will be differences in the exact usage - \c copy 
 * will try to run the \c cp command, whereas \c copied will just remember 
 * the relation.
 *
 * If this command are used without parameters, the currently defined
 * relations are printed; please keep in mind that the \b key is the 
 * destination name, ie. the 2nd line of each pair!
 * 
 * The input format for \c load is newline-separated - first a \c SRC line, 
 * followed by a \c DEST line, then an line with just a dot (<tt>"."</tt>) 
 * as delimiter.  If you've got filenames with newlines or other special 
 * characters, you have to give the paths as arguments. 
 *
 * Internally the paths are stored relative to the working copy base 
 * directory, and they're printed that way, too.
 *
 * Later definitions are \b appended to the internal database; to undo 
 * mistakes, use the \ref revert action.
 *
 * \note <b>Important:</b> User-defined properties like \ref 
 * FSVS_PROP_COMMIT_PIPE "fsvs:commit-pipe" are \b not copied to the 
 * destinations, because of space/time issues (traversing through entire 
 * subtrees, copying a lot of property-files) and because it's not sure 
 * that this is really wanted. \b TODO: option for copying properties?
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
 * \todo Filter for dump (patterns?). */


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
 * <TR><td>\e name<td>
 * The entry has the same name as another entry.
 *
 * <TR><td>\e manber<td>
 * Analysing files of similar size shows some percentage of 
 * (variable-sized) <b>common blocks</b> (ignoring the order of the 
 * blocks).
 *
 * <TR><td>\e dirlist<td>
 * The new directory has similar files to the old directory.\n
 * The percentage is (number_of_common_entries)/(files_in_dir1 + 
 * files_in_dir2 - number_of_common_entries).
 *
 * </table>
 *
 * \note Only \b md5, \b name and \b inode matching currently done.
 *
 * \note If too many possible matches are found, not all may be printed; 
 * only the indicator <tt>...</tt> is shown at the end.
 * 
 * */
/* Or should for dirlist just the raw data be showed - common_files, 
 * files_in_new_dir? */

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
 * 
 *
 * update_dir() does directories shallowly ...
 * Change that?
 *
 * Thinking a bit about copies ...
 *
 * There are two ways to proceed here:
 * - Look what's actually here, and correlate with the entries that should 
 *   be here
 *   Adv: If no copy registered, just no correlation needed.
 * - First copy the tree, and let the update_dir logic sort it out later.
 *   Disadv: if no copy registered, still needs waa__build_tree - that gets 
 *   used less and less, and could get worse (in terms of code quality)
 *   Disadv: if some entry already added (because of some ignore patterns) 
 *   we have to take care to \b not overwrite it.
 *     - Would be easy without added entries - new directory? copy, and 
 *     look what's actually here.
 *     - Now we have to look for already given entries ...
 *
 * copy says A, B, C
 * dir_enum says A, B, D, E
 * added entry (get read \b later in waa__input_tree()) says B, F
 * would have to correlate \b 3 lists
 * at least update_dir is called after all \b expected entries, that 
 * includes added - so at least these are already present.
 *
 * But sub-directories are not yet finished! TODO
 *
 *
 * 2nd way looks easier and cleaner ...
 *
 * usecase: dir or file copied, registered with fsvs.
 * only new entries get checked - fast
 *
 * -----------------------------------------------------------------------
 * Facts:
 * - we have to copy directory structures *after* waa__input_tree(), but 
 *   before waa__partial_update().
 *   - While running waa__input_tree() the source tree might still be in 
 *   work
 *   - While running waa__partial_update() the "old" data of the copied 
 *   entries might already be destroyed
 *   - But here we don't know the new entries yet!
 * - We have to do *all* entries there
 *   - As we do not yet know which part of the tree we'll be working with
 * - We must not save these temporary entries
 *   - Either mark FT_IGNORE
 *   - Or don't load the copies for actions calling waa__output_tree().
 *   - But what about the property functions? They need access to copied 
 *   entries.
 *     - Can do the entries as RF_ADD, as now.
 *   
 * */

/* As the temporary databases (just indizes for detection) are good only 
 * for a single run, we can easily store the address directly. For the real 
 * copy-from db we have to use the path.
 *
 * Temporary storage:
 *   value.dptr=sts;
 *   value.dsize=sizeof(sts);
 * Persistent:
 *   value.dptr=path;
 *   value.dsize=sts->path_len;
 *
 * I don't think we want to keep these files up-to-date ... would mean 
 * constant runtime overhead. */


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


/** Structure for candidate retrieval. */
struct cm___candidate_t {
	/** Candidate entry */
	struct estat *sts;
	/** Bitmask to tell which matching criteria apply. */
	int matches_where;
	/** Count, or for percent display, a number from 0 to 1000. */
	int match_count;
};


/** Function and type declarations for entry-to-hash-key conversions.
 * @{ */
typedef datum (cm___to_datum_t)(const struct estat *sts);
cm___to_datum_t cm___md5_datum;
cm___to_datum_t cm___inode_datum;
cm___to_datum_t cm___name_datum;
/** @} */


/** Structure for storing simple ways of file matching.
 *
 * This is the predeclaration. */
struct cm___match_t;

/** Enters the given entry into the database */
typedef int (cm___register_fn)(struct estat *, struct cm___match_t *);
/** Queries the database for the given entry.
 *
 * Output is (the address of) an array of \ref candidates, and the number 
 * of elements. */
typedef int (cm___get_list_fn)(struct estat *, struct cm___match_t *,
		struct cm___candidate_t **, int *count);
/** Format function for verbose output.
 * Formats the candidate \a cand in match \a match into a buffer, and 
 * returns this buffer. */
typedef char* (cm___format_fn)(struct cm___match_t * match,
		struct cm___candidate_t *cand);


/** Inserts into hash tables. */
cm___register_fn cm___hash_register;
/** Queries the database for the given entry. */
cm___get_list_fn cm___hash_list;
/** Match directories by their children. */
cm___get_list_fn cm___match_children;
/** Outputs percent of match. */
cm___format_fn cm___output_pct;


/** -. */
struct cm___match_t {
/** Name for this way of matching */
	char name[8];
	/** Which entry types are allowed? */
	int entry_types;

	/** Callback function for inserting elements */
	cm___register_fn *insert;
	/** Callback function for querying elements */
	cm___get_list_fn *get_list;
	/** Callback function to format the amount of similarity. */
	cm___format_fn *format;

	/** For simple, GDBM-based database matches */
	/** How to get a key from an entry */
	cm___to_datum_t *to_key;
	/** Filename for this database. */
	char filename[8];
	/** Database handle */
	hash_t db;
	/** Last queried key.
	 * Needed if a single get_list call isn't sufficient (TODO). */
	datum key;
};

/** Enumeration for (some) matching criteria */
enum cm___match_e {
	CM___NAME_F=0,
	CM___NAME_D,
	CM___DIRLIST,
};

/** Array with ways for simple matches.
 *
 * We keep file and directory matching separated; a file cannot be the 
 * copyfrom source of a directory, and vice-versa.
 *
 * The \e important match types are at the start, as they're directly 
 * accessed, too.  */
struct cm___match_t cm___match_array[]=
{
	[CM___NAME_F] = { .name="name", .to_key=cm___name_datum, 
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_types=FT_FILE,  .filename=WAA__FILE_NAME_EXT},
	[CM___NAME_D] = { .name="name", .to_key=cm___name_datum, 
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_types=FT_DIR,  .filename=WAA__DIR_NAME_EXT},
	[CM___DIRLIST] = { .name="dirlist", 
		.get_list=cm___match_children, .format=cm___output_pct,
		.entry_types=FT_DIR, },
	{ .name="md5", .to_key=cm___md5_datum, 
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_types=FT_FILE, .filename=WAA__FILE_MD5s_EXT},
	{ .name="inode", .to_key=cm___inode_datum, 
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_types=FT_FILE,  .filename=WAA__FILE_INODE_EXT},
	{ .name="inode", .to_key=cm___inode_datum, 
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_types=FT_DIR,  .filename=WAA__DIR_INODE_EXT},
};
#define CM___MATCH_NUM (sizeof(cm___match_array)/sizeof(cm___match_array[0]))


/** Gets a \a datum from a struct estat::md5. */
datum cm___md5_datum(const struct estat *sts)
{
	datum d;

	d.dsize=APR_MD5_DIGESTSIZE*2+1;
	d.dptr=cs__md52hex(sts->md5);
	return d;
}


/** Gets a \a datum from the name of an entry; the \\0 gets included (for 
 * easier dumping).  */
datum cm___name_datum(const struct estat *sts)
{
	datum d;

	d.dptr=sts->name;
	/* We have only the full path length stored. */
	d.dsize=strlen(d.dptr)+1;
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


/** Compare function for cm___candidate_t. */
static int cm___cand_compare(const void *_a, const void *_b)
{
	const struct cm___candidate_t *a=_a;
	const struct cm___candidate_t *b=_b;
	return a->sts - b->sts;
}


/** Compare function for cm___candidate_t. */
static int cm___cand_comp_count(const void *_a, const void *_b)
{
	const struct cm___candidate_t *a=_a;
	const struct cm___candidate_t *b=_b;
	return a->match_count - b->match_count;
}


/** -. */
int cm___hash_register(struct estat *sts, struct cm___match_t *match)
{
	int status;


	status=hsh__insert_pointer( match->db, 
			(match->to_key)(sts), sts);

	/* If there is no more space available ... just ignore it. */
	if (status == EFBIG)
		status=0;
	return status;
}


/** -.
 * 
 * The big question is - should this work recursively? Would mean that the 
 * topmost directory would be descended, and the results had to be cached.  
 * */
int cm___match_children(struct estat *sts, struct cm___match_t *match,
		struct cm___candidate_t **list, int *found)
{
	int status;
	/* We take a fair bit more, to get *all* (or at least most) possible 
	 * matches. */
	static struct cm___candidate_t similar_dirs[MAX_DUPL_ENTRIES*4];
	struct cm___candidate_t *cur, tmp_cand={0};
	size_t simil_dir_count;
	int common;
	struct estat **children;
	struct estat **others, *other_dir;
	int other_count, i;
	datum key;
	struct cm___match_t *name_match;


	status=0;
	DEBUGP("child matching for %s", sts->name);

	/* No children => cannot be matched */
	if (!sts->entry_count) goto ex;

	simil_dir_count=0;

	children=sts->by_inode;
	while (*children)
	{
		/* Find entries with the same name. Depending on the type of the entry  
		 * we have to look in one of the two hashes. */
		if ((*children)->entry_type == FT_DIR)
			name_match=cm___match_array+CM___NAME_D;
		else if ((*children)->entry_type == FT_FILE)
			name_match=cm___match_array+CM___NAME_F;
		else goto next_child;


		key=(name_match->to_key)(*children);
		status=hsh__list_get(name_match->db, key, &key, &others, &other_count);


		/* If there are too many entries with the same name, we ignore this 
		 * hint.  */
		if (status != ENOENT && other_count && 
				other_count<MAX_DUPL_ENTRIES)
		{
			for(i=0; i<other_count; i++)
			{
			/* Now we don't take the entry with the same name, but it's parent.  
			 * As the root entry hopefully never matches we don't need to check 
			 * for NULL.
			 * */

				tmp_cand.sts=others[i]->parent;

				cur=lsearch(&tmp_cand, similar_dirs, &simil_dir_count, 
						sizeof(similar_dirs[0]), cm___cand_compare);
				cur->match_count++;
				DEBUGP("dir %s has count %d", cur->sts->name, cur->match_count);

				BUG_ON(simil_dir_count > sizeof(similar_dirs)/sizeof(similar_dirs[0]));
			}
		}

next_child:
		children++;
	}

	/* Now do the comparisions. */
	for(i=0; i<simil_dir_count; i++)
	{
		common=0;
		other_dir=similar_dirs[i].sts;

		int both(struct estat *a, struct estat*b) { common++; return 0; }
		STOPIF( ops__correlate_dirs(sts, other_dir,
					NULL, both, NULL, NULL), NULL);

		/* For directories with more than 2**32/1000 (~4M) *common* entries we 
		 * get an overflow here. But I think that's not important currently :-) 
		 * */
		similar_dirs[i].match_count = 
			1000*common/(sts->entry_count + other_dir->entry_count - common);
	}

	/* Now sort, and return a few. */
	qsort( similar_dirs, simil_dir_count, sizeof(similar_dirs[0]), 
			cm___cand_comp_count);

	*found=simil_dir_count > HASH__LIST_MAX ? HASH__LIST_MAX : simil_dir_count;
	*list=similar_dirs;

ex:
	return status;
}


/** -.
 * */
int cm___hash_list(struct estat *sts, struct cm___match_t *match,
		struct cm___candidate_t **output, int *found)
{
	int status;
	static struct cm___candidate_t arr[MAX_DUPL_ENTRIES];
	struct estat **list;
	int i;

	match->key=(match->to_key)(sts);
	status=hsh__list_get(match->db, match->key, &match->key, &list, found);

	if (status == 0)
	{
		for(i=0; i<*found; i++)
		{
			/** The other members are touched by upper layers, so we have to 
			 * re-initialize them. */
			memset(arr+i, 0, sizeof(*arr));
			arr[i].sts=list[i];
		}
		*output=arr;
	}

	return status;
}


/** Puts cm___candidate_t::match_count formatted into \a buffer. */
char* cm___output_pct(struct cm___match_t *match, 
		struct cm___candidate_t *cand)
{
	static char buffer[8];

	BUG_ON(cand->match_count > 1000 || cand->match_count < 0);

	sprintf(buffer, "=%d.%1d%%",
			cand->match_count/10, cand->match_count % 10);

	return buffer;
}


/** Inserts the given entry in all allowed matching databases. */
int cm___register_entry(struct estat *sts, char *path)
{
	int status;
	int i;
	struct cm___match_t *match;


	status=0;
	if (!(sts->entry_status & FS_NEW))
	{
		for(i=0; i<CM___MATCH_NUM; i++)
		{
			match=cm___match_array+i;
			if ((sts->entry_type & match->entry_types) &&
					match->insert)
			{
				STOPIF( (match->insert)(sts, match), NULL);
				DEBUGP("inserted %s for %s", sts->name, match->name);
			}
		}
	}

ex:
	return status;
}


/** Shows possible copyfrom sources for the given entry.
 * */
static int cm___match(struct estat *entry)
{	
	int status;
	char *path, *formatted;
	int i, count, have_match, j, overflows;
	struct estat *sts;
	struct cm___match_t *match;
	struct cm___candidate_t candidates[HASH__LIST_MAX*CM___MATCH_NUM];
	struct cm___candidate_t *cur, *list;
	size_t candidate_count;
	int output_error;
	FILE *output=stdout;


	/* #error doesn't work with sizeof() ?
	 * But doesn't matter much, this gets removed by the compiler. */
	BUG_ON(sizeof(candidates[0].matches_where) *4 < CM___MATCH_NUM,
			"Wrong datatype chosen for matches_where.");


	formatted=NULL;
	status=0;
	output_error=0;
	candidate_count=0;
	overflows=0;
	path=NULL;

	/* Down below status will get the value ENOENT from the hsh__list_get() 
	 * lookups; we change it back to 0 shortly before leaving. */

	for(i=0; i<CM___MATCH_NUM; i++)
	{
		match=cm___match_array+i;

		/* Avoid false positives. */
		if (!(entry->entry_type & match->entry_types))
			continue;

		/* \todo Loop if too many for a single call. */
		status=match->get_list(entry, match, &list, &count);

		/* ENOENT = nothing to see */
		if (status == ENOENT) continue;

		STOPIF(status, NULL);

		if (count > MAX_DUPL_ENTRIES)
		{
			/* We show one less than we store, so we have the overflow 
			 * information.  */
			overflows++;
			count=MAX_DUPL_ENTRIES;
		}

		for(j=0; j<count; j++)
		{
			/* We could do insertion sort into the candidate array here; 
			 * bsearch() sadly only finds identical, and lsearch() does unsorted 
			 * arrays only.
			 *
			 * We just use lsearch() - that keeps code size small. */
			cur=lsearch(list+j, candidates, &candidate_count, 
					sizeof(candidates[0]), cm___cand_compare);

			BUG_ON(candidate_count > sizeof(candidates)/sizeof(candidates[0]));

			cur->matches_where |= 1 << i;

			/* Copy dirlist value */
			if (i == CM___DIRLIST)
				cur->match_count=list[j].match_count;

			DEBUGP("got %s for %s => 0x%X",
					cur->sts->name, match->name, cur->matches_where);
		}
	}

	status=0;

	if (candidate_count)
	{
		copydetect_count++;

		STOPIF( ops__build_path(&path, entry), NULL);
		STOPIF( hlp__format_path(entry, path, &formatted), NULL);

		/* Print header line for this file. */
		output_error |= fprintf(output, "%s\n", formatted);

		/* Output list of matches */
		for(j=0; j<candidate_count; j++)
		{
			sts=candidates[j].sts;
			have_match=0;

			output_error |= fputs("  ", output);

			for(i=0; i<CM___MATCH_NUM; i++)
			{
				if (candidates[j].matches_where & (1 << i))
				{
					match=cm___match_array+i;

					if (have_match)
						output_error |= fputs(",", output);
					have_match=1;

					output_error |= fputs(match->name, output);
					if (opt_verbose && match->format)
						output_error |= 
							fputs( match->format(match, candidates+j), output);
				}
			}

			STOPIF( ops__build_path(&path, sts), NULL);
			STOPIF( hlp__format_path(sts, path, &formatted), NULL);
			output_error |= fprintf(output, ":%s\n", formatted);
		}


		if (overflows)
			output_error |= fputs("  ...\n", output);
	}
	else
	{
		/* cache might be overwritten again when we're here. */
		STOPIF( ops__build_path(&path, entry), NULL);

		if (opt_verbose>0)
		{
			STOPIF( hlp__format_path(entry, path, &formatted), NULL);
			output_error |= fprintf(output, "- No copyfrom relation found for %s\n", formatted);
		}
		else
			DEBUGP("No sources found for %s", path);
	}

	output_error |= fflush(output);

	/* Could be something else ... but we can't write data, so we quit. */
	if (output_error<0)
		status=EPIPE;

ex:
	return status;
}


int cm__find_dir_source(struct estat *dir)
{
	int status;
	status=0;

	STOPIF( cm___match( dir ), NULL);

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

	STOPIF( cm___match( file ), NULL);

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


/** -.
 * */
int cm__detect(struct estat *root, int argc, char *argv[])
{
	int status, st2;
	char **normalized;
	int i;
	struct cm___match_t *match;
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


	for(i=0; i<CM___MATCH_NUM; i++)
	{
		match=cm___match_array+i;
		if (!match->filename[0]) continue;

		DEBUGP("open hash for %s as %s", match->name, match->filename);

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
	for(i=0; i<CM___MATCH_NUM; i++)
	{
		hash=cm___match_array[i].db;
		cm___match_array[i].db=NULL;
		if (hash)
		{
			st2=hsh__close(hash, status);
			STOPIF_CODE_ERR( st2 && !status, st2, NULL);
		}
	}

	return status;
}


/** Returns the absolute path
 * */ 
int cm___absolute_path(char *path, char **output)
{
	static struct cache_t *cache;
	int status, len;
	char *cp;

	STOPIF( cch__new_cache(&cache, 8), NULL);
	STOPIF( cch__add(cache, 0, NULL, 
				//				wc_path_len + 1 + strlen(path) + 1, &cp), NULL);
		start_path_len + 1 + strlen(path) + 1, &cp), NULL);
	DEBUGP("norm from: %s", path);
	hlp__pathcopy(cp, &len, path, NULL);
	DEBUGP("norm to: %s", cp);

	BUG_ON(len > cache->entries[cache->lru]->len);

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
		IF_FREE(value.dptr);

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

		status=hsh__next(db, &key, &key);
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
		STOPIF( hsh__close(db, status), NULL);

	return status;
}


/** Make the copy in the tree started at \a root.
 *
 * The destination must not already exist in the tree; it can exist in the 
 * filesystem.
 *
 * Uninitialization is done via \c root==NULL.
 *
 * If the flag \a paths_are_wc_relative is set, the paths \a cp_src and \a 
 * cp_dest are taken as-is.
 * Else they're are converted to wc-relative paths by making them absolute 
 * (eventually using \ref start_path as anchor), and cutting the wc-root 
 * off. */
int cm___make_copy(struct estat *root, 
		char *cp_src, char *cp_dest, 
		int paths_are_wc_relative)
{
	int status;
	const char err[]="!The %s path \"%s\" is not below the wc base.";
	struct estat *src, *dest;
	static hash_t db=NULL;
	char *abs_src, *abs_dest;
	char *wc_src, *wc_dest;


	if (!root)
	{
		STOPIF( hsh__close(db, 0), NULL);
		goto ex;
	}

	/* That we shuffle the characters back and forth can be excused because
	 * - either we are cmdline triggered, in which case we have the full task 
	 *   starting overhead, and don't do it here again, and
	 * - if we're doing a list of entries, we have to do it at least here. */

	if (paths_are_wc_relative)
	{
		wc_dest=cp_dest;
		wc_src=cp_src;
	}
	else
	{
		STOPIF( cm___absolute_path(cp_dest, &abs_dest), NULL);
		STOPIF( cm___absolute_path(cp_src, &abs_src), NULL);

		STOPIF( cm___not_below_wcpath(abs_dest, &wc_dest),
				err, "destination", abs_dest);
		STOPIF( cm___not_below_wcpath(abs_src, &wc_src),
				err, "source", abs_src);
	}


	STOPIF( ops__traverse(root, cp_src, 0, 0, &src), NULL);
	/* The directories above must be added; the entries below get RF_COPY_SUB 
	 * set (by waa__copy_entries), and this entry gets overridden to 
	 * RF_COPY_BASE below.  */
	STOPIF( ops__traverse(root, cp_dest, 
				OPS__CREATE, RF_ADD, 
				&dest), NULL);

	STOPIF_CODE_ERR( !(dest->flags & RF_ISNEW), EINVAL,
			"!The destination is already known - must be a new entry.");

	if (!db)
		STOPIF( hsh__new(wc_path, WAA__COPYFROM_EXT, GDBM_WRCREAT, &db), NULL);

	STOPIF( waa__copy_entries(src, dest), NULL);

	/* Mark as base entry for copy; the RF_ADD flag was removed by 
	 * copy_entries above, but the entry really is *new*. */
	dest->flags |= RF_COPY_BASE;
	dest->flags &= ~RF_COPY_SUB;

	STOPIF( hsh__store_charp(db, wc_dest, wc_src), NULL);


ex:
	return status;
}


/** -.
 * */
int cm__work(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;
	int count;
	FILE *input=stdin;
	char *src, *dest, *cp;
	int is_dump, is_load;


	status=0;
	is_load=is_dump=0;

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


	STOPIF( url__load_nonempty_list(NULL, 0), NULL);

	/* Load the current data, without updating */
	status=waa__input_tree(root, NULL, NULL);
	if (status == ENOENT)
		STOPIF(status, "!No data about current entries is available.");
	STOPIF(status, NULL);

	if (is_load)
	{
		/* Load copyfrom data. */
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
			/* These paths were given relative to the cwd, which is changed now, as 
			 * we're in the wc base. Calculate correct names. */

			STOPIF( cm___make_copy(root, src, dest, 0), NULL);
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
		STOPIF( cm___make_copy(root, normalized[0], normalized[1], 1), NULL);
	}

	STOPIF( cm___make_copy(NULL, NULL, NULL, 0), NULL);
	STOPIF( waa__output_tree(root), NULL);

ex:
	return status;
}


/** -.
 * If \a name is not given, it has to be calculated.
 *
 * All of \a source, \a src_name and \a src_rev are optional; if \a root is 
 * given, it helps the searching performance a bit.\n
 * These are always set; if no source is defined, they're set to \c NULL, 
 * \c NULL and \c SVN_INVALID_REVNUM.
 *
 * If the \a src_name is returned, it must be \c free()ed after use.
 *
 * Uninitializing should be done via calling with \c sts==NULL; in this 
 * case the \a register_for_cleanup value is used as success flag.
 *
 * \a sts->copyfrom_src is always set; if no copyfrom source was found, it 
 * get set to \c NULL.
 *
 * If the entry \a sts has \c RF_COPY_BASE set, its copyfrom source is 
 * returned; if \c RF_COPY_SUB is set, the copyfrom URL is reconstructed 
 * via the parent with \c RF_COPY_BASE set; if neither is set, \c ENOENT is 
 * returned.
 * */
int cm__get_source(struct estat *sts, char *name,
		struct estat *root,
		struct estat **src_sts, 
		char **src_name, svn_revnum_t *src_rev,
		int register_for_cleanup)
{
	int status;
	static hash_t hash;
	static int init;
	datum key, value;
	struct estat *src;
	char *source_path;


	status=0;
	src=NULL;
	value.dptr=NULL;
	source_path=NULL;

	if (!sts)
	{
		/* uninit */
		STOPIF( hsh__close(hash, register_for_cleanup), NULL);
		hash=NULL;
		init=0;
		goto ex;
	}

	if (!init)
	{
		/* We cannot simply use !hash as condition; if there is no database with 
		 * copyfrom information, we'd try to open it *every* time we're asked for 
		 * a source, which is clearly not optimal for performance.
		 * So we use an static integer. */
		init=1;

		/* In case there's a cleanup at the end we have to open read/write. */
		status=hsh__new(wc_path, WAA__COPYFROM_EXT, 
				GDBM_WRITER | HASH_REMEMBER_FILENAME, &hash);
		/* If we got an ENOENT here, hash==NULL; so we'll re-set the 
		 * *parameters below and return. */
		if (status != ENOENT)
			STOPIF( status, NULL);
	}

	if (src_name) *src_name=NULL;
	/* svn+ssh has an assertion -- 0 is invalid, SVN_INVALID_REVNUM must be 
	 * used. */
	if (src_rev) *src_rev=SVN_INVALID_REVNUM;
	if (src_sts) *src_sts=NULL;

	if (!hash)
	{
		DEBUGP("No database found");
		/* No database open => no database found. */
		status=ENOENT;
		goto ex;
	}


	if (!(sts->flags & (RF_COPY_BASE | RF_COPY_SUB)))
	{
		DEBUGP("No copy bit set");
		status=ENOENT;
	}
	else if (sts->flags & RF_COPY_SUB)
	{
		/* There are two strategies:
		 * - Look which parent is the copy source, get its URL, and append the 
		 *   path after that to the URL of the copy source:
		 *     root / dir1 / dir2 / <copied_dest> / dir3 / <sts>
		 *   and
		 *     root / dir1 / dir3 / <src>
		 *   Disadvantage: estat::copyfrom_src wouldn't be set; we'd have to 
		 *   traverse the entries, and make the estat::by_name arrays for that.
		 *   As estat::copyfrom_src is defined as output parameter, that 
		 *   doesn't quite work.
		 * - Current implementation: Recurse the parents up, and build the 
		 *   paths down. */

		/* We go for root once, to avoid doing that over and over again. */
		if (!root)
		{
			root=sts;
			while (root->parent) root=root->parent;
		}

		/* Get source of parent. */
		status=cm__get_source(sts->parent, NULL, root, 
				NULL, NULL, src_rev, register_for_cleanup);

		if (status)
		{
			/* That's a bug ... the bit is set, but no source was found?
			 * Could some stale entry cause that? Don't error out now; perhaps at a 
			 * later time. */
			DEBUGP("bit set, no source!");
			/* BUG_ON(1,"bit set, no source!"); */
			goto ex;
		}

		/* Now we know the copyfrom source of the parent; we look for the entry  
		 * with the same name below there.  */
		STOPIF( ops__traverse(sts->parent->copyfrom_src, sts->name, 
					0, 0, &src), NULL);
		sts->copyfrom_src=src;
		/* The src_rev of a copy_sub is the same as of the copied parent, and 
		 * that was already set by the recursive calls.  */
		if (src_sts) *src_sts=src;
		if (src_name)
			STOPIF( ops__build_path(&source_path, src), NULL);
	}
	else
	{
		/* Normal proceedings, this is a direct target of a copy definition. */
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

		if (register_for_cleanup)
			STOPIF( hsh__register_delete(hash, key), NULL);

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
			goto ex;
		}
		STOPIF( status, NULL);


		if (sts)
			sts->copyfrom_src=src;

		if (src_rev) *src_rev=src->url->current_rev;
		if (src_sts) *src_sts=src;

		source_path=value.dptr;
	}

	if (src_name)
	{
		BUG_ON(!source_path);

		*src_name=strdup(source_path);
		STOPIF_ENOMEM(!src_name);
	}

	DEBUGP("source of %s found as %s",
			name, value.dptr);

ex:
	IF_FREE( value.dptr );

	return status;
}


