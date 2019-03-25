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

my $port = 8900;
my $binary_hdr;
my @blocks;

my $ok = GetOptions
    ('port=i' => \$port,
     'bin' => \$binary_hdr);


if( not $ok or scalar(@ARGV) == 0 )
{
    print STDERR "Usage: $0 [options...] BLCK1 [BLCK2 ...]\n",
    "Options:\n",
    "  --port=N        \[$port\] TCP port to listen to websocket connection\n",
    "  --bin           --exp-ws-bin-header is used in exporter\n";
    exit 1;
}

foreach my $blk (@ARGV)
{
    if( $blk !~ /^\d+$/ )
    {
        print STDERR "$blk is not a valid block number\n";
        exit 1;
    }
    push(@blocks, $blk);
}


my $json = JSON->new->pretty->canonical;
my %blocks_requested;


$SIG{INT} = sub {
    print STDERR "Unprocessed blocks: \n", join("\n", sort keys %blocks_requested), "\n";
    die();
};

Net::WebSocket::Server->new(
    listen => $port,
    on_connect => sub {
        my ($serv, $conn) = @_;
        $conn->on(
            'ready' => sub {
                my ($conn) = @_;
                foreach my $blk (@blocks)
                {
                    $blocks_requested{$blk} = 1;
                    $conn->send_binary(sprintf("%d", $blk));
                }
            },
            'binary' => sub {
                my ($conn, $msg) = @_;
                if( $binary_hdr )
                {
                    my ($msgtype, $opts, $js) = unpack('VVa*', $msg);
                    my $data = eval {$json->decode($js)};
                    if( $@ )
                    {
                        print STDERR $@, "\n\n";
                        print STDERR $js, "\n";
                        exit;
                    } 

                    printf("%d %d\n", $msgtype, $opts);
                    print $json->encode($data);
                    print "\n";
                    
                    if( $msgtype == 1010 )
                    {
                        delete $blocks_requested{$data->{'block_num'}};
                    }
                }
                else
                {
                    my $data = eval {$json->decode($msg)};
                    if( $@ )
                    {
                        print STDERR $@, "\n\n";
                        print STDERR $msg, "\n";
                        exit;
                    } 
                    print $json->encode($data), "\n\n";
                    my $type = $data->{'msgtype'};
                    
                    if( $type eq 'BLOCK_COMPLETED' )
                    {
                        delete $blocks_requested{$data->{'block_num'}};
                    }
                }
                
                if( scalar(keys %blocks_requested) == 0 )
                {
                    $conn->disconnect(1000);
                }
            },
            'disconnect' => sub {
                my ($conn, $code) = @_;
                print STDERR "Disconnected: $code\n";
                exit(0);
            },
            
            );
    },
    )->start;



    
