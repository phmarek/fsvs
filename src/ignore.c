/************************************************************************
 * Copyright (C) 2005-2009 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <pcre.h>
#include <sys/mman.h>


#include "global.h"
#include "interface.h"
#include "waa.h"
#include "est_ops.h"
#include "helper.h"
#include "warnings.h"
#include "direnum.h"
#include "ignore.h"
#include "url.h"


/** \file
 * \ref groups and \ref ignore command and functions.
 * */

/* \note Due to restriction in C-comment syntax the above 
 * cases have to separate \c * and \c / to avoid breaking 
 * the code. \c * and \c / would belong together.
 * 
 * As a fix I wrote /§* and *§/, which get changed by a perl 
 * script after generation. */

/**
 * \addtogroup cmds
 * \section groups
 * \anchor ignore
 *
 * \code
 * fsvs groups dump|load
 * fsvs groups [prepend|append|at=n] group-definition [group-def ...]
 * fsvs ignore [prepend|append|at=n] pattern [pattern ...]
 * fsvs groups test [-v|-q] [pattern ...]
 * \endcode
 *
 * This command adds patterns to the end of the pattern list, or, with \c 
 * prepend, puts them at the beginning of the list.
 * With \c at=x the patterns are inserted at the position \c x , 
 * counting from 0.
 *
 * The difference between \c groups and \c ignore is that \c groups \b 
 * requires a group name, whereas the latter just assumes the default group 
 * \c ignore.
 * 
 * For the specification please see the related 
 * \ref groups_format "documentation" .
 * 
 * <tt>fsvs dump</tt> prints the patterns to \c STDOUT . If there are
 * special characters like \c CR or \c LF embedded in the pattern 
 * <b>without encoding</b> (like \c \\r or \c \\n), the 
 * output will be garbled.
 * 
 * The patterns may include \c * and \c ? as wildcards in one directory 
 * level, or \c ** for arbitrary strings.
 *
 * These patterns are only matched against new (not yet known) files; 
 * entries that are already versioned are not invalidated. \n
 * If the given path matches a new directory, entries below aren't found, 
 * either; but if this directory or entries below are already versioned, 
 * the pattern doesn't work, as the match is restricted to the directory.
 *
 * So:
 * \code
 *     fsvs ignore ./tmp
 * \endcode
 * ignores the directory \c tmp; but if it has already been committed,
 * existing entries would have to be unmarked with \ref unversion 
 * "fsvs unversion".
 * Normally it's better to use
 * \code
 *     fsvs ignore ./tmp/§**
 * \endcode
 * as that takes the directory itself (which might be needed after restore
 * as a mount point anyway), but ignore \b all entries below. \n
 * Currently this has the drawback that mtime changes will be reported and 
 * committed; this is not the case if the whole directory is ignored.
 * 
 *
 * Examples:
 * \code
 *     fsvs group group:unreadable,mode:4:0
 *     fsvs group 'group:secrets,/etc/§*shadow'
 *
 *     fsvs ignore /proc
 *     fsvs ignore /dev/pts
 *     fsvs ignore './var/log/§*-*'
 *     fsvs ignore './§**~'
 *     fsvs ignore './§**§/§*.bak'
 *     fsvs ignore prepend 'take,./§**.txt'
 *     fsvs ignore append 'take,./§**.svg'
 *     fsvs ignore at=1 './§**.tmp'
 *
 *     fsvs group dump
 *     fsvs group dump -v
 *
 *     echo "./§**.doc" | fsvs ignore load
 *     # Replaces the whole list
 * \endcode
 *
 * \note Please take care that your wildcard patterns are not expanded
 * by the shell!
 *
 *
 * \subsection groups_test Testing patterns
 *
 * To see more easily what different patterns do you can use the \c test 
 * subcommand. The following combinations are available:<UL>
 * <li><tt>fsvs groups test \e pattern</tt>\n
 * Tests \b only the given pattern against all new entries in your working 
 * copy, and prints the matching paths. The pattern is not stored in the 
 * pattern list.
 * <li><tt>fsvs groups test</tt>\n
 * Uses the already defined patterns on the new entries, and prints the 
 * group name, a tab, and the path.\n
 * With \c -v you can see the matching pattern in the middle column, too.
 * </ul>
 *
 * By using \c -q you can avoid getting the whole list; this makes sense if 
 * you use the \ref o_group_stats "group_stats" option at the same time.
 */

/**
 * \addtogroup cmds
 * \section rign
 *
 * \code
 * fsvs rel-ignore [prepend|append|at=n] path-spec [path-spec ...]
 * fsvs ri [prepend|append|at=n] path-spec [path-spec ...]
 * \endcode
 * 
 * If you keep the same repository data at more than one working copy on 
 * the same machine, it will be stored in different paths - and that makes 
 * absolute ignore patterns infeasible. But relative ignore patterns are 
 * anchored at the beginning of the WC root - which is a bit tiring to type 
 * if you're deep in your WC hierarchy and want to ignore some files.
 * 
 * To make that easier you can use the \c rel-ignore (abbreviated as \c ri) 
 * command; this converts all given path-specifications (which may include 
 * wildcards as per the shell pattern specification above) to WC-relative 
 * values before storing them.
 * 
 * Example for \c /etc as working copy root:
 * \code
 * 		fsvs rel-ignore '/etc/X11/xorg.conf.*'
 * 
 * 		cd /etc/X11
 * 		fsvs rel-ignore 'xorg.conf.*'
 * \endcode
 * Both commands would store the pattern "./X11/xorg.conf.*".
 * 
 * \note This works only for \ref ign_shell "shell patterns".
 *
 * For more details about ignoring files please see the \ref ignore command 
 * and \ref groups_format.
 */

/**
 * \defgroup ignpat_dev Developers' reference
 * \ingroup add_unv
 * 
 * Internal structure, and some explanations.
 *
 * The ignore lists are first loaded into a global array.
 * Then they should be distributed onto the directory structure;
 * all applicable patterns get referenced by a directory.
 *
 * \todo Currently all patterns get tested against all new entries. This 
 * does not seem to be a performance problem.
 * 
 * Eg this directory tree
 * \code
 *     root
 *       +-- dirA
 *       +-- dirB
 *             +-- dirB1
 * \endcode
 * with these ignore patterns 
 * \code
 *        *.tmp
 *        **~
 *        dirA/tmp*.lst
 *        dirB/§**§/§*.o
 * \endcode
 * would result in
 * \code
 *     root:               *.tmp, **~
 *       +-- dirA          **~, tmp*.lst
 *       +-- dirB          **§/§*.o
 *             +-- dirB1   **§/§*.o
 * \endcode
 *
 * Ignore patterns apply only to \b new entries, ie. entries already
 * known stay known.
 *
 * That's why we need an "add" command:
 * \code
 *     $ fsvs ignore '/proc/§*'
 *     $ fsvs add /proc/stat
 * \endcode
 * would version \c /proc/stat , but nothing else from \c /proc .
 * 
 * A negative ignore-list is named \e take list.
 * 
 * The storage format is
 * \code
 *     header: number of entries
 *     %u\n
 *     pattern\0\n
 *     pattern\0\n
 * \endcode
 * 
 * Whitespace are not allowed at the start of a pattern; use <c>./§*</c>
 * or something similar.
 * 
 * As low-level library pcre is used, the given shell-patterns are 
 * translated from the shell-like syntax into PCREs.
 * \code
 *     *		->	[^/]*
 *     **		->	.*
 *     ?		->	.
 *     .		->	\.
 * \endcode
 * All other \c \\W are escaped.
 * 
 **/

// * This part answers the "why" and "how" for ignoring entries.

/** \defgroup groups_spec Using grouping patterns
 * \ingroup userdoc
 *
 *
 * Patterns are used to define groups for new entries; a group can be used 
 * to ignore the given entries, or to automatically set properties when the 
 * entry is taken on the entry list.
 * 
 * So the auto-props are assigned when the entry gets put on the internal 
 * list; that happens for the \ref add, \ref prop-set or \ref prop-del, and 
 * of course \ref commit commands. \n
 * To override the auto-props of some new entry just use the property 
 * commands.
 *
 *
 * \section ign_overview Overview
 *
 * When \c FSVS walks through your working copy it tries to find \b new 
 * (ie.  not yet versioned) entries. Every \b new entry gets tested against 
 * the defined grouping patterns (in the given order!); if a pattern 
 * matches, the corresponding group is assigned to the entry, and no 
 * further matching is done.
 *
 * See also \ref howto_entry_statii "entry statii".
 *
 * \subsection ign_g_ignore Predefined group 1: "ignore"
 *
 * If an entry gets a group named \c "ignore" assigned, it will not be 
 * considered for versioning.
 *
 * This is the only \b really special group name.
 *
 * \subsection ign_g_take Predefined group 2: "take"
 *
 * This group mostly specifies that no further matching is to be done, so 
 * that later \ref ign_g_ignore "ignore" patterns are not tested.
 *
 * Basically the \c "take" group is an ordinary group like all others; it 
 * is just predefined, and available with a 
 * \ref ign_mod_t "short-hand notation".
 *
 *
 * \section ignpat_why Why should I ignore files?
 *
 * Ignore patterns are used to ignore certain directory
 * entries, where versioning makes no sense. If you're
 * versioning the complete installation of a machine, you wouldn't care to
 * store the contents of \c /proc (see <tt>man 5 proc</tt>), or possibly
 * because of security reasons you don't want \c /etc/shadow , \c
 * /etc/sshd/ssh_host_*key , and/or other password- or key-containing 
 * files.
 * 
 * Ignore patterns allow you to define which directory entries (files,
 * subdirectories, devices, symlinks etc.) should be taken, respectively
 * ignored.
 *
 *
 * \section ignpat_why_groups Why should I assign groups?
 *
 * The grouping patterns can be compared with the \c auto-props feature of 
 * subversion; it allows automatically defining properties for new entries, 
 * or ignoring them, depending on various criteria.
 *
 * For example you might want to use encryption for the files in your 
 * users' \c .ssh directory, to secure them against unauthorized access in 
 * the repository, and completely ignore the private key files:
 *
 * Grouping patterns:
 * \code
 * 	group:ignore,/home/§*§/.ssh/id*
 * 	group:encrypt,/home/§*§/.ssh/§**
 * \endcode
 * And the \c $FSVS_CONF/groups/encrypt file would have a definition for 
 * the <tt>fsvs:commit-pipe</tt> (see the \ref s_p_n "special properties"). 
 *
 *
 * \section ignpat_groupdef Syntax of group files
 *
 * A group definition file looks like this:<ul>
 * <li>Whitespace on the beginning and the end of the line is ignored.
 * <li>Empty lines, and lines with the first non-whitespace character being 
 * \c '#' (comments) are ignored.
 * <li>It can have \b either the keywords \c ignore or \c take; if neither 
 * is specified, the group \c ignore has \c ignore as default (surprise, 
 * surprise!), and all others use \c take.
 * <li>An arbitrary (small) number of lines with the syntax\n
 * <tt>auto-prop <i>property-name</i> <i>property-value</i></tt> can be 
 * given; \e property-name may not include whitespace, as there's no 
 * parsing of any quote characters yet.
 * </UL>
 *
 * An example:
 * \code
 *   # This is a comment
 *     # This is another
 *
 *   auto-props    fsvs:commit-pipe    gpg -er admin@my.net
 *
 *   # End of definition
 * \endcode
 *
 *
 * \section groups_format Specification of groups and patterns
 *
 * While an ignore pattern just needs the pattern itself (in one of the 
 * formats below), there are some modifiers that can be additionally 
 * specified:
 * \code
 *   [group:{name},][dir-only,][insens|nocase,][take,][mode:A:C,]pattern
 * \endcode
 * These are listed in the section \ref ign_mod below.
 *
 *
 * These kinds of ignore patterns are available:
 * 
 * \section ign_shell Shell-like patterns
 * 
 * These must start with <tt>./</tt>, just like a base-directory-relative 
 * path.
 * \c ? , \c * as well as character classes \c [a-z] have their usual
 * meaning, and \c ** is a wildcard for directory levels.
 *
 * You can use a backslash \c \\ outside of character classes to match
 * some common special characters literally, eg. \c \\* within a pattern 
 * will match a literal asterisk character within a file or directory name.
 * Within character classes all characters except \c ] are treated
 * literally. If a literal \c ] should be included in a character class,
 * it can be placed as the first character or also be escaped using a
 * backslash.
 *
 * Example for \c / as the base-directory
 * \code
 *     ./[oa]pt
 *     ./sys
 *     ./proc/§*
 *     ./home/§**~
 * \endcode
 *
 * This would ignore files and directories called \c apt or \c opt in the
 * root directory (and files below, in the case of a directory), the
 * directory \c /sys and everything below, the contents of \c /proc
 * (but take the directory itself, so that upon restore it gets created
 * as a mountpoint), and all entries matching \c *~ in and below
 * \c /home .
 *
 * \note The patterns are anchored at the beginning and the end. So a 
 * pattern <tt>./sys</tt> will match \b only a file or directory named \c 
 * sys. If you want to exclude a directories' files, but not the directory 
 * itself, use something like <tt>./dir/§*</tt> or <tt>./dir/§**</tt>
 *
 * If you're deep within your working copy and you'd like to ignore some 
 * files with a WC-relative ignore pattern, you might like to use the 
 * \ref rign "rel-ignore" command.
 * 
 *
 * \subsection ignpat_shell_abs Absolute shell patterns
 *
 * There is another way to specify shell patterns - using absolute paths.  
 * \n
 * The syntax is similar to normal shell patterns; but instead of the 
 * <tt>./</tt> prefix the full path, starting with \c /, is used.
 *
 * \code
 *		 /etc/§**.dpkg-old
 *		 /etc/§**.dpkg-bak
 *		 /§**.bak
 *		 /§**~
 * \endcode
 *
 * The advantage of using full paths is that a later \c dump and \c load in 
 * another working copy (eg. when moving from versioning \c /etc to \c /) 
 * does simply work; the patterns don't have to be modified.
 *
 * Internally this simply tries to remove the working copy base directory 
 * at the start of the patterns (on loading); then they are processed as 
 * usual.
 *
 * If a pattern does \b not match the wc base, and neither has the 
 * wild-wildcard prefix <tt>/§**</tt>, a \ref warn_ign_abs_not_base 
 * "warning" is issued.
 *
 * 
 * 
 * \section ignpat_pcre PCRE-patterns
 * 
 * PCRE stands for Perl Compatible Regular Expressions; you can read about
 * them with <tt>man pcre</tt> (if the manpages are installed), and/or
 * <tt>perldoc perlre</tt> (if perldoc is installed). \n
 * If both fail for you, just google it.
 * 
 * These patterns have the form <tt>PCRE:{pattern}</tt>, with \c PCRE in
 * uppercase.
 * 
 * An example:
 * \code
 *     PCRE:./home/.*~
 * \endcode
 * This one achieves exactly the same as <tt>./home/§**~</tt> .
 * 
 * Another example:
 * \code
 *     PCRE:./home/[a-s]
 * \endcode
 *
 * This would match \c /home/anthony , \c /home/guest , \c /home/somebody 
 * and so on, but would not match \c /home/theodore .
 * 
 * One more:
 * \code
 *     PCRE:./.*(\.(tmp|bak|sik|old|dpkg-\w+)|~)$
 * \endcode
 *
 * Note that the pathnames start with \c ./ , just like above, and that the
 * patterns are anchored at the beginning. To additionally anchor at the 
 * end you could use a <tt>$</tt> at the end.
 * 
 * 
 * \section ign_dev Ignoring all files on a device
 * 
 * Another form to discern what is needed and what not is possible with
 * <tt>DEVICE:[&lt;|&lt;=|&gt;|&gt;=]major[:minor]</tt>.
 * 
 * This takes advantage of the major and minor device numbers of inodes 
 * (see <tt>man 1 stat</tt> and <tt>man 2 stat</tt>).
 * 
 * The rule is as follows:
 * - Directories have their parent matched against the given string
 * - All other entries have their own device matched.
 * 
 * This is because mount-points (ie. directories where other
 * filesystems get attached) show the device of the mounted device, but 
 * should be versioned (as they are needed after restore); all entries (and 
 * all binding mounts) below should not.
 * 
 * The possible options \c &lt;= or \c &gt;= define a less-or-equal-than 
 * respective bigger-or-equal-than relationship, to ignore a set of device 
 * classes.
 * 
 * Examples:
 * \code
 *     tDEVICE:3
 *     ./§*
 * \endcode
 * This patterns would define that all filesystems on IDE-devices (with 
 * major number 3) are \e taken , and all other files are	ignored.
 * 
 * \code
 *    DEVICE:0
 * \endcode
 * This would ignore all filesystems with major number 0 - in	linux these 
 * are the \e virtual filesystems ( \c proc , \c sysfs , \c devpts , etc.;  
 * see \c /proc/filesystems , the lines with \c nodev ).
 * 
 * Mind NFS and smb-mounts, check if you're using \e md , \e lvm and/or
 * \e device-mapper !
 * 
 * 
 * Note: The values are parsed with \c strtoul() , so you can use decimal,
 * hexadecimal (by prepending \c "0x", like \c "0x102") and octal (\c "0", 
 * like \c "0777") notation.
 * 
 * 
 * \section ign_inode Ignoring a single file, by inode
 * 
 * At last, another form to ignore entries is to specify them via the
 * device they are on and their inode:
 * \code
 *     INODE:major:minor:inode
 * \endcode
 * This can be used if a file can be hardlinked to many places, but only 
 * one copy should be stored. Then one path can be marked as to \e take , 
 * and other instances can get ignored.
 *
 * \note That's probably a bad example. There should be a better mechanism 
 * for handling hardlinks, but that needs some help from subversion.
 * 
 * 
 * \section ign_mod Modifiers
 * 
 * All of these patterns can have one or more of these modifiers \b before 
 * them, with (currently) optional \c "," as separators; not all 
 * combinations make sense.
 *
 * For patterns with the \c m (mode match) or \c d (dironly) modifiers the 
 * filename pattern gets optional; so you don't have to give an all-match 
 * wildcard pattern (<tt>./§**</tt>) for these cases.
 *
 * 
 * \subsection ign_mod_t "take": Take pattern
 * This modifier is just a short-hand for assigning the group \ref 
 * ign_g_take "take".
 *
 * \subsection ign_mod_ignore "ignore": Ignore pattern
 * This modifier is just a short-hand for assigning the group 
 * \ref ign_g_ignore "ignore".
 *
 * \subsection ign_mod_i "insens" or "nocase": Case insensitive
 * With this modifier you can force the match to be case-insensitive; this 
 * can be useful if other machines use eg. \c samba to access files, and 
 * you cannot be sure about them leaving \c ".BAK" or \c ".bak" behind.
 * 
 * \subsection ign_mod_d "dironly": Match only directories
 * This is useful if you have a directory tree in which only certain files 
 * should be taken; see below.
 *
 * \subsection ign_mod_m "mode": Match entries' mode
 * This expects a specification of two octal values in the form 
 * <tt>m:<i>and_value</i>:<i>compare_value</i></tt>, like <tt>m:04:00</tt>; 
 * the bits set in \c and_value get isolated from the entries' mode, and 
 * compared against \c compare_value.
 *
 * As an example: the file has mode \c 0750; a specification of<UL>
 *   <LI><tt>m:0700:0700</tt> matches,
 *   <LI><tt>m:0700:0500</tt> doesn't; and
 *   <LI><tt>m:0007:0000</tt> matches, but
 *   <LI><tt>m:0007:0007</tt> doesn't.</UL>
 *
 * A real-world example: <tt>m:0007:0000</tt> would match all entries that 
 * have \b no right bits set for \e "others", and could be used to exclude 
 * private files (like \c /etc/shadow). (Alternatively, the \e others-read 
 * bit could be used: <tt>m:0004:0000</tt>.
 * 
 * FSVS will reject invalid specifications, ie. when bits in \c 
 * compare_value are set that are cleared in \c and_value: these patterns 
 * can never match. \n
 * An example would be <tt>m:0700:0007</tt>.
 *
 *
 * \subsection ign_mod_examples Examples
 *
 * \code
 *     take,dironly,./var/vmail/§**
 *     take,./var/vmail/§**§/.*.sieve
 *     ./var/vmail/§**
 * \endcode
 * This would take all \c ".*.sieve" files (or directories) below 
 * \c /var/vmail, in all depths, and all directories there; but no other
 * files.
 *
 * If your files are at a certain depth, and you don't want all other 
 * directories taken, too, you can specify that exactly:
 * \code
 *     take,dironly,./var/vmail/§*
 *     take,dironly,./var/vmail/§*§/§*
 *     take,./var/vmail/§*§/§*§/.*.sieve
 *     ./var/vmail/§**
 * \endcode
 *
 * \code
 *     mode:04:0
 *     take,./etc/
 *     ./§**
 * \endcode
 * This would take all files from \c /etc, but ignoring the files that are 
 * not world-readable (\c other-read bit cleared); this way only "public" 
 * files would get taken.
 * */

/** \section dev_groups Groups
 * \ingroup dev
 *
 * Some thoughts about groups in FSVS.
 *
 * Groups have to be considered as follows:<UL>
 * <li>On commit the auto-props must be used
 * <li>if an entry was <tt>add</tt>ed manually, they should apply as usual
 * <li>unless they're overridden by \c prop-set or \c prop-del
 * </ul>
 *
 * The easiest way seems to be to write the properties in the filesystem 
 * when the entries are being stored in the entry list, ie. at \c add, \c 
 * prop-set, \c prop-del or \c commit time. \n
 * The simplest way to do that would be in \ref waa__output_tree() - we see 
 * that an entry is newly allocated, and push all (not already set) 
 * properties there. \n
 * But that wouldn't work with the \c prop-del command, as an automatically 
 * assigned property wouldn't get removed.
 *
 * So there's the function ops__apply_group(), which is called in the 
 * appropriate places.
 * */


/** All groups, addressed by name. */
apr_hash_t *ign___groups=NULL;

/** The length of the longest group name, used for formatting the status 
 * output.
 * This is initialized to 6, because "ignore" at least takes that much 
 * space - and "(none)" too. */
int ign__max_group_name_len=6;

/* They are only pointers */
#define RESERVE_IGNORE_ENTRIES (4)

/** Header definition - currently only number of entries. */
static const char ign_header_str[] = "%u",
						 ign__group_take[]="take",
						 ign__group_ign[]="ignore";

const char ign___parm_delimiter=',';

/** For how many grouping patterns memory is allocated. */
int max_ignore_entries=0;
		/** How many grouping patterns are actually used. */
int	used_ignore_entries=0;

/** Allocated array of grouping patterns. */
static struct ignore_t *ignore_list=NULL;

/** Place where the patterns are mmap()ed. */
static char *memory;


/** The various strings that define the pattern types.
 * @{ */
static const char 
pcre_prefix[]="PCRE:",
	dev_prefix[]="DEVICE:",
	inode_prefix[]="INODE:",
	norm_prefix[]= { '.', PATH_SEPARATOR, 0 },
	/* The second PATH_SEPARATOR is not needed. */
	wildcard_prefix[]= { PATH_SEPARATOR, '*', '*', 0 },
	/* Should that be "//" to make a clearer difference? */
	abs_shell_prefix[]= { PATH_SEPARATOR, 0 };
/** @} */


/** Processes a character class in shell ignore patterns.
 * */
int ign___translate_bracketed_expr(char *end_of_buffer,
		char **src, char **dest)
{
	int status = 0;
	int pos_in_bracket_expr = -1; // zero-based, -1 == outside
	int backslashed = 0;


	STOPIF(**src != '[',
			"invalid argument, **src does not point to "
			"start of bracket expression");

	do
	{
		if (backslashed)
		{
			/* Escaped mode; blindly copy the next character. */
			*((*dest)++) = *((*src)++);
			backslashed = 0;
			/* pos_in_bracket_expr has already been increased. */
		}
		else if ( pos_in_bracket_expr == 0 &&
				(**src == '!' || **src == '^') )
		{
			*((*dest)++) = '^';
			++(*src);
			/* "!" or "^" at the start of a bracket expression (negation of the 
			 * bracket expression/character class) do not count as a regular 
			 * content element, so pos_in_bracket_expr is left alone. */
		}
		else
		{
			if (**src == ']' && pos_in_bracket_expr > 0)
			{
				/* Bracket expression ends. Set "end of expression"
					 marker and fall through to copy the closing bracket. */
				pos_in_bracket_expr = -1;
			}
			else
			{
				/* Now we're at the next character position. */
				++pos_in_bracket_expr; 
			}

			/* Enter escaped mode? */
			backslashed = (**src == '\\'); 

			*((*dest)++) = *((*src)++);
		}

		/* end_of_buffer points at character after the allocated destination 
		 * buffer space -- *end_of_buffer is invalid/undefined.
		 * Here we just have to be careful to not overwrite the stack - the 
		 * real length check is in ign__compile_pattern(). */
		STOPIF_CODE_ERR( end_of_buffer - *dest < 5, ENOSPC,
				"not enough space in buffer");
	}
	while(**src && pos_in_bracket_expr >= 0);


ex:
	return status;
}


/** Compiles the given pattern for use with \c PCRE. 
 * */
int ign__compile_pattern(struct ignore_t *ignore)
{
	const char *err;
	int offset;
	int len;
	char *buffer;
	char *src, *dest;
	int status;
	int backslashed;


	status=0;
	if (ignore->type == PT_PCRE)
		dest=ignore->compare_string;
	else if (ignore->type == PT_SHELL ||
			ignore->type == PT_SHELL_ABS)
	{
		/* translate shell-like syntax into pcre */
		len=strlen(ignore->compare_string)*5+16;
		STOPIF( hlp__alloc( &buffer, len), NULL);

		dest=buffer;
		src=ignore->compare_string;

		if (ignore->type == PT_SHELL_ABS)
		{
			/* Strip the wc-path away, and put a . in front. */

			/* The pattern must 
			 * - match all characters of the wc path, or
			 * - start with a wild-wildcard ('/ **') - it's valid everywhere.
			 *
			 * If it only has a single wildcard it's not allowed - it would have 
			 * different meanings depending on the wc base:
			 *   pattern:  / * /dir/
			 *   matches:  /etc/x/dir/    for wc base  /etc
			 *   or        /x/dir         "   "  "     /
			 * So we don't allow that. */
			if (strncmp(src, wc_path, wc_path_len) == 0)
			{
				/* Special case for wc base = / */
				src += 1+ (wc_path_len == 1 ? 0 : wc_path_len);
			}
			else if (strncmp(src, wildcard_prefix, strlen(wildcard_prefix)) == 0)
			{
				/* Has wildcard at start ... just consume the PATH_SEPARATOR, as 
				 * that's included in the norm_prefix.  */
				src++;
			}
			else
				STOPIF( wa__warn(WRN__IGNPAT_WCBASE, EINVAL,
							"The absolute shell pattern\n"
							"  \"%s\"\n"
							"does neither have the working copy base path\n"
							"  \"%s\"\n"
							"nor a wildcard path (like \"%s\") at the beginning;\n"
							"maybe you want a wc-relative pattern, "
							"starting with \"%s\"?",
							src, wc_path, wildcard_prefix, norm_prefix), NULL);

			/* Before:  /etc/X11/?      /etc/X11/?
			 * wc_path: /etc            /
			 * After:      ./X11/?     ./etc/X11/?
			 * */
			/* As norm_prefix is const, the compile should remove the strlen() by 
			 * the value. */
			strncpy(dest, norm_prefix, strlen(norm_prefix));
			dest+=strlen(norm_prefix);
		}

		backslashed = 0;
		do
		{
			if (backslashed)
			{
				// escaped mode
				*(dest++) = *(src++);
				backslashed = 0;
			}
			else
			{
				switch(*src)
				{
					case '*':
						if (src[1] == '*')
						{
							if (dest[-1] == PATH_SEPARATOR && src[2] == PATH_SEPARATOR)
							{
								/* Case 1: "/§**§/xxx"; this gets transformed to
								 * "/(.*§/)?", so that *no* directory level is possible, too. */
								*(dest++) = '(';
								*(dest++) = '.';
								*(dest++) = '*';
								*(dest++) = PATH_SEPARATOR;
								*(dest++) = ')';
								*(dest++) = '?';
								/* Eat the two "*"s, and the PATH_SEPARATOR. */
								src+=3; 
							}
							else
							{
								/* Case 2: "/ ** xxx", without a PATH_SEPARATOR after the 
								 * "**". */
								*(dest++) = '.';
								*(dest++) = '*';
								while (*src == '*') src++;
							}
						}
						else
						{
							/* one directory level */
							*(dest++) = '[';
							*(dest++) = '^';
							*(dest++) = PATH_SEPARATOR;
							*(dest++) = ']';
							*(dest++) = '*';
							src++;
						}
						break;
					case '?':
						*(dest++) = '.';
						src++;
						break;
					case '[':
						// processed bracket expression and advanced src and dest pointers
						STOPIF(ign___translate_bracketed_expr(buffer + len, &src, &dest),
								"processing a bracket expression failed");
						break;
					case '0' ... '9':
					case 'a' ... 'z':
					case 'A' ... 'Z':
						/* Note that here it's not a PATH_SEPARATOR, but the simple 
						 * character -- on Windows there'd be a \, which would trash the 
						 * regular expression! Although we'd have some of these problems on 
						 * Windows ...*/
					case '/': 
					case '-':
						*(dest++) = *(src++);
						break;
					case '\\':
						backslashed = 1; // enter escaped mode
						*(dest++) = *(src++);
						break;
						/* . and all other special characters { ( ] ) } + # " \ $
						 * get escaped. */
					case '.':
					default:
						*(dest++) = '\\';
						*(dest++) = *(src++);
						break;
				}
			}

			/* Ensure that there is sufficient space in the buffer to process the 
			 * next character. A "*" might create up to 5 characters in dest, the 
			 * directory matching patterns appended last will add up to five, and 
			 * we have a terminating '\0'.
			 * Plus add a few. */
			STOPIF_CODE_ERR( buffer+len - dest < 6+5+1+6, ENOSPC,
					"not enough space in buffer");
		} while (*src);

		if (src != ignore->compare_string) 
		{
			*(dest++) = '$'; // anchor regexp

			/* src has moved at least one char, so it's safe to reference [-1] */
			if(src[-1] == PATH_SEPARATOR)
			{
				/* Ok, the glob pattern ends in a PATH_SEPARATOR, so our special 
				 * "ignore directory" handling kicks in. This results in "($|/)" at 
				 * the end. */
				dest[-2] = '(';
				*(dest++) = '|';
				*(dest++) = PATH_SEPARATOR;
				*(dest++) = ')';
			}
		}

		*dest=0;
		/* return unused space */
		STOPIF( hlp__realloc( &buffer, dest-buffer+2), NULL);
		ignore->compare_string=buffer;
		dest=buffer;
	}
	else  /* pattern type */
	{
		BUG("unknown pattern type %d", ignore->type);
		/* this one's for gcc */
		dest=NULL;
	}

	DEBUGP("compiled \"%s\"", ignore->pattern);
	DEBUGP("    into \"%s\"", ignore->compare_string);

	/* compile */
	ignore->compiled = pcre_compile(dest,
			PCRE_DOTALL | PCRE_NO_AUTO_CAPTURE | PCRE_UNGREEDY | PCRE_ANCHORED |
			(ignore->is_icase ? PCRE_CASELESS : 0),
			&err, &offset, NULL);

	STOPIF_CODE_ERR( !ignore->compiled, EINVAL,
			"pattern \"%s\" (from \"%s\") not valid; error %s at offset %d.",
			dest, ignore->pattern, err, offset);

	/* Patterns are used often - so it should be okay to study them.
	 * Although it may not help much?
	 * Performance testing! */
	ignore->extra = pcre_study(ignore->compiled, 0, &err);
	STOPIF_CODE_ERR( err, EINVAL,
			"pattern \"%s\" not studied; error %s.",
			ignore->pattern, err);

ex:
	return status;
}


static int data_seen;
int have_now(struct ignore_t *ignore, int cur, char *err)
{
	int status;

	status=0;
	STOPIF_CODE_ERR(data_seen & cur, EINVAL, 
			"!The pattern \"%s\" includes more than a single %s specification.",
			ignore->pattern, err);
	data_seen |= cur;
ex:
	return status;
}


/** Does all necessary steps to use the given \c ignore_t structure.
 * */
int ign___init_pattern_into(char *pattern, char *end, struct ignore_t *ignore)
{
	int status, stop;
	int and_value, cmp_value, speclen;
	char *cp, *eo_word, *param, *eo_parm;
	int pattern_len;


	status=0;
	pattern_len=strlen(pattern);
	cp=pattern+pattern_len;
	if (!end || end>cp) end=cp;

	/* go over \n and other white space. These are not allowed 
	 * at the beginning of a pattern. */
	while (isspace(*pattern)) 
	{
		pattern++;
		STOPIF_CODE_ERR( pattern>=end, EINVAL, "pattern has no pattern");
	}


	data_seen=0;

	/* gcc reports "used unitialized" - it doesn't see that the loop gets 
	 * terminated in the case speclen==0. */
	eo_parm=NULL;
	/* This are the defaults: */
	memset(ignore, 0, sizeof(*ignore));
	ignore->pattern = pattern;
	while (*pattern)
	{
		eo_word=pattern;
		while (isalpha(*eo_word)) eo_word++;
		speclen=eo_word-pattern;
		/* The used codes are (sorted by first character):
		 *  Ignore types          Other flags
		 *   device,                dironly,
		 *                          group,
		 *   inode,                 ignore,
		 *                          insens (=nocase),
		 *                          mode,
		 *                          nocase,
		 *   pcre,
		 *                          take,
		 *   "./"
		 *   "/"
		 * 
		 * The order below reflects the relative importance for shortened 
		 * strings. */

		/* For shell patterns we need not look for parameters; and a comparison 
		 * with 0 characters makes no sense anyway.  */
		if (speclen == 0)
			goto shell_pattern;

		if (*eo_word == ':')
		{
			/* Look for the end of this specification. */
			param= eo_word + 1;
		}
		else
			param=NULL;

		eo_parm=strchr(eo_word, ign___parm_delimiter);
		if (!eo_parm) eo_parm=eo_word+strlen(eo_word);
		/* Now eo_parm points to the first non-parameter character - either ',' 
		 * or \0. */

		if (strncmp(ign__group_take, pattern, speclen)==0)
		{
			STOPIF( have_now(ignore, HAVE_GROUP, "group"), NULL);
			ignore->group_name=ign__group_take;
		}
		else if (strncmp(ign__group_ign, pattern, speclen)==0)
		{
			STOPIF( have_now(ignore, HAVE_GROUP, "group"), NULL);
			ignore->group_name=ign__group_ign;
		}
		else if (strncmp("group:", pattern, speclen)==0)
		{
			STOPIF( have_now(ignore, HAVE_GROUP, "group"), NULL);
			STOPIF_CODE_ERR( !param || eo_parm==param, EINVAL, 
					"!Missing group name in pattern \"%s\".",
					ignore->pattern);

			speclen=eo_parm-param;
			STOPIF( hlp__strnalloc( speclen, 
						(char**)&ignore->group_name, param), NULL);

			if (speclen > ign__max_group_name_len)
				ign__max_group_name_len=speclen;

			/* Test for valid characters. */
			while (param != eo_parm)
			{
				STOPIF_CODE_ERR( !isalnum(*param), EINVAL,
						"!The group name may (currently) "
						"only use alphanumeric characters;\n"
						"so \"%s\" is invalid.", ignore->pattern);
				param++;
			}
		}
		else if (strncmp("dironly", pattern, speclen)==0)
		{
			ignore->dir_only=1;
			STOPIF( have_now(ignore, HAVE_DIR, "dironly"), NULL);
			data_seen |= HAVE_PATTERN_SUBST;
		}
		else if (strncmp("nocase", pattern, speclen)==0 ||
				strncmp("insens", pattern, speclen)==0)
		{
			ignore->is_icase=1;
			STOPIF( have_now(ignore, HAVE_CASE, "case ignore"), NULL);
		}
		else if (strncmp("mode:", pattern, speclen)==0)
		{
			STOPIF( have_now(ignore, HAVE_MODE, "mode"), NULL);
			STOPIF_CODE_ERR( !param, EINVAL,
					"!Invalid mode specification in \"%s\".", ignore->pattern);

			STOPIF_CODE_ERR( sscanf(param, "%o:%o%n", 
						&and_value, &cmp_value, &stop) != 2, EINVAL,
					"!Ignore pattern \"%s\" has a bad mode specification;\n"
					"the expected syntax is \"mode:<AND>:<CMP>\".",
					ignore->pattern);

			STOPIF_CODE_ERR( param+stop != eo_parm, EINVAL,
					"!Garbage after mode specification in \"%s\".",
					ignore->pattern);

			STOPIF_CODE_ERR( and_value>07777 || cmp_value>07777 ||
					(cmp_value & ~and_value), EINVAL,
					"!Mode matching specification in \"%s\" has invalid numbers.",
					ignore->pattern);

			ignore->mode_match_and=and_value;
			ignore->mode_match_cmp=cmp_value;
			data_seen |= HAVE_PATTERN_SUBST;
			stop=0;
		}
		/* The following branches cause the loop to terminate, as there's no 
		 * delimiter character defined *within* patterns.
		 * (Eg. a PCRE can use *any* character). */
		else if (strncmp(dev_prefix, pattern, strlen(dev_prefix)) == 0)
		{
			ignore->type=PT_DEVICE;
			ignore->compare_string = pattern;
			ignore->compare = PAT_DEV__UNSPECIFIED;
			pattern+=strlen(dev_prefix);

			stop=0;
			while (!stop)
			{
				switch (*pattern)
				{
					case '<': 
						ignore->compare |= PAT_DEV__LESS;
						break;
					case '=': 
						ignore->compare |= PAT_DEV__EQUAL;
						break;
					case '>': 
						ignore->compare |= PAT_DEV__GREATER;
						break;
					default:
						stop=1;
						break;
				}
				if (!stop) pattern++;
			}

			if (ignore->compare == PAT_DEV__UNSPECIFIED)
				ignore->compare = PAT_DEV__EQUAL;

			ignore->major=strtoul(pattern, &cp, 0);
			DEBUGP("device pattern: major=%d, left=%s", ignore->major, cp);
			STOPIF_CODE_ERR( cp == pattern, EINVAL, 
					"!No major number found in \"%s\"", ignore->pattern);

			/* we expect a : here */
			if (*cp)
			{
				STOPIF_CODE_ERR( *(cp++) != ':', EINVAL,
						"!Expected ':' between major and minor number in %s",
						ignore->pattern);

				pattern=cp;
				ignore->minor=strtoul(pattern, &cp, 0);
				STOPIF_CODE_ERR( cp == pattern, EINVAL, 
						"!No minor number in \"%s\"", ignore->pattern);

				STOPIF_CODE_ERR( *cp, EINVAL, 
						"!Garbage after minor number in \"%s\"",
						ignore->pattern);
				ignore->has_minor=1; 
			}
			else 
			{
				ignore->minor=PAT_DEV__UNSPECIFIED;
				ignore->has_minor=0; 
			}
			status=0;
			data_seen |= HAVE_PATTERN;
		}
		else if (strncmp(inode_prefix, pattern, strlen(inode_prefix)) == 0)
		{
#ifdef DEVICE_NODES_DISABLED
			DEVICE_NODES_DISABLED();
#else
			int mj, mn;

			ignore->type=PT_INODE;
			ignore->compare_string = pattern;
			pattern+=strlen(inode_prefix);

			mj=strtoul(pattern, &cp, 0);
			STOPIF_CODE_ERR( cp == pattern || *(cp++) != ':', EINVAL,
					"!No major number in %s?", ignore->pattern);

			pattern=cp;
			mn=strtoul(pattern, &cp, 0);
			STOPIF_CODE_ERR( cp == pattern || *(cp++) != ':', EINVAL,
					"!No minor number in %s?", ignore->pattern);

			ignore->dev=MKDEV(mj, mn);

			pattern=cp;
			ignore->inode=strtoull(pattern, &cp, 0);
			STOPIF_CODE_ERR( cp == pattern || *cp!= 0, EINVAL,
					"!Garbage after inode in %s?", ignore->pattern);

#endif
			status=0;
			data_seen |= HAVE_PATTERN;
		}
		else
		{
shell_pattern:
			if (strncmp(pattern, norm_prefix, strlen(norm_prefix)) == 0)
			{
				ignore->type=PT_SHELL;
				DEBUGP("shell pattern matching");
				/* DON'T pattern+=strlen(norm_prefix) - it's needed for matching ! */
			}
			else if (strncmp(pattern, abs_shell_prefix, strlen(abs_shell_prefix)) == 0)
			{
				ignore->type=PT_SHELL_ABS;
				DEBUGP("absolute shell pattern matching");
			}
			else if (strncmp(pcre_prefix, pattern, strlen(pcre_prefix)) == 0)
			{
				ignore->type=PT_PCRE;
				pattern += strlen(pcre_prefix);
				DEBUGP("pcre matching");
			}
			else
				STOPIF_CODE_ERR(1, EINVAL, 
						"!Expected a shell pattern, starting with \"%s\" or \"%s\"!",
						norm_prefix, abs_shell_prefix);


			STOPIF_CODE_ERR( strlen(pattern)<3, EINVAL,
					"!Pattern \"%s\" too short!", ignore->pattern);


			ignore->compare_string = pattern;
			status=ign__compile_pattern(ignore);
			STOPIF(status, "compile returned an error");
			data_seen |= HAVE_PATTERN;
		}

		/* If we got what we want ... */
		if (data_seen & HAVE_PATTERN) break;


		/* Else do the next part of the string. */
		pattern=eo_parm;
		/* Go beyond the delimiter. */
		while (*pattern == ign___parm_delimiter) pattern++;

		DEBUGP("now at %d == %p; end=%p", *pattern, pattern, end);
		STOPIF_CODE_ERR( pattern>end || (pattern == end && *end!=0), 
				EINVAL, "pattern not \\0-terminated");
	}

	/* Don't know if it makes *really* sense to allow a dironly pattern 
	 * without pattern - but there's no reason to deny it outright. */
	STOPIF_CODE_ERR(!(data_seen & (HAVE_PATTERN | HAVE_PATTERN_SUBST)),
			EINVAL, "!Pattern \"%s\" ends prematurely", ignore->pattern);

	/* If we're in the "ignore" command, and *no* group was given, assign a 
	 * default.
	 * If an empty string was given, put always an error. */
	/* COMPATIBILITY MODE FOR 1.1.18: always put a groupname there, if 
	 * necessary.
	 * Needed so that the old ignore patterns can be read from the ignore 
	 * lists. */
//	if (!ignore->group_name && (action->i_val & HAVE_GROUP))
	if (!ignore->group_name)
	{
		ignore->group_name=ign__group_ign;
		eo_word="group:";
		/* gcc optimizes that nicely. */
		STOPIF( hlp__strmnalloc( 
					strlen(eo_word) + strlen(ign__group_ign) + 1 + 
					pattern_len + 1,
					&ignore->pattern,
					eo_word, ign__group_ign, ",", 
					ignore->pattern, NULL), NULL);
	}

	STOPIF_CODE_ERR( !ignore->group_name || !*ignore->group_name, EINVAL,
			"!No group name given in \"%s\".", ignore->pattern);


	DEBUGP("pattern: %scase, group \"%s\", %s, mode&0%o==0%o",
			ignore->is_icase ? "I" : "",
			ignore->group_name,
			ignore->dir_only ? "dironly" : "all entries",
			ignore->mode_match_and, ignore->mode_match_cmp);

	if (!*pattern)
	{
		/* Degenerate case of shell pattern without pattern; allowed in certain 
		 * cases. */
		ignore->type=PT_SHELL;
	}

ex:
	return status;
}


/** -.
 * */
int ign__load_list(char *dir)
{
	int status, fh, l;
	struct stat st;
	char *cp,*cp2;
	int count;


	fh=-1;
	status=waa__open_byext(dir, WAA__IGNORE_EXT, WAA__READ, &fh);
	if (status == ENOENT)
	{
		DEBUGP("no ignore list found");
		status=0;
		goto ex;
	}
	else STOPIF_CODE_ERR(status, status, "reading ignore list");

	STOPIF_CODE_ERR( fstat(fh, &st), errno, NULL);

	memory=mmap(NULL, st.st_size, 
			PROT_READ, MAP_SHARED, 
			fh, 0);
	/* If there's an error, return it.
	 * Always close the file. Check close() return code afterwards. */
	status=errno;
	l=close(fh);
	STOPIF_CODE_ERR( memory == MAP_FAILED, status, "mmap failed");
	STOPIF_CODE_ERR( l, errno, "close() failed");


	/* make header \0 terminated */
	cp=memchr(memory, '\n', st.st_size);
	if (!cp)
	{
		/* This means no entries.
		 * Maybe we should check?
		 */
		DEBUGP("Grouping list header is invalid.");
		status=0;
		goto ex;
	}

	status=sscanf(memory, ign_header_str, 
			&count);
	STOPIF_CODE_ERR( status != 1, EINVAL, 
			"grouping header is invalid");

	cp++;
	STOPIF( ign__new_pattern(count, NULL, NULL, 0, 0), NULL );


	/* fill the list */
	cp2=memory+st.st_size;
	for(l=0; l<count; l++)
	{
		STOPIF( ign__new_pattern(1, &cp, cp2, 1, PATTERN_POSITION_END), NULL);

		/* All loaded patterns are from the user */
		cp+=strlen(cp)+1;
		if (cp >= cp2) break;
	}

	if (l != count)
		DEBUGP("Ignore-list defect - header count (%u) bigger than actual number"
				"of patterns (%u)",
				count, l);
	if (cp >= cp2) 
		DEBUGP("Ignore-list defect - garbage after counted patterns");
	l=used_ignore_entries;

	status=0;

ex:
	/* to make sure no bad things happen */
	if (status)
		used_ignore_entries=0;

	return status;
}


/** Compares the given \c sstat_t \a st with the \b device ignore pattern 
 * \a ign.
 * Does the less-than, greater-than and/or equal comparision.
 * */
inline int ign___compare_dev(struct sstat_t *st, struct ignore_t *ign)
{
#ifdef DEVICE_NODES_DISABLED
	DEVICE_NODES_DISABLED();
#else
	int mj, mn;

	mj=(int)MAJOR(st->dev);
	mn=(int)MINOR(st->dev);

	if (mj > ign->major) return +2;
	if (mj < ign->major) return -2;

	if (!ign->has_minor) return 0;
	if (mn > ign->minor) return +1;
	if (mn < ign->minor) return -1;
#endif

	return 0;
}


int ign___new_group(struct ignore_t *ign, struct grouping_t **result)
{
	int status;
	int gn_len;
	struct grouping_t *group;


	status=0;
	DEBUGP("making group %s", ign->group_name);
	gn_len=strlen(ign->group_name);

	if (ign___groups)
		group=apr_hash_get(ign___groups, ign->group_name, gn_len);
	else
	{
		ign___groups=apr_hash_make(global_pool);
		group=NULL;
	}

	if (group)
	{
		/* Already loaded by another pattern. */
	}
	else
	{
		STOPIF( hlp__calloc(&group, 1, sizeof(*group)), NULL);
		apr_hash_set(ign___groups, ign->group_name, gn_len, group);
	}

	*result=group;
	ign->group_def=group;

ex:
	return status;
}


/** Loads the grouping definitions, and stores them via a \ref grouping_t.
 **/
int ign___load_group(struct ignore_t *ign)
{
	int status;
	struct grouping_t *group;
	char *copy, *fn, *eos, *conf_start, *input;
	FILE *g_in;
	int is_ok, gn_len;
	static const char ps[]= { PATH_SEPARATOR, 0 };
	char *cause;
	svn_string_t *str;


	BUG_ON(ign->group_def, "already loaded");

	status=0;
	copy=NULL;
	g_in=NULL;
	gn_len=strlen(ign->group_name);

	STOPIF( ign___new_group(ign, &group), NULL);

	/* Initialize default values. */
	if (strcmp(ign->group_name, ign__group_take) == 0)
		is_ok=1;
	else if (strcmp(ign->group_name, ign__group_ign) == 0)
		is_ok=2;
	else
		is_ok=0;


	/* waa__open() could be used for the WC-specific path; but we couldn't 
	 * easily go back to the common directory.
	 * So we just compute the path, and move the specific parts for the 
	 * second try. */
	STOPIF( waa__get_waa_directory( wc_path, 
				&fn, &eos, &conf_start, GWD_CONF), NULL);
	/* We have to use a new allocation, because the group name can be 
	 * (nearly) arbitrarily long. */
	STOPIF( hlp__strmnalloc(waa_tmp_path_len +
				strlen(CONFIGDIR_GROUP) + 1 + gn_len + 1, &copy, 
				fn, CONFIGDIR_GROUP, ps, ign->group_name, NULL), NULL);


	DEBUGP("try specific group: %s", copy);
	g_in=fopen(copy, "rt");
	if (!g_in)
	{
		STOPIF_CODE_ERR(errno != ENOENT, errno,
				"!Cannot read group definition \"%s\"", copy);

		/* This range is overlapping:
		 *   /etc/fsvs/XXXXXXXXXXXX...XXXXXX/groups/<name>
		 *   ^fn       ^conf_start           ^eos
		 * gets
		 *   /etc/fsvs/groups/<name>
		 **/
		memmove(copy + (conf_start-fn),
				copy + (eos-fn),
				strlen(CONFIGDIR_GROUP) + 1 + gn_len + 1); /* ==strlen(eos)+1 */

		DEBUGP("try for common: %s", copy);
		g_in=fopen(copy, "rt");
		STOPIF_CODE_ERR(!g_in && errno != ENOENT, errno,
				"!Cannot read group definition \"%s\"", copy);
	}

	DEBUGP("Got filename %s", copy);

	if (!g_in)
	{
		STOPIF_CODE_ERR(!is_ok, ENOENT,
				"!Group definition for \"%s\" not found;\n"
				"used in pattern \"%s\".",
				ign->group_name, ign->pattern);
		/* Else it's a default name, and we can just use the defaults. */
		goto defaults;
	}


	hlp__string_from_filep(NULL, NULL, NULL, SFF_RESET_LINENUM);

	while (1)
	{
		status=hlp__string_from_filep(g_in, &input, NULL,
				SFF_WHITESPACE | SFF_COMMENT);
		if (status == EOF) break;
		STOPIF(status, "reading group file %s", copy);

		conf_start=input;
		DEBUGP("parsing %s", conf_start);
		eos=hlp__get_word(conf_start, &conf_start);

		if (*eos) *(eos++)=0;
		else eos=NULL;

		if (strcmp(conf_start, "take") == 0)
		{
			group->is_take=1;
			continue;
		}
		else if (strcmp(conf_start, "ignore") == 0)
		{
			group->is_ignore=1;
			continue;
		}
		else if (strcmp(conf_start, "auto-prop") == 0)
		{
			cause="no property name";
			if (!eos) goto invalid;
			cause="no whitespace after name";
			if (sscanf(eos, "%s%n", input, &gn_len) != 1) goto invalid;

			eos=hlp__skip_ws(eos+gn_len);
			DEBUGP("Got property name=%s, value=%s", input, eos);

			cause="no property value";
			if (!*input || !*eos) goto invalid;

			if (!group->auto_props)
				group->auto_props=apr_hash_make(global_pool);

			STOPIF( hlp__strdup( &fn, eos), NULL);

			gn_len=strlen(input);
			STOPIF( hlp__strnalloc( gn_len, &eos, input), NULL);

			/* We could just store the (char*), too; but prp__set_from_aprhash() 
			 * takes the values to be of the kind svn_string_t.  */
			str=svn_string_create(fn, global_pool);
			apr_hash_set(group->auto_props, eos, gn_len, str);
		}
		else
		{
			cause="invalid keyword";
invalid:
			STOPIF( EINVAL, "!Cannot parse line #%d in file \"%s\" (%s).",
					hlp__string_from_filep(NULL, NULL, NULL, SFF_GET_LINENUM), 
					copy, cause);
		}
	}

defaults:
	status=0;

	STOPIF_CODE_ERR( group->is_ignore && group->is_take, EINVAL,
			"Either \"take\" or \"ignore\" must be given, in \"%s\".",
			copy);
	if (!group->is_ignore && !group->is_take)
	{
		if (is_ok == 2)
			group->is_ignore=1;
		else
			group->is_take=1;
	}

	DEBUGP("group has %sauto-props; ign=%d, take=%d, url=%s",
			group->auto_props ? "" : "no ",
			group->is_ignore, group->is_take, 
			group->url ? group->url->url : "(default)");

ex:
	IF_FREE(copy);
	if (g_in) fclose(g_in);
	return status;
}


/** -.
 *
 * Searches this entry for a take/ignore pattern.
 *
 * If a parent directory has an ignore entry which might be valid 
 * for this directory (like **§/§*~), it is mentioned in this
 * directory, too - in case of something like dir/a*§/b*§/§* 
 * a path level value is given.
 *
 * As we need to preserve the _order_ of the ignore/take statements,
 * we cannot easily optimize.
 * is_ignored is set to +1 if ignored, 0 if unknown, and -1 if 
 * on a take-list (overriding later ignore list).
 *
 * \a sts must already have the correct estat::st.mode bits set.
 */
int ign__is_ignore(struct estat *sts,
		int *is_ignored)
{
	struct estat *dir;
	int status, namelen UNUSED, len, i, path_len UNUSED;
	char *path UNUSED, *cp;
	struct ignore_t **ign_list UNUSED;
	struct ignore_t *ign;
	struct sstat_t *st;
	struct estat sts_cmp;


	*is_ignored=0;
	status=0;
	dir=sts->parent;
	/* root directory won't be ignored */
	if (!dir) goto ex;

	if (sts->to_be_ignored)
	{
		*is_ignored=1;
		goto ex;
	}

	/* TODO - see ign__set_ignorelist() */ 
	/* currently all entries are checked against the full ignore list -
	 * not good performance-wise! */
	STOPIF( ops__build_path(&cp, sts), NULL);
	DEBUGP("testing %s for being ignored", cp);

	len=strlen(cp);
	for(i=0; i<used_ignore_entries; i++)
	{
		ign=ignore_list+i;

		if (!ign->group_def)
			STOPIF( ign___load_group(ign), NULL);

		ign->stats_tested++;

		if (ign->type == PT_SHELL || ign->type == PT_PCRE ||
				ign->type == PT_SHELL_ABS)
		{
			DEBUGP("matching %s(0%o) against \"%s\" "
					"(dir_only=%d; and=0%o, cmp=0%o)",
					cp, sts->st.mode, ign->pattern, ign->dir_only,
					ign->mode_match_and, ign->mode_match_cmp);
			if (ign->dir_only && !S_ISDIR(sts->st.mode))
			{
				status=PCRE_ERROR_NOMATCH;
			}
			else if (ign->mode_match_and && 
					((sts->st.mode & ign->mode_match_and) != ign->mode_match_cmp))
			{
				status=PCRE_ERROR_NOMATCH;
			}
			else if (ign->compiled)
			{
				status=pcre_exec(ign->compiled, ign->extra, 
						cp, len, 
						0, 0,
						NULL, 0);
				STOPIF_CODE_ERR( status && status != PCRE_ERROR_NOMATCH, 
						status, "cannot match pattern %s on data %s",
						ign->pattern, cp);
			}
		}
		else if (ign->type == PT_DEVICE)
		{
			/* device compare */
			st=(S_ISDIR(sts->st.mode)) ? &(dir->st) : &(sts->st);

			switch (ign->compare)
			{
				case PAT_DEV__LESS:
					status= ign___compare_dev(st, ign) <  0;
					break;
				case PAT_DEV__LESS | PAT_DEV__EQUAL:
					status= ign___compare_dev(st, ign) <= 0;
					break;
				case PAT_DEV__EQUAL:
					status= ign___compare_dev(st, ign) == 0;
					break;
				case PAT_DEV__EQUAL | PAT_DEV__GREATER:
					status= ign___compare_dev(st, ign) >= 0;
					break;
				case PAT_DEV__GREATER:
					status= ign___compare_dev(st, ign) >  0;
					break;
			}

			/* status = 0 if *matches* ! */
			status = !status;
			DEBUGP("device compare pattern status=%d", status);
		}
		else if (ign->type == PT_INODE)
		{
			sts_cmp.st.dev=ign->dev;
			sts_cmp.st.ino=ign->inode;
			status = dir___f_sort_by_inodePP(&sts_cmp, sts) != 0;
			DEBUGP("inode compare %llX:%llu status=%d", 
					(t_ull)ign->dev, (t_ull)ign->inode, status);
		}
		else
			BUG("unknown pattern type 0x%X", ign->type);

		/* here status == 0 means pattern matches */
		if (status == 0) 
		{
			ign->stats_matches++;
			*is_ignored = ign->group_def->is_ignore ? +1 : -1;
			sts->match_pattern=ign;
			DEBUGP("pattern found -  result %d", *is_ignored);
			goto ex;
		}
	}

	/* no match, no error */
	status=0;

ex:
	return status;
}


/** Writes the ignore list back to disk storage.
 * */
int ign__save_ignorelist(char *basedir)
{
	int status, fh, l, i;
	struct ignore_t *ign;
	char buffer[HEADER_LEN];


	DEBUGP("saving ignore list: have %d", used_ignore_entries);
	fh=-1;
	if (!basedir) basedir=wc_path;

	if (used_ignore_entries==0)
	{
		STOPIF( waa__delete_byext(basedir, WAA__IGNORE_EXT, 1), NULL);
		goto ex;
	}

	STOPIF( waa__open_byext(basedir, WAA__IGNORE_EXT, WAA__WRITE, &fh), NULL);

	/* do header */
	for(i=l=0; i<used_ignore_entries; i++)
	{
		if (ignore_list[i].is_user_pat) l++;
	}

	status=snprintf(buffer, sizeof(buffer)-1,
			ign_header_str, 
			l);
	STOPIF_CODE_ERR(status >= sizeof(buffer)-1, ENOSPC,
			"can't prepare header to write; buffer too small");

	strcat(buffer, "\n");
	l=strlen(buffer);
	status=write(fh, buffer, l);
	STOPIF_CODE_ERR( status != l, errno, "error writing header");


	/* write data */
	ign=ignore_list;
	for(i=0; i<used_ignore_entries; i++)
	{
		if (ignore_list[i].is_user_pat)
		{
			l=strlen(ign->pattern)+1;
			status=write(fh, ign->pattern, l);
			STOPIF_CODE_ERR( status != l, errno, "error writing data");

			status=write(fh, "\n", 1);
			STOPIF_CODE_ERR( status != 1, errno, "error writing newline");
		}

		ign++;
	}

	status=0;

ex:
	if (fh!=-1) 
	{
		l=waa__close(fh, status);
		fh=-1;
		STOPIF(l, "error closing ignore data");
	}

	return status;
}


int ign__new_pattern(unsigned count, char *pattern[], 
		char *ends,
		int user_pattern,
		int position)
{
	int status;
	unsigned i;
	struct ignore_t *ign;


	status=0;
	DEBUGP("getting %d new entries - max is %d, used are %d", 
			count, max_ignore_entries, used_ignore_entries);
	if (used_ignore_entries+count >= max_ignore_entries)
	{
		max_ignore_entries = used_ignore_entries+count+RESERVE_IGNORE_ENTRIES;
		STOPIF( hlp__realloc( &ignore_list, 
					sizeof(*ignore_list) * max_ignore_entries), NULL);
	}


	/* If we're being called without patterns, we should just reserve 
	 * the space in a piece. */
	if (!pattern) goto ex;


	/* Per default new ignore patterns are appended. */
	if (position != PATTERN_POSITION_END && used_ignore_entries>0)
	{
		/* This would be more efficient with a list of pointers.
		 * But it happens only on explicit user request, and is therefore
		 * very infrequent. */
		/* This code currently assumes that all fsvs-system-patterns are
		 * at the front of the list. The only use is currently in waa__init(),
		 * and so that should be ok. */
		/* If we assume that "inserting" patterns happen only when we don't
		 * do anything but read, insert, write, we could even put the new
		 * patterns in front.
		 * On writing only the user-patterns would be written, and so on the next
		 * load the order would be ok. */

		/* Find the first user pattern, and move from there. */
		for(i=0; i<used_ignore_entries; i++)
			if (ignore_list[i].is_user_pat) break;

		/* Now i is the index of the first user pattern, if any.
		 * Before:
		 *   used_ignore_entries=7
		 *   [ SYS SYS SYS User User User User ]
		 *                  i=3
		 * Then, with count=2 new patterns, at position=0:
		 *   [ SYS SYS SYS Free Free User User User User ]
		 *                  i
		 * So we move from ignore_list+3 to ignore_list+5, 4 Elements.
		 * QED :-) */
		i+=position;

		memmove(ignore_list+i+count, ignore_list+i, 
				(used_ignore_entries-i)*sizeof(*ignore_list));
		/* Where to write the new patterns */
		position=i;
	}
	else
		position=used_ignore_entries;

	BUG_ON(position > used_ignore_entries || position<0);

	status=0;
	for(i=0; i<count; i++)
	{
		/* This prints the newline, so debug output is a bit mangled.
		 * Doesn't matter much, and whitespace gets removed in 
		 * ign___init_pattern_into(). */
		DEBUGP("new pattern %s", *pattern);
		ign=ignore_list+i+position;

		/* We have to stop on wrong patterns; at least, if we're
		 * trying to prepend them.
		 * Otherwise we'd get holes in ignore_list, which we must not write. */
		STOPIF( ign___init_pattern_into(*pattern, ends, ign), NULL);

		ign->is_user_pat=user_pattern;
		pattern++;
	}

	used_ignore_entries+=count;

ex:
	return status;
}


/** Parses the optional position specification.
 * */
int ign___parse_position(char *arg, int *position, int *advance)
{
	int status;
	int i;

	status=0;
	*advance=0;

	/* Normal pattern inclusion. May have a position specification here.  */
	*position=PATTERN_POSITION_END;
	if (strcmp(arg, "prepend") == 0)
	{
		*advance=1;
		*position=PATTERN_POSITION_START;
	}
	else if (sscanf(arg, "at=%d", &i) == 1)
	{
		*advance=1;
		STOPIF_CODE_ERR(i > used_ignore_entries, EINVAL,
				"The position %d where the pattern "
				"should be inserted is invalid.\n", i);
		*position=i;
	}
	else if (strcmp(arg, "append") == 0)
	{
		/* Default */
		*advance=1;
	}

ex:
	return status;
}


int ign___test_single_pattern(struct estat *sts)
{
	int status;
	char *path;

	status=0;
	BUG_ON(!(sts->entry_status & FS_NEW));

	if (sts->match_pattern)
	{
		STOPIF( ops__build_path(&path, sts), NULL);
		if (opt__is_verbose() >= 0)
			STOPIF_CODE_EPIPE( printf("%s\n", path), NULL);
	}

ex:
	return status;
}


int ign___test_all_patterns(struct estat *sts)
{
	int status;
	char *path;
	struct ignore_t *ign;


	status=0;
	BUG_ON(!(sts->entry_status & FS_NEW));

	STOPIF( ops__build_path(&path, sts), NULL);
	ign=sts->match_pattern;

	if (opt__is_verbose() >= 0)
		STOPIF_CODE_EPIPE( 
				opt__is_verbose()>0 ? 
				printf("%s\t%s\t%s\n", 
					ign ? ign->group_name : "(none)",
					ign ? ign->pattern : "(none)",
					path) : 
				printf("%s\t%s\n", ign ? ign->group_name : "(none)", path), NULL);

ex:
	return status;	return 0;
}


/** -. */
int ign__print_group_stats(FILE *output)
{
	int status;
	int i;
	struct ignore_t *ign;

	STOPIF_CODE_EPIPE( fprintf(output, "\nGrouping statistics ("
				"tested, matched, groupname, pattern):\n\n"), NULL);

	for(i=0; i<used_ignore_entries; i++)
	{
		ign=ignore_list+i;

		if (ign->is_user_pat || opt__is_verbose()>0)
		{
			STOPIF_CODE_EPIPE( fprintf(output, "%u\t%u\t%s\t%s\n",
						ign->stats_tested, ign->stats_matches, 
						ign->group_name, ign->pattern), NULL);
		}
	}

ex:
	return status;
}


/** -.
 * This is called to append new ignore patterns.
 **/
int ign__work(struct estat *root UNUSED, int argc, char *argv[])
{
	int status;
	int position, i;
	char *cp, *copy;
	char *arg[2];
	struct grouping_t *group;


	status=0;

	/* A STOPIF_CODE_ERR( argc==0, 0, ...) is possible, but not very nice -
	 * the message is not really user-friendly. */
	if (argc==0)
		ac__Usage_this();

	/* Now we can be sure to have at least 1 argument. */

	/* Goto correct base. */
	status=waa__find_common_base(0, NULL, NULL);
	if (status == ENOENT)
		STOPIF(EINVAL, "!No working copy base was found.");
	STOPIF(status, NULL);


	DEBUGP("first argument is %s", argv[0]);

	status=0;
	if (strcmp(argv[0], parm_test) == 0)
	{
		argv++;
		argc--;

		if (argc>0)
		{
			STOPIF( ign___parse_position(argv[0], &position, &i), NULL);
			argv+=i;
			argc-=i;

			/* Even though we might have been called with "groups" instead of 
			 * "ignore", we just assume the "ignore" group, so that testing is 
			 * easier. */
			action->i_val |= HAVE_GROUP;

			STOPIF( ign__new_pattern(argc, argv, NULL, 1, position), NULL);

			action->local_callback=ign___test_single_pattern;
		}
		else
		{
			STOPIF( ign__load_list(NULL), NULL);
			action->local_callback=ign___test_all_patterns;
		}

		opt__set_int(OPT__FILTER, PRIO_MUSTHAVE, FS_NEW);

		/* The entries would be filtered, and not even given to the output 
		 * function, so we have to fake the ignore groups into take groups. */
		for(i=0; i<used_ignore_entries; i++)
		{
			STOPIF( ign___new_group(ignore_list+i, &group), NULL);
			ignore_list[i].group_def->is_ignore=0;
			ignore_list[i].group_def->is_take=1;
		}

		/* We have to load the URLs. */
		STOPIF( url__load_list(NULL, 0), NULL);

		/* We fake the start path as (relative) argument; if it's the WC base, 
		 * we use ".".  */
		if (start_path_len == wc_path_len)
			arg[0]=".";
		else
			arg[0]=start_path+wc_path_len+1;

		arg[1]=NULL;
		STOPIF( waa__read_or_build_tree(root, 1, arg, arg, NULL, 0), NULL);

		if (opt__get_int(OPT__GROUP_STATS))
			STOPIF( ign__print_group_stats(stdout), NULL);

		/* We must not store the list! */
		goto dont_store;
	}
	else if (strcmp(argv[0], parm_load) == 0)
	{
		i=0;
		while (1)
		{
			status=hlp__string_from_filep(stdin, &cp, NULL, SFF_WHITESPACE);
			if (status == EOF) break;

			STOPIF(status, NULL);

			STOPIF( hlp__strdup( &copy, cp), NULL);
			STOPIF( ign__new_pattern(1, &copy, NULL, 
						1, PATTERN_POSITION_END), NULL);
			i++;
		}

		if (opt__is_verbose() >= 0)
			printf("%d pattern%s loaded.\n", i, i==1 ? "" : "s");
	}
	else
	{
		/* We edit or dump the list, so read what we have. */
		STOPIF( ign__load_list(NULL), NULL);

		if (strcmp(argv[0], parm_dump) == 0)
		{
			/* Dump only user-patterns. */
			for(i=position=0; i < used_ignore_entries; i++, position++)
				if (ignore_list[i].is_user_pat)
				{
					if (opt__is_verbose() > 0) 
						printf("%3d: ", position);

					printf("%s\n", ignore_list[i].pattern);
				}

			/* No need to save. */
			goto dont_store;
		}
		else 
		{
			STOPIF( ign___parse_position(argv[0], &position, &i), NULL);
			argv+=i;
			argc-=i;
			STOPIF( ign__new_pattern(argc, argv, NULL, 1, position), NULL);
		}
	} /* not "fsvs load" */

	STOPIF( ign__save_ignorelist(NULL), NULL);

dont_store:
ex:
	return status;
}


/** -.
 * Relativizes the given paths, and stores them.
 **/
int ign__rign(struct estat *root UNUSED, int argc, char *argv[])
{
	int status;
	int i, position;
	char **normalized;

	status=0;
	if (argc==0) ac__Usage_this();

	/* Position given? */
	STOPIF( ign___parse_position(argv[0], &position, &i), NULL);
	argv+=i;
	argc-=i;

	/* Goto correct base. */
	status=waa__find_common_base2(argc, argv, &normalized,
			FCB__PUT_DOTSLASH | FCB__NO_REALPATH);
	if (status == ENOENT)
		STOPIF(EINVAL, "!No working copy base was found.");
	STOPIF(status, NULL);

	/* Load, insert, save. */
	STOPIF( ign__load_list(NULL), NULL);
	STOPIF( ign__new_pattern(argc, normalized, NULL, 1, position), NULL);
	STOPIF( ign__save_ignorelist(NULL), NULL);

ex:
	return status;
}

