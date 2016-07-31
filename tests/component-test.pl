#!/usr/bin/perl

use IPC::Open2;

$logfile=shift;

$|=1;

# Don't try $BINdflt - that might have valgrind or similar in front.
$pid = open2(RDR, WTR, 'gdb ' . $ENV{"BIN_FULLPATH"} . ' 2>&1');
$gdb_prompt_delimiter = "GDB-delim-$$-" . time;

$ign=Exch("set pagination off\nset prompt $gdb_prompt_delimiter\\n");

$match="";
$err=0;
$running=0;
$line=0;

@output=();

while (<>)
{
	$line++;
	chomp;
	next if m/^\s*##/;
	next if m/^\s*$/;

	$match="\\\$\\d+ = $1",next
		if m{^\s*#=\s+(.*)};
	$match=$1,next 
		if m{^\s*#[/~]\s+(.*)};

	if (!$running)
	{
		if (/^\s*(print|set|call)\s*/) 
		{
			$err ||= Exch("b _do_component_tests");
# We have to use -D to avoid getting debug messages ... they'd show 
# up in the output, and potentially mess our matching up.
			$err ||= Exch("r -d -D invalid");
			$running=1;
		}
		else
		{
			$running=1 if m#^\s*(r|R)#;
		}
	}

	if (s#^\+##)
	{
		$_=eval($_);
		die $@ if $@;
	}
	else
	{
# substitute $#$ENV{"WAA"}# and similar.
# We don't use ${} as that's needed for hash lookup (%ENV)
		while (s/\$\#(.*?)\#/eval($1)/e)
		{
			die $@ if $@;
		}
	}


	$err ||= Exch($_, $match);
	$match="";

	$running=0 if m#^\s*kill#;
}

Exch("kill");
Exch("q");

open(LOG, "> $logfile") || die "$logfile: $!\n";
print LOG @output;
close LOG;

print @output if $err || length($ENV{"VERBOSE"});
exit $err;



sub Exch
{
	my($out, $exp)=@_;
	my($input, $ok, $err);
	local(%SIG);
	local($/);

	$/=$gdb_prompt_delimiter;

	$SIG{"ALRM"}=sub { die "Timeout waiting for $exp\n"; };

	print WTR $out,"\n";
	push @output,"send>> ", $out,"\n";

	alarm(4);
	$input=<RDR>;
	alarm(0);


	substr($input, -length($/), length($/))="";

# find non-empty lines
	@in=();
	map {
		push @in, $_;
	} grep(/\S/, split(/\n/, $input));
	@in_str=map { "recv<< " . $_ . "\n"; } @in;
	push @output, @in_str;

	return 0 if (!$exp);

	$found = grep(m/$exp/m, @in);
	$err=!$found;
	push @output, "expect '@in' to match /$exp/: err=$err\n";

	warn("$ARGV:$line: /$exp/ not matched:\n", @in_str)
		if ($err);
	return $err;
}

