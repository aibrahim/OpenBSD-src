package autodie;
use 5.008;
use strict;
use warnings;

use Fatal ();
our @ISA = qw(Fatal);
our $VERSION;

BEGIN {
    $VERSION = '2.06_01';
}

use constant ERROR_WRONG_FATAL => q{
Incorrect version of Fatal.pm loaded by autodie.

The autodie pragma uses an updated version of Fatal to do its
heavy lifting.  We seem to have loaded Fatal version %s, which is
probably the version that came with your version of Perl.  However
autodie needs version %s, which would have come bundled with
autodie.

You may be able to solve this problem by adding the following
line of code to your main program, before any use of Fatal or
autodie.

    use lib "%s";

};

# We have to check we've got the right version of Fatal before we
# try to compile the rest of our code, lest we use a constant
# that doesn't exist.

BEGIN {

    # If we have the wrong Fatal, then we've probably loaded the system
    # one, not our own.  Complain, and give a useful hint. ;)

    if ($Fatal::VERSION ne $VERSION) {
        my $autodie_path = $INC{'autodie.pm'};

        $autodie_path =~ s/autodie\.pm//;

        require Carp;

        Carp::croak sprintf(
            ERROR_WRONG_FATAL, $Fatal::VERSION, $VERSION, $autodie_path
        );
    }
}

# When passing args to Fatal we want to keep the first arg
# (our package) in place.  Hence the splice.

sub import {
        splice(@_,1,0,Fatal::LEXICAL_TAG);
        goto &Fatal::import;
}

sub unimport {
        splice(@_,1,0,Fatal::LEXICAL_TAG);
        goto &Fatal::unimport;
}

1;

__END__

=head1 NAME

autodie - Replace functions with ones that succeed or die with lexical scope

=head1 SYNOPSIS

    use autodie;            # Recommended: implies 'use autodie qw(:default)'

    use autodie qw(:all);   # Recommended more: defaults and system/exec.

    use autodie qw(open close);   # open/close succeed or die

    open(my $fh, "<", $filename); # No need to check!

    {
        no autodie qw(open);          # open failures won't die
        open(my $fh, "<", $filename); # Could fail silently!
        no autodie;                   # disable all autodies
    }

=head1 DESCRIPTION

        bIlujDI' yIchegh()Qo'; yIHegh()!

        It is better to die() than to return() in failure.

                -- Klingon programming proverb.

The C<autodie> pragma provides a convenient way to replace functions
that normally return false on failure with equivalents that throw
an exception on failure.

The C<autodie> pragma has I<lexical scope>, meaning that functions
and subroutines altered with C<autodie> will only change their behaviour
until the end of the enclosing block, file, or C<eval>.

If C<system> is specified as an argument to C<autodie>, then it
uses L<IPC::System::Simple> to do the heavy lifting.  See the
description of that module for more information.

=head1 EXCEPTIONS

Exceptions produced by the C<autodie> pragma are members of the
L<autodie::exception> class.  The preferred way to work with
these exceptions under Perl 5.10 is as follows:

    use feature qw(switch);

    eval {
        use autodie;

        open(my $fh, '<', $some_file);

        my @records = <$fh>;

        # Do things with @records...

        close($fh);

    };

    given ($@) {
        when (undef)   { say "No error";                    }
        when ('open')  { say "Error from open";             }
        when (':io')   { say "Non-open, IO error.";         }
        when (':all')  { say "All other autodie errors."    }
        default        { say "Not an autodie error at all." }
    }

Under Perl 5.8, the C<given/when> structure is not available, so the
following structure may be used:

    eval {
        use autodie;

        open(my $fh, '<', $some_file);

        my @records = <$fh>;

        # Do things with @records...

        close($fh);
    };

    if ($@ and $@->isa('autodie::exception')) {
        if ($@->matches('open')) { print "Error from open\n";   }
        if ($@->matches(':io' )) { print "Non-open, IO error."; }
    } elsif ($@) {
        # A non-autodie exception.
    }

See L<autodie::exception> for further information on interrogating
exceptions.

=head1 CATEGORIES

Autodie uses a simple set of categories to group together similar
built-ins.  Requesting a category type (starting with a colon) will
enable autodie for all built-ins beneath that category.  For example,
requesting C<:file> will enable autodie for C<close>, C<fcntl>,
C<fileno>, C<open> and C<sysopen>.

The categories are currently:

    :all
        :default
            :io
                read
                seek
                sysread
                sysseek
                syswrite
                :dbm
                    dbmclose
                    dbmopen
                :file
                    binmode
                    close
                    fcntl
                    fileno
                    flock
                    ioctl
                    open
                    sysopen
                    truncate
                :filesys
                    chdir
                    closedir
                    opendir
                    link
                    mkdir
                    readlink
                    rename
                    rmdir
                    symlink
                    unlink
                :ipc
                    pipe
                    :msg
                        msgctl
                        msgget
                        msgrcv
                        msgsnd
                    :semaphore
                        semctl
                        semget
                        semop
                    :shm
                        shmctl
                        shmget
                        shmread
                :socket
                    accept
                    bind
                    connect
                    getsockopt
                    listen
                    recv
                    send
                    setsockopt
                    shutdown
                    socketpair
            :threads
                fork
        :system
            system
            exec


Note that while the above category system is presently a strict
hierarchy, this should not be assumed.

A plain C<use autodie> implies C<use autodie qw(:default)>.  Note that
C<system> and C<exec> are not enabled by default.  C<system> requires
the optional L<IPC::System::Simple> module to be installed, and enabling
C<system> or C<exec> will invalidate their exotic forms.  See L</BUGS>
below for more details.

The syntax:

    use autodie qw(:1.994);

allows the C<:default> list from a particular version to be used.  This
provides the convenience of using the default methods, but the surety
that no behavorial changes will occur if the C<autodie> module is
upgraded.

C<autodie> can be enabled for all of Perl's built-ins, including
C<system> and C<exec> with:

    use autodie qw(:all);

=head1 FUNCTION SPECIFIC NOTES

=head2 flock

It is not considered an error for C<flock> to return false if it fails
to an C<EWOULDBLOCK> (or equivalent) condition.  This means one can
still use the common convention of testing the return value of
C<flock> when called with the C<LOCK_NB> option:

    use autodie;

    if ( flock($fh, LOCK_EX | LOCK_NB) ) {
        # We have a lock
    }

Autodying C<flock> will generate an exception if C<flock> returns
false with any other error.

=head2 system/exec

The C<system> built-in is considered to have failed in the following
circumstances:

=over 4

=item *

The command does not start.

=item *

The command is killed by a signal.

=item *

The command returns a non-zero exit value (but see below).

=back

On success, the autodying form of C<system> returns the I<exit value>
rather than the contents of C<$?>.

Additional allowable exit values can be supplied as an optional first
argument to autodying C<system>:

    system( [ 0, 1, 2 ], $cmd, @args);  # 0,1,2 are good exit values

C<autodie> uses the L<IPC::System::Simple> module to change C<system>.
See its documentation for further information.

Applying C<autodie> to C<system> or C<exec> causes the exotic
forms C<system { $cmd } @args > or C<exec { $cmd } @args>
to be considered a syntax error until the end of the lexical scope.
If you really need to use the exotic form, you can call C<CORE::system>
or C<CORE::exec> instead, or use C<no autodie qw(system exec)> before
calling the exotic form.

=head1 GOTCHAS

Functions called in list context are assumed to have failed if they
return an empty list, or a list consisting only of a single undef
element.

=head1 DIAGNOSTICS

=over 4

=item :void cannot be used with lexical scope

The C<:void> option is supported in L<Fatal>, but not
C<autodie>.  To workaround this, C<autodie> may be explicitly disabled until
the end of the current block with C<no autodie>.
To disable autodie for only a single function (eg, open)
use C<no autodie qw(open)>.

=item No user hints defined for %s

You've insisted on hints for user-subroutines, either by pre-pending
a C<!> to the subroutine name itself, or earlier in the list of arguments
to C<autodie>.  However the subroutine in question does not have
any hints available.

=back

See also L<Fatal/DIAGNOSTICS>.

=head1 BUGS

"Used only once" warnings can be generated when C<autodie> or C<Fatal>
is used with package filehandles (eg, C<FILE>).  Scalar filehandles are
strongly recommended instead.

When using C<autodie> or C<Fatal> with user subroutines, the
declaration of those subroutines must appear before the first use of
C<Fatal> or C<autodie>, or have been exported from a module.
Attempting to use C<Fatal> or C<autodie> on other user subroutines will
result in a compile-time error.

Due to a bug in Perl, C<autodie> may "lose" any format which has the
same name as an autodying built-in or function.

C<autodie> may not work correctly if used inside a file with a
name that looks like a string eval, such as F<eval (3)>.

=head2 autodie and string eval

Due to the current implementation of C<autodie>, unexpected results
may be seen when used near or with the string version of eval.
I<None of these bugs exist when using block eval>.

Under Perl 5.8 only, C<autodie> I<does not> propagate into string C<eval>
statements, although it can be explicitly enabled inside a string
C<eval>.

Under Perl 5.10 only, using a string eval when C<autodie> is in
effect can cause the autodie behaviour to leak into the surrounding
scope.  This can be worked around by using a C<no autodie> at the
end of the scope to explicitly remove autodie's effects, or by
avoiding the use of string eval.

I<None of these bugs exist when using block eval>.  The use of
C<autodie> with block eval is considered good practice.

=head2 REPORTING BUGS

Please report bugs via the CPAN Request Tracker at
L<http://rt.cpan.org/NoAuth/Bugs.html?Dist=autodie>.

=head1 FEEDBACK

If you find this module useful, please consider rating it on the
CPAN Ratings service at
L<http://cpanratings.perl.org/rate?distribution=autodie> .

The module author loves to hear how C<autodie> has made your life
better (or worse).  Feedback can be sent to
E<lt>pjf@perltraining.com.auE<gt>.

=head1 AUTHOR

Copyright 2008-2009, Paul Fenwick E<lt>pjf@perltraining.com.auE<gt>

=head1 LICENSE

This module is free software.  You may distribute it under the
same terms as Perl itself.

=head1 SEE ALSO

L<Fatal>, L<autodie::exception>, L<autodie::hints>, L<IPC::System::Simple>

I<Perl tips, autodie> at
L<http://perltraining.com.au/tips/2008-08-20.html>

=head1 ACKNOWLEDGEMENTS

Mark Reed and Roland Giersig -- Klingon translators.

See the F<AUTHORS> file for full credits.  The latest version of this
file can be found at
L<http://github.com/pfenwick/autodie/tree/master/AUTHORS> .

=cut
