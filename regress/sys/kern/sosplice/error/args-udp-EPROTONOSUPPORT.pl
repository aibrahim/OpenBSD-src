# test EPROTONOSUPPORT for splicing tcp with udp sockets

use strict;
use warnings;
use IO::Socket;
use BSD::Socket::Splice "SO_SPLICE";

our %args = (
    errno => 'EPROTONOSUPPORT',
    func => sub {
	my $s = IO::Socket::INET->new(
	    Proto => "udp",
	) or die "socket failed: $!";

	my $ss = IO::Socket::INET->new(
	    Proto => "tcp",
	) or die "socket splice failed: $!";

	$s->setsockopt(SOL_SOCKET, SO_SPLICE, pack('i', $ss->fileno()))
	    and die "splice udp sockets succeeded";
    },
);
