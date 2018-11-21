EOS Chronicle: Design and Architecture
======================================


Introduction
------------

EOS Chronicle project is aiming to build an open-source and open-desugn
platform for processing and storing the history of EOS transactions and
all their details. This data would be consumed by all kinds of block
explorers, wallets, and real-time event processors.


Components
----------

### Message queue

Apache Kafka is used as a mechanism for delivering history events within
the system.

### Persistent storage

ScyllaDB is used as persistent storage for actions history, balances,
and other related data, indexed and prepared for fast retrieval.


### state_history_plugin

`state_history_plugin` developed by Todd Fleming is used as raw
information source from within `nodeos` process. It exports all state
changes and action traces for each accepted block and stores them in its
own indexed log file. It then provides a websocket interface for
retrieving the block and state change information. Probably later it
would be replaced or extended by a plugin which exports this data
directly into a Kafka topic.


### chronicle_sth_decoder

`chronicle_sth_decoder` is a C++ program that retrieves the data from
`state_history_plugin` and decodes it into JSON data. The JSON events
are then pushed to a "jsstate" Kafka topic.

The decoder stores its state variables in a file-mapped shared memory
segment, as follows:

* head block number that was received from `state_history_plugin`. This
  is needed for detecting fork events on the blockchain.

* ABI of each smart contract in the blockchain. It is required in order
  to decode changes in table rows.

The decoder produces all its output in JSON and pushes it into "jsstate"
topic. Kafka will use its built-in LZ4 compression in order to transmit
and store the data in compressed form.


### chronicle_db_writer

`chronicle_db_writer` is a C++ program that processes "jsstate" topics
originated from multiple `nodeos` processes and stores the data in
ScyllaDB database.

It may also utilize a file-mapped shared memory segment for quicker
deduplication of multiple source streams.


### Additional event processors

A number of additional filter programs will be develop. They would read
the "jsstate" topic and react on specific events, such as actions
withiin a subset of contracts, and generate their output toward their
consumers.


### API for external consumers

`chronicle_api` is a rich HTTP-based API for querying the data from
ScyllaDB. There is no intent to keep any backward compatibility with
legacy APIs.

`chronicle_history_v1_api` is an adaptor that presents the data from the
database in the same format as `history_plugin` API does.

`chronicle_light_api` is a port of EOS Light API that retrieves current
token balances and authentication keys, but not the history of accounts.










