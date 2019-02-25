# EOS Chronicle Project

This project is an implementation of work proposal as published at
https://github.com/cc32d9/eos-work-proposals/tree/master/001_EOS_History_Database

The goal is to build a scalable and reliable toolset for maintaining EOS
blockchain history in a database.

The first stage of the project is to produce a feed of blochain events
that could be consumed by other components for further
processing. `chronicle-receiver` is implementing this as described
below.


# chronicle-receiver

The receiver is designed to work with `state_history_plugin` of
`nodeos`. It connects to the websocket endpoint provided by
the state history plugin and starts reading its data from a specific
block number.

The receiver is compiled with a number of exporter plugins, and only one
exporter plugin can be enabled in its configuration. Exporter plugins
organize the data export to their respective consumers.

Exporters may work in one of two modes:

* in unidirectional mode, the exporter pushes the data downstream as it
  arrives from `nodeos`. Should `chronicle-receiver` stop, it will start
  from the next block after last known irreversible one.

* in bidirectional mode, the exporter expects that the consumer
  acknowledges block numbers that it has processed and stored. Should
  `chronicle-receiver` stop, it will start from the block number next
  after acknowledged or last known irreversible, whichever is lower.


Unidirectional mode is synchronous: it reads a new portion of data from
`nodeos` as soon as current information is written in the outbound
socket.

Bidirectional mode benefits from asynchronous processing: the receiver
starts with a parameter indicating the maximum number of unacknowledged
blocks (1000 by default), and it continues retrieveing data from
`nodeos` as long as the consumer confirms the blocks within this
maximum. Received and decoded data is kept in a queue that is fed to the
consumer, allowing it to process the data at its own pace. If the number
of unacknowledged blocks reaches the maxumum, the reader pauses itself
with an increasing timer, varying from 0.1 to 30 seconds. If the pause
exceeds 2 seconds, an informational event is generated.

If `nodeos` stops or restarts, `chronicle-receiver` will automatically
stop and close its downstream connection. Also if the downstream
connection closes, the receiver will stop itself and close the
connection to `nodeos`.

The primary job of `chronicle-receiver` is as follows:

* it monitors account changes, and as soon as a new ABI is set on a
  contract, it stores a copy of ABI in its state memory.

* upon receiving transactoin traces and table deltas, it tries using the
  ABI and decoding the raw binary data into the ABI-defined structures.

* all data received from `state_history_plugin` is converted to JSON
  format. If there's no valid ABI for decoding raw data, it's presented
  as a hex string. In this case, an ABI decoder error event is
  generated.

* it feeds all JSON data and all error events to the exporter plugin,
  and the exporter plugin pushes the JSON data to its consumer. As
  described above, the consumer may or may not send acknowledgements for
  processed block numbers.


## State database

`chronicle-receiver` utilizes `chainbase`, the same shared-memory
database library that is used by `nodeos`, to store its state. This
results in the same behavior as with `nodeos`:

* pre-allocated shared memory file is sparse and mostly empty;

* in case of abnormal termination, the shared memory file becomes dirty
  and unusable.

The state database keeps track of block numbers being processed, and it
stores also ABI for all contracts that it detects from `setabi`
actions. Chainbase is maintaining the history of revisions down to the
unacknowledged or irreversible block, in order to be able to roll back
in case of a fork or in case of receiver restart.



## ZMQ exporter plugin

`exp_zmq_plugin` is only supporting unidirectional mode.

It feeds the JSON data to a ZMQ PUSH socket in the same fashion as the
`zmq_plgin` for nodeos: each message starts with two native 32-bit
integers indicating message type and options, and then JSON data
follows. The message types are listed in `chronicle_msgtypes.h`.


## Websocket exporter plugin

`exp_ws_plugin` exports the data to a websocket server, and it supports
both unidirectional and bidirectional modes.

The plugin connects to a specified websocket host and port and opens a
binary stream.

Each outgoing message is a JSON object with two keys: `msgtype`
indicates the type of the message, and `data` contains the corresponding
JSON object, such as transaction trace or table delta.

In bidirectional mode, it expects that the server sends block number
acknowledgements in text format, each number in an individual binary
message.


# Compiling

Minimum requirements: Ubuntu 18.10, 3GB RAM


```
sudo apt update && \
sudo apt install -y git g++ cmake libboost-dev libboost-thread-dev libboost-test-dev \
 libboost-filesystem-dev libboost-date-time-dev libboost-system-dev libboost-iostreams-dev \
 libboost-program-options-dev libboost-locale-dev libssl-dev libgmp-dev pkg-config libzmq5-dev

mkdir build
cd build
git clone https://github.com/EOSChronicleProject/eos-chronicle.git
cd eos-chronicle
git submodule update --init --recursive
mkdir build
cd build
cmake ..
# use "make -j N" for N CPU cores for faster compiling (may require more RAM)
make
```

`examples/exp-dummy-plugin` explains how to add and compile your own plugin to `chronicle-receiver`.



# Configuring and running

Similarly to `nodeos`, `chronicle-receiver` needs a configuratuion
directory with `config.ini` in it, and a data directory where it stores
its internal state.

Further on, we use Linux user `eosio` for running the receiver, and
`/home/eosio/chronicle-config` as configuration directory, although you
may choose other names.

Here's a minimal configuration for the receiver using Websocket
exporter. It connects to `nodeos` process runnig `state_history_plugin`
at `localhost:8080` and exports the data to a websocket server at
`localhost:8800`. In a production environment, hosts may be different
machines in the network. The example is using bidirectional mode and
default queue sizes.

The receiver would stop immediately if the websocket server is not
responding. For further tests, you need a consumer server ready.

The Perl script `testing/chronicle-ws-dumper.pl` can be used as a
websocket server that dumps the input to standard output, but there's an
issue that is not merged into the CPAN module yet:
https://github.com/vti/protocol-websocket/issues/59


```
mkdir /home/eosio/chronicle-config
cat >/home/eosio/chronicle-config/config.ini <<'EOT'
# connection to nodeos state_history_plugin
host = 127.0.0.1
port = 8080
# Websocket exporter in bidirectional mode
plugin = exp_ws_plugin
exp-ws-host = 127.0.0.1
exp-ws-port = 8800
exp-ws-ack  = true
EOT

# Start the receiver to check that everything is working as
# expected. Use Ctrl-C to stop it.
/home/eosio/build/eos-chronicle/build/chronicle-receiver \
  --config-dir=/home/eosio/chronicle-config --data-dir=/home/eosio/chronicle-data

# Prepare for long-term run inside a systemd unit

cat >chronicle-config/chronicle-receiver.service <<'EOT'
[Unit]
Description=EOS Chronicle receiver
[Service]
Type=simple
ExecStart=/home/eosio/build/eos-chronicle/build/chronicle-receiver --config-dir=/home/eosio/chronicle-config --data-dir=/home/eosio/chronicle-data
TimeoutStopSec=300s
Restart=on-success
RestartSec=10
User=eosio
Group=eosio
KillMode=control-group
[Install]
WantedBy=multi-user.target
EOT

sudo cp chronicle-config/chronicle-receiver.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable chronicle-receiver
sudo systemctl start chronicle-receiver
# check the status
sudo systemctl status chronicle-receiver
```

Only one exporter plugin can be activated at a time.

If you need to move or copy the state data, flush the system cache first:

```
sync; echo 3 > /proc/sys/vm/drop_caches
```

# Command-line and configuration options

The following options are available from command-line only:

* `--data-dir=DIR` (mandatory): Directory containing program runtime
  data;

* `--config-dir=DIR` Directory containing configuration files such as
  config.ini. Defaults to `${HOME}/config-dir`;

* `-h [ --help ]`: Print help message and exit;

* `-v [ --version ]`:  Print version information;

* `--print-default-config`: Print default configuration template. The
  output will have empty `plugin` option, so you will need to add an
  exporter plugin to it.

* `--config=FILE` (=`config.ini`): Configuration file name relative to config-dir;

* `--logconf=FILE` (=`logging.json`): Logging configuration file
  name/path for library users. An example file that is only printing
  error messages is located in `examples/` folder.

The following opttions are available from command line and `config.ini`:

* `host = HOST` (=`localhost`): Host to connect to (nodeos with
  state_history_plugin);

* `port = PORT` (=`8080`): Port to connect to (nodeos with state-history
  plugin);

* `skip-to = BLOCK` (=`0`): At the start, jump to the block number after
  the specified one. This will likely lead to inconsistent ABI
  information;

* `receiver-state-db-size = N` (=`1024`): State database size in MB;

* `--report-every = N` (=`10000`) Print informational messages every so
  many blocks;


Options for `exp_ws_plugin`:

* `exp-ws-host = HOST` (mandatory): Websocket server host to connect to;

* `exp-ws-port = PORT` (mandatory): Websocket server port to connect to;

* `exp-ws-ack = true|false` (=`false`) Enable bidirectional mode
  (websocket server is expected to send acknowledgeements for processed
  blocks);

* `exp-ws-max-unack = N` (=1000): Receiver will pause at so many unacknowledged blocks;

* `exp-ws-max-queue = N` (=10000): Receiver will pause if outbound queue exceeds this limit.


Options for `exp_zmq_plugin`:

* `exp-zmq-bind = ENDPOINT` (=tcp://127.0.0.1:5557): ZMQ PUSH socket binding.





# Sample output

At
https://cloudflare-ipfs.com/ipfs/QmZeR8hq9Vxh3VBy6YDYMJBzGSRR7kRReTiXRAgWRzdgRW
you can download about 76MB of gzipped JSON output. The raw output of
`exp_ws_plugin` is prettyfied and separated by double newline
characters.


## Souce code, license and copyright

Source code repository: https://github.com/EOSChronicleProject/eos-chronicle

Copyright 2018 cc32d9@gmail.com

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
