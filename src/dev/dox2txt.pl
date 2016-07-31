#!/usr/bin/perl

$input=shift;
$output=shift;
open(STDIN, "lynx -dump -nolist -nonumbers $input |") || die $!;
#open(STDOUT, "> $output") || die $!;

# Cut until first <h2> header
while (<STDIN>)
{
# I'd thought lynx had an option to not print these?
# yes ... -nonumbers.
	s#\[\d+\]##;


	$p=m#^Detailed Description# .. m#^\s*Modules#;
	print if ($p =~ m#^\d+$#);
}

