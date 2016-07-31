#!/usr/bin/perl
#
# Checks whether all defined options have some documentation.


%opt=();
open(O, "< " . shift()) || die "can't read options.c: $!";
while (<O>)
{
	chomp;
	if ( /^struct \s+ opt__list_t \s+ opt__list/x .. /^\S;/ )
	{
#		print("found option $1\n"),
		$opt{$1}++ if /\.name \s* = \s* " ([^"]+) "/x;
	}
}

while (<>)
{
	chomp;

	if (/\<LI\>\\c (.+) - /)
	{
		map {
#		print("documented: $_\n");
			delete $opt{$_};
		} split(/, \\c /, $1);

		$opt{$1}++ if /\\ref\s+(\w+)/;
	}

	delete $opt{$2} if / \\(subsection|anchor) \s+ (\w+) /x;
}

exit if !keys %opt;

die "Doc missing for ". join(", ", sort keys %opt) . "\n";

