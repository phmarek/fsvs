#!/usr/bin/perl



$version=shift() || die "Welche Version??\n";
$version =~ m#^(\d+\.)+\d+$# || die "Version ungültig!!\n";


$url="http://fsvs.tigris.org/svn/fsvs";
$tagdir="fsvs-$version";
$tagurl="$url/tags/$tagdir";

system("svn cp -m 'Tagging $version' $url/branches/fsvs-1.2.x/ $tagurl");
warn "Fehler $? beim Taggen!" if $?;

#print "Getaggt!! Warte auf Bestätigung.\n"; $_=<STDIN>;

srand();
$tempdir="/tmp/" . $$ . ".tmp.".rand();

mkdir ($tempdir) || die "mkdir($tempdir): $!";

sub C { system("rm -rf $tempdir"); };

$SIG{"__DIE__"}=sub { print @_; C(); exit($! || 1); };


system("svn export $tagurl/fsvs $tempdir/$tagdir");
#system("svn export $url/trunk/fsvs $tempdir/$tagdir");
die "Fehler $?" if $?;

chdir($tempdir);
system("cd $tagdir && autoconf");
if ($?)
{
#die "Fehler $?" if $?;
print "Fehler $?!!\n";
system("/bin/bash");
}

open(CH, "< $tagdir/CHANGES") || die $!;
open(CHHTML,"> CHANGES.html") || die $!;
while(<CH>)
{
chomp;
	last if /^\s*$/;

	print(CHHTML "<B>$_</B>\n<ul>\n"), next if (/^\w/);
	s#^- #<li>#;
	print CHHTML $_, "\n";
}
print CHHTML "</ul>\n";
close CH; close CHHTML;

system("tar -cvf $tagdir.tar $tagdir");
die "Fehler $?" if $?;

system("bzip2 -v9k $tagdir.tar");
die "Fehler $?" if $?;
system("gzip -v9 $tagdir.tar");
die "Fehler $?" if $?;

system("md5sum *.tar.* > MD5SUM");
die "Fehler $?" if $?;

print "ok\n\n   cd $tempdir\n\n";

#C();

exit(0);

