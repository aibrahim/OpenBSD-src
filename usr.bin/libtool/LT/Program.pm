# $OpenBSD: Program.pm,v 1.8 2012/07/10 13:32:10 espie Exp $

# Copyright (c) 2007-2010 Steven Mestdagh <steven@openbsd.org>
# Copyright (c) 2012 Marc Espie <espie@openbsd.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

use strict;
use warnings;
use feature qw(say switch state);

package LT::Program;
use File::Basename;
use LT::Archive;
use LT::Util;
use LT::Trace;

sub new
{
	my $class = shift;
	bless {}, $class;
}

# write a wrapper script for an executable so it can be executed within
# the build directory
sub write_wrapper
{
	my $self = shift;

	my $program = $self->{outfilepath};
	my $pfile = basename($program);
	my $realprogram = $ltdir . '/' . $pfile;
	open(my $pw, '>', $program) or die "Cannot write $program: $!\n";
	print $pw <<EOF
#!/bin/sh

# $program - wrapper for $realprogram
# Generated by libtool $version

argdir=`dirname \$0`
if test -f "\$argdir/$realprogram"; then
    # Add our own library path to LD_LIBRARY_PATH
    LD_LIBRARY_PATH=\$argdir/$ltdir
    export LD_LIBRARY_PATH

    # Run the actual program with our arguments.
    exec "\$argdir/$realprogram" \${1+"\$\@"}

    echo "\$0: cannot exec $program \${1+"\$\@"}"
    exit 1
else
    echo "\$0: error: \\\`\$argdir/$realprogram' does not exist." 1>&2
    exit 1
fi
EOF
;
	close($pw);
	chmod 0755, $program;
}

sub install
{
	my ($class, $src, $dst, $instprog, $instopts) = @_;

	my $srcdir = dirname $src;
	my $srcfile = basename $src;
	my $realpath = "$srcdir/$ltdir/$srcfile";
	LT::Exec->install(@$instprog, @$instopts, $realpath, $dst);
}

sub link
{
	require LT::Linker;
	return LT::Linker::Program->link(@_);
}

package LT::Linker::Program;
our @ISA = qw(LT::Linker);

use LT::Trace;
use LT::Util;
use File::Basename;

sub link
{
	my ($class, $self, $ltprog, $ltconfig, $dirs, $libs, $deplibs, 
	    $libdirs, $parser, $gp) = @_;

	tsay {"linking program (", ($gp->static ? "not " : ""),
	      	"dynamically linking not-installed libtool libraries)"};

	my $what = ref($self);
	my $fpath  = $self->{outfilepath};
	my $RPdirs = $self->{RPdirs};

	my $odir  = dirname($fpath);
	my $fname = basename($fpath);

	my @libflags;
	my @cmd;
	my $dst;

	tsay {"argvstring (pre resolve_la): @{$parser->{args}}"};
	my $args = $parser->resolve_la($deplibs, $libdirs);
	tsay {"argvstring (post resolve_la): @{$parser->{args}}"};
	my $orderedlibs = [];
	my $staticlibs = [];
	$parser->{args} = $args;
	$parser->{seen_la_shared} = 0;
	$args = $parser->parse_linkargs2(\@main::Rresolved,
		\@main::libsearchdirs, $orderedlibs, $staticlibs, $dirs, $libs);
	tsay {"staticlibs = \n", join("\n", @$staticlibs)};
	tsay {"orderedlibs = @$orderedlibs"};
	my $finalorderedlibs = reverse_zap_duplicates_ref($orderedlibs);
	tsay {"final orderedlibs = @$finalorderedlibs"};

	my $symlinkdir = $ltdir;
	if ($odir ne '.') {
		$symlinkdir = "$odir/$ltdir";
	}
	mkdir $symlinkdir if ! -d $symlinkdir;
	if ($parser->{seen_la_shared}) {
		$dst = ($odir eq '.') ? "$ltdir/$fname" : "$odir/$ltdir/$fname";
		$self->write_wrapper;
	} else {
		$dst = ($odir eq '.') ? $fname : "$odir/$fname";
	}

	my $symbolsfile;
	if ($gp->export_symbols) {
		$symbolsfile = $gp->export_symbols;
	} elsif ($gp->export_symbols_regex) {
		($symbolsfile = "$odir/$ltdir/$fname") =~ s/\.la$/.exp/;
		LT::Archive->get_symbollist($symbolsfile, $gp->export_symbols_regex, $self->{objlist});
	}
	$libdirs = reverse_zap_duplicates_ref($libdirs);
	# add libdirs to rpath if they are not in standard lib path
	for my $l (@$libdirs) {
		my $found = 0;
		for my $d (@main::libsearchdirs) {
			if ($l eq $d) { $found = 1; last; }
		}
		if (!$found) { push @$RPdirs, $l; }
	}
	$RPdirs = reverse_zap_duplicates_ref($RPdirs);
	map { $_ = "-Wl,-rpath,$_" } @$RPdirs;
	foreach my $k (keys %$libs) {
		tprint {"key = $k - "};
		my $r = ref($libs->{$k});
		tsay {"ref = $r"};
		if (!defined $libs->{$k}) {
			tsay {"creating library object for $k"};
			require LT::Library;
			$libs->{$k} = LT::Library->new($k);
		}
		my $l = $libs->{$k};
		$l->find($dirs, 1, $gp->static, $what);
	}

	my @libobjects = values %$libs;
	tsay {"libs:\n", join("\n", keys %$libs)};
	tsay {"libfiles:\n", join("\n", map { $_->{fullpath} } @libobjects)};

	$class->create_symlinks($symlinkdir, $libs);
	foreach my $k (@$finalorderedlibs) {
		my $a = $libs->{$k}->{fullpath} || die "Link error: $k not found in \$libs\n";
		if ($a =~ m/\.a$/) {
			# don't make a -lfoo out of a static library
			push @libflags, $a;
		} else {
			my $lib = basename($a);
			if ($lib =~ m/^lib(.*)\.so(\.\d+){2}/) {
				$lib = $1;
			} else {
				say "warning: cannot derive -l flag from library filename $a, assuming hash key $k";
				$lib = $k;
			}
			push @libflags, "-l$lib";
		}
	}

	@cmd = @$ltprog;
	push @cmd, '-o', $dst;
	push @cmd, @$args if ($args);
	push @cmd, @{$self->{objlist}} if (@{$self->{objlist}});
	push @cmd, @$staticlibs if (@$staticlibs);
	push @cmd, "-L$symlinkdir", @libflags if (@libflags);
	push @cmd, @$RPdirs if (@$RPdirs);
	push @cmd, "-Wl,-retain-symbols-file,$symbolsfile" if ($symbolsfile);
	LT::Exec->link(@cmd);
}
1;
