#!/usr/bin/perl

# read whole files
undef $/;

$exe_lines=$sum_lines=0;
while (<>)
{
	($c_file=$ARGV) =~ s#\.gcov\.smry$##; 

# File 'warnings.c'
# Lines executed:85.71% of 28
# warnings.c:creating 'warnings.c.gcov'
	($pct, $lines) = (m#File '$c_file'\s+Lines executed:([\d\.]+)% of (\d+)#);
	if (!$lines)
	{
		warn "Cannot parse (or no lines executed) for $ARGV.\n";
		next;
	}

	open(SRC, "< " . $c_file) || die $!;
	@funcs_to_ignore = map { 
		m#\s*/// FSVS GCOV MARK: (\w+)# ? $1 : ();
	} split(/\n/,<SRC>);
	close(SRC);

	$ignored=0;
	for $func (@funcs_to_ignore)
	{
		($fexec, $flines) = 
			m#Function '$func'\s+Lines executed:([\d\.]+)\% of (\d+)#;
		if (!defined($flines))
		{
			warn "Function $func should be ignored, but was not found!\n";
		}
		elsif ($fexec>0)
		{
			warn "Function $func should be ignored, but was run!\n";
	}
	else
	{
			$ignored += $flines;
		}
	}

	$covered=int($lines*$pct/100.0+0.5);
	$lines -= $ignored;
	$pct=$covered/$lines*100.0;
	$cover{sprintf("%9.5f-%s",$pct,$ARGV)} = 
		[$lines, $pct, $ARGV, $covered, $ignored];
	$sum_lines+=$lines;
	$exe_lines+=$covered;
}

die "No useful information found!!\n" if !$sum_lines;

$delim="---------+--------+--------+--------------------------------------------------\n";
print "\n\n", $delim;

for (reverse sort keys %cover)
{
	($lines, $pct, $name, $covered, $ignored)=@{$cover{$_}};
	$name =~ s#\.gcov\.smry$##i;
	write;
}


format STDOUT_TOP=
 Percent | exec'd | #lines | #ignrd | Filename
---------+--------+--------+--------+--------------------------------------
.
format STDOUT=
 @##.##% | @##### | @##### | @##### | @<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
	$pct, $covered, $lines, $ignored, $name
.

print $delim;

$pct=100.0*$exe_lines/$sum_lines;
$covered=$exe_lines;
$lines=$sum_lines;
$name="Total";
write;
print $delim, "\n\n";

