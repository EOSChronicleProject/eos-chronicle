// copyright defined in LICENSE.txt

#include "receiver_plugin.hpp"
#include <chainbase/chainbase.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/composite_key.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/program_options.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/filesystem.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/algorithm/string.hpp>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <memory>
#include <string_view>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>
#include <queue>
#include <limits>

using namespace abieos;
using namespace appbase;
using namespace std::literals;

using std::enable_shared_from_this;
using std::exception;
using std::make_shared;
using std::make_unique;
using std::map;
using std::set;
using std::max;
using std::min;
using std::optional;
using std::runtime_error;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::to_string;
using std::variant;
using std::vector;

namespace asio      = boost::asio;
namespace bio       = boost::iostreams;
namespace bpo       = boost::program_options;
namespace websocket = boost::beast::websocket;
namespace bip       = boost::interprocess;
namespace bfs       = boost::filesystem;

using asio::ip::tcp;
using boost::beast::flat_buffer;
using boost::system::error_code;

const string max_uint32_str = to_string(std::numeric_limits<uint32_t>::max());


namespace {
  const char* RCV_HOST_OPT = "host";
  const char* RCV_PORT_OPT = "port";
  const char* RCV_DBSIZE_OPT = "receiver-state-db-size";
  const char* RCV_MODE_OPT = "mode";
  const char* RCV_EVERY_OPT = "report-every";
  const char* RCV_MAX_QUEUE_OPT = "max-queue-size";
  const char* RCV_SKIP_BLOCK_EVT_OPT = "skip-block-events";
  const char* RCV_SKIP_DELTAS_OPT = "skip-table-deltas";
  const char* RCV_SKIP_TRACES_OPT = "skip-traces";
  const char* RCV_SKIP_ACCOUNT_INFO_OPT = "skip-account-info";
  const char* RCV_IRREV_ONLY_OPT = "irreversible-only";
  const char* RCV_START_BLOCK_OPT = "start-block";
  const char* RCV_END_BLOCK_OPT = "end-block";
  const char* RCV_STALE_DEADLINE_OPT = "stale-deadline";
  const char* RCV_ENABLE_RCVR_FILTER_OPT = "enable-receiver-filter";
  const char* RCV_INCLUDE_RECEIVER_OPT = "include-receiver";
  const char* RCV_ENABLE_AUTH_FILTER_OPT = "enable-auth-filter";
  const char* RCV_INCLUDE_AUTH_OPT = "include-auth";
  const char* RCV_BLACKLIST_ACTION_OPT = "blacklist-action";
  const char* RCV_ENABLE_TABLES_FILTER_OPT = "enable-tables-filter";
  const char* RCV_INCLUDE_TABLES_CONTRACT_OPT = "include-tables-contract";
  const char* RCV_BLACKLIST_TABLES_CONTRACT_OPT = "blacklist-tables-contract";

  const char* RCV_MODE_SCAN = "scan";
  const char* RCV_MODE_SCAN_NOEXP = "scan-noexport";
  const char* RCV_MODE_INTERACTIVE = "interactive";
}


// decoder state database objects

namespace chronicle {
  using namespace chainbase;
  using namespace boost::multi_index;

  enum dbtables {
    state_table,
    received_blocks_table,
    contract_abi_objects_table,
    contract_abi_history_table
  };

  struct by_id;
  struct by_blocknum;
  struct by_name;
  struct by_name_and_block;
  struct by_name_and_block_rev;

  // this is a singleton keeping the state of the receiver

  struct state_object : public chainbase::object<state_table, state_object>  {
    CHAINBASE_DEFAULT_CONSTRUCTOR(state_object);
    id_type     id;
    uint32_t    head;
    checksum256 head_id;
    uint32_t    irreversible;
    checksum256 irreversible_id;
  };

  using state_index = chainbase::shared_multi_index_container<
    state_object,
    indexed_by<
      ordered_unique<tag<by_id>, member<state_object, state_object::id_type, &state_object::id>>>>;

  // list of received blocks and their IDs, truncated from head as new blocks are received

  struct received_block_object : public chainbase::object<received_blocks_table, received_block_object>  {
    CHAINBASE_DEFAULT_CONSTRUCTOR(received_block_object);
    id_type      id;
    uint32_t     block_index;
    checksum256  block_id;
  };

  using received_block_index = chainbase::shared_multi_index_container<
    received_block_object,
    indexed_by<
      ordered_unique<tag<by_id>, member<received_block_object,
                                        received_block_object::id_type, &received_block_object::id>>,
      ordered_unique<tag<by_blocknum>, member<received_block_object, uint32_t, &received_block_object::block_index>>>>;

  // latest version of serialized binary ABI for every contract

  struct contract_abi_object : public chainbase::object<contract_abi_objects_table, contract_abi_object> {
    template<typename Constructor, typename Allocator>
    contract_abi_object( Constructor&& c, Allocator&& a ) : abi(a) { c(*this); }
    id_type                   id;
    uint64_t                  account;
    chainbase::shared_string  abi;

    void set_abi(const char* data, size_t size) {
      abi.resize(size, {});
      abi.assign(data, size);
    }
  };

  using contract_abi_index = chainbase::shared_multi_index_container<
    contract_abi_object,
    indexed_by<
      ordered_unique<tag<by_id>, member<contract_abi_object,
                                        contract_abi_object::id_type, &contract_abi_object::id>>,
      ordered_unique<tag<by_name>, member<contract_abi_object, uint64_t, &contract_abi_object::account>>>>;


  // History of ABI for every contract

  struct contract_abi_history : public chainbase::object<contract_abi_history_table, contract_abi_history> {
    template<typename Constructor, typename Allocator>
    contract_abi_history( Constructor&& c, Allocator&& a ) : abi(a) { c(*this); }
    id_type                   id;
    uint64_t                  account;
    uint32_t                  block_index;
    chainbase::shared_string  abi;

    void set_abi(const char* data, size_t size) {
      abi.resize(size, {});
      abi.assign(data, size);
    }
  };

  using contract_abi_hist_index = chainbase::shared_multi_index_container<
    contract_abi_history,
    indexed_by<
      ordered_unique<tag<by_id>,
                     member<contract_abi_history, contract_abi_history::id_type, &contract_abi_history::id>
                     >,
      ordered_unique<tag<by_name_and_block>,
                     composite_key<
                       contract_abi_history,
                       member<contract_abi_history, uint64_t, &contract_abi_history::account>,
                       member<contract_abi_history, uint32_t, &contract_abi_history::block_index>
                       >
                     >,
      ordered_unique<tag<by_name_and_block_rev>,
                     composite_key<
                       contract_abi_history,
                       member<contract_abi_history, uint64_t, &contract_abi_history::account>,
                       member<contract_abi_history, uint32_t, &contract_abi_history::block_index>
                       >,
                     composite_key_compare<std::less<uint64_t>,std::greater<uint32_t>>
                     >
      >
    >;

  // shared-memory mutex for accessing chainbase
  struct shmem_lock {
    bip::interprocess_mutex mutex;
  };
}

CHAINBASE_SET_INDEX_TYPE(chronicle::state_object, chronicle::state_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::received_block_object, chronicle::received_block_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::contract_abi_object, chronicle::contract_abi_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::contract_abi_history, chronicle::contract_abi_hist_index)





class receiver_plugin_impl : std::enable_shared_from_this<receiver_plugin_impl> {
public:
  receiver_plugin_impl() :
    _forks_chan(app().get_channel<chronicle::channels::forks>()),
    _block_started_chan(app().get_channel<chronicle::channels::block_started>()),
    _blocks_chan(app().get_channel<chronicle::channels::blocks>()),
    _block_table_deltas_chan(app().get_channel<chronicle::channels::block_table_deltas>()),
    _transaction_traces_chan(app().get_channel<chronicle::channels::transaction_traces>()),
    _abi_updates_chan(app().get_channel<chronicle::channels::abi_updates>()),
    _abi_removals_chan(app().get_channel<chronicle::channels::abi_removals>()),
    _abi_errors_chan(app().get_channel<chronicle::channels::abi_errors>()),
    _table_row_updates_chan(app().get_channel<chronicle::channels::table_row_updates>()),
    _permission_updates_chan(app().get_channel<chronicle::channels::permission_updates>()),
    _permission_link_updates_chan(app().get_channel<chronicle::channels::permission_link_updates>()),
    _account_metadata_updates_chan(app().get_channel<chronicle::channels::account_metadata_updates>()),
    _receiver_pauses_chan(app().get_channel<chronicle::channels::receiver_pauses>()),
    _block_completed_chan(app().get_channel<chronicle::channels::block_completed>()),
    pause_timer(app().get_io_service()),
    stale_check_timer(app().get_io_service())
  {};

  shared_ptr<chainbase::database>       db;
  bip::mapped_region                    _dblock_mapped_region;
  chronicle::shmem_lock*                dblock;

  shared_ptr<tcp::resolver>             resolver;
  shared_ptr<websocket::stream<tcp::socket>> stream;
  const int                             stream_priority = 40;
  const int                             stream_order = 1000;

  string                                host;
  string                                port;
  uint32_t                              report_every = 0;
  uint32_t                              max_queue_size = 0;
  bool                                  aborting = false;
  bool                                  receiver_ready = false;

  bool                                  interactive_mode;
  chronicle::channels::interactive_requests::channel_type::handle          _interactive_requests_subscription;
  std::queue<std::shared_ptr<chronicle::channels::interactive_request>>    interactive_req_queue;
  bool                                  interactive_req_pending = false;

  bool                                  noexport_mode;
  bool                                  skip_block_events;
  bool                                  skip_table_deltas;
  bool                                  skip_traces;
  bool                                  skip_account_info;
  bool                                  irreversible_only;
  uint32_t                              start_block_num;
  uint32_t                              end_block_num;


  uint32_t                              head            = 0;
  checksum256                           head_id         = {};
  uint32_t                              irreversible    = 0;
  checksum256                           irreversible_id = {};
  uint32_t                              first_bulk      = 0;
  eosio::ship_protocol::block_header    block_header;
  uint32_t                              trx_count;
  uint32_t                              received_blocks = 0;

  // needed for decoding state history input
  map<string, abi_type>                 abi_types;

  // The context keeps decoded versions of contract ABI
  abieos_context*                       contract_abi_ctxt = nullptr;
  set<uint64_t>                         contract_abi_imported;

  std::map<uint64_t,std::set<uint64_t>> blacklist_actions;

  bool                                  enable_rcvr_filter;
  set<uint64_t>                         rcvr_filter;

  bool                                  enable_auth_filter;
  set<uint64_t>                         auth_filter;

  bool                                  enable_tables_filter = false;
  set<uint64_t>                         tables_filter;
  bool                                  enable_tables_blacklist = false;
  set<uint64_t>                         tables_blacklist;

  bool                                  do_trace_filter = false; // true if any of trace filters is enabled
  bool                                  do_trace_blacklist = false;

  chronicle::channels::forks::channel_type&               _forks_chan;
  chronicle::channels::block_started::channel_type&       _block_started_chan;
  chronicle::channels::blocks::channel_type&              _blocks_chan;
  chronicle::channels::block_table_deltas::channel_type&  _block_table_deltas_chan;
  chronicle::channels::transaction_traces::channel_type&  _transaction_traces_chan;
  chronicle::channels::abi_updates::channel_type&         _abi_updates_chan;
  chronicle::channels::abi_removals::channel_type&        _abi_removals_chan;
  chronicle::channels::abi_errors::channel_type&          _abi_errors_chan;
  chronicle::channels::table_row_updates::channel_type&   _table_row_updates_chan;
  chronicle::channels::permission_updates::channel_type&  _permission_updates_chan;
  chronicle::channels::permission_link_updates::channel_type&  _permission_link_updates_chan;
  chronicle::channels::account_metadata_updates::channel_type&  _account_metadata_updates_chan;
  chronicle::channels::receiver_pauses::channel_type&     _receiver_pauses_chan;
  chronicle::channels::block_completed::channel_type&     _block_completed_chan;

  const int channel_priority = 50;

  bool                                  exporter_will_ack = false;
  uint32_t                              exporter_acked_block = 0;
  uint32_t                              exporter_max_unconfirmed;
  uint32_t                              forked_at_block = 0;
  boost::asio::deadline_timer           pause_timer;
  uint32_t                              pause_time_msec = 0;
  bool                                  slowdown_requested = false;

  boost::asio::deadline_timer           stale_check_timer;
  uint32_t                              stale_check_last_head;
  uint32_t                              stale_check_deadline_msec;


  void init() {
    if (interactive_mode) {
      _interactive_requests_subscription =
        app().get_channel<chronicle::channels::interactive_requests>().subscribe
        ([this](std::shared_ptr<chronicle::channels::interactive_request> req){ on_block_req(req); });
    }
  }


  void start() {
    resolver = std::make_shared<tcp::resolver>(app().get_io_service());
    stream = std::make_shared<websocket::stream<tcp::socket>>(app().get_io_service());
    stream->binary(true);
    stream->read_message_max(0x1ull<<36);

    if (!interactive_mode)
      load_state();
    resolver->async_resolve
      (host, port,
       [this](const error_code ec, tcp::resolver::results_type results) {
        if (ec)
          elog("Error during lookup of ${h}:${p} - ${e}", ("h",host)("p",port)("e", ec.message()));

        callback(ec, "resolve", [&] {
            asio::async_connect
              (
               stream->next_layer(),
               results.begin(),
               results.end(),
               [this](const error_code ec, auto&) {
                 callback(ec, "connect", [&] {
                     stream->async_handshake(host, "/", [this](const error_code ec) {
                         callback(ec, "handshake", [&] {
                             start_read();
                           });
                       });
                   });
               });
          });
      });
  }


  void load_state() {
    bool did_undo = false;
    uint32_t depth;
    {
      bip::scoped_lock<bip::interprocess_mutex> lock(dblock->mutex);
      auto &index = db->get_index<chronicle::state_index>();
      if( index.has_undo_session() ) {
        auto range = index.undo_stack_revision_range();
        depth = range.second - range.first;
        ilog("Database has ${d} uncommitted revisions. Reverting back", ("d",depth));
        while (index.has_undo_session() > 0)
          db->undo();
        did_undo = true;
      }
    }

    const auto& idx = db->get_index<chronicle::state_index, chronicle::by_id>();
    auto itr = idx.begin();
    if( itr != idx.end() ) {
      head = itr->head;
      head_id = itr->head_id;
      irreversible = itr->irreversible;
      irreversible_id = itr->irreversible_id;

      if( start_block_num > 0 ) {
        string errmsg = string("start-block can only be specified for initial startup. ") +
          "This Chronicle instance has its state database already";
        elog(errmsg);
        throw runtime_error(errmsg);
      }
    }
    else {
      if( start_block_num > 0 ) {
        head = start_block_num - 1;
        ilog("Re-scanning the state history from block ${b}", ("b",start_block_num));
      } else {
        ilog("Re-scanning the state history from genesis");
      }
      ilog("Issuing an explicit fork event");
      auto fe = std::make_shared<chronicle::channels::fork_event>();
      fe->block_num = head+1;
      forked_at_block = fe->block_num;
      fe->depth = 0;
      fe->fork_reason = chronicle::channels::fork_reason_val::resync;
      fe->last_irreversible = 0;
      _forks_chan.publish(channel_priority, fe);
    }

    if( did_undo ) {
      ilog("Reverted to block=${b}, issuing an explicit fork event", ("b",head));
      auto fe = std::make_shared<chronicle::channels::fork_event>();
      fe->block_num = head + 1;
      forked_at_block = fe->block_num;
      fe->depth = depth;
      fe->fork_reason = chronicle::channels::fork_reason_val::restart;
      fe->last_irreversible = irreversible;
      _forks_chan.publish(channel_priority, fe);
    }

    if( exporter_will_ack )
      exporter_acked_block = head;

    if( head >= end_block_num ) {
      elog("Head (${h}) is already at or past end block number (${e})", ("h",head)("e",end_block_num));
      throw runtime_error("Head is already at or past end block number");
    }

    init_contract_abi_ctxt();
  }



  void save_state() {
    const auto& idx = db->get_index<chronicle::state_index, chronicle::by_id>();
    auto itr = idx.begin();
    if( itr != idx.end() ) {
      db->modify( *itr, [&]( chronicle::state_object& o ) {
          o.head = head;
          o.head_id = head_id;
          o.irreversible = irreversible;
          o.irreversible_id = irreversible_id;
        });
    }
    else {
      db->create<chronicle::state_object>( [&]( chronicle::state_object& o ) {
          o.head = head;
          o.head_id = head_id;
          o.irreversible = irreversible;
          o.irreversible_id = irreversible_id;
        });
    }
  }


  void start_read() {
    auto in_buffer = make_shared<flat_buffer>();
    stream->async_read
      (*in_buffer,
       app().executor().get_priority_queue().wrap(stream_priority, stream_order, [this, in_buffer](const error_code ec, size_t) {
           callback(ec, "async_read", [&] {
               receive_abi(in_buffer);
               receiver_ready = true;
               if (interactive_mode) {
                 process_interactive_reqs();
               } else {
                 request_blocks();
               }
               continue_read();
             });
         }));

    stale_check_last_head = head;
    stale_check_timer.expires_from_now(boost::posix_time::milliseconds(stale_check_deadline_msec));
    stale_check_timer.async_wait
      (app().executor().get_priority_queue().wrap
       (stream_priority, stream_order,
        [this](const error_code ec) {
          callback(ec, "async_wait", [&] {
                                       check_stale_head();
                                     });
        }));
  }


  void continue_read() {
    if (check_pause()) {
      pause_time_msec = 0;
      auto in_buffer = make_shared<flat_buffer>();
      stream->async_read
        (*in_buffer,
         app().executor().get_priority_queue().wrap(stream_priority, stream_order, [this, in_buffer](const error_code ec, size_t) {
             callback(ec, "async_read", [&] {
                 if (!receive_result(in_buffer))
                   return;
                 continue_read();
               });
           }));
    }
  }


  // if consumer fails to acknowledge on time, or processing queues get too big, we pacify the receiver
  bool check_pause() {
    if (slowdown_requested ||
        (exporter_will_ack && (forked_at_block > 0 ||
                               head - exporter_acked_block >= exporter_max_unconfirmed)) ||
        app().executor().get_priority_queue().size() > max_queue_size ||
        (!interactive_mode && head == end_block_num -1)) {

      if ( head == end_block_num -1 && irreversible >= head &&
           (!exporter_will_ack || exporter_acked_block == head) ) {
        ilog("Reached the end block ${b}. Stopping the receiver.", ("b", head));
        commit_db();
        abort_receiver();
        return false;
      }

      if( pause_time_msec == 0 ) {
        pause_time_msec = 50;
      }
      else if( pause_time_msec < 500 ) {
        pause_time_msec += 50;
      }

      auto rp = std::make_shared<chronicle::channels::receiver_pause>();
      rp->head = head;
      rp->acknowledged = exporter_acked_block;
      _receiver_pauses_chan.publish(channel_priority, rp);
      if( pause_time_msec >= 500 ) {
        ilog("Pausing the reader");
      }

      pause_timer.expires_from_now(boost::posix_time::milliseconds(pause_time_msec));
      pause_timer.async_wait
        (app().executor().get_priority_queue().wrap(stream_priority, stream_order, [this](const error_code ec) {
            callback(ec, "async_wait", [&] {
                continue_read();
              });
          }));
      return false;
    }
    return true;
  }

  void check_stale_head() {
    if( stale_check_last_head == head && pause_time_msec == 0 && received_blocks > 0 ) {
      elog("Did not receive anything in ${d} milliseconds. Aborting the receiver", ("d", stale_check_deadline_msec));
      abort_receiver();
    }
    else {
      stale_check_last_head = head;
      stale_check_timer.expires_from_now(boost::posix_time::milliseconds(stale_check_deadline_msec));
      stale_check_timer.async_wait
        (app().executor().get_priority_queue().wrap
         (stream_priority, stream_order,
          [this](const error_code ec) {
            callback(ec, "async_wait", [&] {
                                         check_stale_head();
                                       });
          }));
    }
  }


  void receive_abi(const shared_ptr<flat_buffer> p) {
    std::string json_copy((const char*)p->data().data(), p->data().size());
    std::string error;
    eosio::json_token_stream stream(json_copy.data());
    eosio::abi_def abidef = eosio::from_json<eosio::abi_def>(stream);
    if( !check_abi_version(abidef.version, error) )
      throw runtime_error("abi version error: " + error);
    abieos::abi abi;
    convert(abidef, abi);
    abi_types = std::move(abi.abi_types);
  }


  void request_blocks() {
    jarray positions;
    {
      bip::scoped_lock<bip::interprocess_mutex> lock(dblock->mutex);
      const auto& idx = db->get_index<chronicle::received_block_index, chronicle::by_blocknum>();
      auto itr = idx.lower_bound(irreversible);
      while( itr != idx.end() && itr->block_index <= head ) {
        auto block_id_array = itr->block_id.extract_as_byte_array();
        positions.push_back(jvalue{jobject{
                                           {{"block_num"s}, {std::to_string(itr->block_index)}},
                                           {{"block_id"s}, {abieos::hex(block_id_array.begin(), block_id_array.end())}},
                                           }});
        itr++;
      }
    }

    uint32_t start_block = head + 1;
    ilog("Start block: ${b}", ("b",start_block));

    bool fetch_block = noexport_mode ? false:true;
    bool fetch_traces = (skip_traces || noexport_mode) ? false:true;
    bool fetch_deltas = true;
    send_request(jvalue{jarray{{"get_blocks_request_v0"s},
            {jobject{
                {{"start_block_num"s}, {to_string(start_block)}},
                  {{"end_block_num"s}, {to_string(end_block_num)}},
                    {{"max_messages_in_flight"s}, {max_uint32_str}},
                      {{"have_positions"s}, {positions}},
                        {{"irreversible_only"s}, {irreversible_only}},
                          {{"fetch_block"s}, {fetch_block}},
                            {{"fetch_traces"s}, {fetch_traces}},
                              {{"fetch_deltas"s}, {fetch_deltas}},
                                }}}},
      [&]{});
  }


  void on_block_req(std::shared_ptr<chronicle::channels::interactive_request> req) {
    {
      bip::scoped_lock<bip::interprocess_mutex> lock(dblock->mutex);
      const auto& idx = db->get_index<chronicle::state_index, chronicle::by_id>();
      auto itr = idx.begin();
      if( itr == idx.end() ) {
        elog("Receiver did not process any blocks yet");
        return;
      }
      if( req->block_num_start > itr->head ) {
        elog("Requested start block ${b} is higher than current head ${h}",
             ("b",req->block_num_start)("h", itr->head));
        return;
      }
      if( req->block_num_end > itr->head ) {
        elog("Requested end block ${b} is higher than current head ${h}",
             ("b",req->block_num_end)("h", itr->head));
        return;
      }
    }
    interactive_req_queue.push(req);
    process_interactive_reqs();
  }


  void process_interactive_reqs() {
    if (receiver_ready && !interactive_req_pending && interactive_req_queue.size() > 0 ) {
      auto req = interactive_req_queue.front();
      interactive_req_queue.pop();
      string block_req_str = to_string(req->block_num_start);
      string end_block_str = to_string(req->block_num_end);
      end_block_num = req->block_num_end;
      init_contract_abi_ctxt();
      dlog("Requesting blocks ${s} to ${e}", ("s", block_req_str)("e",end_block_str));
      bool fetch_block = true;
      bool fetch_traces = skip_traces ? false:true;
      bool fetch_deltas = skip_table_deltas ? false:true;
      interactive_req_pending = true;
      send_request(jvalue{jarray{{"get_blocks_request_v0"s},
              {jobject{
                  {{"start_block_num"s}, {block_req_str}},
                    {{"end_block_num"s}, {end_block_str}},
                      {{"max_messages_in_flight"s}, {max_uint32_str}},
                        {{"have_positions"s}, {jarray()}},
                          {{"irreversible_only"s}, {false}},
                            {{"fetch_block"s}, {fetch_block}},
                              {{"fetch_traces"s}, {fetch_traces}},
                                {{"fetch_deltas"s}, {fetch_deltas}},
                                  }}}},
        [&]() {} );
    }
  }


  bool receive_result(const shared_ptr<flat_buffer> p) {
    auto         data = p->data();
    eosio::input_stream bin{(const char*)data.data(), (const char*)data.data() + data.size()};
    check_variant(bin, get_type("result"), "get_blocks_result_v0");

    string error;
    eosio::ship_protocol::get_blocks_result_v0 result;
    from_bin(result, bin);
    if (!result.this_block)
      return true;

    received_blocks++;

    uint32_t    last_irreversible_num = result.last_irreversible.block_num;
    if( last_irreversible_num < irreversible ) {
      elog("Irreversible block in state history (${h}) is lower than the one last seen (${i})",
           ("h", last_irreversible_num)("i", irreversible));
      throw runtime_error("Irreversible block in state history is lower than the one last seen");
    }

    uint32_t    block_num = result.this_block->block_num;
    checksum256 block_id = result.this_block->block_id;

    if (interactive_mode) {
      if( block_num == end_block_num-1 ) {
        interactive_req_pending = false;
        process_interactive_reqs();
      }
    }
    else {
      bip::scoped_lock<bip::interprocess_mutex> lock(dblock->mutex);
      if( db->revision() < block_num-1 ) {
        uint32_t newrev = block_num-1;
        dlog("Current DB revision: ${r}. Setting to ${n}", ("r",db->revision())("n",newrev));
        dlog("Acknowledged: ${a}", ("a",exporter_acked_block));
        db->set_revision(newrev);
      }

      if( block_num > last_irreversible_num ) {
        if (block_num < irreversible) {
          elog("Received block number (${b}) that is lower than last seen irreversible (${i})",
               ("b", block_num)("i", irreversible));
          throw runtime_error("Received block number that is lower than last seen irreversible");
        }

        // we're at the blockchain head
        if (block_num <= head) { //received a block that is lower than what we already saw
          uint32_t depth = head - block_num;
          ilog("fork detected at block ${b}; head=${h}, depth=${d}", ("b",block_num)("h",head)("d",depth));
          init_contract_abi_ctxt();
          while( db->revision() >= block_num ) {
            db->undo();
          }
          dlog("rolled back DB revision to ${r}", ("r",db->revision()));
          if( db->revision() <= 0 ) {
            throw runtime_error(std::string("Cannot rollback, no undo stack at revision ")+
                                std::to_string(db->revision()));
          }

          if( exporter_will_ack && exporter_acked_block > block_num - 1 )
            exporter_acked_block = block_num - 1;

          auto fe = std::make_shared<chronicle::channels::fork_event>();
          fe->block_num = block_num;
          forked_at_block = fe->block_num;
          fe->depth = depth;
          fe->fork_reason = chronicle::channels::fork_reason_val::network;
          fe->last_irreversible = last_irreversible_num;
          _forks_chan.publish(channel_priority, fe);
        }
        else
          if (head > 0 && head > start_block_num &&
              (!result.prev_block || result.prev_block->block_id.value != head_id.value))
            throw runtime_error("prev_block does not match");
      }
    }

    head            = block_num;
    head_id         = block_id;
    irreversible    = last_irreversible_num;
    irreversible_id = result.last_irreversible.block_id;

    if (result.block)
      receive_block(*result.block, p);

    // state changing activities
    if (!interactive_mode) {
        bip::scoped_lock<bip::interprocess_mutex> lock(dblock->mutex);
        auto undo_session = db->start_undo_session(true);

        if (block_num > irreversible) {
          // add the new block
          const auto& idx = db->get_index<chronicle::received_block_index, chronicle::by_blocknum>();
          db->create<chronicle::received_block_object>( [&]( chronicle::received_block_object& o ) {
              o.block_index = block_num;
              o.block_id = block_id;
            });
          // truncate old blocks up to previously known irreversible
          auto itr = idx.begin();
          while( itr->block_index < irreversible && itr != idx.end() ) {
            db->remove(*itr);
            itr = idx.begin();
          }
        }

        if (result.deltas)
          receive_deltas(*result.deltas, p);

        save_state();
        undo_session.push();     // save a new revision
        commit_db();
    }
    else {
      if (result.deltas)
        receive_deltas(*result.deltas, p);
    }

    if (result.traces)
      receive_traces(*result.traces, p);

    auto bf = std::make_shared<chronicle::channels::block_finished>();
    bf->block_num = head;
    bf->block_id = head_id;
    bf->last_irreversible = irreversible;
    bf->block_timestamp = block_header.timestamp;
    bf->producer = block_header.producer;
    bf->previous = block_header.previous;
    bf->transaction_mroot = block_header.transaction_mroot;
    bf->action_mroot = block_header.action_mroot;
    bf->trx_count = trx_count;
    _block_completed_chan.publish(channel_priority, bf);

    if( aborting )
      return false;

    if (interactive_mode) {
      if (report_every > 0 && head % report_every == 0) {
        ilog("block=${h}; irreversible=${i}", ("h",head)("i",irreversible));
        ilog("appbase priority queue size: ${q}", ("q", app().executor().get_priority_queue().size()));
      }
    }
    else {
      if (head == end_block_num-1)
        ilog("Received last block before the end. Waiting for acknowledgement");

      if (report_every > 0 && head % report_every == 0) {
        uint64_t free_bytes = db->get_segment_manager()->get_free_memory();
        uint64_t size = db->get_segment_manager()->get_size();
        ilog("block=${h}; irreversible=${i}; dbmem_free=${m}; received_blocks=${t}",
             ("h",head)("i",irreversible)("m", free_bytes*100/size)("t",received_blocks));
        if( exporter_will_ack )
          ilog("Exporter acknowledged block=${b}, unacknowledged=${u}",
               ("b", exporter_acked_block)("u", head-exporter_acked_block));
        ilog("appbase priority queue size: ${q}", ("q", app().executor().get_priority_queue().size()));
      }
    }

    return true;
  }


  void commit_db() {
    // if exporter is acknowledging, we only commit what is confirmed
    auto commit_rev = irreversible;
    if( exporter_will_ack && exporter_acked_block < commit_rev ) {
      commit_rev = exporter_acked_block;
    }
    db->commit(commit_rev);
  }


  void receive_block(eosio::input_stream& bin, const shared_ptr<flat_buffer>& p) {
    if (head == irreversible && !irreversible_only) {
      ilog("Crossing irreversible block=${h}", ("h",head));
    }

    auto block_ptr = std::make_shared<chronicle::channels::block>();
    block_ptr->block_num = head;
    block_ptr->block_id = head_id;
    block_ptr->last_irreversible = irreversible;
    block_ptr->buffer = p;

    from_bin(block_ptr->block, bin);
    block_header = block_ptr->block;
    trx_count = block_ptr->block.transactions.size();

    if( _block_started_chan.has_subscribers() ) {
      auto bb = std::make_shared<chronicle::channels::block_begins>();
      bb->block_num = head;
      bb->block_timestamp = block_header.timestamp;
      bb->last_irreversible = irreversible;
      _block_started_chan.publish(channel_priority, bb);
    }

    if (!skip_block_events) {
      _blocks_chan.publish(channel_priority, block_ptr);
    }
  }



  void receive_deltas(eosio::input_stream& bin, const shared_ptr<flat_buffer>& p) {
    uint32_t num;
    varuint32_from_bin(num, bin);
    for (uint32_t i = 0; i < num; ++i) {
      check_variant(bin, get_type("table_delta"), "table_delta_v0");

      auto bltd = std::make_shared<chronicle::channels::block_table_delta>();
      bltd->block_timestamp = block_header.timestamp;
      bltd->buffer = p;


      from_bin(bltd->table_delta, bin);
      auto& variant_type = get_type(bltd->table_delta.name);

      auto var = variant_type.as_variant();
      if (!var || !var->at(0).type->as_struct())
        throw std::runtime_error("don't know how to proccess " + variant_type.name);

      if ( !interactive_mode && bltd->table_delta.name == "account") {  // memorize contract ABI
        for (auto& row : bltd->table_delta.rows) {
          check_variant(row.data, variant_type, 0u);
          if (row.present) {
            eosio::ship_protocol::account_v0 acc;
            from_bin(acc, row.data);
            if( acc.abi.remaining() == 0 ) {
              clear_contract_abi(acc.name);
            }
            else {
              save_contract_abi(acc.name, acc.abi);
            }
          }
        }
      }
      else if (!noexport_mode && !skip_table_deltas) {
        if (bltd->table_delta.name == "contract_row" &&
            (_table_row_updates_chan.has_subscribers() || _abi_errors_chan.has_subscribers())) {
          for (auto& row : bltd->table_delta.rows) {
            check_variant(row.data, variant_type, 0u);
            auto tru = std::make_shared<chronicle::channels::table_row_update>();
            from_bin(tru->kvo, row.data);
            bool take = true;
            if (enable_tables_filter && tables_filter.count(tru->kvo.code.value) == 0) {
              take = false;
            }
            if (enable_tables_blacklist && tables_blacklist.count(tru->kvo.code.value) > 0) {
              take = false;
            }
            if (take) {
              if( get_contract_abi_ready(tru->kvo.code, interactive_mode) ) {
                tru->block_num = head;
                tru->block_timestamp = block_header.timestamp;
                tru->buffer = p;
                tru->added = row.present;
                _table_row_updates_chan.publish(channel_priority, tru);
              }
              else {
                auto ae =  std::make_shared<chronicle::channels::abi_error>();
                ae->block_num = head;
                ae->block_timestamp = block_header.timestamp;
                ae->account = tru->kvo.code;
                ae->error = "cannot decode table delta because of missing ABI";
                _abi_errors_chan.publish(channel_priority, ae);
              }
            }
          }
        }
        else if (!skip_account_info) {
          if (bltd->table_delta.name == "permission" && _permission_updates_chan.has_subscribers() ) {
            for (auto& row : bltd->table_delta.rows) {
              check_variant(row.data, variant_type, 0u);
              auto pu = std::make_shared<chronicle::channels::permission_update>();
              pu->block_num = head;
              pu->block_timestamp = block_header.timestamp;
              pu->buffer = p;
              from_bin(pu->permission, row.data);
              pu->added = row.present;
              _permission_updates_chan.publish(channel_priority, pu);
            }
          }
          else if (bltd->table_delta.name == "permission_link" && _permission_link_updates_chan.has_subscribers() ) {
            for (auto& row : bltd->table_delta.rows) {
              check_variant(row.data, variant_type, 0u);
              auto plu = std::make_shared<chronicle::channels::permission_link_update>();
              plu->block_num = head;
              plu->block_timestamp = block_header.timestamp;
              plu->buffer = p;
              from_bin(plu->permission_link, row.data);
              plu->added = row.present;
              _permission_link_updates_chan.publish(channel_priority, plu);
            }
          }
          else if (bltd->table_delta.name == "account_metadata" && _account_metadata_updates_chan.has_subscribers() ) {
            for (auto& row : bltd->table_delta.rows) {
              check_variant(row.data, variant_type, 0u);
              auto amu = std::make_shared<chronicle::channels::account_metadata_update>();
              amu->block_num = head;
              amu->block_timestamp = block_header.timestamp;
              amu->buffer = p;
              from_bin(amu->account_metadata, row.data);
              _account_metadata_updates_chan.publish(channel_priority, amu);
            }
          }
        }
      }

      if( _block_table_deltas_chan.has_subscribers() ) {
        _block_table_deltas_chan.publish(channel_priority, bltd);
      }
    }
  } // receive_deltas


  void init_contract_abi_ctxt() {
    if( contract_abi_ctxt ) {
      // dlog("Destroying ABI cache");
      abieos_destroy(contract_abi_ctxt);
      contract_abi_imported.clear();
    }
    contract_abi_ctxt = abieos_create();
  }


  void clear_contract_abi(name account) {
    if( contract_abi_imported.count(account.value) > 0 ) {
      init_contract_abi_ctxt(); // abieos_contract does not support removals, so we have to destroy it
    }

    const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
    auto itr = idx.find(account.value);
    if( itr != idx.end() ) {
      // dlog("Clearing contract ABI for ${a}", ("a",(std::string)account));
      db->remove(*itr);

      auto ar =  std::make_shared<chronicle::channels::abi_removal>();
      ar->block_num = head;
      ar->block_timestamp = block_header.timestamp;
      ar->account = account;
      _abi_removals_chan.publish(channel_priority, ar);

      string empty_string;
      save_contract_abi_history(account, empty_string.data(), empty_string.size());
    }
  }



  void save_contract_abi(name account, eosio::input_stream& data) {
    // dlog("Saving contract ABI for ${a}", ("a",(std::string)account));
    if( contract_abi_imported.count(account.value) > 0 ) {
      init_contract_abi_ctxt();
    }

    const char* bin_start = data.get_pos();
    size_t bin_size = data.remaining();

    try {
      // this checks the validity of ABI
      if( !abieos_set_abi_bin(contract_abi_ctxt, account.value, data.get_pos(), data.remaining()) ) {
        throw runtime_error( abieos_get_error(contract_abi_ctxt) );
      }
      contract_abi_imported.insert(account.value);

      {
        const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
        auto itr = idx.find(account.value);
        if( itr != idx.end() ) {
          db->modify( *itr, [&]( chronicle::contract_abi_object& o ) {
            o.set_abi(bin_start, bin_size);
            });
        }
        else {
          db->create<chronicle::contract_abi_object>( [&]( chronicle::contract_abi_object& o ) {
              o.account = account.value;
              o.set_abi(bin_start, bin_size);
            });
        }
      }

      if (_abi_updates_chan.has_subscribers()) {
        auto abiupd = std::make_shared<chronicle::channels::abi_update>();
        abiupd->block_num = head;
        abiupd->block_timestamp =block_header.timestamp;
        abiupd->account = account;
        abiupd->binary.assign(bin_start, bin_start + bin_size);
        from_bin(abiupd->abi, data);
        _abi_updates_chan.publish(channel_priority, abiupd);
      }
    }
    catch (const exception& e) {
      wlog("Cannot use ABI for ${a}: ${e}", ("a",(std::string)account)("e",e.what()));
      auto ae = std::make_shared<chronicle::channels::abi_error>();
      ae->block_num = head;
      ae->block_timestamp = block_header.timestamp;
      ae->account = account;
      ae->error = e.what();
      _abi_errors_chan.publish(channel_priority, ae);
    }

    save_contract_abi_history(account, bin_start, bin_size);
  }


  void save_contract_abi_history(name account, const char* data, size_t size) {
    const auto& idx = db->get_index<chronicle::contract_abi_hist_index, chronicle::by_name_and_block>();
    auto itr = idx.find(boost::make_tuple(account.value, head));
    if( itr != idx.end() ) {
      wlog("Multiple setabi for ${a} in the same block ${h}", ("a",(std::string)account)("h",head));
      db->modify( *itr, [&]( chronicle::contract_abi_history& o ) {
        o.set_abi(data, size);
        });
    }
    else {
      db->create<chronicle::contract_abi_history>( [&]( chronicle::contract_abi_history& o ) {
          o.account = account.value;
          o.block_index = head;
          o.set_abi(data, size);
        });
    }
    /* debugging */
    /*
    int count = 0;
    auto itr2 = idx.lower_bound(boost::make_tuple(account.value, 0));
    while( itr2 != idx.end() && itr2->account == account.value ) {
      count++;
      itr2++;
    }
    if( count == 0 ) {
      elog("Cannot find any entries in ABI history for ${a}", ("a",(std::string)account));
    }
    else if( count > 2 ) {
      dlog("${c} ABI revisions for ${a}", ("c",count)("a",(std::string)account));
    }
    */
  }


  bool get_contract_abi_ready(name account, bool lock) {
    if( contract_abi_imported.count(account.value) > 0 )
      return true; // the context has this contract loaded
    if (interactive_mode) {
      dlog("ABI requested for ${a} and block ${h}", ("a",(std::string)account)("h",head));
      if (lock)
        dblock->mutex.lock();
      const auto& idx = db->get_index<chronicle::contract_abi_hist_index, chronicle::by_name_and_block_rev>();
      auto itr = idx.lower_bound(boost::make_tuple(account.value, head));
      if (lock)
        dblock->mutex.unlock();
      if( itr != idx.end() ) {
        dlog("Found in history: ABI for ${a}, block ${b}", ("a",(std::string)account)("b",itr->block_index));
        abieos_set_abi_bin(contract_abi_ctxt, account.value, itr->abi.data(), itr->abi.size());
        contract_abi_imported.insert(account.value);
        return true;
      }
    }
    else {
      if (lock)
        dblock->mutex.lock();
      const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
      auto itr = idx.find(account.value);
      if (lock)
        dblock->mutex.unlock();
      if( itr != idx.end() ) {
        // dlog("Found in DB: ABI for ${a}", ("a",(std::string)account));
        abieos_set_abi_bin(contract_abi_ctxt, account.value, itr->abi.data(), itr->abi.size());
        contract_abi_imported.insert(account.value);
        return true;
      }
    }
    return false;
  }


  void receive_traces(eosio::input_stream& bin, const shared_ptr<flat_buffer>& p) {
    if (_transaction_traces_chan.has_subscribers()) {
      uint32_t num;
      varuint32_from_bin(num, bin);
      for (uint32_t i = 0; i < num; ++i) {
        auto tr = std::make_shared<chronicle::channels::transaction_trace>();
        tr->buffer = p;
        tr->bin_start = bin.get_pos();
        from_bin(tr->trace, bin);
        tr->bin_size = bin.get_pos() - tr->bin_start;

        // check blacklist and filter
        bool matched_blacklist = false;
        bool matched_rcvr_filter = false;
        bool matched_auth_filter = false;

        if( do_trace_filter || do_trace_blacklist ) {
          auto& trace = std::get<eosio::ship_protocol::transaction_trace_v0>(tr->trace);
          for( auto& atrace: trace.action_traces ) {

            eosio::ship_protocol::action*   act;
            eosio::name                     receiver;

            size_t index = atrace.index();
            if( index == 0 ) {
              eosio::ship_protocol::action_trace_v0& at = std::get<eosio::ship_protocol::action_trace_v0>(atrace);
              act = &at.act;
              receiver = at.receiver;
            }
            else if( index == 1 ) {
              eosio::ship_protocol::action_trace_v1& at = std::get<eosio::ship_protocol::action_trace_v1>(atrace);
              act = &at.act;
              receiver = at.receiver;
            }
            else {
              throw std::runtime_error(string("Invalid variant option in action_trace: ") + std::to_string(index));
            }

            // lookup in blacklist
            if( do_trace_blacklist ) {
              auto search_acc = blacklist_actions.find(act->account.value);
              if( search_acc != blacklist_actions.end() ) {
                if( search_acc->second.count(act->name.value) != 0 ) {
                  matched_blacklist = true;
                  break;
                }
              }
            }

            // check the receivers filter
            if( enable_rcvr_filter ) {
              if( rcvr_filter.count(receiver.value) > 0 ) {
                matched_rcvr_filter = true;
                break;
              }
            }

            // check auth filter
            if( enable_auth_filter ) {
              for( auto auth : act->authorization ) {
                if( auth_filter.count(auth.actor.value) > 0 ) {
                  matched_auth_filter = true;
                  break;
                }
              }

              if( matched_auth_filter )
                break;
            }
          }
        }

        if( !matched_blacklist ) {
          if( !do_trace_filter ||
              (enable_rcvr_filter && matched_rcvr_filter) ||
              (enable_auth_filter && matched_auth_filter) ) {
            tr->block_num = head;
            tr->block_timestamp = block_header.timestamp;
            _transaction_traces_chan.publish(channel_priority, tr);
          }
        }
      }
    }
  }


  const abi_type& get_type(const string& name) {
    auto it = abi_types.find(name);
    if (it == abi_types.end())
      throw runtime_error("unknown type "s + name);
    return it->second;
  }


  template <typename F>
  void send_request(const jvalue& value, F f) {
    auto bin = make_shared<vector<char>>();
    json_to_bin(*bin, &get_type("request"), value, [&]() {});
    stream->async_write(asio::buffer(*bin),
                        [bin, this, f](const error_code ec, size_t) {
                          callback(ec, "async_write", f); });
  }


  void check_variant(eosio::input_stream& bin, const abi_type& type, uint32_t expected) {
    uint32_t index;
    varuint32_from_bin(index, bin);
    auto var = type.as_variant();
    if (!var)
      throw runtime_error(type.name + " is not a variant"s);
    if (index >= var->size())
      throw runtime_error("expected "s + to_string(expected) + " got " + to_string(index));
    if (index != expected)
      throw runtime_error("expected "s + var->at(expected).name + " got " + var->at(index).name);
  }


  void check_variant(eosio::input_stream& bin, const abi_type& type, const char* expected) {
    uint32_t index;
    varuint32_from_bin(index, bin);
    auto var = type.as_variant();
    if (!var)
      throw runtime_error(type.name + " is not a variant"s);
    if (index >= var->size())
      throw runtime_error("expected "s + expected + " got " + to_string(index));
    if (var->at(index).name != expected)
      throw runtime_error("expected "s + expected + " got " + var->at(index).name);
  }


  template <typename F>
  void catch_and_close(F f) {
    try {
      f();
    } catch (const exception& e) {
      elog("ERROR: ${e}", ("e",e.what()));
      close();
    } catch (...) {
      elog("ERROR: unknown exception");
      close();
    }
  }


  template <typename F>
  void callback(const error_code ec, const char* what, F f) {
    if (ec)
      return on_fail(ec, what);
    catch_and_close(f);
  }

  void on_fail(const error_code ec, const char* what) {
    try {
      elog("ERROR: ${e}", ("e",ec.message()));
      close();
      abort_receiver();
    } catch (...) {
      elog("ERROR: exception while closing");
    }
  }

  void close() {
    if( !aborting && stream.use_count() > 0 && stream->is_open() ) {
      stream->next_layer().close();
      ilog("closed the receiver websocket connection");
      aborting = true;
    }
  }
};



receiver_plugin::receiver_plugin() : my(new receiver_plugin_impl)
{
  assert(receiver_plug == nullptr);
  receiver_plug = this;
};

receiver_plugin::~receiver_plugin(){
};


void receiver_plugin::set_program_options( options_description& cli, options_description& cfg ) {
  cfg.add_options()
    (RCV_HOST_OPT, bpo::value<string>()->default_value("localhost"), "Host to connect to (nodeos)")
    (RCV_PORT_OPT, bpo::value<string>()->default_value("8080"), "Port to connect to (nodeos state-history plugin)")
    (RCV_DBSIZE_OPT, bpo::value<uint32_t>()->default_value(16384), "database size in MB")
    (RCV_MODE_OPT, bpo::value<string>(), "Receiver mode. Values:\n"
     " scan:          \tread blocks sequentially and export\n"
     " scan-noexport: \tread blocks sequentially without export\n"
     " interactive:   \trandom access\n")
    (RCV_EVERY_OPT, bpo::value<uint32_t>()->default_value(10000), "Report current state every N blocks")
    (RCV_MAX_QUEUE_OPT, bpo::value<uint32_t>()->default_value(10000), "Maximum size of appbase priority queue")
    (RCV_SKIP_BLOCK_EVT_OPT, bpo::value<bool>()->default_value(false), "Do not produce BLOCK events")
    (RCV_SKIP_DELTAS_OPT, bpo::value<bool>()->default_value(false), "Do not produce table delta events")
    (RCV_SKIP_TRACES_OPT, bpo::value<bool>()->default_value(false), "Do not produce transaction trace events")
    (RCV_SKIP_ACCOUNT_INFO_OPT, bpo::value<bool>()->default_value(false), "Do not produce permissions and account metadata events")
    (RCV_IRREV_ONLY_OPT, bpo::value<bool>()->default_value(false), "Fetch only irreversible blocks")
    (RCV_START_BLOCK_OPT, bpo::value<uint32_t>()->default_value(0),
     "Start from a snapshot block instead of genesis")
    (RCV_END_BLOCK_OPT, bpo::value<uint32_t>()->default_value(std::numeric_limits<uint32_t>::max()),
     "Stop receiver before this block number")
    (RCV_STALE_DEADLINE_OPT, bpo::value<uint32_t>()->default_value(10000), "Stale socket deadline, msec")
    (RCV_ENABLE_RCVR_FILTER_OPT, bpo::value<bool>()->default_value(false), "Filter traces by receiver account")
    (RCV_INCLUDE_RECEIVER_OPT, bpo::value<vector<string>>(), "Account(s) to match in action receipts")
    (RCV_ENABLE_AUTH_FILTER_OPT, bpo::value<bool>()->default_value(false), "Filter traces by authorizer")
    (RCV_INCLUDE_AUTH_OPT, bpo::value<vector<string>>(), "Account(s) to match in authorizers")
    (RCV_BLACKLIST_ACTION_OPT, bpo::value<vector<string>>(), "contract:action to exclude from traces")
    (RCV_ENABLE_TABLES_FILTER_OPT, bpo::value<bool>()->default_value(false), "Filter table deltas by contract")
    (RCV_INCLUDE_TABLES_CONTRACT_OPT, bpo::value<vector<string>>(), "Contract account(s) to match in table deltas")
    (RCV_BLACKLIST_TABLES_CONTRACT_OPT, bpo::value<vector<string>>(), "Blacklisted contract account(s) for table deltas")
    ;
}


void receiver_plugin::plugin_initialize( const variables_map& options ) {
  try {
    if( !options.count("data-dir") ) {
      throw std::runtime_error("--data-dir option is required");
    }

    if( !options.count(RCV_MODE_OPT) ) {
      throw std::runtime_error("mode option is required");
    }

    my->irreversible_only = options.at(RCV_IRREV_ONLY_OPT).as<bool>();

    string receiver_mode = options.at(RCV_MODE_OPT).as<string>();
    if (receiver_mode == RCV_MODE_SCAN) {
      my->noexport_mode = false;
      my->interactive_mode = false;
    }
    else if (receiver_mode == RCV_MODE_SCAN_NOEXP) {
      my->noexport_mode = true;
      my->interactive_mode = false;
    }
    else if (receiver_mode == RCV_MODE_INTERACTIVE) {
      my->noexport_mode = false;
      my->interactive_mode = true;
      my->irreversible_only = true;
    }

    ilog("Starting in ${m} mode", ("m", receiver_mode));

    string dbdir = app().data_dir().native() + "/receiver-state";
    bfs::create_directories(dbdir);

    bool new_lock = false;
    string dblock_shm_file = dbdir + "/lock.bin";
    if(!bfs::exists(dblock_shm_file)) {
      std::ofstream ofs(dblock_shm_file, std::ofstream::trunc);
      ofs.close();
      bfs::resize_file(dblock_shm_file, sizeof(chronicle::shmem_lock));
      new_lock = true;
    }

    ilog("Using shared memory lock at ${f}", ("f", dblock_shm_file));
    my->_dblock_mapped_region =
      bip::mapped_region(bip::file_mapping(dblock_shm_file.c_str(), bip::read_write),
                         bip::read_write,
                         0, sizeof(chronicle::shmem_lock));

    void* dblock_addr = my->_dblock_mapped_region.get_address();
    if (new_lock) {
      my->dblock = new (dblock_addr) chronicle::shmem_lock;
    }
    else {
      my->dblock = static_cast<chronicle::shmem_lock*>(dblock_addr);
    }

    {
      bip::scoped_lock<bip::interprocess_mutex> lock(my->dblock->mutex);
      if (my->interactive_mode) {
        my->db = std::make_shared<chainbase::database>(dbdir, chainbase::database::read_only, 0, true);
      }
      else {
        my->db = std::make_shared<chainbase::database>
          (dbdir, chainbase::database::read_write,
           1024ULL * 1024 * options.at(RCV_DBSIZE_OPT).as<uint32_t>() );
      }

      my->db->add_index<chronicle::state_index>();
      my->db->add_index<chronicle::received_block_index>();
      my->db->add_index<chronicle::contract_abi_index>();
      my->db->add_index<chronicle::contract_abi_hist_index>();
    }

    my->host = options.at(RCV_HOST_OPT).as<string>();
    my->port = options.at(RCV_PORT_OPT).as<string>();
    my->report_every = options.at(RCV_EVERY_OPT).as<uint32_t>();
    my->max_queue_size = options.at(RCV_MAX_QUEUE_OPT).as<uint32_t>();

    my->skip_block_events = options.at(RCV_SKIP_BLOCK_EVT_OPT).as<bool>();
    if( my->skip_block_events )
      ilog("Skipping BLOCK events");

    my->skip_table_deltas = options.at(RCV_SKIP_DELTAS_OPT).as<bool>();
    if( my->skip_table_deltas )
      ilog("Skipping table delta events");

    my->skip_traces = options.at(RCV_SKIP_TRACES_OPT).as<bool>();
    if( my->skip_traces )
      ilog("Skipping transaction trace events");

    my->skip_account_info = options.at(RCV_SKIP_ACCOUNT_INFO_OPT).as<bool>();
    if( my->skip_account_info )
      ilog("Skipping account permissions and metadata events");

    if( my->irreversible_only )
      ilog("Fetching irreversible blocks only");

    my->start_block_num = options.at(RCV_START_BLOCK_OPT).as<uint32_t>();
    if( my->interactive_mode and my->start_block_num > 0 ) {
      throw std::runtime_error("cannot use start-block in interactive mode");
    }

    my->end_block_num = options.at(RCV_END_BLOCK_OPT).as<uint32_t>();

    my->stale_check_deadline_msec = options.at(RCV_STALE_DEADLINE_OPT).as<uint32_t>();

    my->enable_rcvr_filter = options.at(RCV_ENABLE_RCVR_FILTER_OPT).as<bool>();
    if( my->enable_rcvr_filter ) {
      ilog("Enabled receiver filter:");

      if( !options.count(RCV_INCLUDE_RECEIVER_OPT) ) {
        throw std::runtime_error("Need at least one name in include-receiver option");
      }

      auto filter_accs = options.at(RCV_INCLUDE_RECEIVER_OPT).as<vector<string>>();

      for(string accstr : filter_accs) {
        uint64_t accname = eosio::string_to_name_strict(accstr);
        my->rcvr_filter.insert(accname);
        ilog("  filtering on account: ${a}", ("a",accstr));
      }
    }

    my->enable_auth_filter = options.at(RCV_ENABLE_AUTH_FILTER_OPT).as<bool>();
    if( my->enable_auth_filter ) {
      ilog("Enabled authorizer filter:");

      if( !options.count(RCV_INCLUDE_AUTH_OPT) ) {
        throw std::runtime_error("Need at least one name in include-auth option");
      }

      auto filter_accs = options.at(RCV_INCLUDE_AUTH_OPT).as<vector<string>>();

      for(string accstr : filter_accs) {
        uint64_t accname = eosio::string_to_name_strict(accstr);
        my->auth_filter.insert(accname);
        ilog("  filtering on authorizer: ${a}", ("a",accstr));
      }
    }

    if( my->enable_rcvr_filter || my->enable_auth_filter ) {
      my->do_trace_filter = true;
    }

    vector<string> blacklist_str;

    if( options.count(RCV_BLACKLIST_ACTION_OPT) ) {
      auto blacklist_entries = options.at(RCV_BLACKLIST_ACTION_OPT).as<vector<string>>();
      for(string bl_entry : blacklist_entries) {
        blacklist_str.emplace_back(bl_entry);
      }
      my->do_trace_blacklist = true;
    }

    for( auto blpair : blacklist_str ) {
      std::vector<std::string> parts;
      boost::split( parts, blpair, boost::is_any_of(":"));
      if( parts.size() != 2 )
        throw std::runtime_error(string("Invalid blacklist entry: ") + blpair);

      std::vector<uint64_t> parts_n;
      for( auto s : parts ) {
        uint64_t v = eosio::string_to_name_strict(s);
        parts_n.emplace_back(v);
      }

      my->blacklist_actions[parts_n[0]].insert(parts_n[1]);
    }

    for( auto itr_contract = my->blacklist_actions.begin();
         itr_contract != my->blacklist_actions.end();
         ++itr_contract ) {

      for( auto itr_action = itr_contract->second.begin();
           itr_action != itr_contract->second.end();
           ++itr_action ) {
        ilog("Action blacklist entry: ${c}:${a}",
             ("c", eosio::name_to_string(itr_contract->first))("a", eosio::name_to_string(*itr_action)));
      }
    }

    my->enable_tables_filter = options.at(RCV_ENABLE_TABLES_FILTER_OPT).as<bool>();
    if( my->enable_tables_filter ) {
      ilog("Enabled table deltas filter:");

      if( !options.count(RCV_INCLUDE_TABLES_CONTRACT_OPT) ) {
        throw std::runtime_error("Need at least one name in include-tables-contract option");
      }

      auto filter_accs = options.at(RCV_INCLUDE_TABLES_CONTRACT_OPT).as<vector<string>>();

      for(string accstr : filter_accs) {
        uint64_t accname = eosio::string_to_name_strict(accstr);
        my->tables_filter.insert(accname);
        ilog("  filtering contract: ${a}", ("a",accstr));
      }
    }

    if( options.count(RCV_BLACKLIST_TABLES_CONTRACT_OPT) ) {
      my->enable_tables_blacklist = true;
      auto blacklist_table_accs = options.at(RCV_BLACKLIST_TABLES_CONTRACT_OPT).as<vector<string>>();
      for(string accstr : blacklist_table_accs) {
        uint64_t accname = eosio::string_to_name_strict(accstr);
        my->tables_blacklist.insert(accname);
        ilog("  blacklisted contract for table deltas: ${a}", ("a",accstr));
      }
    }

    my->init();
    ilog("Initialized receiver_plugin");
  }
  FC_LOG_AND_RETHROW();
}



void receiver_plugin::start_after_dependencies() {
  bool mustwait = false;
  while( dependent_plugins.size() > 0 && !mustwait ) {
    if( std::get<0>(dependent_plugins[0])->get_state() == appbase::abstract_plugin::started ) {
      ilog("Dependent plugin has started: ${p}", ("p",std::get<1>(dependent_plugins[0])));
      dependent_plugins.erase(dependent_plugins.begin());
    }
    else {
      ilog("Dependent plugin has not yet started: ${p}", ("p",std::get<1>(dependent_plugins[0])));
      mustwait = true;
    }
  }

  if( mustwait ) {
    ilog("Waiting for dependent plugins");
    my->pause_timer.expires_from_now(boost::posix_time::milliseconds(50));
    my->pause_timer.async_wait(boost::bind(&receiver_plugin::start_after_dependencies, this));
  }
  else {
    ilog("All dependent plugins started, launching the receiver");
    my->start();
  }
};


void receiver_plugin::plugin_startup(){
  start_after_dependencies();
  ilog("Started receiver_plugin");
}


void receiver_plugin::plugin_shutdown() {
  my->close();
  ilog("receiver_plugin stopped");
}


bool receiver_plugin::is_interactive() {
  return my->interactive_mode;
}

bool receiver_plugin::is_noexport() {
  return my->noexport_mode;
}


void receiver_plugin::exporter_will_ack_blocks(uint32_t max_unconfirmed) {
  assert(!my->exporter_will_ack);
  assert(max_unconfirmed > 0);
  if (my->interactive_mode)
    throw runtime_error("Exporter must not acknowledge blocks in interactive mode");
  if (my->noexport_mode)
    throw runtime_error("Exporter must not acknowledge blocks in noexport mode");
  my->exporter_will_ack = true;
  my->exporter_max_unconfirmed = max_unconfirmed;
  ilog("Receiver will pause at ${u} unacknowledged blocks", ("u", my->exporter_max_unconfirmed));
}


void receiver_plugin::ack_block(uint32_t block_num) {
  assert(my->exporter_will_ack);
  if( my->forked_at_block > 0 ) {
    uint32_t wait_for_ack = my->forked_at_block - 1;

    // ignore all confirmations for higher blocks until we receive fork-1 acknowledged
    if( block_num > wait_for_ack ) {
      ilog("Waiting for ${w} acknowledgement after a fork, received ack for block=${b}, ignoring it",
           ("w",wait_for_ack)("b",block_num));
      return;
    }

    if( block_num == wait_for_ack ) {
      ilog("Received expected ack after a fork for block=${b}, resuming the flow", ("b",block_num));
      my->forked_at_block = 0;
    }
    else {
      ilog("Waiting for ${w} acknowledgement after a fork, received ack for a block prior to it: block=${b}",
           ("w",wait_for_ack)("b",block_num));
    }
  }
  else {
    if( block_num < my->exporter_acked_block ) {
      elog("Exporter acked block=${a}, but block=${k} was already acknowledged",
           ("a",block_num)("k",my->exporter_acked_block));
      ::abort_receiver();
      return;
    }
  }
  my->exporter_acked_block = block_num;
  //dlog("Acked block=${b}", ("b",block_num));
}


void receiver_plugin::slowdown(bool pause) {
  my->slowdown_requested = pause;
}


abieos_context* receiver_plugin::get_contract_abi_ctxt(abieos::name account) {
  my->get_contract_abi_ready(account, true);
  return my->contract_abi_ctxt;
}


void receiver_plugin::add_dependency(appbase::abstract_plugin* plug, string plugname) {
  dependent_plugins.emplace_back(std::make_tuple(plug, plugname));
}


void receiver_plugin::abort_receiver() {
  if( my ) {
    my->close();
  }
}


void receiver_plugin::walk_abi_history(std::function<void (uint64_t account, uint32_t block_index,
                                                           const char* abi_data, size_t abi_size)> callback)
{
  const auto& idx = my->db->get_index<chronicle::contract_abi_hist_index, chronicle::by_id>();
  auto itr = idx.begin();
  while( itr != idx.end() ) {
    callback(itr->account, itr->block_index, itr->abi.data(), itr->abi.size());
    ++itr;
  }
}


bool is_noexport_opt(const variables_map& options)
{
  if( !options.count(RCV_MODE_OPT) ) {
    throw std::runtime_error("mode option is required");
  }
  return (options.at(RCV_MODE_OPT).as<string>() == RCV_MODE_SCAN_NOEXP);
}


static bool have_exporter = false;

void exporter_initialized() {
  if( have_exporter )
    throw runtime_error("Only one exporter plugin is allowed");
  have_exporter = true;
}

receiver_plugin* receiver_plug = nullptr;

void exporter_will_ack_blocks(uint32_t max_unconfirmed)
{
  receiver_plug->exporter_will_ack_blocks(max_unconfirmed);
}

// receiver should not start collecting data before all dependent plugins are ready
void donot_start_receiver_before(appbase::abstract_plugin* plug, string plugname) {
  receiver_plug->add_dependency(plug, plugname);
}

void abort_receiver() {
  receiver_plug->abort_receiver();
  app().quit();
}
