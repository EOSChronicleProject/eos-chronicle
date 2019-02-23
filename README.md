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
with an increasing timer, varying from 1 to 32 seconds.

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

TODO: full installation procedure on bare Ubuntu

cd build
cmake ..
make


`examples/exp-dummy-plugin` explains how to add and compile your own plugin to `chronicle-receiver`.



## Souce code, license and copyright

Source code repository: https://github.com/cc32d9/dappscatalog

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
