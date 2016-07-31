#!/bin/sh

# So that the defined group can access the data
umask 007

# In case the process calling apt-get had some paths defined, they might
# not be what FSVS expects.
# Re-set the defaults.
export FSVS_CONF=/etc/fsvs
export FSVS_WAA=/var/spool/fsvs/
# Possibly run this script or FSVS via env(1)?
# Would clean *all* FSVS_* variables.

# Tell the author as "apt", because we're called by apt-get.
fsvs ci -o author=apt /etc -m "${1:-Auto-commit after dpkg}" -q

