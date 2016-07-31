#!/usr/bin/perl -ni.bak

use strict;

our $counter;

if ($. == 1)
{
	# Header line
	print;
	$counter=$_+0;
}
else
{

	# Normal line
	my $orig=$_;

	# Remove optional separators
	1 while s{^([^/.]+),+}{$1};

	my %fields;
	my $group="ignore";
	$fields{"mode$2"}++ while s/^(\w*?)m(:\d+:\d+)/$1/;
	$group='take' while s/^(\w*?)t+/$1/;
	$fields{"dironly"}++ while s/^(\w*?)d+/$1/;
	$fields{"insens"}++ while s/^(\w*?)i+/$1/;

# "./" or "/" at the start
	die "Ignore pattern '$orig' in $ARGV not converted, got '$_'" unless m{^\.?/};

	print join(",", sort("group:$group", keys %fields), $_);

	die "Line numbering wrong" 
	if !defined($counter) || $counter<1; 

	$counter--;
}

if (eof)
{
	close ARGV;
	die "Counter '$counter' wrong?" unless $counter==0;
}

# vim: set sw=2 ts=2 et
