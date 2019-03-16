# This script works as a websocket server for Chronicle websocket exporter
# and dumps pretty-formatted JSON messages to stdout

# install dependencies:
#  sudo apt install cpanminus libjson-xs-perl libjson-perl
#  sudo cpanm Net::WebSocket::Server

use strict;
use warnings;
use JSON;
use Getopt::Long;
use Time::HiRes qw(time);

use Net::WebSocket::Server;
use Protocol::WebSocket::Frame;

$Protocol::WebSocket::Frame::MAX_PAYLOAD_SIZE = 100*1024*1024;
$Protocol::WebSocket::Frame::MAX_FRAGMENTS_AMOUNT = 102400;
    
$| = 1;

my $port = 8800;
my $ack;

my $ok = GetOptions
    ('port=i' => \$port,
     'ack=f' => \$ack);


if( not $ok or scalar(@ARGV) > 0 )
{
    print STDERR "Usage: $0 [options...]\n",
    "Options:\n",
    "  --port=N        \[$port\] TCP port to listen to websocket connection\n",
    "  --ack=N         Send acknowledgements every N seconds\n";
    exit 1;
}


my $json = JSON->new->pretty->canonical;
my $last_ack = time();
my $last_block = 0;

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
                if( defined($ack) )
                {
                    my $type = $data->{'msgtype'};
                    
                    if( $type eq 'BLOCK' )
                    {
                        $last_block = $data->{'data'}{'block_num'};
                    }
                    
                    if( $type eq 'BLOCK'  or $type eq 'RCVR_PAUSE' )
                    {
                        if( time() - $last_ack >= $ack )
                        {
                            $last_ack = time();
                            $conn->send_binary(sprintf("%d", $last_block));
                        }
                    }
                }
            },
            'disconnect' => sub {
                my ($conn, $code) = @_;
                print STDERR "Disconnected: $code\n";
            },
            
            );
    },
    )->start;



