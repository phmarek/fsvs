#!/usr/bin/perl



$version=shift() || die "Welche Version??\n";
$version =~ m#^(\d+\.)+\d+$# || die "Version ungültig!!\n";


$tagdir="fsvs-$version";

system("git tag '$tagdir'");
warn "Fehler $? beim Taggen!" if $?;

#print "Getaggt!! Warte auf Bestätigung.\n"; $_=<STDIN>;

srand();
$tempdir="/tmp/" . $$ . ".tmp.".rand();

mkdir ($tempdir) || die "mkdir($tempdir): $!";

sub C { system("rm -rf '$tempdir'"); };

$SIG{"__DIE__"}=sub { print @_; C(); exit($! || 1); };

chdir ".." || die $! if -e 'ac_list.c';

system("git archive --prefix '$tagdir/' '$tagdir' | tar -xf - -C '$tempdir'");
die "Fehler $?" if $?;

chdir($tempdir);
system("cd $tagdir && autoconf");
if ($?)
{
	#die "Fehler $?" if $?;
	print "Fehler $?!!\n";
	system("/bin/bash");
}

# open(CH, "< $tagdir/CHANGES") || die $!;
# open(CHHTML,"> CHANGES.html") || die $!;
# while(<CH>)
# {
# 	chomp;
# 	last if /^\s*$/;
# 
# 	print(CHHTML "<B>$_</B>\n<ul>\n"), next if (/^\w/);
# 	s#^- #<li>#;
# 	print CHHTML $_, "\n";
# }
# print CHHTML "</ul>\n";
# close CH; close CHHTML;

chdir($tempdir);

system("tar -cvf $tagdir.tar $tagdir");
die "Fehler $?" if $?;

system("bzip2 -v9k $tagdir.tar");
die "Fehler $?" if $?;
system("gzip -v9 $tagdir.tar");
die "Fehler $?" if $?;

system("md5sum *.tar.* > MD5SUM");
die "Fehler $?" if $?;
system("sha256sum *.tar.* > SHA256SUM");
die "Fehler $?" if $?;

print "ok\n\n   cd $tempdir\n\n";

#C();

exit(0);

