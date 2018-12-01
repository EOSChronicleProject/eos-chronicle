// copyright defined in LICENSE.txt

#include "receiver_plugin.hpp"
#include <chainbase/chainbase.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/program_options.hpp>
#include <boost/asio/signal_set.hpp>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <memory>
#include <string_view>



using namespace abieos;
using namespace appbase;
using namespace std::literals;
using namespace chain_state;

using std::cerr;
using std::enable_shared_from_this;
using std::exception;
using std::make_shared;
using std::make_unique;
using std::map;
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



// decoder state database objects

namespace chronicle {
  using namespace chainbase;
  using namespace boost::multi_index;
  
  enum dbtables {
    state_table,
    received_blocks_table,
  };

  struct by_id;
  struct by_blocknum;

  // this is a singleton
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

  // list of received blocks andn their IDs, truncated from head as new blocks are received
  struct received_block_object : public chainbase::object<received_blocks_table, received_block_object>  {
    CHAINBASE_DEFAULT_CONSTRUCTOR(received_block_object);
    id_type      id;
    uint32_t     block_index;
    checksum256  block_id;
  };

  using received_block_index = chainbase::shared_multi_index_container<
    received_block_object,
    indexed_by<
      ordered_unique<tag<by_id>, member<received_block_object, received_block_object::id_type, &received_block_object::id>>,
      ordered_unique<tag<by_blocknum>, BOOST_MULTI_INDEX_MEMBER(received_block_object, uint32_t, block_index)>>>;
}

CHAINBASE_SET_INDEX_TYPE(chronicle::state_object, chronicle::state_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::received_block_object, chronicle::received_block_index)


std::vector<char> zlib_decompress(input_buffer data) {
  std::vector<char>      out;
  bio::filtering_ostream decomp;
  decomp.push(bio::zlib_decompressor());
  decomp.push(bio::back_inserter(out));
  bio::write(decomp, data.pos, data.end - data.pos);
  bio::close(decomp);
  return out;
}





class receiver_plugin_impl : std::enable_shared_from_this<receiver_plugin_impl> {
public:
  shared_ptr<chainbase::database>       db;
  shared_ptr<tcp::resolver>             resolver;
  shared_ptr<websocket::stream<tcp::socket>> stream;
  string                                host;
  string                                port;
  uint32_t                              skip_to;
  
  uint32_t                              head            = 0;
  checksum256                           head_id         = {};
  uint32_t                              irreversible    = 0;
  checksum256                           irreversible_id = {};
  uint32_t                              first_bulk      = 0;
  abi_def                               abi{};
  map<string, abi_type>                 abi_types;
 

  void start() {
    load_state();
    resolver->async_resolve
      (host, port,
       [this](error_code ec, tcp::resolver::results_type results) {
        if (ec)
          std::cerr << "during lookup of " << host << ":" << port << ": " << ec;
        
        callback(ec, "resolve", [&] {
            asio::async_connect
              (
               stream->next_layer(),
               results.begin(),
               results.end(),
               [this](error_code ec, auto&) {
                 callback(ec, "connect", [&] {
                     stream->async_handshake(host, "/", [this](error_code ec) {
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
    const auto& idx = db->get_index<chronicle::state_index, chronicle::by_id>();
    auto itr = idx.begin();
    if( itr != idx.end() ) {
      head = itr->head;
      head_id = itr->head_id;
      irreversible = itr->irreversible;
      irreversible_id = itr->irreversible_id;
    }
    else {
      head = 0;
      head_id = {};
      irreversible = 0;
      irreversible_id = {};
    }
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
    stream->async_read(*in_buffer, [this, in_buffer](error_code ec, size_t) {
        callback(ec, "async_read", [&] {
            receive_abi(in_buffer);
            request_blocks();
            continue_read();
          });
      });
  }

  void continue_read() {
    auto in_buffer = make_shared<flat_buffer>();
    stream->async_read(*in_buffer, [this, in_buffer](error_code ec, size_t) {
        callback(ec, "async_read", [&] {
            if (!receive_result(in_buffer))
              return;
            continue_read();
          });
      });
  }

  
  void receive_abi(const shared_ptr<flat_buffer>& p) {
    auto data = p->data();
    if (!json_to_native(abi, string_view{(const char*)data.data(), data.size()}))
      throw runtime_error("abi parse error");
    check_abi_version(abi.version);
    abi_types    = create_contract(abi).abi_types;
  }

  
  void request_blocks()
  {
    jarray positions;
    const auto& idx = db->get_index<chronicle::received_block_index, chronicle::by_blocknum>();
    auto itr = idx.lower_bound(irreversible);
    while( itr != idx.end() && itr->block_index <= head ) {
      positions.push_back(jvalue{jobject{
            {{"block_num"s}, {itr->block_index}},
              {{"block_id"s}, {(string)itr->block_id}},
                }});
    }

    send(jvalue{jarray{{"get_blocks_request_v0"s},
            {jobject{
                {{"start_block_num"s}, {to_string(max(skip_to, head + 1))}},
                  {{"end_block_num"s}, {"4294967295"s}},
                    {{"max_messages_in_flight"s}, {"4294967295"s}},
                      {{"have_positions"s}, {positions}},
                        {{"irreversible_only"s}, {false}},
                          {{"fetch_block"s}, {true}},
                            {{"fetch_traces"s}, {true}},
                              {{"fetch_deltas"s}, {true}},
                                }}}});
  }

  
  bool receive_result(const shared_ptr<flat_buffer>& p) {
    auto         data = p->data();
    input_buffer bin{(const char*)data.data(), (const char*)data.data() + data.size()};
    check_variant(bin, get_type("result"), "get_blocks_result_v0");

    get_blocks_result_v0 result;
    if (!bin_to_native(result, bin))
      throw runtime_error("result conversion error");

    if (!result.this_block)
      return true;

    if (result.this_block->block_num <= head) {
      std::cerr << "switch forks at block " << result.this_block->block_num;
      app().get_channel<chronicle::channels::forks>().publish(result.this_block->block_num);
    }

    std::cerr << "block " << result.this_block->block_num <<"\n";
    
    if (head > 0 && (!result.prev_block || result.prev_block->block_id.value != head_id.value))
      throw runtime_error("prev_block does not match");
    
    if (result.block)
      receive_block(result.this_block->block_num, result.this_block->block_id, *result.block);
    if (result.deltas)
      receive_deltas(result.this_block->block_num, *result.deltas);
    if (result.traces)
      receive_traces(result.this_block->block_num, *result.traces);

    head            = result.this_block->block_num;
    head_id         = result.this_block->block_id;
    irreversible    = result.last_irreversible.block_num;
    irreversible_id = result.last_irreversible.block_id;

    save_state();    
    return true;
  }


  
  void
  receive_block(uint32_t block_index, const checksum256& block_id, input_buffer bin) {
    std::shared_ptr<signed_block> block_ptr = std::make_shared<signed_block>();
    if (!bin_to_native(*block_ptr, bin))
      throw runtime_error("block conversion error");
    app().get_channel<chronicle::channels::blocks>().publish(block_ptr);
  } // receive_block

  
  void receive_deltas(uint32_t block_num, input_buffer buf) {
    auto         data = zlib_decompress(buf);
    input_buffer bin{data.data(), data.data() + data.size()};

    auto     num     = read_varuint32(bin);
    for (uint32_t i = 0; i < num; ++i) {
      check_variant(bin, get_type("table_delta"), "table_delta_v0");

      std::shared_ptr<chronicle::channels::block_table_delta> bltd =
        std::make_shared<chronicle::channels::block_table_delta>();

      if (!bin_to_native(bltd->table_delta, bin))
        throw runtime_error("table_delta conversion error (1)");

      auto& variant_type = get_type(bltd->table_delta.name);
      if (!variant_type.filled_variant || variant_type.fields.size() != 1 || !variant_type.fields[0].type->filled_struct)
        throw std::runtime_error("don't know how to proccess " + variant_type.name);
      auto& type = *variant_type.fields[0].type;

      size_t num_processed = 0;
      for (auto& row : bltd->table_delta.rows) {
        check_variant(row.data, variant_type, 0u);
      }

      app().get_channel<chronicle::channels::block_table_deltas>().publish(bltd);
    }
  } // receive_deltas

  
  void receive_traces(uint32_t block_num, input_buffer buf) {
    auto         data = zlib_decompress(buf);
    input_buffer bin{data.data(), data.data() + data.size()};
    auto         num = read_varuint32(bin);
    for (uint32_t i = 0; i < num; ++i) {
      transaction_trace trace;
      if (!bin_to_native(trace, bin))
        throw runtime_error("transaction_trace conversion error (1)");
      write_transaction_trace(block_num, trace);
    }
  }

  
  void write_transaction_trace(uint32_t block_num, transaction_trace& ttrace) {
    string id     = ttrace.failed_dtrx_trace.empty() ? "" : string(ttrace.failed_dtrx_trace[0].id);
    int32_t num_actions = 0;
    for (auto& atrace : ttrace.action_traces)
      write_action_trace(block_num, ttrace, num_actions, 0, atrace);
    if (!ttrace.failed_dtrx_trace.empty()) {
      auto& child = ttrace.failed_dtrx_trace[0];
      write_transaction_trace(block_num, child);
    }
  } // write_transaction_trace


  void write_action_trace(uint32_t block_num, transaction_trace& ttrace, int32_t& num_actions,
                          int32_t parent_action_index, action_trace& atrace) {
    const auto action_index = ++num_actions;

    for (auto& child : atrace.inline_traces)
      write_action_trace(block_num, ttrace, num_actions, action_index, child);

    write_action_trace_subtable("action_trace_authorization", block_num, ttrace, action_index, atrace.authorization);
    write_action_trace_subtable("action_trace_auth_sequence", block_num, ttrace, action_index, atrace.receipt_auth_sequence);
    write_action_trace_subtable("action_trace_ram_delta", block_num, ttrace, action_index, atrace.account_ram_deltas);
  } // write_action_trace

  
  template <typename T>
  void write_action_trace_subtable(const std::string& name, uint32_t block_num, transaction_trace& ttrace,
                                   int32_t action_index, T& objects) {
    int32_t num = 0;
    for (auto& obj : objects)
      write_action_trace_subtable(name, block_num, ttrace, action_index, num, obj);
  }

  
  template <typename T>
  void write_action_trace_subtable(const std::string& name, uint32_t block_num, transaction_trace& ttrace,
                                   int32_t action_index, int32_t& num, T& obj) {
    ++num;
  }


  const abi_type& get_type(const string& name) {
    auto it = abi_types.find(name);
    if (it == abi_types.end())
      throw runtime_error("unknown type "s + name);
    return it->second;
  }


  void send(const jvalue& value) {
    auto bin = make_shared<vector<char>>();
    if (!json_to_bin(*bin, &get_type("request"), value))
      throw runtime_error("failed to convert during send");

    stream->async_write(asio::buffer(*bin),
                       [bin, this](error_code ec, size_t) {
                         callback(ec, "async_write", [&] {}); });
  }

  
  void check_variant(input_buffer& bin, const abi_type& type, uint32_t expected) {
    auto index = read_varuint32(bin);
    if (!type.filled_variant)
      throw runtime_error(type.name + " is not a variant"s);
    if (index >= type.fields.size())
      throw runtime_error("expected "s + type.fields[expected].name + " got " + to_string(index));
    if (index != expected)
      throw runtime_error("expected "s + type.fields[expected].name + " got " + type.fields[index].name);
  }

  
  void check_variant(input_buffer& bin, const abi_type& type, const char* expected) {
    auto index = read_varuint32(bin);
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
      cerr << "error: " << e.what() << "\n";
      close();
    } catch (...) {
      cerr << "error: unknown exception\n";
      close();
    }
  }

  
  template <typename F>
  void callback(error_code ec, const char* what, F f) {
    if (ec)
      return on_fail(ec, what);
    catch_and_close(f);
  }

  void on_fail(error_code ec, const char* what) {
    try {
      cerr << what << ": " << ec.message() << "\n";
      close();
    } catch (...) {
      cerr << "error: exception while closing\n";
    }
  }

  void close() {
    stream->next_layer().close();
  }
};



receiver_plugin::receiver_plugin() :my(new receiver_plugin_impl){
};

receiver_plugin::~receiver_plugin(){
};


void receiver_plugin::set_program_options( options_description& cli, options_description& cfg ) {
  cfg.add_options()
    ("host", bpo::value<string>()->default_value("localhost"), "Host to connect to (nodeos)")
    ("port", bpo::value<string>()->default_value("8080"), "Port to connect to (nodeos state-history plugin)")
    ("skip-to", bpo::value<uint32_t>()->default_value(0), "Skip blocks before [arg]")
    ("receiver-state-db-size", bpo::value<uint32_t>()->default_value(1024), "database size in MB")
    ;
}

  
void receiver_plugin::plugin_initialize( const variables_map& options ) {
  try {
    if( !options.count("data-dir") ) {
      throw std::runtime_error("--data-dir option is required");
    }
    
    my->db = std::make_shared<chainbase::database>
      (options.at("data-dir").as<string>() + "/receiver-state",
       chainbase::database::read_write,
       options.at("receiver-state-db-size").as<uint32_t>() * 1024*1024);
    my->db->add_index<chronicle::state_index>();
    my->db->add_index<chronicle::received_block_index>();
    
    my->resolver = std::make_shared<tcp::resolver>(std::ref(app().get_io_service()));
    
    my->stream = std::make_shared<websocket::stream<tcp::socket>>(std::ref(app().get_io_service()));
    my->stream->binary(true);
    my->stream->read_message_max(1024 * 1024 * 1024);
    
    my->host = options.at("host").as<string>();
    my->port = options.at("port").as<string>();
    my->skip_to = options.at("skip-to").as<uint32_t>();
    
    std::cerr << "initialized receiver_plugin\n";
  } catch ( const boost::exception& e ) {
    std::cerr << boost::diagnostic_information(e) << "\n";
    throw;
  } catch ( const std::exception& e ) {
    std::cerr << e.what() << "\n";
    throw;
  } catch ( ... ) {
    std::cerr << "unknown exception\n";
    throw;
  }
}


void receiver_plugin::plugin_startup(){
  my->start();
  std::cerr << "started receiver_plugin\n";
}

void receiver_plugin::plugin_shutdown() {
  std::cerr << "receiver_plugin stopped\n";
}

