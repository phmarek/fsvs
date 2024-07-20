/************************************************************************
 * Copyright (C) 2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 ************************************************************************/


/** \file
 *
 * A chroot environment for fsvs.
 *
 * Please see the \ref howto_chroot documentation for details.  */

/** \defgroup howto_chroot HOWTO: About running fsvs on older systems
 * \ingroup howto
 *
 * This document explains how the chroot-wrapper for FSVS works, and how 
 * it has to be configured.
 *
 *
 * \section chrooter_why Why do I need this?
 *
 * You possibly want to use FSVS on some older system, but don't want to 
 * build everything needed there - apr, subversion, neon, ...
 *
 *
 * \section chrooter_not How it doesn't work
 *
 * An easy workaround would be using a chroot-environment - but then you
 * don't have access to the data you'd like versioned.
 * 
 * Another way is to use \c LD_LIBRARY_PATH - but that doesn't work (at 
 * least for me) for the later-loaded libraries, like \c libnss_dns and so 
 * on.
 * 
 * Even using the \c rpath parameter for linking doesn't quite work - all 
 * dynamically loaded things, like locale data, timezones, message tables, 
 * and so on are taken from the current root on - and may not match the 
 * needed versions.
 * 
 *
 * \section chrooter_how How it does work
 * 
 * A small helper program allows to copy FSVS (with needed libraries) 
 * from a current system (like <i>debian unstable</i>) to any 
 * (architecturally matching) other distribution, without worrying about 
 * library incompatibilities.
 *
 * This works by calling this wrapper program; it goes into a \c chroot 
 * jail and calls FSVS with additional data; FSVS then tries to load 
 * all needed libraries (see \ref hlp__chrooter), and goes out of the jail 
 * to resume operations from the default environment.
 *
 *
 * \section chrooter_old On the old system
 *
 * On your \e old system you use an additional parameter for 
 * <tt>configure</tt>:
 * \code
 * ./configure --with-chroot=/usr/local/fsvs-chroot
 * make
 * \endcode
 * 
 * This builds only \c tools/fsvs-chrooter -- this you put into \c \c 
 * /usr/local/bin or wherever you like. It should be in a directory listed 
 * in \c PATH!
 *
 *
 * \section chrooter_current What to do on the current (updated) machine
 *
 * You take FSVS and copy that <b>with all needed libraries</b> into 
 * some new directory structure on your old system; eg. \c 
 * /usr/local/fsvs-chroot.

 * Don't forget to copy the later-loaded libraries and data files - <tt>ldd 
 * fsvs</tt> won't give you the whole list! You can get a good list to 
 * start (on the current machine) with
 * \code
 * strace -e open -o /tmp/list fsvs remote-status
 * \endcode
 * as that opens a repository connection. Not everything from this list is 
 * needed; generally only files matching <tt>*.so.*</tt>, and \c 
 * locale-archive.
 *
 * Please create the whole structure (as far as needed) as it is - ie.
 * \code
 * 		/usr/local/fsvs-chroot/
 * 			lib/
 * 				libc.so.6
 * 				ld-linux.so.2
 * 				...
 * 			usr/
 * 				lib/
 * 					libnss_dns.so
 * 					...
 * 				local/
 * 					bin/
 * 						fsvs
 * \endcode
 *
 * Why? First, it's easier for you to update later, and second the dynamic 
 * linker knows where to look.
 *
 * \note You'll also see some additional files in the \c strace output - 
 * such things as \c /etc/hosts, \c /etc/resolv.conf, \c /etc/nsswitch.conf 
 * and so on. These tell the network libraries how to resolve names via 
 * DNS, and similar data. \n They should normally be identical to the file 
 * on the \b target machine; to keep them the same, it might be a good idea 
 * to have them copied into the chroot jail from time to time. \note A 
 * binding mount would be better still - but as \c /etc/ld.so.cache should 
 * be taken from the newer machine, you'd have to do every single file.  
 * \note It should be possible to simply have \b no \c ld.so.cache file; 
 * then the dynamic linker would have to search the directories by himself.
 * 
 *
 * \section chrooter_usage How is this used, then?
 *
 * FSVS-chrooter can be called just like fsvs - it relays all parameters 
 * into the jailed binary.
 *
 * Although it might be better to set the environment variables for \c 
 * fsvs-chrooter in a shell script named FSVS - then the other programs 
 * won't have to put up with the long library list. \n The \ref chrooter_sh 
 * "prepare script" below generates such a file.
 *
 *
 * \section chrooter_sh Prepare script
 *
 * If you look into \c tools/, you'll find a script named \c 
 * prepare-chroot.pl. This is what I use to create the \c chroot jail on my 
 * debian unstable machine.
 *
 * \note Most of the libraries listed in the environment variable could be 
 * removed, as they're referenced in the fsvs binary. Only the few that are 
 * \b not automatically loaded have to be in the list.
 *
 *
 * \section chrooter_rest Some thoughts and technical details
 *
 * \note Why does FSVS-chrooter set two directory variables? \n
 * We need the old \c / to set the correct root directory back; and the 
 * current working directory has to be restored, too.\n
 * If we did a <tt>chroot(current working directory)</tt>, we'd see a 
 * completely different directory structure than all the other filesystem 
 * tools (except for the case <tt>cwd = "/"</tt>, of course).
 *
 * \note Maybe give the chrooter setuid and drop privileges after 
 * returning from chroot() jail? Not sure about security implications, 
 * seems to be unsafe. Does anybody know how to do that <b>in a safe 
 * manner</b>?
 *
 * \note If your \e old system is a \b really old system, with a kernel 
 * before 2.4.17 or something like that, you \b might get problems with the 
 * threading libraries - \c libpthread.so. \n
 * Search for \c LD_ASSUME_KERNEL to read a bit about the implications. \n \n
 * Information about how to proceed there is wanted.
 *
 * If this doesn't work for you, because some function which would load 
 * additional datafiles isn't called, try the \c strace trick.
 * Patches are welcome!
 *
 *
 * Ideas, suggestions, feedback please to the mailing lists.
 * */


#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include "../config.h"
#include "../interface.h"


#define STOP(...)                                                   	\
	do {                                                              	\
		fprintf(stderr, __VA_ARGS__);                                   	\
		fprintf(stderr, "\n%s (%d)\n"                                   	\
				"fsvs-chrooter (licensed under the GPLv3), (C) by Ph. Marek;"	\
				" version " FSVS_VERSION "\n", 																\
		strerror(errno), errno);                                        	\
		exit(errno);                                                    	\
	} while (0)


void open_keep_set(char *fn, char *env)
{
	char stg[10];
	int hdl;
	int flags;
	int status;


	hdl=open(fn, O_RDONLY);
	if (hdl<0) STOP("Cannot open directory %s", fn);

	flags=fcntl(hdl, F_GETFD);
	if ( flags == -1 )
		STOP("Cannot get fd flags");

	flags &= ~FD_CLOEXEC;
	status=fcntl(hdl, F_SETFD, flags);
	if ( flags == -1 )
		STOP("Cannot set fd flags");

	sprintf(stg,"%d",hdl);
  setenv(env, stg, 1);
}


int main(int argc, char *args[])
{
	errno=0;

	if ( getenv(CHROOTER_LIBS_ENV) == NULL)
		STOP("Please specify in %s which libraries should be preloaded.",
				CHROOTER_LIBS_ENV);

	open_keep_set("/", CHROOTER_ROOT_ENV);
	open_keep_set(".", CHROOTER_CWD_ENV);

	if (chroot(CHROOTER_JAIL)==-1)
		STOP("Cannot do chroot(%s)", CHROOTER_JAIL);

	if (chdir("/") == -1)
		STOP("Cannot do chdir(/) call");

	execvp("fsvs",args);
	STOP("Executing fsvs in the chroot jail failed");

	return 0;
}

