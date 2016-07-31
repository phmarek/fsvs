#!/usr/bin/perl

# Replaces the password in shadow-like files
# Keeps single-character values (for deactivated etc.)

while (<>)
{
	@f=split(/(:)/);
	$f[2]='-' if length($f[2]) > 1;
	print join("", @f);
}
