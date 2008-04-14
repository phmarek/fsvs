#!/usr/bin/perl

# read whole files
undef $/;

$exe_lines=$sum_lines=0;
%runs=();
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

# #####:   77:	STOPIF( st__status(sts, path), NULL);
# TODO: Count the whole block; eg. DEBUG normally has more than a single 
# line.
	open(GCOV, "< $c_file.gcov");
	{
		local($/)="\n";
		$last_line=$cur=0;
# find untested lines, and count them
   $this_run=0;
		while (<GCOV>)
		{
			$cur++;
			if (/^\s*(#####|-):\s+\d+:\s+(STOPIF|BUG|BUG_ON|DEBUGP)?/)
			{
				$stopif_lines++ if $2;
				if ($last_line == $cur -1)
				{
					$old=delete $runs{$c_file . "\0" . $last_line};

# An line without executable code (mark '-') is taken as continuation, but 
# doesn't add to unexecuted lines.
					$runs{$c_file . "\0" . $cur} = 
						[ $old->[0] + ($1 eq "#####" ? 1 : 0), 
						$old->[1] || $cur ];
				}
				$last_line=$cur;
			}

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
	$ntest=$lines-$covered;
	$name =~ s#\.gcov\.smry$##i;
	write;
}


format STDOUT_TOP=
 Percent | exec'd | #lines | #!test | #ignrd | Filename
---------+--------+--------+--------+--------+----------------------------
.
format STDOUT=
 @##.##% | @##### | @##### | @##### | @##### | @<<<<<<<<<<<<<<<<<<<<<<<<<<
	$pct, $covered, $lines, $ntest, $ignored, $name
.

print $delim;

$pct=100.0*$exe_lines/$sum_lines;
$covered=$exe_lines;
$lines=$sum_lines;
$name="Total";
write;
print $delim;

printf " %6.2f%% coverage when counting %d error handling lines as executed\n",
	100.0*($exe_lines+$stopif_lines)/$sum_lines, $stopif_lines;

print "-" x (length($delim)-1), "\n\n";


# Print runs
@runs_by_length=();
map { $runs_by_length[$runs{$_}[0]]{$_}=$runs{$_}; } keys %runs;
$max=10;
print "Longest runs:\n";
while ($max>0 && @runs_by_length)
{
	$this_length=$#runs_by_length;
	printf "  %3d# ",$this_length;
	$length_arr=delete $runs_by_length[$this_length];
	for (sort keys %$length_arr)
	{
		($file, $last)=split(/\0/);
		print " ",$file,":",$length_arr->{$_}[1];
		$max--;
	}
	print "\n";
}

print "\n\n";

