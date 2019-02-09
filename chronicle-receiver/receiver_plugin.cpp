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
    contract_abi_objects_table,
    table_id_object_table
  };

  struct by_id;
  struct by_blocknum;
  struct by_name;
  struct by_tid;
  
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
      ordered_unique<tag<by_id>, member<received_block_object, received_block_object::id_type, &received_block_object::id>>,
      ordered_unique<tag<by_blocknum>, BOOST_MULTI_INDEX_MEMBER(received_block_object, uint32_t, block_index)>>>;

  // serialized binary ABI for every contract

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
      ordered_unique<tag<by_id>, member<contract_abi_object, contract_abi_object::id_type, &contract_abi_object::id>>,
      ordered_unique<tag<by_name>, BOOST_MULTI_INDEX_MEMBER(contract_abi_object, uint64_t, account)>>>;
}

CHAINBASE_SET_INDEX_TYPE(chronicle::state_object, chronicle::state_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::received_block_object, chronicle::received_block_index)
CHAINBASE_SET_INDEX_TYPE(chronicle::contract_abi_object, chronicle::contract_abi_index)


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
  uint32_t                              skip_to = 0;
  
  uint32_t                              head            = 0;
  checksum256                           head_id         = {};
  uint32_t                              irreversible    = 0;
  checksum256                           irreversible_id = {};
  uint32_t                              first_bulk      = 0;
  abi_def                               abi{};
  map<string, abi_type>                 abi_types;

  map<uint64_t, shared_ptr<contract>>   contract_abi_cache; // Cache is cleared on forks, so it will never grow too high

  void start() {
    load_state();
    resolver->async_resolve
      (host, port,
       [this](error_code ec, tcp::resolver::results_type results) {
        if (ec)
          cerr << "during lookup of " << host << ":" << port << ": " << ec;
        
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
    bool did_undo = false;
    {
      auto &index = db->get_index<chronicle::state_index>();
      if( index.stack().size() > 0 ) {
        cerr << "Database has " << index.stack().size() << " uncommitted revisions. Reverting back.\n";
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

    if( did_undo ) {
      cerr << "Reverted to block=" << head << ", issuing an explicit fork event\n";
      app().get_channel<chronicle::channels::forks>().publish(head);
    }
    
    if( skip_to > 0 ) {
      if( skip_to <= head ) {
        throw runtime_error("skip-to is behind the current head");
      }
      head = 0;
      head_id = {};
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
            {{"block_num"s}, {std::to_string(itr->block_index)}},
              {{"block_id"s}, {(string)itr->block_id}},
                }});
      itr++;
    }

    uint32_t start_block = max(skip_to, head + 1);
    cerr << "Start block: " << start_block << "\n";
    
    send(jvalue{jarray{{"get_blocks_request_v0"s},
            {jobject{
                {{"start_block_num"s}, {to_string(start_block)}},
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
      
    uint32_t    last_irreversoble_num = result.last_irreversible.block_num;
    if( skip_to > last_irreversoble_num ) {
      throw runtime_error("skip-to cannot be past irreversible block");
    }

    uint32_t    block_num = result.this_block->block_num;
    checksum256 block_id = result.this_block->block_id;

    bool srart_undo_session = false;
    
    if( last_irreversoble_num >= block_num ) {
      db->set_revision(block_num); // we're catching up nehind the irreversible
    }
    else {
      // we're at the blockchain head
      
      if (block_num <= head) { //received a block that is lower than what we already saw
        cerr << "fork detected at block " << block_num <<"; head=" << head <<"\n";
        contract_abi_cache.clear();
        while( db->revision() >= block_num ) {
          db->undo();
        }
        if( db->revision() == -1 ) {
          throw runtime_error(std::string("Cannot rollback, no undo stack at revision ")+
                              std::to_string(db->revision()));
        }
        app().get_channel<chronicle::channels::forks>().publish(block_num);
      }
      else
        if (head > 0 && (!result.prev_block || result.prev_block->block_id.value != head_id.value))
          throw runtime_error("prev_block does not match");

      srart_undo_session = true;
    }
    
    auto undo_session = db->start_undo_session(srart_undo_session);

    if( block_num > irreversible ) {
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
    irreversible    = result.last_irreversible.block_num;
    irreversible_id = result.last_irreversible.block_id;
    
    if (result.block)
      receive_block(*result.block);
    if (result.deltas)
      receive_deltas(*result.deltas);
    if (result.traces)
      receive_traces(*result.traces);

    save_state();

    // save a new revision
    undo_session.push();
    
    db->commit(irreversible);
    return true;
  }


  
  void receive_block(input_buffer bin) {
    if( head == irreversible ) {
      cerr << "crossing irreversible block=" << head <<"\n";
    }
      
    if (head % 1000 == 0) {
         uint64_t free_bytes = db->get_segment_manager()->get_free_memory();
         uint64_t size = db->get_segment_manager()->get_size();
         cerr << "block=" << head << "; irreversible=" << irreversible << "; dbmem_free=" << free_bytes*100/size << "%\n";
    }

    /*
      as of now, we don't do anything with raw block data, but we can, if needed:
      std::shared_ptr<signed_block> block_ptr = std::make_shared<signed_block>();
      if (!bin_to_native(*block_ptr, bin))
        throw runtime_error("block conversion error");
    */
       
    auto& channel = app().get_channel<chronicle::channels::blocks>();    
    if (channel.has_subscribers()) {
      std::shared_ptr<chronicle::channels::block> block_ptr = std::make_shared<chronicle::channels::block>();
      block_ptr->block_num = head;
      block_ptr->last_irreversible = irreversible;
      channel.publish(block_ptr);
    }
  }


  
  void receive_deltas(input_buffer buf) {
    auto         data = zlib_decompress(buf);
    input_buffer bin{data.data(), data.data() + data.size()};
    auto& rowschannel = app().get_channel<chronicle::channels::table_row_updates>();
    auto& errchannel = app().get_channel<chronicle::channels::abi_errors>();
    auto& btdchannel = app().get_channel<chronicle::channels::block_table_deltas>();
    
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

      if (bltd->table_delta.name == "account") {  // memorize contract ABI
        for (auto& row : bltd->table_delta.rows) {
          if (row.present) {
            account_object acc;
            if (!bin_to_native(acc, row.data))
              throw runtime_error("account row conversion error");
            if( acc.abi.data.size() == 0 ) {
              clear_contract_abi(acc.name);
            }
            else {
              save_contract_abi(acc.name, acc.abi.data);
            }
          }
        }
      }
      else {
        if (bltd->table_delta.name == "contract_row" && 
            (rowschannel.has_subscribers() || errchannel.has_subscribers())) {
          for (auto& row : bltd->table_delta.rows) {
            std::shared_ptr<chronicle::channels::table_row_update> tru =
              std::make_shared<chronicle::channels::table_row_update>();
            if (!bin_to_native(tru->kvo, row.data))
              throw runtime_error("cannot read table row object");
            tru->ctr = get_contract_abi(tru->kvo.code);
            if( tru->ctr != nullptr ) {
              tru->added = row.present;
              rowschannel.publish(tru);
            }
            else {
              std::shared_ptr<chronicle::channels::abi_error> ae =
                std::make_shared<chronicle::channels::abi_error>();
              ae->block_num = head;
              ae->account = tru->kvo.code;
              ae->error = "cannot decode table delta because of mising ABI";
              errchannel.publish(ae);
            }
          }
        }
      }
      btdchannel.publish(bltd);
    }
  } // receive_deltas


  void clear_contract_abi(name account) {    
    auto cache_itr = contract_abi_cache.find(account.value);
    if( cache_itr != contract_abi_cache.end() ) {
      if( cache_itr->second == nullptr )
        return;
    }
    
    contract_abi_cache[account.value] = nullptr;

    const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
    auto itr = idx.find(account.value);
    if( itr != idx.end() ) {
      cerr << "Clearing contract ABI for " << (std::string)account << "\n";
      db->remove(*itr);
      app().get_channel<chronicle::channels::abi_removals>().publish(account);
    }
  }


  
  void save_contract_abi(name account, std::vector<char> data) {
    cerr << "Saving contract ABI for " << (std::string)account << "\n";
    
    std::shared_ptr<chronicle::channels::abi_update> abiupd =
      std::make_shared<chronicle::channels::abi_update>();

    input_buffer bin{data.data(), data.data() + data.size()};

    try {
      check_abi_version(bin);
      if (!bin_to_native(abiupd->abi, bin)) {
        throw runtime_error("contract abi deserialization error");
      }
      
      std::shared_ptr<contract> ctr = std::make_shared<contract>(create_contract(abiupd->abi));
      contract_abi_cache[account.value] = ctr;

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
      abiupd->account = account;
      app().get_channel<chronicle::channels::abi_updates>().publish(abiupd);
    }
    catch (const exception& e) {
      cerr << "ERROR: Cannot use ABI for " << (std::string)account << ": " << e.what() << "\n";
      std::shared_ptr<chronicle::channels::abi_error> ae =
        std::make_shared<chronicle::channels::abi_error>();
      ae->block_num = head;
      ae->account = account;
      ae->error = e.what();
      app().get_channel<chronicle::channels::abi_errors>().publish(ae);
    }
  }

  
  std::shared_ptr<contract> get_contract_abi(name account) {    
    auto cache_itr = contract_abi_cache.find(account.value);
    if( cache_itr != contract_abi_cache.end() ) {
      return cache_itr->second;
    }
    
    const auto& idx = db->get_index<chronicle::contract_abi_index, chronicle::by_name>();
    auto itr = idx.find(account.value);
    if( itr != idx.end() ) {
      cerr << "Found in DB: ABI for " << (std::string)account << "\n";
      input_buffer bin{itr->abi.data(), itr->abi.data()+itr->abi.size()};
      check_abi_version(bin);
      abi_def contract_abi{};
      if (!bin_to_native(contract_abi, bin)) {
        throw runtime_error("contract abi deserialization error");
      }      
      std::shared_ptr<contract> ctr = std::make_shared<contract>(create_contract(contract_abi));
      contract_abi_cache[account.value] = ctr;
      return ctr;
    }
    contract_abi_cache[account.value] = nullptr;
    return nullptr;
  }
  
  
  void receive_traces(input_buffer buf) {
    auto& channel = app().get_channel<chronicle::channels::transaction_traces>();
    if (channel.has_subscribers()) {
      auto         data = zlib_decompress(buf);
      input_buffer bin{data.data(), data.data() + data.size()};
      auto         num = read_varuint32(bin);
      for (uint32_t i = 0; i < num; ++i) {
        std::shared_ptr<chronicle::channels::transaction_trace> tr =
          std::make_shared<chronicle::channels::transaction_trace>();
        if (!bin_to_native(tr->trace, bin))
          throw runtime_error("transaction_trace conversion error (1)");
        tr->block_num = head;
        channel.publish(tr);
      }
    }
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
    my->db->add_index<chronicle::contract_abi_index>();
    
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


std::shared_ptr<abieos::contract> receiver_plugin::retrieve_contract_abi(abieos::name account) {
  return my->get_contract_abi(account);
}

// global implementation of ABI retriever

static receiver_plugin* receiver_plug = nullptr;

std::shared_ptr<contract> retrieve_contract_abi(name account) {
  if( ! receiver_plug ) {
    receiver_plug = app().find_plugin<receiver_plugin>();
  }
  return receiver_plug->retrieve_contract_abi(account);
}



