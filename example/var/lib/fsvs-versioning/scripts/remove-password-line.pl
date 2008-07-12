#!/usr/bin/perl

while (<>)
{
# No substitution value, could be used wrongly
  s#^(\s*password\s*=).*#\1#;
	print;
}
