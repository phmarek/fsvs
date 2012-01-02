/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "est_ops.h"
#include "direnum.h"
#include "warnings.h"
#include "global.h"
#include "helper.h"


/** \file
 * Directory enumerator functions. */


/** \defgroup getdents Directory reading
 * \ingroup perf
 * How to read a million inodes as fast as possible
 *
 * \section getdents_why Why?
 * Why do we care for \a getdents64 instead of simply using the
 * (portable) \a readdir()?
 * - \a getdents64 gives 64bit inodes (which we need on big
 *   filesystems)
 * - as \a getdents64 gives up to (currently) 4096 bytes of directory
 *   data, we save some amount of library and/or kernel calls - 
 *   for 32 byte per directory entry (estimated, measured, averaged)
 *   we get a maximum of about 128 directory entries per call - which 
 *   saves many syscalls and much time.
 *   Not counting the overhead of the apr- and libc-layers ... which we
 *   should (have to) use for eg. windows.
 *
 * \section getdents_how How?
 * We have two kinds of directory reading codes.
 * - A fast one with \a getdents64() (linux-specific)
 * - A compatibility layer using \a opendir() / \a readdir() / \a closedir().
 *
 * Which one to use is defined by \c configure. 
 * */
/** @{ */

#undef HAVE_GETDENTS64
#ifdef HAVE_LINUX_TYPES_H 
#ifdef HAVE_LINUX_UNISTD_H
/** If the system fulfills all necessary checks to use getdents(), this macro
 * is set. */
#define HAVE_GETDENTS64 1
#endif
#endif


#ifdef HAVE_GETDENTS64
/* Fast linux version. */
#include <linux/types.h>
#include <linux/unistd.h>

/** The type of handle.  */
typedef int dir__handle;
/** A compatibility structure.
 * It has an inode; a name; and a record length in it, to get from one
 * record to the next. */
typedef struct dirent64 fsvs_dirent;


/** Starts enumeration of the given \a path. The directory handle is returned
 * in \a *dirp.
 * \return 0 for success, or an error code. */
inline int dir__start_enum(dir__handle *dh, char *path)
{	
	int status;

	status=0;
	*dh=open(path, O_RDONLY | O_DIRECTORY);
	STOPIF_CODE_ERR( *dh <= 0, errno,
			"open directory %s for reading", path);

ex:
	return status;
}


/** The enumeration function.
 * \param dh The handle given by dir__start_enum.
 * \param dirp The space where data should be returned
 * \param count The maximum number of bytes in \a dirp.
 *
 * \return The number of bytes used in \a dirp. */
inline int dir__enum(dir__handle dh, fsvs_dirent *dirp, unsigned int count)
{
	return syscall(__NR_getdents64, dh, dirp, count);
}


/** Simply closes the handle \a dh.
 * */
inline int dir__close(dir__handle dh)
{
	int status;

	status=0;
	STOPIF_CODE_ERR( close(dh) == -1, errno,
			"closing dir-handle");

ex:
	return status;
}


/** How to get the length of a directory (in bytes), from a handle \a dh,
 * into \a st->size. */
inline int dir__get_dir_size(dir__handle dh, struct sstat_t *st)
{
	int status;

	status=0;
	STOPIF( hlp__fstat(dh, st), "Get directory size");
ex:
	return status;
}

#else

/* We fake something compatible with what we need.
 * That's not the finest way, but it works (TM). */

#include <limits.h>
#include <dirent.h>
struct fsvs_dirent_t {
	uint64_t	d_ino;
	int 		d_reclen;
	char		d_name[NAME_MAX+1];
};


typedef struct fsvs_dirent_t fsvs_dirent;
typedef DIR* dir__handle;


inline int dir__start_enum(dir__handle *dh, char *path)
{
	int status;

	status=0;
	STOPIF_CODE_ERR( (*dh=opendir(path)) == NULL, errno,
			"Error opening directory %s", path);
ex:
	return status;
}


/* Impedance matching .. don't like it. */
inline int dir__enum(dir__handle dh, fsvs_dirent *dirp, unsigned int count)
{
	struct dirent *de;

	de=readdir(dh);
	/* EOD ? */
	if (!de) return 0; 

	dirp[0].d_ino = de->d_ino;
	strcpy( dirp[0].d_name, de->d_name);

	dirp[0].d_reclen = sizeof(dirp[0])-sizeof(dirp[0].d_name) +
		strlen(dirp[0].d_name) + 1;
	return dirp[0].d_reclen;
}


inline int dir__close(dir__handle dh)
{
	int status;

	status=0;
	STOPIF_CODE_ERR( closedir(dh) == -1, errno,
			"Error closing directory handle");
ex:
	return status;
}


inline int dir__get_dir_size(dir__handle dh, struct sstat_t *st)
{
	int status;

	status=0;
	st->size=0;
#ifdef HAVE_DIRFD
	STOPIF( hlp__fstat(dirfd(dh), st), 
			"Get directory size()");
#endif

ex:
	return status;
}

#endif

/** @} */



/** The amount of memory that should be allocated for directory reading.
 * This value should be bigger (or at least equal) than the number of 
 * bytes returned by \a getdents().
 * For the compatibility layer it's more or less the maximum filename length
 * plus the inode and record length lengths.
 *
 * This many bytes \b more will also be allocated for the filenames in a 
 * directory; if we get this close to the end of the buffer, 
 * the memory area will be reallocated. */
#define FREE_SPACE (4096)


/** Compares two struct estat pointers by device/inode.
 * \return +2, +1, 0, -1, -2, suitable for \a qsort().
 *
 * That is now an inline function; but without force gcc doesn't inline it 
 * on 32bit, because of the size (64bit compares, 0x6b bytes).
 * [ \c __attribute__((always_inline)) in declaration]. */
int dir___f_sort_by_inodePP(struct estat *a, struct estat *b) 
{
	register const struct sstat_t* __a=&(a->st);
	register const struct sstat_t* __b=&(b->st);

	if (__a->dev > __b->dev) return +2;
	if (__a->dev < __b->dev) return -2;
	if (__a->ino > __b->ino) return +1;
	if (__a->ino < __b->ino) return -1;

	return 0;
}


/** Compares the data inside two struct estat pointers to pointers by 
 * device/inode. 
 * \return +2, +1, 0, -1, -2, suitable for \a qsort(). */
int dir___f_sort_by_inode(struct estat **a, struct estat **b)
{
	return dir___f_sort_by_inodePP(*a, *b);
}


/** Compares two names/strings.
 * Used for type checking cleanliness. 
 * 'C' as for 'Const'.
 * \return +2, +1, 0, -1, -2, suitable for \a qsort(). */
inline int dir___f_sort_by_nameCC(const void *a, const void *b)
{
	return strcoll(a,b);
}


/** Compares the data inside two struct estat pointers to pointers 
 * by name.
 * \return +2, +1, 0, -1, -2, suitable for \a qsort(). */
int dir___f_sort_by_name(const void *a, const void *b)
{
	register const struct estat * const *_a=a;
	register const struct estat * const *_b=b;

	return dir___f_sort_by_nameCC((*_a)->name, (*_b)->name);
}


/** Compares a pointer to name (string) with a struct estat pointer 
 * to pointer. 
 * \return +2, +1, 0, -1, -2, suitable for \a qsort(). */
int dir___f_sort_by_nameCS(const void *a, const void *b)
{
	register const struct estat * const *_b=b;

	return dir___f_sort_by_nameCC(a, (*_b)->name);
}


/** -.
 * If it has no entries, an array with NULL is nonetheless allocated. */
int dir__sortbyname(struct estat *sts)
{
	int count, status;


//	BUG_ON(!S_ISDIR(sts->st.mode));
	count=sts->entry_count+1;
	/* After copying we can release some space, as 64bit inodes
	 * are smaller than 32bit pointers.
	 * Or otherwise we may have to allocate space anyway - this
	 * happens automatically on reallocating a NULL pointer. */
	STOPIF( hlp__realloc( &sts->by_name, count*sizeof(*sts->by_name)), NULL);

	if (sts->entry_count!=0)
	{
		memcpy(sts->by_name, sts->by_inode, count*sizeof(*sts->by_name));
		qsort(sts->by_name, sts->entry_count, sizeof(*sts->by_name), dir___f_sort_by_name); 
	}

	sts->by_name[sts->entry_count]=NULL;
	status=0;

ex:
	return status;
}


/** -.
 * */
int dir__sortbyinode(struct estat *sts)
{
//	BUG_ON(!S_ISDIR(sts->st.mode));
	if (sts->entry_count)
	{
		BUG_ON(!sts->by_inode);

		qsort(sts->by_inode, sts->entry_count, sizeof(*sts->by_inode), 
				(comparison_fn_t)dir___f_sort_by_inode); 
	}

	return 0;
}


/** -.
 * The entries are sorted by inode number and stat()ed.
 *
 * \param this a pointer to this directory's stat - for estimating
 * the number of entries. Only this->st.st_size is used for that - 
 * it may have to be zeroed before calling.
 * \param est_count is used to give an approximate number of entries, to
 * avoid many realloc()s.
 * \param give_by_name simply tells whether the ->by_name array should be
 * created, too.
 *
 * The result is written back into the sub-entry array in \a this.
 *
 * To avoid reallocating (and copying!) large amounts of memory,
 * this function fills some arrays from the directory, then allocates the
 * needed space, sorts the data (see note below) and adds all other data. 
 * See \a sts_array, \a names and \a inode_numbers.
 *
 * \note Sorting by inode number brings about 30% faster lookup
 * times on my test environment (8 to 5 seconds) on an \b empty cache.
 * Once the cache is filled, it won't make a difference.
 *
 * \return 0 for success, else an errorcode.
 */ 
int dir__enumerator(struct estat *this,
		int est_count,
		int give_by_name)
{
	dir__handle dirhandle;
	int size;
	int count;
	int i,j,l;
	int sts_free;
	int status;

	/* Struct \a estat pointer for temporary use. */
	struct estat *sts=NULL;

	/* The estimated number of entries. */
	int	alloc_count; 

	/* Stores the index of the next free byte in \a strings. */
	int	mark; 
	/* Filename storage space. Gets stored in the directories \a ->strings
	 * for memory management purposes. */
	void *strings=NULL;
	/* Array of filenames. As the data space potentially has to be
	 * reallocated at first only the offsets into \a *strings is stored. 
	 * These entries must be of the same size as a pointer, because the array
	 * is reused as \c sts_array[] .*/
	long *names=NULL;

	/* The buffer space, used as a struct \a fsvs_dirent */
	char buffer[FREE_SPACE]; 
	/* points into and walks over the \a buffer */
	fsvs_dirent *p_de; 

	/* Array of the struct \a estat pointers. Reuses the storage space
	 * of the \a names Array. */
	struct estat **sts_array=NULL;
	/* Array of inodes. */
	ino_t *inode_numbers=NULL; 


	STOPIF( dir__start_enum(&dirhandle, "."), NULL);
	if (!this->st.size)
		STOPIF( dir__get_dir_size(dirhandle, &(this->st)), NULL);

	/* At least a long for the inode number, and 3 characters + 
	 * a \0 per entry. But assume an average of 11 characters + \0.
	 * If that's incorrect, we'll have to do an realloc. Oh, well.
	 *
	 * Another estimate which this function gets is the number of files
	 * last time this directory was traversed.
	 *
	 * Should maybe be tunable in the future.
	 *
	 * (On my system I have an average of 13.9 characters per entry, 
	 * without the \0) */
	alloc_count=this->st.size/(sizeof(*p_de) - sizeof(p_de->d_name) + 
			ESTIMATED_ENTRY_LENGTH +1);
	/* + ca. 20% */
	est_count= (est_count*19)/16 +1; 
	if (alloc_count > est_count) est_count=alloc_count;
	/* on /proc, which gets reported with 0 bytes,
	 * only 1 entry is allocated. This entry multiplied with 19/16
	 * is still 1 ... crash. 
	 * So all directories reported with 0 bytes are likely virtual
	 * file systems, which can have _many_ entries ... */
	if (est_count < 32) est_count=32;

	size=FREE_SPACE + est_count*( ESTIMATED_ENTRY_LENGTH + 1 );
	STOPIF( hlp__alloc( &strings, size), NULL);

	mark=count=0;
	inode_numbers=NULL;
	names=NULL;
	alloc_count=0;
	/* read the directory and count entries */
	while ( (i=dir__enum(dirhandle, (fsvs_dirent*)buffer, sizeof(buffer))) >0)
	{
		/* count entries, copy name and inode nr */
		j=0;
		while (j<i)
		{
			/* allocate or resize. */
			if (count >= alloc_count)
			{
				/* If we already started, put a bit more space here. 
				 * Should maybe be configurable. */
				if (!alloc_count) 
					alloc_count=est_count;
				else
					alloc_count=alloc_count*19/16;

				STOPIF( hlp__realloc( &names, alloc_count*sizeof(*names)), NULL);

				/* temporarily we store the inode number in the *entries_by_inode
				 * space; that changes when we've sorted them. */
				STOPIF( hlp__realloc( &inode_numbers, 
							alloc_count*sizeof(*inode_numbers)), NULL);
			}

			p_de=(fsvs_dirent*)(buffer+j);

			DEBUGP("found %llu %s", (t_ull)p_de->d_ino, p_de->d_name);

			if (p_de->d_name[0] == '.' &&
					((p_de->d_name[1] == '\0') ||
					 (p_de->d_name[1] == '.' &&
					  p_de->d_name[2] == '\0')) )
			{
				/* just ignore . and .. */
			}
			else
			{
				/* store inode for sorting */
				inode_numbers[count] = p_de->d_ino;

				/* Store pointer to name.
				 * In case of a realloc all pointers to the strings would get 
				 * invalid. So don't store the addresses now - only offsets. */
				names[count] = mark;

				/* copy name, mark space as used */
				l=strlen(p_de->d_name);
				strcpy(strings+mark, p_de->d_name);
				mark += l+1;

				count++;
			}

			/* next */
			j += p_de->d_reclen;
		}

		/* Check for free space.
		 * We read at most FREE_SPACE bytes at once,
		 * so it's enough to have FREE_SPACE bytes free.
		 * Especially because there are some padding and pointer bytes
		 * which get discarded. */
		if (size-mark < FREE_SPACE)
		{
			/* Oh no. Have to reallocate.
			 * But we can hope that this (big) chunk is on the top
			 * of the heap, so that it won't be copied elsewhere.
			 *
			 * How much should we add? For now, just give about 30%. */
			/* size*21: Let's hope that this won't overflow :-) */ 
			size=(size*21)/16;
			/* If +20% is not at least the buffer size (FREE_SPACE), 
			 * take at least that much memory. */
			if (size < mark+FREE_SPACE) size=mark+FREE_SPACE;

			STOPIF( hlp__realloc( &strings, size), NULL);
			DEBUGP("strings realloc(%p, %d)", strings, size);
		}
	}

	STOPIF_CODE_ERR(i<0, errno, "getdents64");

	DEBUGP("after loop found %d entries, %d bytes string-space", count, mark);
	this->entry_count=count;

	/* Free allocated, but not used, memory. */
	STOPIF( hlp__realloc( &strings, mark), NULL);
	/* If a _down_-sizing ever gives an error, we're really botched.
	 * But if it's an empty directory, a NULL pointer will be returned. */
	BUG_ON(mark && !strings);
	this->strings=strings;
	/* Now this space is used - don't free. */
	strings=NULL;

	/* Same again. Should never be NULL, as the size is never 0. */
	STOPIF( hlp__realloc( &inode_numbers, 
				(count+1)*sizeof(*inode_numbers)), NULL);
	STOPIF( hlp__realloc( &names, (count+1)*sizeof(*names)), NULL);

	/* Store end-of-array markers */
	inode_numbers[count]=0;
	names[count]=0;


	/* Now we know exactly how many entries, we build the array for sorting.
	 * We don't do that earlier, because resizing (and copying!)
	 * is slow. Doesn't matter as much if it's just pointers,
	 * but for bigger structs it's worth avoiding.
	 * Most of the structures get filled only after sorting! */
	/* We reuse the allocated array for names (int**) for storing
	 * the (struct estat**). */
	sts_array=(struct estat**)names;
	sts_free=0;
	for(i=0; i<count; i++)
	{
		if (sts_free == 0)
			STOPIF( ops__allocate(count-i, &sts, &sts_free), NULL);

		/* The names-array has only the offsets stored.
		 * So put correct values there. */
		sts->name=this->strings + names[i];
		sts->st.ino=inode_numbers[i];

		/* now the data is copied, we store the pointer. */
		sts_array[i] = sts;

		sts++;
		sts_free--;
	}
	/* now names is no longer valid - space was taken by sts_array. */
	names=NULL;

	this->by_inode=sts_array;
	/* Now the space is claimed otherwise - so don't free. */
	sts_array=NULL;

	/* See inodeSort */
	STOPIF( dir__sortbyinode(this), NULL);


	//	for(i=0; i<count; i++) printf("%5ld %s\n",de[i]->d_ino, de[i]->d_name);
	for(i=0; i<count; i++)
	{
		sts=this->by_inode[i];
		sts->parent=this;
		sts->repos_rev=SVN_INVALID_REVNUM;
		status=hlp__lstat(sts->name, &(sts->st));
		if (abs(status) == ENOENT)
		{
			DEBUGP("entry \"%s\" not interesting - maybe a fifo or socket?", 
					sts->name);
			sts->to_be_ignored=1;
		}
		else
			STOPIF( status, "lstat(%s)", sts->name);

		/* New entries get that set, because they're "updated". */
		sts->old_rev_mode_packed = sts->local_mode_packed= 
			MODE_T_to_PACKED(sts->st.mode);
	}


	/* Possibly return list sorted by name. */
	if (give_by_name)
		STOPIF(dir__sortbyname(this), NULL);
	else
		/* should not be needed - but it doesn't hurt, either. */
		this->by_name=NULL;

	status=0;

ex:
	IF_FREE(strings);
	IF_FREE(names);
	IF_FREE(inode_numbers);
	IF_FREE(sts_array);

	if (dirhandle>=0) dir__close(dirhandle);
	return status;
}


