#!/usr/bin/perl

$config = shift || die "which config.h?";
$output = shift || die "which .c?";


%ignore=map { ($_,1); } 
	qw(__CONFIG_H__ FASTCALL MKDEV MAJOR MINOR);

%syms=();
open(F,"<", $config) || die "open $config: $!";
while (<F>)
{
	$syms{$1}++ if /^\s*#(?:define|undef)\s+(\w+)/ && !$ignore{$1};
}

open(F,"<", $output) || die "open $output: $!";
undef $/;
$file=<F>;
close F;
($_) = ($file =~ /\s Version \s* \( [^)]* \) \s* 
	\n \{ ([\x00-\xff]*) 
	\n \}
	/xm);
die "No Version() found." unless $_;
study($_);

for $sym (keys %syms)
{
	warn("Not seen: $sym\n") 
	unless m#\b$sym\b#;
}

