#!/bin/bash
# To be started from the src/ directory, to have matching paths

set -e

. ../tests/test_functions


# look for UTF8
utf8_locale=`locale -a | grep .utf8 | head -1`
# look for non-utf8
loc_locale=`locale -a | egrep -iv "(POSIX|C|utf8$)" | head -1`

fail=0
sum=0
export CURRENT_TEST=ext-tests
start=`date +%s`

# Use special directories, so that normal system operation is not harmed.
PTESTBASE=/tmp/fsvs-tests-permutated
test -d $PTESTBASE || mkdir $PTESTBASE
sudo chmod 777 $PTESTBASE

exec 10> /tmp/fsvs-tests.csv
echo "Nr","Prot","LANG","priv","config","Result" >&10

# My default is debug - so do that last, to have a 
# correctly configured environment :-)
# Furthermore the "unusual" configurations are done first.
for release in "" --enable-release --enable-debug 
do
	for prot in svn+ssh file://
	do
		for user in sudo ""
		do
		  # We have to make the conf and waa directory depend on the
			# user, so that root and normal user don't share the same base -
			# the user would get some EPERM.
			PTESTBASE2=$PTESTBASE/u.$user
			export WAAtmp=$PTESTBASE2/waa
			mkdir $WAAtmp 2> /dev/null || true
			export CONFtmp=$PTESTBASE2/conf
			mkdir $CONFtmp 2> /dev/null || true
			for lang in $loc_locale $utf8_locale `locale | grep LC_CTYPE | cut -f2 -d'"'`
			do
				sum=`expr $sum + 1`
				# sudo removes some environment variables, so set FSVS_WAA again.
				cmd="$user make run-tests LC_ALL=$lang FSVS_WAA=$WAAtmp FSVS_CONF=$CONFtmp PROTOCOL=$prot"
				exec 9> /tmp/fsvs-test-$sum.log
				echo "Testing #$sum: (configure=$release) $cmd ("`date`")"
				echo "Testing #$sum: (configure=$release) $cmd ("`date`")" >&9
				echo "Starting "`date` >&9
				echo "" >&9
				( cd .. && ./configure $release ) >&9
				# make sure that the binary gets recompiled
				touch config.h
				if $cmd < /dev/null >&9 2>&9
				then
					$INFO "$cmd done"
					status=OK
				else
					fail=`expr $fail + 1`
					$WARN "Test #$sum FAILED ($cmd)"
					status=FAIL
				fi
				echo "" >&9
				echo "$status: $cmd" >&9
				echo "$sum,'$prot','$lang','${user:-user}','$release','$status','$fail'" >&10
			done
		done
	done
done

end=`date +%s`
$INFO "Finished after "`expr $end - $start`" seconds ("`date`")."

if [[ $fail -ge 1 ]]
then
	$ERROR "$fail of $sum tests failed."
else
  $SUCCESS "All $sum tests passed."
fi

