/************************************************************************
 * Copyright (C) 2007-2009 Philipp Marek.
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
#include "options.h"
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
 * fsvs cp [-r rev] SRC DEST
 * fsvs cp dump
 * fsvs cp load
 * \endcode
 *
 * The \c copy command marks \c DEST as a copy of \c SRC at revision \c 
 * rev, so that on the next commit of \c DEST the corresponding source path 
 * is sent as copy source.
 *
 * The default value for \c rev is \c BASE, ie. the revision the \c SRC 
 * (locally) is at.
 *
 * Please note that this command works \b always on a directory \b 
 * structure - if you say to copy a directory, the \b whole structure is 
 * marked as copy. That means that if some entries below the copy are 
 * missing, they are reported as removed from the copy on the next commit.
 * \n (Of course it is possible to mark files as copied, too; non-recursive 
 * copies are not possible, but can be emulated by having parts of the 
 * destination tree removed.)
 *
 * \note TODO: There will be differences in the exact usage - \c copy will 
 * try to run the \c cp command, whereas \c copied will just remember the 
 * relation.
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
 * mistakes, use the \ref uncopy action.
 *
 * \note <b>Important:</b> User-defined properties like \ref 
 * FSVS_PROP_COMMIT_PIPE "fsvs:commit-pipe" are \b not copied to the 
 * destinations, because of space/time issues (traversing through entire 
 * subtrees, copying a lot of property-files) and because it's not sure 
 * that this is really wanted. \b TODO: option for copying properties?
 *
 * \todo -0 like for xargs?
 *
 * \todo Are different revision numbers for \c load necessary? Should \c 
 * dump print the source revision number?
 *
 * \todo Copying from URLs means update from there
 *
 * \note As subversion currently treats a rename as copy+delete, the \ref 
 * mv command is an alias to \ref cp.
 *
 * If you have a need to give the filenames \c dump or \c load as first 
 * parameter for copyfrom relations, give some path, too, as in 
 * <tt>"./dump"</tt>.
 *
 * \note The source is internally stored as URL with revision number, so 
 * that operations like these \code
 *   $ fsvs cp a b
 *   $ rm a/1
 *   $ fsvs ci a
 *   $ fsvs ci b
 * \endcode
 * work - FSVS sends the old (too recent!) revision number as source, and 
 * so the local filelist stays consistent with the repository. \n But it is 
 * not implemented (yet) to give an URL as copyfrom source directly - we'd 
 * have to fetch a list of entries (and possibly the data!) from the 
 * repository.
 *
 * \todo Filter for dump (patterns?).
 * */


/**
 * \addtogroup cmds
 *
 * \section cpfd copyfrom-detect
 *
 * \code
 * fsvs copyfrom-detect [paths...]
 * \endcode
 *
 * This command tells FSVS to look through the new entries, and see 
 * whether it can find some that seem to be copied from others already 
 * known. \n
 * It will output a list with source and destination path and why it could 
 * match. 
 *
 * This is just for information purposes and doesn't change any FSVS state, 
 * (TODO: unless some option/parameter is set).</i>
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
 * \note \b manber matching is not implemented yet.
 *
 * \note If too many possible matches for an entry are found, not all are 
 * printed; only an indicator <tt>...</tt> is shown at the end.
 * 
 * */

/**
 * \addtogroup cmds
 *
 * \section uncp
 *
 * \code
 * fsvs uncopy DEST [DEST ...]
 * \endcode
 *
 * The \c uncopy command removes a \c copyfrom mark from the destination 
 * entry. This will make the entry unknown again, and reported as \c New on 
 * the next invocations.
 *
 * Only the base of a copy can be un-copied; if a directory structure was 
 * copied, and the given entry is just implicitly copied, this command will 
 * return an error.
 *
 * This is not folded in \ref revert, because it's not clear whether \c 
 * revert on copied, changed entries should restore the original copyfrom 
 * data or remove the copy attribute; by using another command this is no 
 * longer ambiguous. 
 *
 * Example:
 * \code
 *   $ fsvs copy SourceFile DestFile
 *   # Whoops, was wrong!
 *   $ fsvs uncopy DestFile
 * \endcode
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
 * So a "copy" does copy all entries for the list, too; but that means that 
 * a bit more data has to be written out.
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
 * Output is (the address of) an array of cm___candidates_t, and the number 
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
	/** Which entry type is allowed? */
	mode_t entry_type;
	/** Whether this can be avoided by an option. */
	int is_expensive:1;
	/** Whether this match is allowed. */
	int is_enabled:1;

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
		.entry_type=S_IFREG,  .filename=WAA__FILE_NAME_EXT},
	[CM___NAME_D] = { .name="name", .to_key=cm___name_datum, 
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_type=S_IFDIR,  .filename=WAA__DIR_NAME_EXT},

	[CM___DIRLIST] = { .name="dirlist", 
		.get_list=cm___match_children, .format=cm___output_pct,
		.entry_type=S_IFDIR, },

	{ .name="md5", .to_key=cm___md5_datum, .is_expensive=1,
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_type=S_IFREG, .filename=WAA__FILE_MD5s_EXT},

	{ .name="inode", .to_key=cm___inode_datum, 
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_type=S_IFDIR, .filename=WAA__FILE_INODE_EXT},
	{ .name="inode", .to_key=cm___inode_datum, 
		.insert=cm___hash_register, .get_list=cm___hash_list,
		.entry_type=S_IFREG, .filename=WAA__DIR_INODE_EXT},
};
#define CM___MATCH_NUM (sizeof(cm___match_array)/sizeof(cm___match_array[0]))


/** Gets a \a datum from a struct estat::md5. */
datum cm___md5_datum(const struct estat *sts)
{
	datum d;

	d.dsize=APR_MD5_DIGESTSIZE*2+1;
	d.dptr=cs__md5tohex_buffered(sts->md5);
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



static int common;
int both(struct estat *a, struct estat *b) { common++; return 0; }


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
	struct estat **children, *curr;
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
		curr=*children;
		/* Find entries with the same name. Depending on the type of the entry  
		 * we have to look in one of the two hashes. */
		if (S_ISDIR(curr->st.mode))
			name_match=cm___match_array+CM___NAME_D;
		else if (S_ISREG(curr->st.mode))
			name_match=cm___match_array+CM___NAME_F;
		else goto next_child;


		key=(name_match->to_key)(curr);
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
int cm___register_entry(struct estat *sts)
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
			/* We need the original value (st.mode). estat::local_mode_packed 
			 * would be 0 for a deleted node. */
			if (match->is_enabled && match->insert &&
					(sts->st.mode & S_IFMT) == match->entry_type )
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
	FILE *output=stdout;


	/* #error doesn't work with sizeof() ?
	 * But doesn't matter much, this gets removed by the compiler. */
	BUG_ON(sizeof(candidates[0].matches_where) *4 < CM___MATCH_NUM,
			"Wrong datatype chosen for matches_where.");


	formatted=NULL;
	status=0;
	candidate_count=0;
	overflows=0;
	path=NULL;

	/* Down below status will get the value ENOENT from the hsh__list_get() 
	 * lookups; we change it back to 0 shortly before leaving. */

	for(i=0; i<CM___MATCH_NUM; i++)
	{
		match=cm___match_array+i;

		/* Avoid false positives. */
		if ((entry->st.mode & S_IFMT) != match->entry_type)
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
		STOPIF_CODE_EPIPE( fprintf(output, "%s\n", formatted), NULL);

		/* Output list of matches */
		for(j=0; j<candidate_count; j++)
		{
			sts=candidates[j].sts;
			have_match=0;

			STOPIF_CODE_EPIPE( fputs("  ", output), NULL);

			for(i=0; i<CM___MATCH_NUM; i++)
			{
				if (candidates[j].matches_where & (1 << i))
				{
					match=cm___match_array+i;

					if (have_match)
						STOPIF_CODE_EPIPE( fputs(",", output), NULL);
					have_match=1;

					STOPIF_CODE_EPIPE( fputs(match->name, output), NULL);
					if (opt__is_verbose()>0 && match->format)
						STOPIF_CODE_EPIPE( 
								fputs( match->format(match, candidates+j), output), 
								NULL);
				}
			}

			STOPIF( ops__build_path(&path, sts), NULL);
			STOPIF( hlp__format_path(sts, path, &formatted), NULL);
			STOPIF_CODE_EPIPE( fprintf(output, ":%s\n", formatted), NULL);
		}


		if (overflows)
			STOPIF_CODE_EPIPE( fputs("  ...\n", output), NULL);
	}
	else
	{
		/* cache might be overwritten again when we're here. */
		STOPIF( ops__build_path(&path, entry), NULL);

		if (opt__is_verbose() > 0)
		{
			STOPIF( hlp__format_path(entry, path, &formatted), NULL);
			STOPIF_CODE_EPIPE( fprintf(output, 
						"- No copyfrom relation found for %s\n", 
						formatted), NULL);
		}
		else
			DEBUGP("No sources found for %s", path);
	}

	STOPIF_CODE_EPIPE( fflush(output), NULL);

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
			switch (sts->st.mode & S_IFMT)
			{
				case S_IFDIR:
					STOPIF( cm__find_dir_source(sts), NULL);
					break;
				case S_IFLNK:
				case S_IFREG:
					STOPIF( cm__find_file_source(sts), NULL);
					break;
				default:
					DEBUGP("Don't handle entry %s", sts->name);
			}
		}

		if (S_ISDIR(sts->st.mode) && 
				(sts->entry_status & (FS_CHILD_CHANGED | FS_CHANGED)) )
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
	 * repository. */
	opt__set_int(OPT__CHANGECHECK, PRIO_MUSTHAVE, CHCHECK_NONE);

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

		match->is_enabled= !match->is_expensive || 
			opt__get_int(OPT__COPYFROM_EXP);

		if (!match->filename[0]) continue;

		DEBUGP("open hash for %s as %s", match->name, match->filename);

		/* Create database file for WC root. */
		STOPIF( hsh__new(wc_path, match->filename, 
					HASH_TEMPORARY, & match->db), NULL);
	}


	/* We read all entries, and show some progress. */
	status=waa__read_or_build_tree(root, argc, normalized, argv, 
			cm___register_entry, 1);
	if (status == -ENOENT)
		STOPIF(status, "!No committed working copy found.");
	STOPIF(status, NULL);


	copydetect_count=0;

	STOPIF( cm__find_copied(root), NULL);

	if (!copydetect_count)
		STOPIF_CODE_EPIPE( printf("No copyfrom relations found.\n"), NULL);
	else if (opt__is_verbose() > 0)
		STOPIF_CODE_EPIPE( printf("%d copyfrom relation%s found.\n",
					copydetect_count, copydetect_count == 1 ? "" : "s"), NULL);

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


/** Converts the (internally stored) string into revision/URL.
 *
 * The \a string is not freed; that has to be done by the caller. */
int cm___string_to_rev_path(char *string, char **out_url, svn_revnum_t *orev)
{
	char *path;
	svn_revnum_t rev;

	/* Reserved for future use */
	if (string[0] != '0' || 
			string[1] != ' ') goto inval;
	string+=2;

	rev=strtol(string, &path, 10);
	if (orev) *orev=rev;
	if (string == path) goto inval;

	if (!isspace(*path)) goto inval;

	path=hlp__skip_ws(path);
	*out_url=path;

	DEBUGP("string parsed to r%llu of %s", (t_ull)rev, path);
	return 0;

inval:
	DEBUGP("cannot parse %s", string);
	return EINVAL;
}


/** Formats the \a revision and \a url for storage in the hash table. 
 *
 * \a *string must not be free()ed by the caller. */
int cm___rev_path_to_string(char *url, svn_revnum_t revision, char **string)
{
	int status;
	static struct cache_entry_t *c=NULL;
	int buflen, used;
	char *buffer;


	/* We need to prepend a revision number, and a space.
	 * We could do a log10(), but I don't think that's necessary ...
	 * even log2l() (which is a single instruction on some processors) uses 
	 * floating point operations here. */
	buflen=10 + 1 + strlen(url) + 1;
	STOPIF( cch__entry_set( &c, 0, NULL, buflen, 0, &buffer), NULL);

	used=snprintf(buffer, buflen, "0 %llu %s", (t_ull)revision, url);
	BUG_ON(used >= buflen);

	*string=buffer;

ex:
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
	char *path;
	svn_revnum_t rev;


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

		STOPIF( cm___string_to_rev_path( value.dptr, &path, &rev), NULL);

		status |= fprintf(output, "%s\n%s\n", path, key.dptr);
		IF_FREE(value.dptr);

		STOPIF_CODE_ERR( status < 0, -EPIPE, "output error");

		status=hsh__next(db, &key, &key);
		have++;
	}

	if (!have)
	{
no_copyfrom:
		fprintf(output, "No copyfrom information was written.\n");
	}
	else
		if (opt__is_verbose() > 0)
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
 * If \a revision is not \c 0 (which corresponds to \c BASE), the correct 
 * list of entries must be taken from the corresponding repository.
 *
 * Uninitialization is done via \c root==NULL.
 *
 * If the flag \a paths_are_wc_relative is set, the paths \a cp_src and \a 
 * cp_dest are taken as-is.
 * Else they're are converted to wc-relative paths by making them absolute 
 * (eventually using \ref start_path as anchor), and cutting the wc-root 
 * off. */
int cm___make_copy(struct estat *root, 
		char *cp_src, svn_revnum_t revision,
		char *cp_dest, 
		int paths_are_wc_relative)
{
	int status;
	static const char err[]="!The %s path \"%s\" is not below the wc base.";
	struct estat *src, *dest;
	static hash_t db=NULL;
	char *abs_src, *abs_dest;
	char *wc_src, *wc_dest;
	char *buffer, *url;


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

	/* TODO: Make copying copied entries possible.
	 * But as we only know the URL, we'd either have to do a checkout, or try 
	 * to parse back to the original entry. */
	STOPIF_CODE_ERR( src->flags & RF___IS_COPY, EINVAL,
			"!Copied entries must be committed before using them as copyfrom source.");

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

	if (revision)
	{
		BUG_ON(1, "fetch list of entries from the repository");
	}
	else
	{
		STOPIF( waa__copy_entries(src, dest), NULL);
		revision=src->repos_rev;
	}

	/* Mark as base entry for copy; the RF_ADD flag was removed by 
	 * copy_entries above, but the entry really is *new*. */
	dest->flags |= RF_COPY_BASE;
	dest->flags &= ~RF_COPY_SUB;


	STOPIF( url__full_url( src, &url), NULL);
	STOPIF( cm___rev_path_to_string(url, revision, &buffer), NULL);
	STOPIF( hsh__store_charp(db, wc_dest, buffer), NULL);

ex:
	return status;
}


/** Sets all entries that are just implicitly copied to ignored.
 * Explicitly added entries (because of \ref add, or \ref prop-set) are 
 * kept.
 *
 * Returns a \c 0 or \c 1, with \c 1 saying that \b all entries below are 
 * ignored, and so whether \a cur can (perhaps) be completely ignored, too.
 * */
int cm___ignore_impl_copied(struct estat *cur)
{
	struct estat **sts;
	int all_ign;


	all_ign=1;
	cur->flags &= ~RF_COPY_SUB;

	if (cur->flags & (RF_ADD | RF_PUSHPROPS))
		all_ign=0;

	if (ops__has_children(cur))
	{
		sts=cur->by_inode;
		while (*sts)
		{
			all_ign &= cm___ignore_impl_copied(*sts);
			sts++;
		}
	}

	if (all_ign)
		cur->to_be_ignored=1;
	else
		/* We need that because of its children, and we have to check. */
		cur->flags |= RF_ADD | RF_CHECK;
	DEBUGP("%s: all_ignore=%d", cur->name, all_ign);

	return all_ign;
}


/** -.
 * */
int cm__uncopy(struct estat *root, int argc, char *argv[])
{
	int status;
	char **normalized;
	struct estat *dest;


	/* Do only the selected elements. */
	opt_recursive=-1;

	if (!argc)
		ac__Usage_this();

	STOPIF( waa__find_common_base(argc, argv, &normalized), NULL);

	STOPIF( url__load_nonempty_list(NULL, 0), NULL);


	/* Load the current data, without updating */
	status=waa__input_tree(root, NULL, NULL);
	if (status == ENOENT)
		STOPIF( EINVAL, "!No working copy could be found.");
	else
		STOPIF( status, NULL);

	while (*normalized)
	{
		DEBUGP("uncopy %s %s", *normalized, normalized[1]);

		STOPIF( ops__traverse(root, *normalized, 
					OPS__FAIL_NOT_LIST, 0, 
					&dest), 
				"!The entry \"%s\" is not known.", *normalized);
		STOPIF_CODE_ERR( !(dest->flags & RF_COPY_BASE), EINVAL,
				"!The entry \"%s\" is not a copy base.", *normalized);

		/* Directly copied, unchanged entry.
		 * Make it unknown - remove copy relation (ie. mark hash value for 
		 * deletion), and remove entry from local list. */
		STOPIF( cm__get_source(dest, NULL, NULL, NULL, 1), NULL);

		dest->flags &= ~RF_COPY_BASE;

		/* That removes all not explicitly added entries from this subtree. */
		cm___ignore_impl_copied(dest);

		normalized++;
	}

	STOPIF( waa__output_tree(root), NULL);
	/* Purge. */
	STOPIF( cm__get_source(NULL, NULL, NULL, NULL, 0), NULL);

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
	svn_revnum_t revision;


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


	switch (opt_target_revisions_given)
	{
		case 0:
			/* Default is \c BASE. */
			revision=0;
			break;
		case 1:
			revision=opt_target_revision;
		default:
			STOPIF( EINVAL, "!Only a single revision number may be given.");
	}


	STOPIF( url__load_nonempty_list(NULL, 0), NULL);

	/* Load the current data, without updating; so st.mode equals 
	 * st.local_mode_packed and so on. */
	status=waa__input_tree(root, NULL, NULL);
	if (status == -ENOENT)
		STOPIF(status, "!No entries are currently known, "
				"so you can't define copy or move relations yet.\n");
	STOPIF(status, NULL);

	hlp__string_from_filep(NULL, NULL, NULL, SFF_RESET_LINENUM);

	if (is_load)
	{
		/* Load copyfrom data. */
		count=0;

		while (1)
		{
			status=hlp__string_from_filep(input, &cp, NULL, 0);
			if (status == EOF) 
			{
				status=0;
				break;
			}
			STOPIF( status, "Failed to read copyfrom source");

			STOPIF_CODE_ERR( !*cp, EINVAL, 
					"!Copyfrom source must not be empty.");
			STOPIF( hlp__strdup( &src, cp), NULL);


			status=hlp__string_from_filep(input, &cp, NULL, 0);
			STOPIF_CODE_ERR( status == EOF, EINVAL,
					"!Expected a target specification, got EOF!");
			STOPIF( status, "Failed to read copyfrom destination");

			STOPIF( hlp__strdup( &dest, cp), NULL);


			/* Get the empty line */
			status=hlp__string_from_filep(input, &cp, NULL, SFF_WHITESPACE);
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

			STOPIF( cm___make_copy(root, src, revision, dest, 0), NULL);
			count++;

			free(dest);
			free(src);
		}

		if (opt__is_verbose() >= 0)
			printf("%d copyfrom relation%s loaded.\n", count, count==1 ? "" : "s");
	}
	else
	{
		STOPIF_CODE_ERR(argc != 2, EINVAL, 
				"!At least source and destination, "
				"or \"dump\" resp. \"load\" must be given.");

		/* Create database file for WC root. */
		STOPIF( cm___make_copy(root, 
					normalized[0], revision, 
					normalized[1], 1), 
				"Storing \"%s\" as source of \"%s\" failed.",
				normalized[0], normalized[1]);
	}

	STOPIF( cm___make_copy(NULL, NULL, 0, NULL, 0), NULL);
	STOPIF( waa__output_tree(root), NULL);

ex:
	return status;
}



/** Get the source of an entry with \c RF_COPY_BASE set.
 * See cm__get_source() for details.
 * */
int cm___get_base_source(struct estat *sts, char *name,
		char **src_url, svn_revnum_t *src_rev,
		int alloc_extra,
		int register_for_cleanup)
{
	int status;
	datum key, value;
	static hash_t hash;
	static int init=0;
	char *url;


	value.dptr=NULL;
	status=0;
	if (src_url) *src_url=NULL;
	if (src_rev) *src_rev=SVN_INVALID_REVNUM;

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

	/* Extract the revision number. */
	STOPIF( cm___string_to_rev_path( value.dptr, 
				&url, src_rev), NULL);

	if (src_url)
	{
		BUG_ON(!url);

		status=strlen(url);
		/* In case the caller wants to do something with this buffer, we return 
		 * more. We need at least the additional \0; and we give a few byte 
		 * extra, gratis, free for nothing (and that's cutting my own throat)!  
		 * */
		STOPIF( hlp__strnalloc( status + 1 +alloc_extra + 4, 
					src_url, url), NULL);
		status=0;
	}

ex:
	IF_FREE(value.dptr);
	return status;
}


/** Recursively creating the URL.
 * As most of the parameters are constant, we could store them statically 
 * ... don't know whether it would make much difference, this function 
 * doesn't get called very often.
 * \a length_to_add is increased while going up the tree; \a eobuffer gets 
 * handed back down. */
int cm___get_sub_source_rek(struct estat *cur, int length_to_add,
		char **dest_buffer, svn_revnum_t *src_rev,
		char **eobuffer)
{
	int status;
	struct estat *copied;
	int len;


	/* Get source of parent.
	 * Just because this entry should be removed from the copyfrom database 
	 * that isn't automatically true for the corresponding parent. */
	copied=cur->parent;
	BUG_ON(!copied, "Copy-sub but no base?");
	len=strlen(cur->name);

	length_to_add+=len+1;

	if (copied->flags & RF_COPY_BASE)
	{
		/* Silent error return. */
		status=cm___get_base_source(copied, NULL, 
				dest_buffer, src_rev, 
				length_to_add, 0);
		if (status) goto ex;

		*eobuffer=*dest_buffer+strlen(*dest_buffer);
		DEBUGP("after base eob-5=%s", *eobuffer-5);
	}
	else
	{
		/* Maybe better do (sts->path_len - copied->path_len))?
		 * Would be faster. */
		status=cm___get_sub_source_rek(copied, length_to_add,
				dest_buffer, src_rev, eobuffer);
		if (status) goto ex;
	}

	/* Now we have the parent's URL ... put cur->name after it. */
	/* Not PATH_SEPARATOR, it's an URL and not a pathname. */
	**eobuffer = '/';
	strcpy( *eobuffer +1, cur->name );
	*eobuffer += len+1;

	DEBUGP("sub source of %s is %s", cur->name, *dest_buffer);

ex:
	return status;
}


/** Get the source of an entry with \c RF_COPY_SUB set.
 * See cm__get_source() for details.
 *
 * This function needs no cleanup.
 * */
int cm___get_sub_source(struct estat *sts, char *name,
		char **src_url, svn_revnum_t *src_rev)
{
	int status;
	char *eob;


	/* As we only store the URL in the hash database, we have to proceed as 
	 * follows:
	 * - Look which parent is the copy source,
	 * - Get its URL
	 * - Append the path after that to the URL of the copy source:
	 *     root / dir1 / dir2 / <copied_dest> / dir3 / <sts>
	 *   and
	 *     root / dir1 / dir3 / <src>
	 * Disadvantage: 
	 * - we have to traverse the entries, and make the estat::by_name 
	 *   arrays for all intermediate nodes.
	 *
	 * We do that as a recursive sub-function, to make bookkeeping easier. */
	STOPIF( cm___get_sub_source_rek(sts, 0, src_url, src_rev, &eob), NULL);

ex:
	return status;
}


/** -.
 *
 * Wrapper around cm___get_base_source() and cm___get_sub_source().
 * 
 * If \c *src_url is needed, it is allocated and must be \c free()ed after 
 * use.
 *
 * If \a name is not given, it has to be calculated.
 *
 * Both \a src_name and \a src_rev are optional.
 * These are always set; if no source is defined, they're set to \c NULL, 
 * \c NULL and \c SVN_INVALID_REVNUM.
 *
 * Uninitializing should be done via calling with \c sts==NULL; in this 
 * case the \a register_for_cleanup value is used as success flag.
 *
 * If no source could be found, \c ENOENT is returned. */
int cm__get_source(struct estat *sts, char *name,
		char **src_url, svn_revnum_t *src_rev,
		int register_for_cleanup)
{
	int status;


	if (!sts)
	{
		status=cm___get_base_source(NULL, NULL, NULL, NULL, 0, 0);
		goto ex;
	}

	if (sts->flags & RF_COPY_BASE)
	{
		status= cm___get_base_source(sts, name, 
				src_url, src_rev, 0,
				register_for_cleanup);
	}
	else if (sts->flags & RF_COPY_SUB)
	{
		status= cm___get_sub_source(sts, name, 
				src_url, src_rev);
	}
	else
	{
		status=ENOENT;
		goto ex;
	}

	if (src_url)
		DEBUGP("source of %s is %s", sts->name, *src_url);

	if (status)
	{
		/* That's a bug ... the bit is set, but no source was found?
		 * Could some stale entry cause that? Don't error out now; perhaps at a 
		 * later time. */
		DEBUGP("bit set, no source!");
		/* BUG_ON(1,"bit set, no source!"); */
		goto ex;
	}

ex:
	return status;
}

