#!/usr/bin/perl

use open qw(:std :utf8);
use IPC::Open2;

$input=shift;
$output=shift;

$pid = open2(my $r, my $w, qw"lynx -dump -nolist -nonumbers -stdin") or die $!;
binmode($r, "encoding(UTF-8)");
binmode($w, "encoding(UTF-8)");

open(STDIN, "<", $input) or die $!;
binmode(STDIN, "encoding(UTF-8)");
while (<STDIN>)
{
# There's no space around <code> anymore.
	s,(\S)(\<code\>),$1 $2,g;
	print $w $_;
}
close $w;

#open(STDOUT, "> $output") || die $!;

# Cut until first <h2> header
while (<$r>)
{
# I'd thought lynx had an option to not print these?
# yes ... -nonumbers.
	s#\[\d+\]##;
	next if m#^\[#;

	s/\xe2/--/g; 

#	$p=m#^SYNOPSIS# .. m#^\s*-{30,}#;
	$p=m#^\w# .. m#^\s*_{30,}#;
	print if ($p =~ m#^\d+$#);
}

