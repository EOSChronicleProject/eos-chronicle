[![Actions Status: build](https://github.com/EOSChronicleProject/eos-chronicle/workflows/build/badge.svg)](https://github.com/EOSChronicleProject/eos-chronicle/actions/workflows/build.yml/badge.svg)

# Antelope Chronicle Project

Chronicle is a software component designed to process the history of
an [Antelope](https://github.com/AntelopeIO/leap) (formely EOSIO)
blockchain.


# chronicle-receiver

The receiver is designed to work with `state_history_plugin` of
`nodeos`. It connects to the websocket endpoint provided by
the state history plugin and starts reading its data from a specific
block number.

The receiver can be compiled with a number of exporter plugins, and only
one exporter plugin can be enabled in its configuration. Exporter
plugins organize the data export to their respective consumers.

At the moment only one exporter plugin is implemented in the core
Chronicle package: `exp_ws_plugin`. Also, a new project called
"[chronos](https://github.com/EOSChronicleProject/chronos)" implments
an exporter plugin that writes the blockchain updates directly in its
database.

Exporters must work in bidirectional mode: the exporter expects that
the consumer acknowledges block numbers that it has processed and
stored. Should `chronicle-receiver` stop, it will start from the block
number next after acknowledged or last known irreversible, whichever
is lower.

The communication between exporter and consumer is performed
asynchronously: the receiver starts with a parameter indicating the
maximum number of unacknowledged blocks (1000 by default), and it
continues retrieving data from `nodeos` as long as the consumer
confirms the blocks within this limit. Received and decoded data is
kept in a queue that is fed to the consumer, allowing the consumer to
process the data at its own pace. If the number of unacknowledged
blocks reaches the maximum, the reader pauses itself with an
increasing timer, varying from 0.05 to 0.5 seconds. If the pause
exceeds 500 milliseconds, an informational message is printed on
console output.

If `nodeos` stops or restarts, `chronicle-receiver` will automatically
stop and close its downstream connection. Also if the downstream
connection closes, the receiver will stop itself and close the
connection to `nodeos`. The package includes a systemd unit file which
would restart the receiver automatically in this case.



## Scanning mode

When `mode` option is set to `scan`, `chronicle-receiver` operates as
follows:

* it reads all available blocks sequentially from state history.

* it monitors account changes, and as soon as a new ABI is set on a
  contract, it stores a copy of the ABI in its state memory. The state
  memory keeps all revisions of every contract ABI.

* upon receiving transaction traces or table deltas, it tries to
  decode the raw data using the available contract ABI.

* all data received from `state_history_plugin` is converted to JSON
  format. If there's no valid ABI for decoding raw data, it's presented
  as a hex string. In this case, an ABI decoder error event is
  generated.

* it feeds all JSON data and all error events to the exporter plugin,
  and the exporter plugin pushes the JSON data to its consumer. As
  described above, the consumer must send acknowledgements for processed
  block numbers.


In `scan-noexport` mode, the receiver requests the blocks from state
history sequentially and stores all revisions of contract ABI in its
database. This allows the ABIs to be quickly available for the
interactive mode.


## Interactive mode

In `interactive` mode, `chronicle-receiver` uses the state database
populated by another receiver process that is running in scanning
mode. Only one process is allowed to run in scanning mode, and multiple
processes can be started in interactive mode.

The exporter plugin, or probably some other plugin, receives a request
for particular block number or a range of blocks. This request is passed
to the receiver and requested from `state_history_plugin`. If a range is
specified, blocks up to the last before the end block are exported.

During request processing, the decoder retrieves the required contract
ABI from its ABI history, so that it's the latest copy from a block
number that is below the requested block.

Then, the same way as in scanning mode, decoded data is translated into
JSON and passed to the exporter plugin.

The receiver does not expect any acknowledgements in interactive mode.

Only irreversible blocks are available for interactive mode.

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
JSON object with two keys: `msgtype` indicates the type of the
message, and `data` contains the corresponding JSON object, such as
transaction trace or table delta. This mode is deprecated and will be
removed from future releases, as the binary mode is about 15% faster.

In binary header mode (`exp-ws-bin-header=true`), each message
consists of a binary 64-bit header and JSON data: the header consists
of two 32-bit unsigned integers indicating message type and options,
and the rest of the message is JSON data. Message type values are
available in `chronicle_msgtypes.h` header file. The second integer,
options, is currently always set to zero.

In scanning mode, the exporter expects that the server sends back
block number acknowledgements as decimal numbers in text format, each
number in an individual binary message.

In interactive mode, the exporter expects that the server sends each
request as a single binary message. The content of each message is
either one block number in decimal text notation, or two decimal
integers separated by minus sign (-) indicating a range of blocks.


# Compiling

Minimum requirements: Cmake 3.11, Boost 1.67, GCC 8.3.0.

The pinned build script will produce a Debian package and a binary
archive.

```
mkdir -p /opt/src/
cd /opt/src/
git clone --recursive https://github.com/EOSChronicleProject/eos-chronicle.git
cd eos-chronicle
./pinned_build/install_deps.sh && mkdir build && \
 nice ./pinned_build/chronicle_pinned_build.sh /opt/src/chronicle-deps /opt/src/eos-chronicle/build $(nproc)
```



# State history plugin in `nodeos`

In order for Chronicle to function properly, both `trace-history` and
`chain-state-history` need to be enabled. Also if contract console
output needs to be present in Chronicle output,
`trace-history-debug-mode` needs to be enabled too. The state history
endpoint address and port needs to be reachable from the host where
Chronicle receiver is running.

Example `config.ini` for `nodeos`:

```
contracts-console = true
validation-mode = light
plugin = eosio::state_history_plugin
trace-history = true
chain-state-history = true
trace-history-debug-mode = true
state-history-endpoint = 0.0.0.0:8080
```


# Configuring and running

Similarly to `nodeos`, `chronicle-receiver` needs a configuration
directory with `config.ini` in it, and a data directory where it stores
its internal state.

See the [Chronicle
tutorial](https://github.com/EOSChronicleProject/chronicle-tutorial)
for a more detailed and complete example.

Here's a minimal configuration for the receiver using Websocket
exporter. It connects to `nodeos` process running `state_history_plugin`
at `localhost:8080` and exports the data to a websocket server at
`localhost:8800`. In a production environment, hosts may be different
machines in the network.

The receiver would stop immediately if the websocket server is not
responding. For further tests, you need a consumer server ready.

The Perl script `testing/chronicle-ws-dumper.pl` can be used as a test
websocket server that dumps the input to standard output.

```
mkdir -p /srv/memento_wax1/chronicle-config
cat >/srv/memento_wax1/chronicle-config/config.ini <<'EOT'
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
/usr/local/sbin/chronicle-receiver \
  --config-dir=/srv/memento_wax1/chronicle-config --data-dir=/srv/memento_wax1/chronicle-data

# install systemd unit file
cp /usr/local/share/chronicle_receiver\@.service /etc/systemd/system/
systemctl daemon-reload

# You may need to initialize the Chronicle database from the first block
# in the state history archive. See the Chronicle Tutorial for more
# details. You may point it to some other state history source during
# the initialization. Here we launch it in scan-noexport mode for faster initialization.
/usr/local/sbin/chronicle-receiver --config-dir=/srv/memento_wax1/chronicle-config \
 --data-dir=/srv/memento_wax1/chronicle-data \
 --host=my.ship.host.domain.com --port=8080 \
 --start-block=186332760 --mode=scan-noexport --end-block=186332800


# Once it stops at the end block, launch the service
systemctl enable chronicle_receiver@memento_wax1
systemctl start chronicle_receiver@memento_wax1

# watch the log
journalctl -u memento_dbwriter@wax1 -f
```

# Portable snapshots

As of Chronicle versions 2.7 and 3.2, two new command-line options
`--save-snapshot` and `--restore-snapshot` allow saving a Chronicle
state database to a snapshot file or restoring it from such a snapshot
file. Both options can only be used when the chronicle-receiver
process is stopped.

The snapshots are compatioble with versions 2.7 or higher and 3.2 or
higher.

Snapshots for some public networks are available for downloading:

https://snapshots.eosamsterdam.net/public/chronicle_snapshots/

An example of initializing Chronicle data from a snapshot:

```
cd /var/local
wget https://snapshots.eosamsterdam.net/public/chronicle_snapshots/chronicle_snapshot_wax_253338878.gz
gzip -d chronicle_snapshot_wax_253338878.gz

chronicle-receiver --config-dir=/srv/memento_wax1/chronicle-config --data-dir=/srv/memento_wax1/chronicle-data --restore-snapshot=chronicle_snapshot_wax_253338878

systemctl enable chronicle_receiver@memento_wax1
systemctl start chronicle_receiver@memento_wax1
```

Portable snapshots can be utilized for upgrading Chronicle from version 2.x to 3.x. 

```
# stop all runing Chronicle processes
systemctl stop -a  'chronicle_receiver@*'

# download and install the 2.7 package
cd /var/local
apt install ./eosio-chronicle-2.7-Clang-11.0.1-ubuntu20.04-x86_64.deb

# save the current chronicle state for all instances
chronicle-receiver --config-dir=/srv/memento_wax1/chronicle-config --data-dir=/srv/memento_wax1/chronicle-data --save-snapshot=wax.snapshot
chronicle-receiver --config-dir=/srv/memento_eos1/chronicle-config --data-dir=/srv/memento_eos1/chronicle-data --save-snapshot=eos.snapshot
chronicle-receiver --config-dir=/srv/memento_proton1/chronicle-config --data-dir=/srv/memento_proton1/chronicle-data --save-snapshot=proton.snapshot
chronicle-receiver --config-dir=/srv/memento_telos1/chronicle-config --data-dir=/srv/memento_telos1/chronicle-data --save-snapshot=telos.snapshot

# uninstall Chronicle 2.7, download and install Chronicle 3.2
apt remove eosio-chronicle
apt install ./antelope-chronicle-3.2-Clang-11.0.1-ubuntu20.04-x86_64.deb

# remove 2.x Chronicle data
rm -r /srv/*/chronicle-data

# restore the Chronicle data from snapshots
chronicle-receiver --config-dir=/srv/memento_eos1/chronicle-config --data-dir=/srv/memento_eos1/chronicle-data --restore-snapshot=eos.snapshot
chronicle-receiver --config-dir=/srv/memento_proton1/chronicle-config --data-dir=/srv/memento_proton1/chronicle-data --restore-snapshot=proton.snapshot
chronicle-receiver --config-dir=/srv/memento_telos1/chronicle-config --data-dir=/srv/memento_telos1/chronicle-data --restore-snapshot=telos.snapshot
chronicle-receiver --config-dir=/srv/memento_wax1/chronicle-config --data-dir=/srv/memento_wax1/chronicle-data --restore-snapshot=wax.snapshot

# start all Chronicle processes
systemctl start -a  'chronicle_receiver@*'

# check the consumer health
journalctl -u memento_dbwriter@wax1 -f
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

  * `interactive`: interactive mode allows the consumer request random
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

* `skip-account-info = true|false` (=`false`) Disable account
  permissions and metainformation in the export.

* `irreversible-only = true|false` (=`false`) fetch irreversible blocks
only

* `start-block = N` (=`0`) Initialize Chronicle state from given
  block. This is intended for starting Chronicle off a node that started
  from a portable snapshot. The snapshot has all table contents in
  the beginning, so Chronicle will process them all before continuing
  with the blocks. It may take some significant time. This option is
  only allowed when Chronicle data directory is empty.

* `end-block = N` (=`4294967295`)  Stop receiver before this block number

* `stale-deadline = N` (=`10000`) If there were no new blocks from
  state history socket within this time (in milliseconds),
  chronicle-receiver will stop and exit. The deadline timer is not
  used if the receiver is paused by a slow consumer.

* `enable-receiver-filter = true|false` (=`false`) if enabled,
  activates output filtering on traces matching the `include-receiver`
  filters.

* `include-receiver = NAME` If `enable-receivers-filter` is enabled,
  one or multiple `include-receiver` options specify the EOSIO account
  names that need to be matched in traces. The receiver looks for
  these names in receipt receivers of every action in trace, and
  outputs the trace only if at least one name matches. The smart
  contract executing an action is always receiving a receipt, so you
  can easily filter by contracts. Also in token transfers, normally
  payer and payee are receiving receipts.

* `enable-auth-filter = true|false` (=`false`) if enabled, activates
  output filtering on traces matching `include-auth` filters.

* `include-auth = NAME` If `enable-auth-filter` is enabled, one or
  multiple `include-auth` options specify the account names that are
  looked up in action authorizations. Only the traces matching at
  least one authorization will be included in the output.

* `blacklist-action = CONTRACT:ACTION` This option defines action
  names for specific contracts that are blocking the output of
  corresponding traces. Multiple (contract:action) tuples can be
  specified. By default, only `eosio:onblock` is blacklisted.

* `enable-tables-filter = true|false` (=`false`) if enabled, activates
  output filtering on table deltas for contracts matching
  `include-tables-contract` filters.

* `include-tables-contract = NAME` If `enable-tables-filter` is
  enabled, one or multiple `include-tables-contract` options specufy
  the contract names for which table deltas would be included in the
  output.

* `blacklist-tables-contract = NAME` This option allows excluding
  contract names from table deltas. Multiple options can be specified,
  and those contracts will be blacklisted from table deltas export.

If both `enable-receiver-filter` and `enable-auth-filter` are enabled,
the output will include traces matching any of the filters. The
blacklist has absolute precedence: regardless of filters
configuration, if a transaction matches the blacklist, it is dropped
from output.


Options for `exp_ws_plugin`:

* `exp-ws-host = HOST` (mandatory): Websocket server host to connect to;

* `exp-ws-port = PORT` (mandatory): Websocket server port to connect to;

* `exp-ws-path = PATH` (/): Websocket server URL path;

* `exp-ws-bin-header = true|false` (=`false`) Enable binary header mode
  (message type and options as binary integers, followed by JSON);

* `exp-ws-max-unack = N` (=1000): Receiver will pause at so many unacknowledged blocks;

* `exp-ws-max-queue = N` (=10000): Receiver will pause if outbound queue exceeds this limit.







# Release notes

## Release 1.0

This release is based on Block.one libraries of particular older
versions. It uses `abieos` library from November 13th, with an
additional patch. Newer versions of those libraries are introducing some
incompatible changes, and the work is in progress to adapt Chronicle to
those changes.

## Release 1.1

* Unidirectional mode is no longer supported.

* `skip-to` option is removed.

* `exp_zmq_plugin` is removed because of instable work with Boost ASIO.

* Newest libraries from Block.one repositories are used, and the most
  dramatic change is that channels are processed asynchronously. Also
  all asynchronous tasks must be wrapped in `appbase` priority queue.

* In addition to latest copy of ABI for each contract, the internal
  state database stores a history of all ABI revisions for all
  contracts. This is used in interactive mode.

* New configuration option: `mode` and 3 modes: `scan`, `scan-noexport`,
  and `interactive`. Interactive mode allows requesting individual
  blocks and block ranges.

* New options: `irreversible-only`, `end-block`.


## Release 1.2

This release supports nodeos versions 1.8 ans 2.0, and not compatible with
nodeos-1.7. 

* New message types: 1011, 1012, 1013 (PERMISSION, PERMISSION_LINK,
  ACC_METADATA).

* New field `block_id` in BLOCK and BLOCK_COMPLETED messages.

* New options: `stale-deadline`, `exp-ws-path`, `start-block`,
  `skip-traces`.

* Bugfixes and improvements.


## Release 1.3

* New options for filtering: `enable-receiver-filter`,
  `include-receiver`, `enable-auth-filter`, `include-auth`,
  `blacklist-action`

## Release 1.5

* changed default value for receiver-state-db-size from 1024 to 16384

* new option: blacklist-tables-contract

* Debian package builder and packages published on Github

## Release 1.6

* Replaced external dependencies from B1 repo to our own repo

* Debian package includes `/usr/local/share/chronicle_receiver@.service`

## Release 2.0

* Added compatibility with Leap 3.1

* Added pinned_build scripts, fixating on Boost 1.80.0 and Clang 11.0.1

* The state database is not compatible with 1.6 state, so Chronicle needs to be reinitialized.

## Release 2.1

* Bugfix: primary_key resolved as boolean in decoder_plugin.cpp

## Release 2.2

* Bugfix: ack for a block lower than the previously aknowledged was crashing chronicle

## Release 2.3

* Bugfix: if action arguments or a table row contained trailing garbage, Chronicle failed to decode it.

## Release 2.4

* Bugfix: consumer sending websocket close message causes a crash

## Release 2.5

* Updated external dependencies to match Leap 4.0

## Release 2.6

* Bugfix in integer to JSON conversion: primary key in table deltas lost lower 32 bits. 

## Release 2.7

* Added options: `--save-snapshot` and `--restore-snapshot`

## Release 3.0

* Chronicle 3.0 database is not compatible with that of 2.x, so a
  rescan or restart from a snapshot is required.

* CMake files optimized for customized builds, like the Chronos project.

* Enabled compiler optimization for faster performance.

* Bugfix in Chronicle database: it had an entry for every account even
  if ABI was empty.

* Improvements in socket error handling.

* New event type: `BLOCK_STARTED (1014)`.

* New attributes in `BLOCK_COMPLETED (1010)` event: `producer`,
  `previous`, `transaction_mroot`, `action_mroot`, `trx_count`.

* More frequent pause events; slower pause timer ramp-up.

* default value for receiver-state-db-size set to 1024

* The package is renamed from `eosio-chronicle` to `antelope-chronicle`.

## Release 3.1

* Bugfix in integer to JSON conversion: primary key in table deltas lost lower 32 bits. 

## Release 3.2

* Added options: `--save-snapshot` and `--restore-snapshot`

## Release 3.3

* abieos updated to PR#24, fixing a floating-point overflow bug


# Ecosystem links

* [Chronicle Telegram chat](https://t.me/+TMWWcV1gBxQiqIkm)

* [Chronicle
  tutorial](https://github.com/EOSChronicleProject/chronicle-tutorial)
  explains the nodeos and Chronicle server installation in detail.

* [chronicle-consumer-npm](https://github.com/EOSChronicleProject/chronicle-consumer-npm)
  is a Node.js module that consumer processes can be based on.

* [chronicle-consumer-npm
  examples](https://github.com/EOSChronicleProject/chronicle-consumer-npm-examples)
  is a number of examples using the Node.js module.

* [Awesome
Chronicle](https://github.com/EOSChronicleProject/awesome-chronicle)
is a list of software projects and services using the software.

* [Docker file provided by EOS
  Tribe](https://github.com/EOSTribe/eos-chronicle-docker)





# Source code, license and copyright

Source code repository: https://github.com/EOSChronicleProject/eos-chronicle

Copyright 2018-2023 cc32d9@gmail.com

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
