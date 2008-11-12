#!/usr/bin/perl
#
# This has gone much too far.
# Doxygen just writes garbage as man page.
# But what else should I use?
#

$output=shift || die "output?";
$new_title=shift;

($section) = ($output =~ m/(\d)$/);
open(STDOUT, "> $output") || die "write $output: $!";

$done=0;
$had_title=0;
while (<STDIN>)
{
	# Change title and section.
	$done=s{^
		(\.TH \s+) 
		"([^"]+)"
		\s+ \d \s+ 
	}{
		$1 . 
		'"' . ($new_title || $2) . '"' .
		" $section "
	}ex unless $done; 

	# Title again - it's merged with the first headline.
	s/^(.*\S)\s*- (\.SH ".*")/($new_title || $1) . "\n" . $2/e;

	# Doxygen generates wrong lines before headlines.
	if ($_ eq "\\fC \\fP\n")
	{
		 $x=<STDIN>;
		 # Only print this string if next line is no header.
		 print $_ if $x !~ m/^\.S[HS]/;

		 $_=$x;
	}

	# \fC may not have a ' directly afterwards.
	s#^\\fC'#\\fC '#;

	print($_) || die $!;
}

close(STDOUT) || die $!;

exit !$done;
