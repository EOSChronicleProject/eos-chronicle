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
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/program_options.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/deadline_timer.hpp>
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

using namespace chain_state;
using namespace state_history;

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
  const char* RCV_IRREV_ONLY_OPT = "irreversible-only";
  const char* RCV_END_BLOCK_OPT = "end-block";

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

    void set_abi(const std::vector<char> data) {
      abi.resize(data.size());
      abi.assign(data.data(), data.size());
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

    void set_abi(const std::vector<char> data) {
      abi.resize(data.size());
      abi.assign(data.data(), data.size());
    }
  };

  // History is
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
}

CHAINBASE_SET_INDEX_TYPE(chronicle::state_object, chronicle::state_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::received_block_object, chronicle::received_block_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::contract_abi_object, chronicle::contract_abi_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::contract_abi_history, chronicle::contract_abi_hist_index)


std::shared_ptr<std::vector<char>> zlib_decompress(input_buffer data) {
  std::shared_ptr<std::vector<char>> out = std::make_shared<std::vector<char>>();
  bio::filtering_ostream decomp;
  decomp.push(bio::zlib_decompressor());
  decomp.push(bio::back_inserter(*out));
  bio::write(decomp, data.pos, data.end - data.pos);
  bio::close(decomp);
  return out;
}





class receiver_plugin_impl : std::enable_shared_from_this<receiver_plugin_impl> {
public:
  receiver_plugin_impl() :
    _forks_chan(app().get_channel<chronicle::channels::forks>()),
    _blocks_chan(app().get_channel<chronicle::channels::blocks>()),
    _block_table_deltas_chan(app().get_channel<chronicle::channels::block_table_deltas>()),
    _transaction_traces_chan(app().get_channel<chronicle::channels::transaction_traces>()),
    _abi_updates_chan(app().get_channel<chronicle::channels::abi_updates>()),
    _abi_removals_chan(app().get_channel<chronicle::channels::abi_removals>()),
    _abi_errors_chan(app().get_channel<chronicle::channels::abi_errors>()),
    _table_row_updates_chan(app().get_channel<chronicle::channels::table_row_updates>()),
    _receiver_pauses_chan(app().get_channel<chronicle::channels::receiver_pauses>()),
    _block_completed_chan(app().get_channel<chronicle::channels::block_completed>()),
    mytimer(std::ref(app().get_io_service()))
  {};

  shared_ptr<chainbase::database>       db;
  shared_ptr<tcp::resolver>             resolver;
  shared_ptr<websocket::stream<tcp::socket>> stream;
  const int                             stream_priority = 40;

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
  bool                                  irreversible_only;
  uint32_t                              end_block_num;


  uint32_t                              head            = 0;
  checksum256                           head_id         = {};
  uint32_t                              irreversible    = 0;
  checksum256                           irreversible_id = {};
  uint32_t                              first_bulk      = 0;
  abieos::block_timestamp               block_timestamp;

  // needed for decoding state history input
  map<string, abi_type>                 abi_types;

  // The context keeps decoded versions of contract ABI
  abieos_context*                       contract_abi_ctxt = nullptr;
  set<uint64_t>                         contract_abi_imported;

  std::map<name,std::set<name>>         blacklist_actions;

  chronicle::channels::forks::channel_type&               _forks_chan;
  chronicle::channels::blocks::channel_type&              _blocks_chan;
  chronicle::channels::block_table_deltas::channel_type&  _block_table_deltas_chan;
  chronicle::channels::transaction_traces::channel_type&  _transaction_traces_chan;
  chronicle::channels::abi_updates::channel_type&         _abi_updates_chan;
  chronicle::channels::abi_removals::channel_type&        _abi_removals_chan;
  chronicle::channels::abi_errors::channel_type&          _abi_errors_chan;
  chronicle::channels::table_row_updates::channel_type&   _table_row_updates_chan;
  chronicle::channels::receiver_pauses::channel_type&     _receiver_pauses_chan;
  chronicle::channels::block_completed::channel_type&     _block_completed_chan;

  const int channel_priority = 50;

  bool                                  exporter_will_ack = false;
  uint32_t                              exporter_acked_block = 0;
  uint32_t                              exporter_max_unconfirmed;
  boost::asio::deadline_timer           mytimer;
  uint32_t                              pause_time_msec = 0;
  bool                                  slowdown_requested = false;


  void init() {
    if (interactive_mode) {
      _interactive_requests_subscription =
        app().get_channel<chronicle::channels::interactive_requests>().subscribe
        ([this](std::shared_ptr<chronicle::channels::interactive_request> req){ on_block_req(req); });
    }
  }


  void start() {
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
      auto &index = db->get_index<chronicle::state_index>();
      if( index.stack().size() > 0 ) {
        depth = index.stack().size();
        ilog("Database has ${d} uncommitted revisions. Reverting back", ("d",depth));
        while (index.stack().size() > 0)
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
    }
    else {
      ilog("Re-scanning the state history from genesis. Issuing an explicit fork event");
      auto fe = std::make_shared<chronicle::channels::fork_event>();
      fe->fork_block_num = 0;
      fe->depth = 0;
      fe->fork_reason = chronicle::channels::fork_reason_val::resync;
      fe->last_irreversible = 0;
      _forks_chan.publish(channel_priority, fe);
    }

    if( did_undo ) {
      ilog("Reverted to block=${b}, issuing an explicit fork event", ("b",head));
      auto fe = std::make_shared<chronicle::channels::fork_event>();
      fe->fork_block_num = head;
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
       app().get_priority_queue().wrap(stream_priority, [this, in_buffer](const error_code ec, size_t) {
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
  }


  void continue_read() {
    if (check_pause()) {
      pause_time_msec = 0;
      auto in_buffer = make_shared<flat_buffer>();
      stream->async_read
        (*in_buffer,
         app().get_priority_queue().wrap(stream_priority, [this, in_buffer](const error_code ec, size_t) {
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
        (exporter_will_ack && head - exporter_acked_block >= exporter_max_unconfirmed) ||
        app().get_priority_queue().size() > max_queue_size ||
        (!interactive_mode && head == end_block_num -1)) {

      slowdown_requested = false;

      if ( head == end_block_num -1 && irreversible >= head &&
           (!exporter_will_ack || exporter_acked_block == head) ) {
        ilog("Reached the end block ${b}. Stopping the receiver.", ("b", head));
        commit_db();
        abort_receiver();
        return false;
      }

      if( pause_time_msec == 0 ) {
        pause_time_msec = 100;
      }
      else if( pause_time_msec < 8000 ) {
        pause_time_msec *= 2;
      }

      if( pause_time_msec >= 1000 ) {
        auto rp = std::make_shared<chronicle::channels::receiver_pause>();
        rp->head = head;
        rp->acknowledged = exporter_acked_block;
        _receiver_pauses_chan.publish(channel_priority, rp);
        ilog("Pausing the reader");
      }

      mytimer.expires_from_now(boost::posix_time::milliseconds(pause_time_msec));
      mytimer.async_wait
        (app().get_priority_queue().wrap(stream_priority, [this](const error_code ec) {
            callback(ec, "async_wait", [&] {
                continue_read();
              });
          }));
      return false;
    }
    return true;
  }

  void receive_abi(const shared_ptr<flat_buffer> p) {
    auto data = p->data();
    std::string error;
    abi_def abi{};
    if (!json_to_native(abi, error, string_view{(const char*)data.data(), data.size()}))
      throw runtime_error("abi parse error: " + error);
    if( !check_abi_version(abi.version, error) )
      throw runtime_error("abi version error: " + error);
    abieos::contract c;
    if( !fill_contract(c, error, abi) )
      throw runtime_error(error);
    abi_types = std::move(c.abi_types);
  }


  void request_blocks() {
    jarray positions;
    const auto& idx = db->get_index<chronicle::received_block_index, chronicle::by_blocknum>();
    auto itr = idx.lower_bound(irreversible);
    while( itr != idx.end() && itr->block_index <= head ) {
      positions.push_back(jvalue{jobject{
            {{"block_num"s}, {std::to_string(itr->block_index)}},
              {{"block_id"s}, {(string)itr->block_id}},
                }});
      itr++;
    }

    uint32_t start_block = head + 1;
    ilog("Start block: ${b}", ("b",start_block));

    bool fetch_block = noexport_mode ? false:true;
    bool fetch_traces = noexport_mode ? false:true;
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
      bool fetch_traces = true;
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
    input_buffer bin{(const char*)data.data(), (const char*)data.data() + data.size()};
    check_variant(bin, get_type("result"), "get_blocks_result_v0");

    string error;
    get_blocks_result_v0 result;
    if (!bin_to_native(result, error, bin))
      throw runtime_error("result conversion error: " + error);

    if (!result.this_block)
      return true;

    uint32_t    last_irreversible_num = result.last_irreversible.block_num;

    uint32_t    block_num = result.this_block->block_num;
    checksum256 block_id = result.this_block->block_id;

    if (interactive_mode) {
      if( block_num == end_block_num-1 ) {
        interactive_req_pending = false;
        process_interactive_reqs();
      }      
    }
    else {
      if( db->revision() < block_num-1 ) {
        uint32_t newrev = block_num-1;
        dlog("Current DB revision: ${r}. Setting to ${n}", ("r",db->revision())("n",newrev));
        dlog("Acknowledged: ${a}", ("a",exporter_acked_block));
        db->set_revision(newrev);
      }

      if( block_num > last_irreversible_num ) {
        // we're at the blockchain head
        if (block_num <= head) { //received a block that is lower than what we already saw
          ilog("fork detected at block ${b}; head=${h}", ("b",block_num)("h",head));
          uint32_t depth = head - block_num;
          init_contract_abi_ctxt();
          while( db->revision() >= block_num ) {
            db->undo();
          }
          dlog("rolled back DB revision to ${r}", ("r",db->revision()));
          if( db->revision() <= 0 ) {
            throw runtime_error(std::string("Cannot rollback, no undo stack at revision ")+
                                std::to_string(db->revision()));
          }

          if( exporter_will_ack && exporter_acked_block > block_num )
            exporter_acked_block = block_num;

          auto fe = std::make_shared<chronicle::channels::fork_event>();
          fe->fork_block_num = block_num;
          fe->depth = depth;
          fe->fork_reason = chronicle::channels::fork_reason_val::network;
          fe->last_irreversible = last_irreversible_num;
          _forks_chan.publish(channel_priority, fe);
        }
        else
          if (head > 0 && (!result.prev_block || result.prev_block->block_id.value != head_id.value))
            throw runtime_error("prev_block does not match");
      }
    }

    auto undo_session = db->start_undo_session( !interactive_mode );

    if (!interactive_mode && block_num > irreversible ) {
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

    head            = block_num;
    head_id         = block_id;
    irreversible    = last_irreversible_num;
    irreversible_id = result.last_irreversible.block_id;

    if (result.block)
      receive_block(*result.block, p);
    if (result.deltas)
      receive_deltas(*result.deltas, p);
    if (result.traces)
      receive_traces(*result.traces, p);

    auto bf = std::make_shared<chronicle::channels::block_finished>();
    bf->block_num = head;
    bf->last_irreversible = irreversible;
    _block_completed_chan.publish(channel_priority, bf);

    if( aborting )
      return false;

    if (!interactive_mode) {
      save_state();
      undo_session.push();     // save a new revision
      commit_db();

      if (head == end_block_num-1)
        ilog("Received last block before the end. Waiting for acknowledgement");

      if (report_every > 0 && head % report_every == 0) {
        uint64_t free_bytes = db->get_segment_manager()->get_free_memory();
        uint64_t size = db->get_segment_manager()->get_size();
        ilog("block=${h}; irreversible=${i}; dbmem_free=${m}",
             ("h",head)("i",irreversible)("m", free_bytes*100/size));
        if( exporter_will_ack )
          ilog("Exporter acknowledged block=${b}, unacknowledged=${u}",
               ("b", exporter_acked_block)("u", head-exporter_acked_block));
        ilog("appbase priority queue size: ${q}", ("q", app().get_priority_queue().size()));
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


  void receive_block(input_buffer bin, const shared_ptr<flat_buffer>& p) {
    if (head == irreversible) {
      ilog("Crossing irreversible block=${h}", ("h",head));
    }

    auto block_ptr = std::make_shared<chronicle::channels::block>();
    block_ptr->block_num = head;
    block_ptr->last_irreversible = irreversible;
    block_ptr->buffer = p;

    string error;
    if (!bin_to_native(block_ptr->block, error, bin))
      throw runtime_error("block conversion error: " + error);
    block_timestamp = block_ptr->block.timestamp;
    if (!skip_block_events) {
      _blocks_chan.publish(channel_priority, block_ptr);
    }
  }



  void receive_deltas(input_buffer buf, const shared_ptr<flat_buffer>& p) {
    auto         data = zlib_decompress(buf);
    input_buffer bin{data->data(), data->data() + data->size()};

    uint32_t num;
    string error;
    if( !read_varuint32(bin, error, num) )
      throw runtime_error(error);
    for (uint32_t i = 0; i < num; ++i) {
      check_variant(bin, get_type("table_delta"), "table_delta_v0");

      auto bltd = std::make_shared<chronicle::channels::block_table_delta>();
      bltd->block_timestamp = block_timestamp;
      bltd->buffer = data;
        
      string error;
      if (!bin_to_native(bltd->table_delta, error, bin))
        throw runtime_error("table_delta conversion error: " + error);

      auto& variant_type = get_type(bltd->table_delta.name);
      if (!variant_type.filled_variant || variant_type.fields.size() != 1 || !variant_type.fields[0].type->filled_struct)
        throw std::runtime_error("don't know how to proccess " + variant_type.name);
      auto& type = *variant_type.fields[0].type;

      size_t num_processed = 0;
      for (auto& row : bltd->table_delta.rows) {
        check_variant(row.data, variant_type, 0u);
      }

      if ( !interactive_mode && bltd->table_delta.name == "account") {  // memorize contract ABI
        for (auto& row : bltd->table_delta.rows) {
          if (row.present) {
            string error;
            account_object acc;
            if (!bin_to_native(acc, error, row.data))
              throw runtime_error("account row conversion error: " + error);
            if( acc.abi.data.size() == 0 ) {
              clear_contract_abi(acc.name);
            }
            else {
              save_contract_abi(acc.name, acc.abi.data);
            }
          }
        }
      }
      else if (!noexport_mode && !skip_table_deltas) {
        if (bltd->table_delta.name == "contract_row" &&
            (_table_row_updates_chan.has_subscribers() || _abi_errors_chan.has_subscribers())) {
          for (auto& row : bltd->table_delta.rows) {
            auto tru = std::make_shared<chronicle::channels::table_row_update>();
            tru->block_num = head;
            tru->block_timestamp = block_timestamp;
            tru->buffer = data;
            
            string error;
            if (!bin_to_native(tru->kvo, error, row.data))
              throw runtime_error("cannot read table row object" + error);
            if( get_contract_abi_ready(tru->kvo.code) ) {
              tru->added = row.present;
              _table_row_updates_chan.publish(channel_priority, tru);
            }
            else {
              auto ae =  std::make_shared<chronicle::channels::abi_error>();
              ae->block_num = head;
              ae->block_timestamp = block_timestamp;
              ae->account = tru->kvo.code;
              ae->error = "cannot decode table delta because of missing ABI";
              _abi_errors_chan.publish(channel_priority, ae);
            }
          }
        }
      }
      _block_table_deltas_chan.publish(channel_priority, bltd);
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
    if( contract_abi_imported.count(account.value) > 0 )
      init_contract_abi_ctxt(); // abieos_contract does not support removals, so we have to destroy it
    const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
    auto itr = idx.find(account.value);
    if( itr != idx.end() ) {
      // dlog("Clearing contract ABI for ${a}", ("a",(std::string)account));
      db->remove(*itr);

      auto ar =  std::make_shared<chronicle::channels::abi_removal>();
      ar->block_num = head;
      ar->block_timestamp = block_timestamp;
      ar->account = account;
      _abi_removals_chan.publish(channel_priority, ar);
    }
    save_contract_abi_history(account, std::vector<char>());
  }



  void save_contract_abi(name account, std::vector<char> data) {
    // dlog("Saving contract ABI for ${a}", ("a",(std::string)account));
    if( contract_abi_imported.count(account.value) > 0 ) {
      init_contract_abi_ctxt();
    }

    try {
      // this checks the validity of ABI
      if( !abieos_set_abi_bin(contract_abi_ctxt, account.value, data.data(), data.size()) ) {
        throw runtime_error( abieos_get_error(contract_abi_ctxt) );
      }
      contract_abi_imported.insert(account.value);

      const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
      auto itr = idx.find(account.value);
      if( itr != idx.end() ) {
        db->modify( *itr, [&]( chronicle::contract_abi_object& o ) {
            o.set_abi(data);
          });
      }
      else {
        db->create<chronicle::contract_abi_object>( [&]( chronicle::contract_abi_object& o ) {
            o.account = account.value;
            o.set_abi(data);
          });
      }

      if (_abi_updates_chan.has_subscribers()) {
        auto abiupd = std::make_shared<chronicle::channels::abi_update>();
        abiupd->block_num = head;
        abiupd->block_timestamp = block_timestamp;
        abiupd->account = account;
        abiupd->abi_bytes = bytes {data};
        input_buffer buf{data.data(), data.data() + data.size()};
        string error;
        if (!bin_to_native(abiupd->abi, error, buf))
          throw runtime_error(error);
        _abi_updates_chan.publish(channel_priority, abiupd);
      }
    }
    catch (const exception& e) {
      wlog("Cannot use ABI for ${a}: ${e}", ("a",(std::string)account)("e",e.what()));
      auto ae = std::make_shared<chronicle::channels::abi_error>();
      ae->block_num = head;
      ae->block_timestamp = block_timestamp;
      ae->account = account;
      ae->error = e.what();
      _abi_errors_chan.publish(channel_priority, ae);
    }

    save_contract_abi_history(account, data);
  }


  void save_contract_abi_history(name account, std::vector<char> data) {
    const auto& idx = db->get_index<chronicle::contract_abi_hist_index, chronicle::by_name_and_block>();
    auto itr = idx.find(boost::make_tuple(account.value, head));
    if( itr != idx.end() ) {
      wlog("Multiple setabi for ${a} in the same block ${h}", ("a",(std::string)account)("h",head));
      db->modify( *itr, [&]( chronicle::contract_abi_history& o ) {
          o.set_abi(data);
        });
    }
    else {
      db->create<chronicle::contract_abi_history>( [&]( chronicle::contract_abi_history& o ) {
          o.account = account.value;
          o.block_index = head;
          o.set_abi(data);
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


  bool get_contract_abi_ready(name account) {
    if( contract_abi_imported.count(account.value) > 0 )
      return true; // the context has this contract loaded
    if (interactive_mode) {
      dlog("ABI requested for ${a} and block ${h}", ("a",(std::string)account)("h",head));
      const auto& idx = db->get_index<chronicle::contract_abi_hist_index, chronicle::by_name_and_block_rev>();
      auto itr = idx.lower_bound(boost::make_tuple(account.value, head));
      if( itr != idx.end() ) {
        dlog("Found in history: ABI for ${a}, block ${b}", ("a",(std::string)account)("b",itr->block_index));
        abieos_set_abi_bin(contract_abi_ctxt, account.value, itr->abi.data(), itr->abi.size());
        contract_abi_imported.insert(account.value);
        return true;
      }
    }
    else {
      const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
      auto itr = idx.find(account.value);
      if( itr != idx.end() ) {
        // dlog("Found in DB: ABI for ${a}", ("a",(std::string)account));
        abieos_set_abi_bin(contract_abi_ctxt, account.value, itr->abi.data(), itr->abi.size());
        contract_abi_imported.insert(account.value);
        return true;
      }
    }
    return false;
  }


  void receive_traces(input_buffer buf, const shared_ptr<flat_buffer>& p) {
    if (_transaction_traces_chan.has_subscribers()) {
      auto         data = zlib_decompress(buf);
      input_buffer bin{data->data(), data->data() + data->size()};
      uint32_t num;
      string       error;
      if( !read_varuint32(bin, error, num) )
        throw runtime_error(error);
      for (uint32_t i = 0; i < num; ++i) {
        auto tr = std::make_shared<chronicle::channels::transaction_trace>();
        tr->buffer = data;
        if (!bin_to_native(tr->trace, error, bin))
          throw runtime_error("transaction_trace conversion error: " + error);
        // check blacklist
        bool blacklisted = false;
        auto& trace = std::get<state_history::transaction_trace_v0>(tr->trace);
        if( trace.action_traces.size() > 0 ) {
          auto &at = std::get<state_history::action_trace_v0>(trace.action_traces[0]);
          auto search_acc = blacklist_actions.find(at.receiver);
          if(search_acc != blacklist_actions.end()) {
            if( search_acc->second.count(at.act.name) != 0 ) {
              blacklisted = true;
            }
          }
        }
        if( !blacklisted ) {
          tr->block_num = head;
          tr->block_timestamp = block_timestamp;
          _transaction_traces_chan.publish(channel_priority, tr);
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
    string error;
    auto bin = make_shared<vector<char>>();
    if (!json_to_bin(*bin, error, &get_type("request"), value))
      throw runtime_error("failed to convert during send_request: " + error);

    stream->async_write(asio::buffer(*bin),
                        [bin, this, f](const error_code ec, size_t) {
                          callback(ec, "async_write", f); });
  }


  void check_variant(input_buffer& bin, const abi_type& type, uint32_t expected) {
    string error;
    uint32_t index;
    if( !read_varuint32(bin, error, index) )
      throw runtime_error(error);
    if (!type.filled_variant)
      throw runtime_error(type.name + " is not a variant"s);
    if (index >= type.fields.size())
      throw runtime_error("expected "s + type.fields[expected].name + " got " + to_string(index));
    if (index != expected)
      throw runtime_error("expected "s + type.fields[expected].name + " got " + type.fields[index].name);
  }


  void check_variant(input_buffer& bin, const abi_type& type, const char* expected) {
    string error;
    uint32_t index;
    if( !read_varuint32(bin, error, index) )
      throw runtime_error(error);
    if (!type.filled_variant)
      throw runtime_error(type.name + " is not a variant"s);
    if (index >= type.fields.size())
      throw runtime_error("expected "s + expected + " got " + to_string(index));
    if (type.fields[index].name != expected)
      throw runtime_error("expected "s + expected + " got " + type.fields[index].name);
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
    if( stream.use_count() > 0 && stream->is_open() ) {
      stream->next_layer().close();
    }
    aborting = true;
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
    (RCV_DBSIZE_OPT, bpo::value<uint32_t>()->default_value(1024), "database size in MB")
    (RCV_MODE_OPT, bpo::value<string>(), "Receiver mode. Values:\n"
     " scan:          \tread blocks sequentially and export\n"
     " scan-noexport: \tread blocks sequentially without export\n"
     " interactive:   \trandom access\n")
    (RCV_EVERY_OPT, bpo::value<uint32_t>()->default_value(10000), "Report current state every N blocks")
    (RCV_MAX_QUEUE_OPT, bpo::value<uint32_t>()->default_value(10000), "Maximum size of appbase priority queue")
    (RCV_SKIP_BLOCK_EVT_OPT, bpo::value<bool>()->default_value(false), "Do not produce BLOCK events")
    (RCV_SKIP_DELTAS_OPT, bpo::value<bool>()->default_value(false), "Do not produce table delta events")
    (RCV_IRREV_ONLY_OPT, bpo::value<bool>()->default_value(false), "Fetch only irreversible blocks")
    (RCV_END_BLOCK_OPT, bpo::value<uint32_t>()->default_value(std::numeric_limits<uint32_t>::max()),
     "Stop receiver before this block number")
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
    if (my->interactive_mode) {
      my->db = std::make_shared<chainbase::database>(dbdir, chainbase::database::read_only, 0, true);
    }
    else {
      my->db = std::make_shared<chainbase::database>
        (dbdir, chainbase::database::read_write,
         options.at(RCV_DBSIZE_OPT).as<uint32_t>() * 1024*1024);
    }

    my->db->add_index<chronicle::state_index>();
    my->db->add_index<chronicle::received_block_index>();
    my->db->add_index<chronicle::contract_abi_index>();
    my->db->add_index<chronicle::contract_abi_hist_index>();

    my->resolver = std::make_shared<tcp::resolver>(std::ref(app().get_io_service()));

    my->stream = std::make_shared<websocket::stream<tcp::socket>>(std::ref(app().get_io_service()));
    my->stream->binary(true);
    my->stream->read_message_max(1024 * 1024 * 1024);

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

    if( my->irreversible_only )
      ilog("Fetching irreversible blocks only");

    my->end_block_num = options.at(RCV_END_BLOCK_OPT).as<uint32_t>();

    my->blacklist_actions.emplace
      (std::make_pair(abieos::name("eosio"),
                      std::set<abieos::name>{abieos::name("onblock")} ));
    my->blacklist_actions.emplace
      (std::make_pair(abieos::name("blocktwitter"),
                      std::set<abieos::name>{abieos::name("tweet")} ));
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
    my->mytimer.expires_from_now(boost::posix_time::milliseconds(50));
    my->mytimer.async_wait(boost::bind(&receiver_plugin::start_after_dependencies, this));
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
  if( block_num < my->exporter_acked_block ) {
    elog("Exporter acked block=${a}, but block=${k} was already acknowledged",
         ("a",block_num)("k",my->exporter_acked_block));
    throw runtime_error("Exporter acked block below prevuously acked one");
  }
  my->exporter_acked_block = block_num;
  //dlog("Acked block=${b}", ("b",block_num));
}


void receiver_plugin::slowdown() {
  my->slowdown_requested = true;
}


abieos_context* receiver_plugin::get_contract_abi_ctxt(abieos::name account) {
  my->get_contract_abi_ready(account);
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
