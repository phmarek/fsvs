#!/bin/sh

location=/var/lib/fsvs-versioning/repository
scripts=/var/lib/fsvs-versioning/scripts
group=sysver

set -e
set -x

cd /etc



# Ignore if group already exists
addgroup $group || true

if fsvs info > /dev/null 2>&1
then
	echo Already configured for /etc.
else
  if ! svnlook info -r0 $location >/dev/null 2>&1
	then
# Keep the data secret
		mkdir -m 2770 -p $location
# BDB is faster than FSFS, especially for many small files in 
# many revisions.
	  svnadmin create --fs-type bdb $location
# branches might not be needed, but tags could make sense.
	  svn mkdir file://$location/trunk file://$location/tags -m "create trunk and tags"
# We keep the directory structure 1:1, so it could easily be 
# moved to include the full system.
#
# Note: If we'd do the versioning at the root, we'd have either
# to exclude everything except /etc (tricky, and error-prone), or
# have some take pattern - but then new ignore patterns (by other
# packages) couldn't simply be appended.
	  svn mkdir file://$location/trunk/etc -m "create etc"
		chown 0.$group $location -R
	fi

# Create local filelist, to make "fsvs ps" work.
	fsvs checkout file://$location/trunk/etc 

	conf_path=`fsvs info . | grep Conf-Path | cut -f2 -d:`

  fsvs ignore '/etc/**.dpkg-old' '/etc/**.dpkg-new' '/etc/**.dpkg-dist' '/etc/**.dpkg-bak'
  fsvs ignore '/etc/**.bak' '/etc/**.old' '/etc/**~' '/**.swp'
# easy to remake, no big deal (?)
  fsvs ignore '/etc/ssh/ssh_host_*key'

# Not used?
	fsvs ignore /etc/apt/secring.gpg

	fsvs ignore /etc/mtab
	fsvs ignore /etc/ld.so.cache /etc/adjtime

# Just compiled data?
  fsvs ignore '/etc/selinux/*.pp'

# unknown whether that should be backuped.
  fsvs ignore '/etc/identd.key'
  fsvs ignore '/etc/ppp/*-secrets'
  
  fsvs ps fsvs:commit-pipe /var/lib/fsvs-versioning/scripts/remove-password-line.pl ddclient.conf || true

# Are there non-shadow systems?
#  fsvs ignore './shadow' './gshadow'
  fsvs ps fsvs:commit-pipe /var/lib/fsvs-versioning/scripts/shadow-clean.pl shadow gshadow 

# Match entries that are not world-readable.
	fsvs group 'group:unreadable,m:4:0'

# Lock-files are not needed, are they?
  fsvs ignore './**.lock' './**.LOCK'

# Should we commit the current ignore list?
# fsvs commit -m "Initial import"

# Should we ignore the "Urls" file changing?
# Having it in shows which revision /etc was at.
fi
