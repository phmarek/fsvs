#!/usr/bin/perl

# Replaces the password in shadow-like files
# Keeps single-character values (for deactivated etc.)

while (<>)
{
	@f=split(/:/);
	$f[1]='-' if length($f[1]) > 1;
	print;
}
