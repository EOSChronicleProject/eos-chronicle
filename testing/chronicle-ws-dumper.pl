# This script works as a websocket server for Chronicle websocket exporter
# and dumps pretty-formatted JSON messages to stdout

# install dependencies:
#  sudo apt install cpanminus libjson-xs-perl libjson-perl
#  sudo cpanm Net::WebSocket::Server

use strict;
use warnings;
use JSON;
use Getopt::Long;
use Net::WebSocket::Server;

$| = 1;

my $port = 8800;

my $ok = GetOptions
    ('port=i' => \$port);


if( not $ok or scalar(@ARGV) > 0 )
{
    print STDERR "Usage: $0 [options...]\n",
    "Options:\n",
    "  --port=N        \[$port\] TCP port to listen to websocket connection\n";
    exit 1;
}


my $json = JSON->new->pretty->canonical;

Net::WebSocket::Server->new(
    listen => $port,
    on_connect => sub {
        my ($serv, $conn) = @_;
        $conn->on(
            'binary' => sub {
                my ($conn, $msg) = @_;
                my $data = eval {$json->decode($msg)};
                if( $@ )
                {
                    print STDERR $@, "\n\n";
                    print STDERR $msg, "\n";
                    exit;
                } 
                print $json->encode($data), "\n\n";
            },
            );
    },
    )->start;



