# EOS Chronicle Project

This project is an implementation of work proposal as published at
https://github.com/cc32d9/eos-work-proposals/tree/master/001_EOS_Chronicle

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

The receiver can be compiled with a number of exporter plugins, and only
one exporter plugin can be enabled in its configuration. Exporter
plugins organize the data export to their respective consumers.

Exporters must work in bidirectional mode: the exporter expects that the
consumer acknowledges block numbers that it has processed and
stored. Should `chronicle-receiver` stop, it will start from the block
number next after acknowledged or last known irreversible, whichever is
lower.

The communication between exporter and consumer is performed
asynchronously: the receiver starts with a parameter indicating the
maximum number of unacknowledged blocks (1000 by default), and it
continues retrieveing data from `nodeos` as long as the consumer
confirms the blocks within this maximum. Received and decoded data is
kept in a queue that is fed to the consumer, allowing it to process the
data at its own pace. If the number of unacknowledged blocks reaches the
maxumum, the reader pauses itself with an increasing timer, varying from
0.1 to 8 seconds. If the pause exceeds 1 second, an informational event
is generated.

If `nodeos` stops or restarts, `chronicle-receiver` will automatically
stop and close its downstream connection. Also if the downstream
connection closes, the receiver will stop itself and close the
connection to `nodeos`.

Chronicle receiver version 1.1 is compatible with `nodeos` versions
prior to 1.8.


## Scanning mode

When `mode` option is set to `scan`, `chronicle-receiver` operates as
follows:

* it reads all available blocks sequentially from state history.

* it monitors account changes, and as soon as a new ABI is set on a
  contract, it stores a copy of ABI in its state memory. The state
  memory keeps all revisions of every contract ABI.

* upon receiving transaction traces and table deltas, it tries using the
  ABI and decoding the raw binary data into the ABI-defined structures.

* all data received from `state_history_plugin` is converted to JSON
  format. If there's no valid ABI for decoding raw data, it's presented
  as a hex string. In this case, an ABI decoder error event is
  generated.

* it feeds all JSON data and all error events to the exporter plugin,
  and the exporter plugin pushes the JSON data to its consumer. As
  described above, the consumer must send acknowledgements for processed
  block numbers.


In `scan-noexport` mode, the receiver requestts the blocks from state
history sequentially and stores all revisions of contract ABI in its
memory. This allows it to be quickly available for interactive mode.


## Interactive mode

In `interactive` mode, `chronicle-receiver` uses the state database
populated by another receiver process that is running in scanning
mode. Only one process is allowed to run in scanning mode, and multiple
processes can be started in interactive mode.

The exporter plugin, or probably some other plugin, receives a request
for particular block number or a range of blocks. This request is passed
to the receiver and requested from `state_history_plugin`. If a range is
specified, blocks up to the last before the end block are exported.

During request processing, the decoder retrieves required contract ABI
from its ABI history, so that it's the latest copy from a block number
that is below the requested block.

Then, the same way as in scanning mode, decoded data is translated into
JSON and passed to the exporter plugin.

Receiver does not expect any acknowledgements in interactive mode.

Only irreversoble blocks are available for interactive mode.

Note that in case of `exp_ws_plugin`, you need to specify a different
TCP port of the websocket server, so that it does not interfere with the
websocket communication in scanning mode when export is enabled.



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




## Websocket exporter plugin

`exp_ws_plugin` exports the data to a websocket server.

The plugin connects to a specified websocket host and port and opens a
binary stream.

The plugin works in one of two possible modes:

In JSON mode (`exp-ws-bin-header=false`), each outgoing message is a
JSON object with two keys: `msgtype` indicates the type of the message,
and `data` contains the corresponding JSON object, such as transaction
trace or table delta.

In binary header mode (`exp-ws-bin-header=true`), the message format is
similar to that used in `zmq-plugin` for nodeos: each message starts
with two native 32-bit unsigned integers indicating message type and
options, and the rest of the message is JSON data. Message type values
are available in `chronicle_msgtypes.h` header file. The second integer,
options, is currently always zero.

The binary header mode increases the exporter performance by
approximately 15%.

In scanning mode, the exporter expects that the server sends back block
number acknowledgements in text format, each number in an individual
binary message.

In interactive mode, the exporter expects that the server sends each
request as a single binary message. The content of each message is
either one block number in decimal text notation, or two decimal
integers separated by minus sign (-) indicating a range of blocks.


# Compiling

Minimum requirements: Boost libraries version 1.67 or higher.

3GB RAM is required for sucessful compilation. Smaller RAM will cause
heavy swapping during the compilation.

Ubuntu 18.10 delivers Boost 1.67 in binary packages. Earlier versions of
Ubuntu will require manual compilation of Boost.



```
sudo apt update && \
sudo apt install -y git g++ cmake libboost-dev libboost-thread-dev libboost-test-dev \
 libboost-filesystem-dev libboost-date-time-dev libboost-system-dev libboost-iostreams-dev \
 libboost-program-options-dev libboost-locale-dev libssl-dev libgmp-dev

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

The Perl script `testing/chronicle-ws-dumper.pl` can be used as a test
websocket server that dumps the input to standard output.


```
mkdir /home/eosio/chronicle-config
cat >/home/eosio/chronicle-config/config.ini <<'EOT'
# connection to nodeos state_history_plugin
host = 127.0.0.1
port = 8080
mode = scan
plugin = exp_ws_plugin
exp-ws-host = 127.0.0.1
exp-ws-port = 8800
exp-ws-bin-header = true
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


The following options are available from command line and `config.ini`:

* `host = HOST` (=`localhost`): Host to connect to (nodeos with
  state_history_plugin);

* `port = PORT` (=`8080`): Port to connect to (nodeos with state-history
  plugin);

* `receiver-state-db-size = N` (=`1024`): State database size in MB;

* `mode = MODE`: mandatory receiver mode. Possible values:

  * `scan`: read state history blocks sequentially and export via export
    plugin.

  * `scan-noexport`: read state history blocks sequentially and skip any
    export. This is the fastest mode to collect ABI revisions so that
    interactive access can fetch required blocks.

  * `interactive`: interactove mode allows the consumer request random
    blocks. Irreversible-only mode is automatically set in this mode.

* `report-every = N` (=`10000`) Print informational messages every so
  many blocks;

* `max-queue-size = N` (=`10000`) If the asynchronous processing queue
  reaches this limit, the receiver will pause.

* `skip-block-events = true|false` (=`false`) Disable BLOCK events in
  export. This saves CPU time if you don't need block attributes, such
  as BP signatures and block ID.

* `skip-table-deltas = true|false` (=`false`) Disable table delta events
  in the export.

* `skip-traces = true|false` (=`false`) Disable transaction trace events
  in the export.

* `irreversible-only = true|false` (=`false`) fetch irreversible blocks
only

* `end-block = N` (=`4294967295`)  Stop receiver before this block number


Options for `exp_ws_plugin`:

* `exp-ws-host = HOST` (mandatory): Websocket server host to connect to;

* `exp-ws-port = PORT` (mandatory): Websocket server port to connect to;

* `exp-ws-bin-header = true|false` (=`false`) Enable binary header mode
  (message type and options as binary integers, followed by JSON);

* `exp-ws-max-unack = N` (=1000): Receiver will pause at so many unacknowledged blocks;

* `exp-ws-max-queue = N` (=10000): Receiver will pause if outbound queue exceeds this limit.






# Sample output

At
https://cloudflare-ipfs.com/ipfs/Qmb2JKi5PrYFBinXW2wbYS4A75YdhDRWEy1DLnPuxq8Hyj
you can download about 31MB of gzipped JSON output. The raw output of
`exp_ws_plugin` is prettyfied and separated by double newline
characters.


# Release notes

## Release 1.0

This release is based on Block.one libraries of particular older
versions. It uses `abieos` library from Novemner 13th, with an
additional patch. Newer versions of those libraries are introducing some
incompatible changes, and the work is in progress to adapt Chronicle to
those changes.

## Release 1.1

* Unidirectional mode is no longer supported.

* `skip-to` option is removed.

* `exp_zmq_plugin` is removed because of instable work with Boost ASIO.

* Newest libraries from Block One repositories are used, and the most
  dramatic change is that channels are processed asynchronously. Also
  all asynchronous tasks must be wrapped in `appbase` priority queue.

* In addition to latest copy of ABI for each contract, the internal
  state database stores a history of all ABI revisions for all
  contracts. This is used in interactive mode.

* New configuration option: `mode` and 3 modes: `scan`, `scan-noexport`,
  and `interactive`. Interactive mode allows requesting individual
  blocks and block ranges.

* New options: `irreversible-only`, `end-block`.


# Thirt-party software

* Docker file provided by EOS Tribe:
  https://github.com/EOSTribe/eos-chronicle-docker




# Souce code, license and copyright

Source code repository: https://github.com/EOSChronicleProject/eos-chronicle

Copyright 2018-2019 cc32d9@gmail.com

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
