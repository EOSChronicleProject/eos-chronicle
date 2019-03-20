# install dependencies:
#  sudo apt install cpanminus libjson-xs-perl libjson-perl
#  sudo cpanm ZMQ::Raw


use strict;
use warnings;
use ZMQ::Raw;
use JSON;
use Getopt::Long;


$| = 1;

my $ep_data = 'tcp://127.0.0.1:5557';
my $ep_acks = 'tcp://127.0.0.1:5558';
my $ack = 100;

my $ok = GetOptions
    ('edata=s' => \$ep_data,
     'eacks=s' => \$ep_acks,
     'ack=i' => \$ack);


if( not $ok or scalar(@ARGV) > 0  )
{
    print STDERR "Usage: $0 [options...]\n",
    "The utility connects to Chronicle ZMQ export plugin and dumps prettyfied JSON to stdout\n",
    "Options:\n",
    "  --edata=ENDPOINT  \[$ep_data\] data ZMQ socket\n",
    "  --eacks=ENDPOINT  \[$ep_acks\] acknowledgements ZMQ socket\n",
    "  --ack=N           \[$ack\] Send acknowledgements every N blocks\n";
    exit 1;
}



my $ctxt = ZMQ::Raw::Context->new;

my $s_data = ZMQ::Raw::Socket->new ($ctxt, ZMQ::Raw->ZMQ_PULL);
$s_data->setsockopt(ZMQ::Raw::Socket->ZMQ_RCVBUF, 10240);
$s_data->connect($ep_data);

my $s_acks = ZMQ::Raw::Socket->new ($ctxt, ZMQ::Raw->ZMQ_PUB);
$s_acks->bind($ep_acks);


my $sighandler = sub {
    print STDERR ("Disconnecting the ZMQ sockets\n");
    $s_data->disconnect($ep_data);
    $s_data->close();
    $s_acks->disconnect($ep_acks);
    $s_acks->close();
    print STDERR ("Finished\n");
    exit;
};


$SIG{'HUP'} = $sighandler;
$SIG{'TERM'} = $sighandler;
$SIG{'INT'} = $sighandler;


my $json = JSON->new->pretty->canonical;
my $last_ack = 0;
my $last_block = 0;

while(1)
{
    my $data = $s_data->recv();
    my ($msgtype, $opts, $js) = unpack('VVa*', $data);
    $data = $json->decode($js);

    printf("%d %d\n", $msgtype, $opts);
    print $json->encode($data);
    print "\n";

    if( $msgtype == 1002 )
    {
        $last_block = $data->{'block_num'};
    }
    
    if( ($msgtype == 1002 and $last_block - $last_ack >= $ack) or
        $msgtype == 1009 )
    {
        $last_ack = $last_block - 1;
        $s_acks->send(sprintf("%d", $last_ack));
        print STDERR "ack $last_ack\n";
    }
}

print "The stream ended\n";
