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

Apache Kafka is used as a mechanism for delivering history events within
the system.

ScyllaDB is used as persistent storage for actions history, balances,
and other related data, indexed and prepared for fast retrieval.

`state_history_plugin` developed by Todd Fleming is used as raw
information source from within `nodeos` process. It exports all state
changes and action traces for each accepted block and stores them in its
own indexed log file. It then provides a websocket interface for
retrieving the block and state change information. Probably later it
would be replaced or extended by a plugin which exports this data
directly into a Kafka topic.

`chronicle_sth_receiver` is a C++ program that retrieves the data from
`state_history_plugin` and stores it in Kafka topic "rawstate" (topic
names are only given for reference and are configurable). If several
`nodeos` processes send data to the same Kafka cluster, their
corresponding receivers should publish the data into different topics.


`chronicle_raw_decoder` is a C++ program that reads the "rawstate" topic
and decodes the raw binary data into JSON representation. The decoded
data is published into "jsstate" topic. Kafka's built-in LZ4 compression
will ensure that the resulting data is transferred and stored in
compressed form. In order to decode changes in contract table rows, the
decoder has to have access to ABI in every contract. ABI are transmitted
as raw messages in "rawstate" topic, and the decoder has to store them
persistently in some kind of local database. Probably some file-based
key-value database will be used, such as BerkeleyDB. The ABI has to be
stored in native form for fastest access. The decoder also detects forks
in the blockchain and signals them in "jsstate" topic.


`chronicle_db_writer` is a C++ program that processes "jsstate" topics
originated from multiple `nodeos` processes and stores the data in
ScyllaDB database.

A number of additional filter programs will be develop. They would read
the "jsstate" topic and react on specific events, such as actions
withiin a subset of contracts, and generate their output toward their
consumers.


`chronicle_api` is a rich HTTP-based API for querying the data from
ScyllaDB. There is no intent to keep any backward compatibility with
legacu APIs.

`chronicle_history_v1_api` is an adaptor that presents the data from the
database in the same format as `history_plugin` API does.









