// copyright defined in LICENSE.txt

#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"

#include <iostream>
#include <string>
#include <fc/io/json.hpp>
#include <sstream>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

using namespace chronicle::channels;
using namespace abieos;
using namespace chain_state;

using std::make_shared;



namespace json_encoder {
  inline constexpr bool trace_native_to_json = false;
  
  struct native_to_json_state {
    rapidjson::Writer<rapidjson::StringBuffer>& writer;
    vector<string>* encoder_errors;    
  };

  inline void native_to_json(const std::string& str, native_to_json_state& state) {
    state.writer.String(str.c_str(), str.size());
  }

  inline void native_to_json(const optional<std::string>& str, native_to_json_state& state) {
    if( str ) {
      state.writer.String(str.value().data(), str.value().size());
    }
    else {
      state.writer.String("");
    }
  }
  
  inline void arithmetic_to_json(const uint64_t& v, native_to_json_state& state) {
    string str = to_string(v);
    state.writer.String(str.data(), str.length());
  }

  inline void arithmetic_to_json(const int64_t& v, native_to_json_state& state) {
    string str = to_string(v);
    state.writer.String(str.data(), str.length());
  }

  inline void arithmetic_to_json(const uint32_t& v, native_to_json_state& state) {
    string str = to_string(v);
    state.writer.String(str.data(), str.length());
  }

  inline void arithmetic_to_json(const int32_t& v, native_to_json_state& state) {
    string str = to_string(v);
    state.writer.String(str.data(), str.length());
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
    
  inline void native_to_json(const name& obj, native_to_json_state& state) {
    std::string result = name_to_string(obj.value);
    state.writer.String(result.c_str(), result.size());
  }
  
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
    const char* str = obj ? "true" : "false";
    state.writer.String(str);
  }

  inline void native_to_json(const varuint32& obj, native_to_json_state& state) {
    arithmetic_to_json(obj.value, state);
  }
  
  inline void native_to_json(const chain_state::action_trace& obj, native_to_json_state& state) {
    state.writer.StartObject();
    for_each_field((chain_state::action_trace*)nullptr, [&](auto* name, auto member_ptr) {
        if( string("dummy") == name || string("receipt_dummy") == name ) {
          return;
        }
        state.writer.Key(name);
        if( string("data") == name ) {
          // encode action data according to ABI
          auto ctxt = get_contract_abi_ctxt(obj.account);
          try {
            const char* action_type = abieos_get_type_for_action(ctxt, obj.account.value, obj.name.value);
            if( action_type == nullptr )
              action_type = abieos_name_to_string(ctxt, obj.name.value);
            try {
              const char* datajs = abieos_bin_to_json(ctxt, obj.account.value, action_type,
                                                      obj.data.data.data(), obj.data.data.size());
              if( datajs == nullptr )
                throw error("abieos_bin_to_json returned null");
              state.writer.RawValue(datajs, strlen(datajs), rapidjson::kObjectType);
            }
            catch (...) {
              throw error(abieos_get_error(ctxt));
            }
          }
          catch ( const std::exception& e  ) {
            if( state.encoder_errors ) {
              std::ostringstream os;
              os << "Cannot decode action data for " << abieos_name_to_string(ctxt, obj.account.value) <<": "
                 << abieos_name_to_string(ctxt, obj.name.value)
                 << " - " << e.what();
              state.encoder_errors->emplace_back(os.str());
            }
            native_to_json(obj.data, state);
          }
        }
        else {
          native_to_json(member_from_void(member_ptr, &obj), state);
        }
      });
    state.writer.EndObject();
  }
  
  void native_to_json(const chain_state::recurse_action_trace& obj, native_to_json_state& state) {
    const chain_state::action_trace& o = obj; native_to_json(o, state);
  }

  
  inline void native_to_json(const chain_state::transaction_trace& obj, native_to_json_state& state) {
    state.writer.StartObject();
    for_each_field((chain_state::transaction_trace*)nullptr, [&](auto* name, auto member_ptr) {
        if( string("dummy") == name ) {
          return;
        }
        state.writer.Key(name);
        native_to_json(member_from_void(member_ptr, &obj), state);
      });
    state.writer.EndObject();
  }

  void native_to_json(const chain_state::recurse_transaction_trace& obj, native_to_json_state& state) {
    const chain_state::transaction_trace& o = obj; native_to_json(o, state);
  }
    

  inline void native_to_json(const chain_state::key_value_object& obj, native_to_json_state& state) {
    state.writer.StartObject();
    for_each_field((chain_state::key_value_object*)nullptr, [&](auto* name, auto member_ptr) {
        state.writer.Key(name);
        if( string("value") == name ) {
          // encode table row according to ABI
          auto ctxt = get_contract_abi_ctxt(obj.code);
          try {
            const char* table_type = abieos_get_type_for_table(ctxt, obj.code.value, obj.table.value);
            if( table_type == nullptr )
              table_type = abieos_name_to_string(ctxt, obj.table.value);
            try {
              const char* valjs = abieos_bin_to_json(ctxt, obj.code.value, table_type,
                                                     obj.value.data.data(), obj.value.data.size());
              if( valjs == nullptr )
                throw error("abieos_bin_to_json returned null");
              state.writer.RawValue(valjs, strlen(valjs), rapidjson::kObjectType);
            }
            catch (...) {
              throw error(abieos_get_error(ctxt));
            }
          }
          catch ( const std::exception& e ) {
            if( state.encoder_errors ) {
              std::ostringstream os;
              os << "Cannot decode table row for " << abieos_name_to_string(ctxt, obj.code.value)
                 <<": " << abieos_name_to_string(ctxt, obj.table.value)
                 << " - " << e.what();
              state.encoder_errors->emplace_back(os.str());
            }
            native_to_json(obj.value, state);
          }
        }
        else {
          native_to_json(member_from_void(member_ptr, &obj), state);
        }
      });
    state.writer.EndObject();

  }

  template <typename T>
  inline void native_to_json(const abieos::might_not_exist<T>& obj, native_to_json_state& state) {
    native_to_json(obj.value, state);
  }

  template <typename T>
  inline void native_to_json(const std::optional<T>& obj, native_to_json_state& state) {
    if( obj ) {
      native_to_json(obj.value(), state);
    }
    else {
      state.writer.Null();
    }
  }
  
  template <typename T1, typename T2>
  inline void native_to_json(const std::pair<T1,T2>& obj, native_to_json_state& state) {
    state.writer.StartArray();
    native_to_json(obj.first, state);
    native_to_json(obj.second, state);
    state.writer.EndArray();
  }

  inline void native_to_json(const abieos::block_timestamp& obj, native_to_json_state& state) {
    string str = string(obj);
    state.writer.String(str.data(), str.length());
  }

  inline void native_to_json(const abieos::public_key& obj, native_to_json_state& state) {
    string str = public_key_to_string(obj);
    state.writer.String(str.data(), str.length());
  }

  inline void native_to_json(const abieos::signature& obj, native_to_json_state& state) {
    string str = signature_to_string(obj);
    state.writer.String(str.data(), str.length());
  }

  // this is only to break compiler's recursion
  inline void native_to_json(const chain_state::packed_transaction& obj, native_to_json_state& state) {
    state.writer.StartObject();
    for_each_field((chain_state::packed_transaction*)nullptr, [&](auto* name, auto member_ptr) {
        state.writer.Key(name);
        native_to_json(member_from_void(member_ptr, &obj), state);
      });
    state.writer.EndObject();
  }

  inline void native_to_json(const chain_state::transaction_variant& obj, native_to_json_state& state) {
    if( obj.index() == 0 ) {
      const checksum256& v = std::get<checksum256>(obj);
      native_to_json(v, state);
    }
    else {
      const packed_transaction& v = std::get<packed_transaction>(obj);
      native_to_json(v, state);
    }
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
  void native_to_json(T& v, std::string& dest, vector<string>* encoder_errors=nullptr) {
    rapidjson::StringBuffer buffer{};
    rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};
    native_to_json_state state{writer, encoder_errors};
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
    _js_table_row_updates_chan(app().get_channel<chronicle::channels::js_table_row_updates>()),
    _js_abi_decoder_errors_chan(app().get_channel<chronicle::channels::js_abi_decoder_errors>())
  {}
  
  chronicle::channels::js_forks::channel_type&               _js_forks_chan;
  chronicle::channels::js_blocks::channel_type&              _js_blocks_chan;
  chronicle::channels::js_transaction_traces::channel_type&  _js_transaction_traces_chan;
  chronicle::channels::js_abi_updates::channel_type&         _js_abi_updates_chan;
  chronicle::channels::js_abi_removals::channel_type&        _js_abi_removals_chan;
  chronicle::channels::js_abi_errors::channel_type&          _js_abi_errors_chan;
  chronicle::channels::js_table_row_updates::channel_type&   _js_table_row_updates_chan;
  chronicle::channels::js_abi_decoder_errors::channel_type&  _js_abi_decoder_errors_chan;

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
    _js_blocks_chan.publish(output);
  }

  void on_transaction_trace(std::shared_ptr<chronicle::channels::transaction_trace> ccttr) {
    vector<string> encoder_errors;
    auto output = make_shared<string>();
    json_encoder::native_to_json(*ccttr, *output, &encoder_errors);
    _js_transaction_traces_chan.publish(output);
    if( encoder_errors.size() > 0 ) {
      map<string, string> attrs;
      attrs["where"] = "transaction_trace";
      attrs["block_num"] = std::to_string(ccttr->block_num);
      attrs["block_timestamp"] = string(ccttr->block_timestamp);
      attrs["trx_id"] = string(ccttr->trace.id);
      report_encoder_errors(encoder_errors, attrs);
    }
  }

  void on_abi_update(std::shared_ptr<chronicle::channels::abi_update> abiupd) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(*abiupd, *output);
    _js_abi_updates_chan.publish(output);
  }

  void on_abi_removal(abieos::name conrtract) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(conrtract, *output);
    _js_abi_removals_chan.publish(output);
  }

  void on_abi_error(std::shared_ptr<chronicle::channels::abi_error> abierr) {
    auto output = make_shared<string>();
    json_encoder::native_to_json(*abierr, *output);
    _js_abi_errors_chan.publish(output);
  }
  
  void on_table_row_update(std::shared_ptr<chronicle::channels::table_row_update> trupd) {
    vector<string> encoder_errors;
    auto output = make_shared<string>();
    json_encoder::native_to_json(*trupd, *output, &encoder_errors);
    _js_table_row_updates_chan.publish(output);
    if( encoder_errors.size() > 0 ) {
      map<string, string> attrs;
      attrs["where"] = "table_row_update";
      attrs["block_num"] = std::to_string(trupd->block_num);
      attrs["block_timestamp"] = string(trupd->block_timestamp);
      attrs["added"] = trupd->added ? "true":"false";
      attrs["code"] = string(trupd->kvo.code);
      attrs["scope"] = string(trupd->kvo.scope);
      attrs["table"] = string(trupd->kvo.table);
      attrs["primary_key"] = trupd->kvo.primary_key;
      report_encoder_errors(encoder_errors, attrs);
    }
  }

  inline void report_encoder_errors(vector<string>& encoder_errors, map<string, string>& attrs) {
    rapidjson::StringBuffer buffer{};
    rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};
    writer.StartObject();
    for (auto const& p : attrs) {
      writer.Key(p.first.c_str());
      writer.String(p.second.c_str());
    }
    writer.Key("errors");
    writer.StartArray();
    for (auto& err : encoder_errors)
      writer.String(err.c_str());
    writer.EndArray();
    writer.EndObject();
    auto output = make_shared<string>(buffer.GetString());
    _js_abi_decoder_errors_chan.publish(output);
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
    donot_start_receiver_before(this, "decoder_plugin");
    ilog("Initialized decoder_plugin");
  }
  FC_LOG_AND_RETHROW();
}


void decoder_plugin::plugin_startup(){
  my->start();
  ilog("Started decoder_plugin");
}

void decoder_plugin::plugin_shutdown() {
  ilog("decoder_plugin stopped");
}



