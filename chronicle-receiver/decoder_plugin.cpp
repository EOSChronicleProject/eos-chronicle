// copyright defined in LICENSE.txt

#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"

#include <iostream>
#include <string>
#include <fc/io/json.hpp>


using namespace chronicle::channels;
using namespace abieos;
using namespace chain_state;

using std::make_shared;



namespace json_encoder {
  inline constexpr bool trace_native_to_json = false;
  
  struct native_to_json_state {
    rapidjson::Writer<rapidjson::StringBuffer>& writer;
    
    native_to_json_state(rapidjson::Writer<rapidjson::StringBuffer>& writer)
      : writer{writer} {}
  };

  inline void native_to_json(const std::string& str, native_to_json_state& state) {
    state.writer.String(str.c_str(), str.size());
  }

  inline void native_to_json(const optional<std::string>& str, native_to_json_state& state) {
    if( str ) {
      state.writer.String(str.value().c_str(), str.value().size());
    }
    else {
      state.writer.String("");
    }
  }
  
  inline void arithmetic_to_json(const uint64_t& v, native_to_json_state& state) {
    state.writer.Uint64(v);
  }

  inline void arithmetic_to_json(const int64_t& v, native_to_json_state& state) {
    state.writer.Int64(v);
  }

  inline void arithmetic_to_json(const uint32_t& v, native_to_json_state& state) {
    state.writer.Uint(v);
  }

  inline void arithmetic_to_json(const int32_t& v, native_to_json_state& state) {
    state.writer.Int(v);
  }
  
  // These two functions are stolen from native_to_bin branch in abieos and should go away as
  // soon as it merges
  template <class C, typename M>
  const C* class_from_void(M C::*, const void* v) {
    return reinterpret_cast<const C*>(v);
  }
  
  template <auto P>
  auto& member_from_void(const member_ptr<P>&, const void* p) {
    return class_from_void(P, p)->*P;
  }
  
  template <typename T>
  void native_to_json(const std::vector<T>& obj, native_to_json_state& state) {
    state.writer.StartArray();
    for (auto& v : obj)
      native_to_json(v, state);
    state.writer.EndArray();
  }

  inline void native_to_json(const chain_state::transaction_status& obj, native_to_json_state& state) {
    std::string result = to_string(obj);
    state.writer.String(result.c_str(), result.size());
  }
    
  inline void native_to_json(const name& obj, native_to_json_state& state) { arithmetic_to_json(obj.value, state); }
  
  inline void native_to_json(const bytes& obj, native_to_json_state& state) {
    std::string result;
    boost::algorithm::hex(obj.data.begin(), obj.data.end(), std::back_inserter(result));
    state.writer.String(result.c_str(), result.size());
  }

  template <unsigned size>
  inline void native_to_json(const fixed_binary<size>& obj, native_to_json_state& state) {
    std::string result;
    boost::algorithm::hex(obj.value.begin(), obj.value.end(), std::back_inserter(result));
    state.writer.String(result.c_str(), result.size());
  }
  
  inline void native_to_json(const bool& obj, native_to_json_state& state) {
    state.writer.Bool(obj);
  }

  inline void native_to_json(const varuint32& obj, native_to_json_state& state) {
    arithmetic_to_json(obj.value, state);
  }
  
  inline void native_to_json(const chain_state::action_trace& obj, native_to_json_state& state) {
    native_to_json(obj.receipt_receiver, state);
    native_to_json(obj.receipt_act_digest, state);
    native_to_json(obj.receipt_global_sequence, state);
    native_to_json(obj.receipt_recv_sequence, state);
    native_to_json(obj.receipt_auth_sequence, state);
    native_to_json(obj.receipt_code_sequence, state);
    native_to_json(obj.receipt_abi_sequence, state);
    native_to_json(obj.account, state);
    native_to_json(obj.name, state);
    native_to_json(obj.authorization, state);

    // encode action data accordin gto ABI
    auto ctxt = get_contract_abi_ctxt(obj.account);
    const string action_name = name_to_string(obj.name.value);
    string datajs = abieos_bin_to_json(ctxt, obj.account.value, action_name.c_str(),
                                       obj.data.data.data(), obj.data.data.size());
    state.writer.RawValue(datajs.c_str(), datajs.size(), rapidjson::kObjectType);
    
    native_to_json(obj.context_free, state);
    native_to_json(obj.elapsed, state);
    native_to_json(obj.console, state);
    native_to_json(obj.account_ram_deltas, state);
    native_to_json(obj.except, state);
    native_to_json(obj.inline_traces, state);
  }
  
  inline void native_to_json(const chain_state::recurse_action_trace& obj, native_to_json_state& state) {
    const chain_state::action_trace& o = obj; native_to_json(o, state);
  }

  
  inline void native_to_json(const chain_state::transaction_trace& obj, native_to_json_state& state) {
    native_to_json(obj.id, state);
    native_to_json(obj.status, state);
    native_to_json(obj.cpu_usage_us, state);
    native_to_json(obj.net_usage_words, state);
    native_to_json(obj.elapsed, state);
    native_to_json(obj.net_usage, state);
    native_to_json(obj.scheduled, state);
    native_to_json(obj.action_traces, state);
    native_to_json(obj.except, state);
    native_to_json(obj.failed_dtrx_trace, state);
  }

  inline void native_to_json(const chain_state::recurse_transaction_trace& obj, native_to_json_state& state) {
    const chain_state::transaction_trace& o = obj; native_to_json(o, state);
  }
    

  
  template <typename T>
  void native_to_json(const T& obj, native_to_json_state& state) {
    if constexpr (std::is_class_v<T>) {
        state.writer.StartObject();
        for_each_field((T*)nullptr, [&](auto* name, auto member_ptr) {
            state.writer.Key(name);
            native_to_json(member_from_void(member_ptr, &obj), state);
          });
        state.writer.EndObject();
      }
    else {
      static_assert(std::is_arithmetic_v<T>);
      arithmetic_to_json(obj, state);
    }
  }
  
  template <typename T>
  void native_to_json(T& v, std::string& dest) {
    rapidjson::StringBuffer buffer{};
    rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};
    native_to_json_state state{writer};
    native_to_json(v, state);
    dest = buffer.GetString();
  }
}


class decoder_plugin_impl : std::enable_shared_from_this<decoder_plugin_impl> {
public:
  decoder_plugin_impl():
    _js_forks_chan(app().get_channel<chronicle::channels::js_forks>()),
    _js_blocks_chan(app().get_channel<chronicle::channels::js_blocks>()),
    _js_transaction_traces_chan(app().get_channel<chronicle::channels::js_transaction_traces>()),
    _js_abi_updates_chan(app().get_channel<chronicle::channels::js_abi_updates>()),
    _js_abi_removals_chan(app().get_channel<chronicle::channels::js_abi_removals>()),
    _js_abi_errors_chan(app().get_channel<chronicle::channels::js_abi_errors>()),
    _js_table_row_updates_chan(app().get_channel<chronicle::channels::js_table_row_updates>())
  {}
  
  chronicle::channels::js_forks::channel_type&               _js_forks_chan;
  chronicle::channels::js_blocks::channel_type&              _js_blocks_chan;
  chronicle::channels::js_transaction_traces::channel_type&  _js_transaction_traces_chan;
  chronicle::channels::js_abi_updates::channel_type&         _js_abi_updates_chan;
  chronicle::channels::js_abi_removals::channel_type&        _js_abi_removals_chan;
  chronicle::channels::js_abi_errors::channel_type&          _js_abi_errors_chan;
  chronicle::channels::js_table_row_updates::channel_type&   _js_table_row_updates_chan;

  chronicle::channels::forks::channel_type::handle               _forks_subscription;
  chronicle::channels::blocks::channel_type::handle              _blocks_subscription;
  chronicle::channels::transaction_traces::channel_type::handle  _transaction_traces_subscription;
  chronicle::channels::abi_errors::channel_type::handle          _abi_errors_subscription;
  chronicle::channels::abi_removals::channel_type::handle        _abi_removals_subscription;
  chronicle::channels::abi_updates::channel_type::handle         _abi_updates_subscription;
  chronicle::channels::table_row_updates::channel_type::handle   _table_row_updates_subscription;

  // we only siubscribe to receiver channels at startup, assuming our consumers have subscribed at init
  void start() {
    if (_js_forks_chan.has_subscribers()) {
      _forks_subscription =
        app().get_channel<chronicle::channels::forks>().subscribe
        ([this](std::shared_ptr<chronicle::channels::fork_event> fe){
          on_fork(fe);
        });
    }
    if (_js_blocks_chan.has_subscribers()) {
      _blocks_subscription =
        app().get_channel<chronicle::channels::blocks>().subscribe
        ([this](std::shared_ptr<chronicle::channels::block> block_ptr){
          on_block(block_ptr);
        });
    }
    if (_js_transaction_traces_chan.has_subscribers()) {
      _transaction_traces_subscription =
        app().get_channel<chronicle::channels::transaction_traces>().subscribe
        ([this](std::shared_ptr<chronicle::channels::transaction_trace> tr){
          on_transaction_trace(tr);
        });
    }
    if (_js_abi_updates_chan.has_subscribers()) {
      _abi_updates_subscription =
        app().get_channel<chronicle::channels::abi_updates>().subscribe
        ([this](std::shared_ptr<chronicle::channels::abi_update> abiupd){
          on_abi_update(abiupd);
        });
    }
    if (_js_abi_removals_chan.has_subscribers()) {
      _abi_removals_subscription =
        app().get_channel<chronicle::channels::abi_removals>().subscribe
        ([this](abieos::name contract){
          on_abi_removal(contract);
        });
    }
    if (_js_abi_errors_chan.has_subscribers()) {
      _abi_errors_subscription =
        app().get_channel<chronicle::channels::abi_errors>().subscribe
        ([this](std::shared_ptr<chronicle::channels::abi_error> abierr){
          on_abi_error(abierr);
        });
    }
    if (_js_table_row_updates_chan.has_subscribers()) {
      _table_row_updates_subscription =
        app().get_channel<chronicle::channels::table_row_updates>().subscribe
        ([this](std::shared_ptr<chronicle::channels::table_row_update> trupd){
          on_table_row_update(trupd);
        });
    }
  }

  void on_fork(std::shared_ptr<chronicle::channels::fork_event> fe) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(*fe, *output);
    _js_forks_chan.publish(output);
  }

  void on_block(std::shared_ptr<chronicle::channels::block> block_ptr) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(*block_ptr, *output);
    _js_forks_chan.publish(output);
  }

  void on_transaction_trace(std::shared_ptr<chronicle::channels::transaction_trace> ccttr) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(*ccttr, *output);
    _js_forks_chan.publish(output);
  }

  void on_abi_update(std::shared_ptr<chronicle::channels::abi_update> abiupd) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(*abiupd, *output);
    _js_forks_chan.publish(output);
  }

  void on_abi_removal(abieos::name conrtract) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(conrtract, *output);
    _js_forks_chan.publish(output);
  }

  void on_abi_error(std::shared_ptr<chronicle::channels::abi_error> abierr) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(*abierr, *output);
    _js_forks_chan.publish(output);
  }
  
  void on_table_row_update(std::shared_ptr<chronicle::channels::table_row_update> trupd) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(*trupd, *output);
    _js_forks_chan.publish(output);
  }
};



decoder_plugin::decoder_plugin() :my(new decoder_plugin_impl){
}

decoder_plugin::~decoder_plugin(){
}


void decoder_plugin::set_program_options( options_description& cli, options_description& cfg ) {
}

  
void decoder_plugin::plugin_initialize( const variables_map& options ) {
  try {
    std::cerr << "initialized decoder_plugin\n";
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


void decoder_plugin::plugin_startup(){
  my->start();
  std::cerr << "started decoder_plugin\n";
}

void decoder_plugin::plugin_shutdown() {
  std::cerr << "decoder_plugin stopped\n";
}



