#!/usr/bin/perl

print "/* This file is generated, do not edit!\n",
" * Last done on ", scalar(gmtime(time())),"\n",
" * */\n",
"\n\n";

while (<>)
{
	chomp;

	next if /(_{30,})/;
	next if /^\s*$/ && !@text;

	$sect=$1 if /^_?([\w\-]{1,5}[a-zA-Z0-9])/;
#	print STDERR "sect=$sect old=$old_sect\n";
	if ($sect ne $old_sect)
	{
		print "const char hlp_${old_sect}[]=\"" . 
			join("\"\n  \"", @text),"\";\n\n"
			if ($old_sect && $old_sect =~ /^[a-z]/);
		@text=();
		$sect =~ s#-#_#g;
		$old_sect=$sect;
	}
	else
	{ 
# make \ safe
		s#\\#\\\\#g;
# make " safe
		s#"#\\"#g;
# remove space at beginning
#		s#^ ##;
		push(@text,$_ . "\\n");
	}
}

print "\n\n// vi: filetype=c\n";
