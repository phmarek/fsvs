/************************************************************************
 * Copyright (C) 2005-2008 Philipp Marek.
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
#include <time.h>


#include "global.h"
#include "status.h"
#include "cache.h"
#include "actions.h"
#include "est_ops.h"
#include "direnum.h"
#include "warnings.h"
#include "helper.h"
#include "checksum.h"
#include "url.h"

/** \file
 * Handling of single struct \a estat s.
 *
 * */


/** Single-linked list for storing the freed entries.
 * The struct \a free_estat get written above the struct \a estat it replaces.
 * */
struct free_estat
{
	/** Next free block(s) */
	struct free_estat *next;
	/** Number of "struct estat"s that can be stored here. */
	int count; 
};



/* there are 2 formats, one for writing, one for reading. */
const char 
/** Formats for reading/writing entries in the \a dir files.
 * "mode ctime mtime repo_flags dev_descr MD5_should
 *   size repos_version url# dev# inode# parent_inode# entry_count
 *   uid gid '#'name\\0\\n"
 * Directories have an \c x instead of MD5_*.
 *
 * \note The \c # before the name is needed to know where filenames start.  
 * Since sscanf() skips over the whitespace (as wanted), we loose 
 * informations. */
ops__dir_info_format_s[]="%llo %lx %lx %x %n%*s %n%*s "
		"%lld %ld %u %lx %lld %lld %u "
		"%u %u%n", 
	ops__dir_info_format_p[]="%07llo %8x %8x %x %s %s "
		"%lld %ld %u %lx %lld %lld %u "
		"%u %u %s";
#define WAA_MAX_DIR_INFO_CHARS (11+1+8+1+8+1+8+1+APR_MD5_DIGESTSIZE*2+1 \
     +18+1+9+1+9+1+16+1+18+1+18+1+9+1+ \
		 9+1+9+1+NAME_MAX+1+1)


/** -.
 *
 * It's a bit unaesthetical that devices use a " " for the repository data,
 * but a ":" in the waa as delimiter.
 * But "link " is specified in subversion, and having the repository data
 * different would not be better.
 * So we just allow both at parsing, and use the "right" for each target. */
const char link_spec[]="link ",
			cdev_spec[]="cdev",
			bdev_spec[]="bdev";

static struct free_estat *free_list = NULL;

 

/** -.
 * 
 * For a symlink \c info is returned as the path it points to; devices are 
 * fully decoded and return \c info==NULL. */
int ops__string_to_dev(struct estat *sts, char *data, char **info)
{
	int maj, min;
	int ft, mode;
	char delimiter;
	int status;


	status=0;
	if (0 == strncmp(data, link_spec, strlen(link_spec)))
	{
		mode=S_IFLNK;
		if (info)
			*info=data+5;
	}
	else
	{
		if (0 == strncmp(data, cdev_spec, strlen(cdev_spec)))
		{
			data+=strlen(cdev_spec);
			mode=S_IFCHR;
		}
		else if (0 == strncmp(data, bdev_spec, strlen(bdev_spec)))
		{
			data+=strlen(bdev_spec);
			mode=S_IFBLK;
		}
		else mode=0;

		if (info) *info=NULL;

		ft=sscanf(data, "%c0x%X:0x%X", &delimiter, &maj, &min);

		STOPIF_CODE_ERR(mode == 0 ||
				ft != 3 || 
				(delimiter != ':' && delimiter != ' '),
				EINVAL,
				"'%s' is not parseable as a special description", data);

#ifdef DEVICE_NODES_DISABLED
		DEVICE_NODES_DISABLED();
#else
		sts->st.rdev=MKDEV(maj, min);
#endif
	}

	sts->updated_mode=sts->st.mode= (sts->st.mode & ~S_IFMT) | mode;

ex:
	return status;
}


/** -.
 * The subversion header string for special nodes is prepended.
 *
 * The returned pointer in \a *erg must not be free()d. */
int ops__link_to_string(struct estat *sts, char *filename,
		char **erg)
{
	static struct cache_t *cache=NULL;
	char *cp;
	int l, status, hlen;


	STOPIF( cch__new_cache(&cache, 4), NULL);


	status=0;
	BUG_ON(!S_ISLNK(sts->st.mode));

	if (!filename)
		STOPIF( ops__build_path(&filename, sts), NULL); 

	hlen=strlen(link_spec);
	l=sts->st.size + hlen + 1 + 8;
	STOPIF( cch__add(cache, 0, NULL, l, &cp), NULL);

	strcpy(cp, link_spec);
	STOPIF_CODE_ERR( readlink(filename, cp+hlen, sts->st.size) == -1,
			errno, "can't read link %s", filename);

	cp[hlen+sts->st.size]=0;
	*erg=cp;

ex:
	return status;
}


char *ops___dev_to_string(struct estat *sts, char delimiter)
{
	static char buffer[64];

/* I'm not fully sure about that. */
	BUG_ON(!(sts->remote_status & FS_NEW) && 
			!(S_ISBLK(sts->updated_mode) || S_ISCHR(sts->updated_mode)),
			"%s: mode is 0%o", sts->name, sts->st.mode);

#ifdef DEVICE_NODES_DISABLED
	DEVICE_NODES_DISABLED();
#else
	sprintf(buffer, "%s%c0x%x:0x%x",
			S_ISBLK(sts->st.mode) ? bdev_spec : cdev_spec,
			delimiter,
			(int)MAJOR(sts->st.rdev),
			(int)MINOR(sts->st.rdev));
#endif

	return buffer;
}


/** -.
 * */
char *ops__dev_to_waa_string(struct estat *sts)
{
	return ops___dev_to_string(sts, ':');
}


/** -.
 * */
char *ops__dev_to_filedata(struct estat *sts)
{
	return ops___dev_to_string(sts, ' ');
}


/** -.
 * Returns the change mask as a binary OR of the various \c FS_* constants, 
 * see \ref fs_bits.  */
int ops__stat_to_action(struct estat *sts, struct sstat_t *new)
{
	struct sstat_t *old;
	int ft_old, ft_new;
	int file_status;


	old=&(sts->st);

	/* The exact comparison here would be
	 *   old->_mtime != new->_mtime	||
	 *   old->_ctime != new->_ctime ? FS_META_MTIME : 0;
	 * but that doesn't work, as most filesystems don't have 
	 * nanoseconds stored. Furthermore we get only usec in the repository
	 * (due to svn_time_to_string), so the nsec make no sense here.
	 * We compare only the "coarse", but common, granularity of seconds.
	 * VFAT can store only even seconds!
	 *
	 * The problem gets a bit more complicated as the linux kernel keeps
	 * nsec in the dentry (cached inode), but as soon as the inode has to be
	 * read from disk it has possibly only seconds!
	 *
	 * There's a long thread on dev@subversion.tigris.org about the
	 * granularity of timestamps - auto detecting vs. setting, etc. */
	file_status = 
		old->mtim.tv_sec != new->mtim.tv_sec ? FS_META_MTIME : 0;
	/* We don't show a changed ctime as "t" any more. On commit nothing 
	 * would change in the repository, and it looks a bit silly.
	 * A changed ctime is now only used as an indicator for changes. */

	if (old->uid != new->uid)
		file_status |= FS_META_OWNER;
	if (old->gid != new->gid)
		file_status |= FS_META_GROUP;

	if (old->mode != new->mode)
		file_status |= FS_META_UMODE;

	/* both of same type ? */
	ft_old = old->mode & S_IFMT;
	ft_new = new->mode & S_IFMT;

	if (ft_old != ft_new)
	{
		file_status |= FS_REPLACED;
		goto ex;
	}

	/* same type - compare */
	BUG_ON(sts->to_be_ignored);
	switch (ft_new)
	{
		case S_IFBLK:
		case S_IFCHR:
			DEBUGP("old=%llu new=%llu", (t_ull)old->rdev, (t_ull)new->rdev);
			file_status |= 
				(old->rdev == new->rdev) 
				? FS_NO_CHANGE : FS_REPLACED;
			break;

		case S_IFLNK:
		case S_IFREG:
			if (old->size != new->size)
				file_status |= FS_CHANGED;
			else
				/* The changed flag can be set or cleared by cs__compare_file().
				 * We don't set it until we *know* the entry has changed. */
			if ( (file_status & FS_META_MTIME) ||
					old->ctim.tv_sec != new->ctim.tv_sec )
				file_status |= FS_LIKELY;
			break;

		case S_IFDIR:
			/* This entry *could* be changed.
			 * But as the changed flag is set if a child entry is missing
			 * or if new entries are found, but never cleared, we don't set 
			 * it here. */
			if ( (file_status & FS_META_MTIME) ||
					old->ctim.tv_sec != new->ctim.tv_sec )
				file_status |= FS_LIKELY;
			break;

		default:
			BUG_ON(1);
//		case FT_IGNORE:
			file_status=FS_NO_CHANGE;
	}

ex:
	DEBUGP("change: types 0%o vs 0%o; 0x%x=%s", 
			ft_old, ft_new,
			file_status, 
			st__status_string_fromint(file_status));
	return file_status;
}


/** -.
 * 
 * The \a filename still points into the buffer (\c mmap()ed area) and must 
 * be copied.
 *
 * \a mem_pos is advanced, and points \b after the \c \\0. If a \c \\n is 
 * seen immediately afterwards, it is skipped, too.
 *
 * \a parent_i gets set to the stored value; the translation to a \c parent 
 * pointer must be done in the caller.
 *
 * \c EOF cannot be reliable detected here; but we are guaranteed a 
 * <tt>\\0\\n</tt> at the end of the string, to have a filename 
 * termination. */
int ops__load_1entry(char **mem_pos, struct estat *sts, char **filename,
		ino_t *parent_i)
{
	char *buffer;
	int i, p, status;
	ino_t parent_inode;
	int pos_should, pos_dev;
	t_ull par_ino, size, this_ino, mode;
	t_ul dev;
	unsigned internal_number;


	status=0;
	buffer=*mem_pos;
	/* Now parse. Use temporary variables of defined size 
	 * for some inputs. */
	i=sscanf(buffer, ops__dir_info_format_s, 
			&mode,
			&(sts->st.ctim.tv_sec),
			&(sts->st.mtim.tv_sec),
			&(sts->flags),
			&pos_dev, 
			&pos_should, 
			&size,
			&(sts->repos_rev),
			&internal_number,
			&dev,
			&this_ino,
			&par_ino,
			&(sts->entry_count),
			&(sts->st.uid),
			&(sts->st.gid),
			&p);
	parent_inode=par_ino;
	sts->st.dev=dev;
	sts->st.ino=this_ino;
	sts->st.size=size;
	sts->updated_mode=sts->st.mode=mode;
	sts->old_rev = sts->repos_rev;

	/* The %n are not counted on glibc.
	 * "man sscanf" warns:
	 *   "Probably it is wise not to make any assumptions on the effect of
	 *    %n conversions on the return value."
	 * We hope and try, maybe we'll need a autoconf/configure test. */
	STOPIF_CODE_ERR( i!=13, EINVAL, 
			"cannot parse entry line - %d tokens found", i);


	/* Only the root entry has parent_inode==0; the others start 
	 * counting with 1. */
	if (parent_inode)
	{
		/* There may be entries without an URL associated - eg. entries which
		 * were just added, but not committed. */
		if (internal_number)
			STOPIF( url__find_by_intnum(internal_number, &(sts->url)), NULL);
	}
	else
	{
		/* The root entry gets the highest priority url.
		 * There may be no URLs defined! */
		sts->url= urllist_count ? 
			urllist[urllist_count-1] : 
			NULL;
	}

	/* Only a directory may have children */
	BUG_ON(sts->entry_count && !S_ISDIR(sts->st.mode));

	/* Devices have major:minor stored */
	if (S_ISBLK(sts->st.mode) || S_ISCHR(sts->st.mode))
		STOPIF( ops__string_to_dev(sts, buffer+pos_dev, NULL), NULL);
	/* All entries but directories have MD5 */
	if (!S_ISDIR(sts->st.mode))
	{
		STOPIF( cs__char2md5(buffer+pos_should, sts->md5),
				"Parsing the md5 failed");
	}


	/* Skip over exactly one space - else we'd loose information about 
	 * filenames starting with whitespaces. */
	buffer += p;
	BUG_ON(*buffer != ' ');
	*filename=buffer+1;

	if (parent_i) *parent_i=parent_inode;

	/* Advance memory pointer past end of filename.
	 * Skip \0 and \n.
	 * If we didn't skip the \n, the next sscanf() should work, too;
	 * but the caller would have a hard time figuring if we're already
	 * finished. */
	*mem_pos = *filename + strlen(*filename) + 1;
	if (**mem_pos == '\n') (*mem_pos)++;

ex:
	return status;
}


/** -.
 * The parameter \a parent_ino is a(n integer) reference to the parent 
 * directory - the line number in which it was written.
 * The format is fixed (see \c ops__dir_info_format_p); the string includes 
 * a \c \\n at the end, and a \c \\0 for filename termination just before 
 * that.
 * Any other characters that are allowed in a filename can be written - 
 * even control characters like \c \\n, \c \\r, \c \\f and so on.
 * */
int ops__save_1entry(struct estat *sts,
		ino_t parent_ino,
		int filehandle)
{
	int len;
	static char buffer[WAA_MAX_DIR_INFO_CHARS+2] = 
	{ 
		// overrun detection for debugging
		[sizeof(buffer)-1]=0xff,
		[sizeof(buffer)-2]=0x0,
	};
	int is_dir, is_dev, status, is_spec;
	int intnum;


#if 0
	if (sts->parent)
	{
		/* For entries other than the root node:
		 * If the entry was not added or copied, it has to have an URL.
		 *
		 * But we cannot test for that, as _build_list does exactly that - and 
		 * is needed by the tests. */
		BUG_ON(!sts->url && 
				!(sts->flags & (RF_COPY_SUB | RF_COPY_BASE | RF_ADD)));
	}
#endif

	is_dir = S_ISDIR(sts->st.mode);
	is_dev = S_ISBLK(sts->st.mode) || S_ISCHR(sts->st.mode);
	is_spec = S_ISBLK(sts->st.mode) || S_ISCHR(sts->st.mode) ||
		S_ISLNK(sts->st.mode);

	if (sts->url)
		intnum=sts->url->internal_number;
	else
	{
		/* A non-root entry has no url. May happen with _build_list, when
		 * there are no urls. */
		if (sts->parent)
			DEBUGP("Non-root entry %s has no URL", sts->name);
		intnum=0;
	}

	len=sprintf(buffer, ops__dir_info_format_p,
			(t_ull)sts->st.mode,
			(int)sts->st.ctim.tv_sec,
			(int)sts->st.mtim.tv_sec,
			sts->flags & RF___SAVE_MASK,
			( is_dev ? ops__dev_to_waa_string(sts) : "nd" ),
			( is_dir ? "x" : cs__md52hex(sts->md5) ),
			(t_ull)sts->st.size,
			sts->repos_rev == SET_REVNUM ? sts->url->current_rev : sts->repos_rev,
			intnum,
			(t_ul)sts->st.dev,
			(t_ull)sts->st.ino,
			(t_ull)parent_ino,
			is_dir ? sts->entry_count : 0,
			sts->st.uid,
			sts->st.gid,
			sts->name
			);
	BUG_ON(len > sizeof(buffer)-2);
	len++; // include \0
	buffer[len++]='\n';

	// redundant (?) check
	BUG_ON(buffer[sizeof(buffer)-1]!=0xff || 
			buffer[sizeof(buffer)-2]!=0x0);

	is_dir=write(filehandle, buffer, len);
	STOPIF_CODE_ERR(is_dir != len, errno, 
			"write entry");

	status=0;

ex:
	return status;
}


/** -.
 *
 * If no \c PATH_SEPARATOR is found in the \a path, the \a path itself is 
 * returned. */
inline const char *ops__get_filename(const char *path)
{
	char *cp;

	cp=strrchr(path, PATH_SEPARATOR);
	return cp ? cp+1 : path;
}


/** Returns the "rest" of the path; a \c \\0 is written over the
 * path separator.
 *
 * So <tt>path="abc/def/ghi"</tt> becomes <tt>"abc\0def/ghi"</tt> and the 
 * returned * pointer points to <tt>"def/ghi"</tt>.
 *
 * If there's only a filename left (no \c / found), this returns \c NULL.  
 * */
inline const char *ops___split_fnpart(const char *path)
{
	char *cp;

	cp=strchr(path, PATH_SEPARATOR);
	if (!cp) return NULL;

	/* Overwrite multiple path separators */
	while (*cp == PATH_SEPARATOR) *(cp++)=0;

	/* If the path looks like "name////", there's no next_part, too. */
	if (!*cp) return NULL;

	return cp;
}


/** The \e real recursive part of ops__build_path().
 *
 * This function has a non-standard return parameter - it gives the number 
 * of characters written, and 0 denotes an error. */
int ops__build_path2(char *path, int max, struct estat *sts)
{
	int l,i;

	l=strlen(sts->name);
	if (l+1 > max) return 0;

	if (sts->parent)
	{
		i=ops__build_path2(path, max - (l+1), sts->parent);

		/* not enough space ? */
		if (!i) return 0; 
	}
	else
	{
		i=0;
	}

	strcpy(path+i, sts->name);
	path[i+l+0]=PATH_SEPARATOR;
	path[i+l+1]=0;

	return i+l+1;
}


/** -.
 * This function returns the number of characters needed.
 * We don't return success or failure; there should never be a problem, and 
 * if we'd return 0 for success someone might put a \c STOPIF() in the 
 * recursive call below, which would double the size of this function :-) 
 *
 * We don't include the trailing \c \\0, as that would be counted on each 
 * level. */
int ops__calc_path_len(struct estat *sts)
{
	int plen;

	if (sts->parent)
	{
		if (!sts->parent->path_len) 
			ops__calc_path_len(sts->parent);
		/* Include the path separator. */
		plen=sts->parent->path_len+1;
	}
	else
		plen=0;

	sts->path_len = plen + strlen(sts->name);
	return sts->path_len;
}


/** -.
 * This function uses a rotating array of \c cache_entry_t.
 * This means that a few paths will be usable at once;
 * if some path has to be stored for a (possibly indefinite) time it should 
 * be \c strdup()ed, or re-built upon need. 
 *
 * A LRU eviction scheme is used - with last one marked. 
 *
 * If some function modifies that memory, it should set the first
 * char to \c \\0, to signal that it's no longer valid for other users.
 *
 * \todo A further optimization would be to check if a parent is already 
 * present, and append to that path. Similar for a neighbour entry.
 *
 * The \c cache_entry_t::id member is used as a pointer to the struct \ref  
 * estat.
 * */
int ops__build_path(char **value, struct estat *sts)
{
	static struct cache_t *cache=NULL;
	int status, i;
	unsigned needed_space;
	char *data;

/* Please note that in struct \ref estat there's a bitfield, and its member 
 * \ref cache_index must take the full range plus an additional "out of 
 * range" value! */
	STOPIF( cch__new_cache(&cache, 48), NULL);

	/* Look if it's cached. */
	if (sts->cache_index>0 &&
			sts->cache_index<=cache->max && 
			cache->entries[sts->cache_index-1]->id == (cache_value_t)sts &&
			cache->entries[sts->cache_index-1]->data[0])
	{
		/* the found entry has index i; we'd like that to be the LRU. */
		i=sts->cache_index-1;
		DEBUGP("%p found in cache index %d; lru %d",
				sts, i, cache->lru);
		cch__set_active(cache, i);
		goto ex;
	}

	if (!sts->path_len)
		ops__calc_path_len(sts);

	needed_space=sts->path_len+1;

	STOPIF( cch__add(cache, (cache_value_t)sts, NULL, 
				needed_space, &data), NULL);

	/* Now we have an index, and enough space. */
	status=ops__build_path2(data, needed_space, sts);
	if (status == 0)
	{
		/* Something happened with our path length counting -
		 * it's really a bug. */
		BUG("path len counting went wrong");
	}

	data[status-1]=0;
	sts->cache_index=cache->lru+1;
	status=0;

ex:
//	DEBUGP("status=%d; path=%s", status, cache->entries[cache->lru]->data);
	if (!status) *value=cache->entries[cache->lru]->data;
	return status;
}


/** -.
 * The directory gets by_name removed; by_inode is extended and sorted.
 * \note If this gets called multiple times for the same directory, 
 * depending on the accesses in-between it might be possible to do the 
 * sorting only once.  */
int ops__new_entries(struct estat *dir,
		int count,
		struct estat **new_entries)
{
	int status;


	status=0;
	/* By name is no longer valid. */
	IF_FREE(dir->by_name);
	/* Now insert the newly found entries in the dir list. */
	dir->by_inode = realloc(dir->by_inode, 
			(dir->entry_count+count+1) * sizeof(dir->by_inode[0]));
	STOPIF_ENOMEM(!dir->by_inode);

	memcpy(dir->by_inode+dir->entry_count,
			new_entries, count*sizeof(dir->by_inode[0]));
	dir->entry_count += count;
	dir->by_inode[dir->entry_count]=NULL;

	/* Re-sort the index next time it's needed. */
	dir->to_be_sorted=1;

ex:
	return status;
}


/** -.
 *
 * This function doesn't return \c ENOENT, if no entry is found; \a *sts 
 * will just be \c NULL.
 * */
int ops__find_entry_byname(struct estat *dir, const char *name, 
		struct estat **sts,
		int ignored_too)
{
	int status;
	struct estat **sts_p;
	const char *filename;


	status=0;
	BUG_ON(!S_ISDIR(dir->st.mode));

	if (!dir->by_name)
		STOPIF(dir__sortbyname(dir), NULL);

	/* Strip the path, leave the file name */
	filename=ops__get_filename(name);

	/* find entry, binary search. */
	sts_p=bsearch(filename, dir->by_name, dir->entry_count, 
			sizeof(dir->by_name[0]), 
			(comparison_fn_t)dir___f_sort_by_nameCS);

	if (sts_p)
		DEBUGP("found %s on %p; ignored: 0x%x", name, sts_p,
				(*sts_p)->to_be_ignored);

	/* don't return removed entries, if they're not wanted */
	*sts=sts_p && (!(*sts_p)->to_be_ignored || ignored_too) ?
		*sts_p : NULL;

	if (!*sts)
		DEBUGP("Searching for %s (%s) found no entry (ignored_too=%d)",
				filename, name, ignored_too);

ex:
	return status;
}


#if 0
// Currently unused
/** -.
 * */
int ops__find_entry_byinode(struct estat *dir, 
		dev_t dev, 
		ino_t inode, 
		struct estat **sts)
{
	int status;
	struct estat **sts_p;
	struct estat sts_cmp;


	status=0;
	BUG_ON(!S_ISDIR(dir->st.mode));

	if (!dir->by_inode) 
		STOPIF(dir__sortbyinode(dir), NULL);

	sts_cmp.st.dev=dev;
	sts_cmp.st.ino=inode;

	/* find entry, binary search. */
	sts_p=bsearch(&sts_cmp, dir->by_inode, dir->entry_count, 
			sizeof(dir->by_inode[0]), 
			(comparison_fn_t)dir___f_sort_by_inode);

	*sts=sts_p && ((*sts_p)->entry_status != FT_IGNORE) ?
		*sts_p : NULL;

ex:
	return status;
}
#endif


/** Inline function to abstract a move. */
inline void ops___move_array(struct estat **array, int index, int len)
{
	DEBUGP("moving index %d in [%d]", index, len);
	/* From A B C D E F i H J K l  NULL
	 * to   A B C D E F H J K l  NULL */
	memmove( array+index, array+index+1,
			/* +1 below is for the NULL */
			(len-index-1+1) * sizeof(*array) );
}


/** -.
 * The returned area is zeroed. */
int ops__allocate(int needed, 
		struct estat **where, int *count)
{
	struct free_estat *free_p;
	int status, remain;
	int returned;


	status=0;
	BUG_ON(needed <=0, "not even a single block needed?");

	DEBUGP("need %d blocks, freelist=%p", needed, free_list);
	if (free_list)
	{
		free_p=free_list;
		VALGRIND_MAKE_MEM_DEFINED(free_p, sizeof(*free_p));

		if (free_p->count <= needed)
		{
			/* Whole free block is used up */
			free_list=free_p->next;

			returned=free_p->count;
			*where=(struct estat*)free_p;
		}
		else
		{
			/* Only part of this block is needed.
			 * We return the "higher" part in memory, so that the free list
			 * is not changed.
			 *
			 * Needed: 3
			 * Free block:  [0 1 2 3 4 5] size=6
			 * Returned:           ^ 
			 *    size=3 after*/
			returned=needed;

			remain=free_p->count-needed;
			*where=((struct estat*)free_p) + remain;
			free_p->count=remain;
			DEBUGP("splitting block; %d remain", remain);
		}

		VALGRIND_MAKE_MEM_DEFINED(*where, sizeof(**where)*returned);
		/* Clear the memory. Not needed for calloc(). */
		memset(*where, 0, sizeof(**where) * returned);
	}
	else
	{
		DEBUGP("no free list, allocating");
		/* No more free entries in free list. Allocate. */
		returned=needed;
		/* Allocate at least a certain block size. */
		if (needed < 8192/sizeof(**where)) 
			needed=8192/sizeof(**where);
		*where=calloc(needed, sizeof(**where));
		STOPIF_ENOMEM(!*where);

		if (needed > returned)
		{
			free_list=(struct free_estat*)(*where+returned);
			free_list->next=NULL;
			free_list->count=needed-returned;
		}
	}

	DEBUGP("giving %d blocks at %p", returned, *where);
	BUG_ON(!returned, "Not even a single block returned!!");
	if (count)
		*count=returned;

	/* The memory is cleared; cache_index == 0 means uninitialized, which is 
	 * exactly what we want. */

	VALGRIND_MAKE_MEM_DEFINED(*where, sizeof(**where) * returned);

ex:
	return status;
}


/** -.
 * The pointer to the entry is set to \c NULL, to avoid re-using. */
int ops__free_entry(struct estat **sts_p)
{
	int i, status;
	struct estat *sts=*sts_p;
	struct free_estat *free_p, *free_p2;
	struct free_estat **prev;
	struct free_estat *block;


	status=0;
	if (sts->old)
		STOPIF( ops__free_entry(& sts->old), NULL);
	if (S_ISDIR(sts->updated_mode))
	{
		BUG_ON(sts->entry_count && !sts->by_inode);

		for(i=0; i<sts->entry_count; i++)
			STOPIF( ops__free_entry(sts->by_inode+i), NULL);

		IF_FREE(sts->by_inode);
		IF_FREE(sts->by_name);
		IF_FREE(sts->strings);
		sts->updated_mode=0;
	}

	/* Clearing the memory here serves no real purpose;
	 * the free list written here overwrites parts.
	 * So we clear on allocate. */

	/* TODO: insert into free list (pointer, element count) with merging. 
	 * That requires finding a free block just below or just above
	 * the current sts, and check if the current and the free can be merged.
	 * Currently the list is just prepended.
	 *
	 * TODO: The list should be sorted in some way.
	 * Possibly by address and size in two trees, to quickly find
	 * the largest free block or the nearest block.
	 */
	/* The freed block stores the list data to other blocks. */
	DEBUGP("freeing block %p", *sts_p);

	block=(struct free_estat*)*sts_p;
	/* Can we merge? */
	free_p=free_list;
	prev=&free_list;
	while (free_p)
	{
		VALGRIND_MAKE_MEM_DEFINED(free_p, sizeof(*free_p));
		if ((char*)block + sizeof(struct estat) == (char*)free_p)
		{
			/* Copy data */
			block->count = free_p->count+1;
			block->next = free_p->next;
			if (prev != &free_list)
				VALGRIND_MAKE_MEM_DEFINED(prev, sizeof(*prev));
			*prev = block;
			if (prev != &free_list)
				VALGRIND_MAKE_MEM_NOACCESS(prev, sizeof(*prev));
			break;
		}

		if ((char*)block == (char*)free_p+sizeof(struct estat)*free_p->count)
		{
			free_p->count++;
			break;
		}

		prev=&free_p->next;
		free_p2=free_p;

		free_p=*prev;

		VALGRIND_MAKE_MEM_NOACCESS(free_p2, sizeof(*free_p2));
	}


	if (free_p)
	{
		DEBUGP("merged to %p; now size %d", block, free_p->count);
		VALGRIND_MAKE_MEM_NOACCESS(free_p, sizeof(*free_p));
	}
	else
	{
		block->next=free_list;
		block->count=1;
		free_list=block;
		DEBUGP("new entry in free list");
	}

	VALGRIND_MAKE_MEM_NOACCESS( block, sizeof(struct estat));

	*sts_p=NULL;

ex:
	return status;
}


/** -.
 * Only one the 3 specifications my be given; the other 2 values must be a 
 * \c NULL resp. \a UNKNOWN_INDEX.
 *
 * If the entry is given via \a sts, but is not found, \c ENOENT is 
 * returned.
 *
 * If an invalid index is given, we mark a \a BUG(). 
 *
 * \todo Use a binary search in the \c by_inode and \c by_name arrays. */
int ops__delete_entry(struct estat *dir, 
		struct estat *sts, 
		int index_byinode, 
		int index_byname)
{
	int i;
	int status;

	BUG_ON( (sts ? 1 : 0) +
			(index_byinode >=0 ? 1 : 0) +
			(index_byname >=0 ? 1 : 0) 
			!= 1,
			"must have exactly 1 definition!!!");


	BUG_ON(!S_ISDIR(dir->st.mode), "can remove only from directory");

	if (!sts)
	{
		if (index_byinode != UNKNOWN_INDEX)
		{
			BUG_ON(index_byinode > dir->entry_count, "i > c");
			sts=dir->by_inode[index_byinode];
		}
		else
		{
			BUG_ON(index_byname > dir->entry_count, "i > c");
			sts=dir->by_name[index_byname];
		}
	}


	i=0;
	if (dir->by_inode)
	{
		if (index_byinode == UNKNOWN_INDEX)
		{
			/* Maybe use ops__find_entry_byinode ? Would be faster for large 
			 * arrays - but the bsearch wouldn't return an index, only a pointer.  
			 * */
			for(index_byinode=dir->entry_count-1; 
					index_byinode>=0; 
					index_byinode--)
				if (dir->by_inode[index_byinode] == sts) break;

			BUG_ON(index_byinode == UNKNOWN_INDEX);
		}

		ops___move_array(dir->by_inode, index_byinode,
				dir->entry_count);
		i=1;
	}

	if (dir->by_name)
	{
		if (index_byname == UNKNOWN_INDEX)
		{
			/* Maybe use ops__find_entry_byname? Would do a binary search, but 
			 * using string compares. */
			for(index_byname=dir->entry_count-1; 
					index_byname>=0; 
					index_byname--)
				if (dir->by_name[index_byname] == sts) break;

			BUG_ON(index_byname == UNKNOWN_INDEX);
		}

		ops___move_array(dir->by_name, index_byname,
				dir->entry_count);
		i=1;
	}

	STOPIF( ops__free_entry(&sts), NULL);

ex:
	DEBUGP("entry count was %d; flag to remove is %d",
			dir->entry_count, i);
	if (i)
		dir->entry_count--;
	return i ? 0 : ENOENT;
}


/** -.
 * An entry is marked by having estat::to_be_ignored set; and such entries 
 * are removed here.
 *
 * If \a fast_mode is set, the entries are get removed from the list are 
 * not free()d, nor do the pointer arrays get resized. */
int ops__free_marked(struct estat *dir, int fast_mode)
{
	struct estat **src, **dst;
	int i, new_count;
	int status;


	BUG_ON(!S_ISDIR(dir->st.mode));
	status=0;

	IF_FREE(dir->by_name);

	src=dst=dir->by_inode;
	new_count=0;
	for(i=0; i<dir->entry_count; i++)
	{
		if (!(*src)->to_be_ignored)
		{
			*dst=*src;
			dst++;
			new_count++;
		}
		else
		{
			if (!fast_mode)
				STOPIF( ops__free_entry(src), NULL);
		}

		src++;
	}

	if (new_count != dir->entry_count)
	{
		if (!fast_mode)
		{
			/* resize by_inode - should never give NULL. */
			dir->by_inode=realloc(dir->by_inode, 
					sizeof(*(dir->by_inode)) * (new_count+1) );
			BUG_ON(!dir->by_inode);
		}

		dir->by_inode[new_count]=NULL;
		dir->entry_count=new_count;
	}

ex:
	return status;
}


/** -.
 * Does not modify path.
 *
 * The \a flags parameter tells about the policy regarding tree walking.
 *
 * For \a add, \a unversion we need to create the given path with the 
 * specified flags; in \a add it should exist, for unversion is needs not.
 * For \a diff / \a info we only walk the tree without creating or checking 
 * for current status (info, repos/repos diff for removed files).
 * For \a prop_set / \a prop_get / \a prop_list we need an existing path, 
 * which might be not versioned currently.
 * For \a revert we need to look in the tree, and find removed entries, 
 * too.
 * In \a waa__partial_update() (status check with given subtrees) we create 
 * the paths as necessary. If they do not exist we'd like to print them as  
 * removed.
 *
 * So we need to know:
 *  - Create paths or walk only (\a OPS__CREATE)
 *  - Has the given path to exist? (\a OPS__FAIL_NOT_LIST)
 *    - Should we update this entry, or all below? (OPS__ON_UPD_LIST)
 *  - Which flags the newly created entries should get (in \a sts_flags)
 * */
int ops__traverse(struct estat *current, char *fullpath, 
		int flags,
		int sts_flags,
		struct estat **ret)
{
	int status;
	char *next_part;
	struct estat *sts;
	int quit;
	char *copy, *path;


	status=0;
	copy=strdup(fullpath);
	STOPIF_ENOMEM(!copy);
	path=copy;

	quit=0;
	while (path)
	{
		next_part=(char*)ops___split_fnpart(path);

		BUG_ON(!path[0]);

		/* Check special cases. */
		if (path[0] == '.' && 
				path[1] == '\0')
		{
			/* This happens for the start of a wc-relative path: ./dir/file */
			path=next_part;
			continue;
		}

		if (path[0] == '.' && 
				path[1] == '.' &&
				path[2] == '\0')
		{
			/* This shouldn't happen; the paths being worked on here should be 
			 * normalized.  */
			BUG("Path '%s' includes '..'!", fullpath);
		}


		/* Look in this directory for the wanted entry.
		 * If there's an ignored entry, we'll take that, too. */
		STOPIF( ops__find_entry_byname(current, path, &sts, 1), NULL);

		if (!sts)
		{
			/* If we may not create it, print the optional warning, and possibly  
			 * return an error.
			 * Print no error message, as the caller may want to catch this. */
			if (!(flags & OPS__CREATE))
			{
				if (flags & OPS__FAIL_NOT_LIST)
					STOPIF_CODE_ERR( 1, ENOENT,
							"!The entry '%s' was not found.", fullpath);

				status=ENOENT;
				goto ex;
			}

			/* None found, make a new. */
			STOPIF( ops__allocate(1, &sts, NULL), NULL);
			sts->name=strdup(path);
			STOPIF_ENOMEM(!sts->name);

			if (flags & OPS__ON_UPD_LIST)
				STOPIF( waa__insert_entry_block(sts, 1), NULL);

			/* Fake a directory node. */
			sts->st.mode=S_IFDIR | 0700;
			sts->st.size=0;
			sts->entry_count=0;
			sts->parent=current;
			/* Add that directory with the next commit. */
			sts->flags=sts_flags | RF_ISNEW;


			STOPIF( ops__new_entries(current, 1, &sts), NULL);
		}

		current=sts;

		path=next_part;
	}

	*ret=current;

ex: 
	IF_FREE(copy);
	return status;
}


/** -.
 *
 * The parent directory should already be done, so that removal of whole 
 * trees is done without doing unneeded \c lstat()s.
 *
 * Depending on \c o_chcheck a file might be checked for changes by a MD5 
 * comparision.
 *
 * Per default \c only_check_status is not set, and the data from \c 
 * lstat() is written into \a sts. Some functions need the \b old values 
 * and can set this flag; then only \c entry_status is modified.
 *
 * If \a output is not NULL, then it is overwritten, and \a sts->st is not 
 * changed - independent of \c only_check_status. In case of a removed 
 * entry \a *output is not changed. */
int ops__update_single_entry(struct estat *sts, struct sstat_t *output)
{
	int status;
	struct sstat_t st;
	int i;
	char *fullpath;


	STOPIF( ops__build_path(&fullpath, sts), NULL);

	/* If we see that the parent has been removed, there's no need
	 * to check this entry - the path will surely be invalid. */
	if (sts->parent)
		if (sts->parent->entry_status & FS_REMOVED)
		{
			goto removed;
		}

	/* Check for current status */
	status=hlp__lstat(fullpath, &st);

	if (status)
	{
		DEBUGP("lstat whines %d", status);

		/* only valid error is ENOENT - then this entry has been removed */
		/* If we did STOPIF_CODE_ERR(status != ENOENT ...), then status
		 * would be overwritten with the value of the comparison. */
		if (status != ENOENT) 
			STOPIF(status, "cannot lstat(%s)", fullpath);

removed:
		/* Re-set the values, if needed */
		if (st.mode)
			memset(&st, 0, sizeof(st));

		sts->entry_status=FS_REMOVED;
		/* Only ENOENT gets here, and that's ok. */
		status=0;
	}
	else
	{
		/* Entry exists. Check for changes. */
		sts->entry_status=ops__stat_to_action(sts, &st);

		/* May we print a '?' ? */
		if ( ((opt__get_int(OPT__CHANGECHECK) & CHCHECK_FILE) && 
					(sts->entry_status & FS_LIKELY)) ||
				(opt__get_int(OPT__CHANGECHECK) & CHCHECK_ALLFILES) )
		{
			/* If the type changed (symlink => file etc.) there's no 'likely' - 
			 * the entry *was* changed.
			 * So if we get here, we can check either type - st or sts->st. */
			if (S_ISREG(st.mode) || S_ISLNK(st.mode))
			{
				/* make sure, one way or another */
				STOPIF( cs__compare_file(sts, fullpath, &i), NULL);

				if (i>0)
					sts->entry_status= (sts->entry_status & ~ FS_LIKELY) | FS_CHANGED;
				else if (i==0)
					sts->entry_status= sts->entry_status  & ~(FS_LIKELY  | FS_CHANGED);
			}
			/* Directories will be checked later, on finishing their children; 
			 * devices have already been checked, and other types are not 
			 * allowed. */
		}
	}

	/* Now we've compared we take the new values.
	 * Better for display, needed for commit (current values) */
	/* Before an update (and some other operations) we only set 
	 * sts->entry_status - to keep the old values intact. */
	if (output)
		*output=st;
	else
		if (!only_check_status)
			sts->st=st;

	DEBUGP("known %s: action=%X, flags=%X, mode=0%o, status=%d",
			fullpath, sts->entry_status, sts->flags, sts->updated_mode, status);
	sts->updated_mode=st.mode;

ex:
	return status;
}


/** Set the estat::do_* bits, depending on the parent.
 * May not be called for the root.
 * */
inline void ops___set_todo_bits(struct estat *sts)
{
	/* For recursive operation: If we should do the parent completely, we do 
	 * the sub-entries, too. */
	if (opt_recursive>0)
		sts->do_userselected |= sts->parent->do_userselected;
	/* For semi-recursive operation: Do the child, if the parent was 
	 * wanted. */
	if (opt_recursive>=0)
		sts->do_this_entry |= sts->parent->do_userselected | sts->do_userselected;
}


/** -.
 * May not be called for the root. */
int ops__set_todo_bits(struct estat *sts)
{
	int status;

	status=0;

	/* We don't know any better yet. */
	sts->do_filter_allows=1;
	sts->do_filter_allows_done=1;

	ops___set_todo_bits(sts);

	DEBUGP("user,this,child=%d.%d parent=%d.%d", 
			sts->do_userselected, 
			sts->do_this_entry,
			sts->parent ? sts->parent->do_userselected : 0,
			sts->parent ? sts->parent->do_this_entry : 0);

	return status;
}


/** -.
 *
 * Calls \c ops__set_to_handle_bits() and maybe \c 
 * ops__update_single_entry(), and depending on the filter settings \c 
 * sts->do_this_entry might be cleared.
 * */
int ops__update_filter_set_bits(struct estat *sts)
{
	int status;
	struct sstat_t stat;

	if (sts->parent)
		STOPIF( ops__set_todo_bits(sts), NULL);

	if (sts->do_this_entry)
	{
		STOPIF( ops__update_single_entry(sts, &stat), NULL);

		if (ops__calc_filter_bit(sts))
		{
			/* We'd have an invalid value if the entry is removed. */
			if ((sts->entry_status & FS_REPLACED) != FS_REMOVED)
				if (!only_check_status)
					sts->st = stat;
		}
	}

	DEBUGP("filter says %d", sts->do_filter_allows);

ex:
	return status;
}

/** -.
 *
 * We have to preserve the \c parent pointer and the \c name of \a dest.  
 * */
void ops__copy_single_entry(struct estat *src, struct estat *dest)
{
	dest->st=src->st;

	dest->repos_rev=SVN_INVALID_REVNUM;
	/* parent is kept */
	/* name is kept */

	/* But, it being a non-committed entry, it has no URL yet. */
	dest->url=NULL;


	if (S_ISDIR(dest->st.mode))
	{
#if 0
			/* Currently unused. */
			dest->by_inode=NULL;
			dest->by_name=NULL;
			dest->entry_count=0;
			dest->strings=NULL;
			dest->other_revs=0;
			dest->to_be_sorted=0;
#endif
	}
	else
	{
		memcpy(dest->md5, src->md5, sizeof(dest->md5));
#if 0
		{
			memset(dest->md5, 0, sizeof(dest->md5));
			/* Currently unused. */
			dest->change_flag=CF_NOTCHANGED;
			dest->decoder=src->decoder;
			dest->has_orig_md5=src->has_orig_md5;
		}
#endif
	}

#if 0
		/* The temporary area is mostly void, but to be on the safe side ... */
		dest->child_index=0;
		dest->dir_pool=NULL;
#endif

	dest->flags=RF_ISNEW | RF_COPY_SUB;

	/* Gets recalculated on next using */
	dest->path_len=0;
	dest->path_level=dest->parent->path_level+1;

	/* The entry is not marked as to-be-ignored ... that would change the 
	 * entry type, and we have to save it anyway. */
	dest->entry_status=FS_NEW;
	dest->remote_status=FS_NEW;

	dest->cache_index=0;
	dest->decoder_is_correct=src->decoder_is_correct;

	dest->was_output=0;
	dest->do_userselected = dest->do_child_wanted = dest->do_this_entry = 0;
	dest->arg=NULL;
}


/** -.
 * \a only_A, \a both, and \a only_B are called, then \a for_every (if not 
 * \c NULL).
 *
 * This builds and loops throught the sts::by_name lists, so modifying them 
 * must be done carefully, to change only the elements already processed.
 *
 * Returning an error from any function stops the loop.
 * */
int ops__correlate_dirs(struct estat *dir_A, struct estat *dir_B,
		ops__correlate_fn1_t only_A,
		ops__correlate_fn2_t both,
		ops__correlate_fn1_t only_B,
		ops__correlate_fn2_t for_every)
{
	int status, comp;
	struct estat **list_A, **list_B;


	status=0;
	DEBUGP("correlating %s and %s", dir_A->name, dir_B->name);

	/* We compare the sorted list of entries. */
	STOPIF( dir__sortbyname(dir_A), NULL);
	STOPIF( dir__sortbyname(dir_B), NULL);

	list_A=dir_A->by_name;
	list_B=dir_B->by_name;

	while (*list_A)
	{
		if (!*list_B) goto a_only;

		comp=dir___f_sort_by_name( list_A, list_B );
		DEBUGP("comp %s, %s => %d", 
				(*list_A)->name,
				(*list_B)->name, comp);

		if (comp == 0)
		{
			/* Identical names */
			if (both) 
				STOPIF( both(*list_A, *list_B), NULL);
			if (for_every) 
				STOPIF( for_every(*list_A, *list_B), NULL);

			list_A++;
			list_B++;
		}
		else if (comp > 0)
		{
			/* *list_B > *list_A; entry is additional in list_B. */
			if (only_B) 
				STOPIF( only_B(*list_B, list_B), NULL);
			if (for_every) 
				STOPIF( for_every(NULL, *list_B), NULL);

			list_B++;
		}
		else
		{
a_only:
			/* *list_A < *list_B; so this entry does not exist in dir_B. */
			if (only_A) 
				STOPIF( only_A(*list_A, list_A), NULL);
			if (for_every) 
				STOPIF( for_every(*list_A, NULL), NULL);

			list_A++;
		}
	}

	/* Do remaining list_B entries, if necessary. */
	if (only_B || for_every)
	{
	  while (*list_B)
		{
			if (only_B) 
				STOPIF( only_B(*list_B, list_B), NULL);
			if (for_every) 
				STOPIF( for_every(NULL, *list_B), NULL);

			list_B++;
		}
	}

ex:
	return status;
}


/** -.
 * The specified stream gets rewound, read up to \a max bytes (sane default 
 * for 0), and returned (zero-terminated) in \a *buffer allocated in \a 
 * pool.
 *
 * The real length can be seen via \a real_len.
 *
 * If \a filename is given, the file is removed.
 *
 * If \a pool is \c NULL, the space is \c malloc()ed and must be \c free()d 
 * by the caller.
 * */
/* mmap() might be a bit faster; but for securities' sake we put a \0 at 
 * the end, which might not be possible with a readonly mapping (although 
 * it should be, by using MAP_PRIVATE - but that isn't available with 
 * apr_mmap_create(), at least with 1.2.12).  */
int ops__read_special_entry(apr_file_t *a_stream,
		char **data,
		int max, ssize_t *real_len,
		char *filename,
		apr_pool_t *pool)
{
	int status;
	apr_off_t special_len, bof;
	apr_size_t len_read;
	char *special_data;


	status=0;
	special_len=0;


	/* Remove temporary file. Can be done here because we still have the 
	 * handle open.  */
	if (filename)
		STOPIF_CODE_ERR( unlink(filename) == -1, errno,
				"Cannot remove temporary file \"%s\"", filename);


	/* Get length */
	STOPIF( apr_file_seek(a_stream, APR_CUR, &special_len), NULL);

	/* Some arbitrary limit ... */
	if (!max) max=8192;
	STOPIF_CODE_ERR( special_len > max, E2BIG, 
			"!The special entry \"%s\" is too long (%llu bytes, max %llu).\n"
			"Please contact the dev@ mailing list.",
			filename, (t_ull)special_len, (t_ull)max);


	/* Rewind */
	bof=0;
	STOPIF( apr_file_seek(a_stream, APR_SET, &bof), NULL);

	special_data= pool ? 
		apr_palloc( pool, special_len+1) : 
		malloc(special_len+1);
	STOPIF_ENOMEM(!special_data);


	/* Read data. */
	len_read=special_len;
	STOPIF( apr_file_read( a_stream, special_data, &len_read), NULL);
	STOPIF_CODE_ERR( len_read != special_len, ENODATA, 
			"Reading was cut off at byte %llu of %llu",
			(t_ull)len_read, (t_ull)special_len);
	special_data[len_read]=0;

	DEBUGP("got special value %s", special_data);

	if (real_len) *real_len=special_len;
	*data=special_data;

ex:
	return status;
}


/** -.
 * */
int ops__are_children_interesting(struct estat *dir)
{
	struct estat tmp;

	tmp.parent=dir;
	tmp.do_this_entry = tmp.do_userselected = tmp.do_child_wanted = 0;

	ops___set_todo_bits(&tmp);

	return tmp.do_this_entry;
}

