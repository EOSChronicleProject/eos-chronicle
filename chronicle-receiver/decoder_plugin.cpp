// copyright defined in LICENSE.txt

#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"

#include <iostream>
#include <string>
#include <sstream>

#include "rapidjson/reader.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <fc/crypto/hex.hpp>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

using namespace chronicle::channels;
using namespace abieos;

using std::make_shared;
using std::string;



namespace json_encoder {
  inline constexpr bool trace_native_to_json = false;

  struct native_to_json_state {
    rapidjson::Writer<rapidjson::StringBuffer>& writer;
    vector<string>* encoder_errors;
  };

  inline void native_to_json(const std::string& str, native_to_json_state& state) {
    state.writer.String(str.data(), str.size());
  }

  inline void native_to_json(const std::optional<std::string>& str, native_to_json_state& state) {
    if( str ) {
      state.writer.String(str.value().data(), str.value().size());
    }
    else {
      state.writer.String("");
    }
  }

  inline void arithmetic_to_json(const uint64_t& v, native_to_json_state& state) {
    string str = std::to_string(v);
    state.writer.String(str.data(), str.length());
  }

  inline void arithmetic_to_json(const int64_t& v, native_to_json_state& state) {
    string str = std::to_string(v);
    state.writer.String(str.data(), str.length());
  }

  inline void arithmetic_to_json(const uint32_t& v, native_to_json_state& state) {
    string str = std::to_string(v);
    state.writer.String(str.data(), str.length());
  }

  inline void arithmetic_to_json(const int32_t& v, native_to_json_state& state) {
    string str = std::to_string(v);
    state.writer.String(str.data(), str.length());
  }

  inline void native_to_json(const chronicle::channels::fork_reason_val& obj, native_to_json_state& state) {
    std::string result = to_string(obj);
    state.writer.String(result.data(), result.size());
  }

  inline void native_to_json(const checksum256& obj, native_to_json_state& state) {
    const auto bytes = obj.extract_as_byte_array();
    std::string result = fc::to_hex((const char*)bytes.data(), bytes.size());
    state.writer.String(result.data(), result.size());
  }

  template <typename T>
  void native_to_json(const std::vector<T>& obj, native_to_json_state& state) {
    state.writer.StartArray();
    for (auto& v : obj)
      native_to_json(v, state);
    state.writer.EndArray();
  }

  inline void native_to_json(const eosio::ship_protocol::transaction_status& obj, native_to_json_state& state) {
    std::string result = to_string(obj);
    state.writer.String(result.data(), result.size());
  }

  inline void native_to_json(const eosio::name& obj, native_to_json_state& state) {
    std::string result = eosio::name_to_string(obj.value);
    state.writer.String(result.data(), result.size());
  }

  template <typename T>
  inline void native_to_json(const eosio::might_not_exist<T>& obj, native_to_json_state& state) {
    native_to_json(obj.value, state);
  }

  inline void native_to_json(const eosio::input_stream& obj, native_to_json_state& state) {
    std::string result = fc::to_hex(obj.get_pos(), obj.remaining());
    state.writer.String(result.data(), result.size());
  }
  
  inline void native_to_json(const bool& obj, native_to_json_state& state) {
    const char* str = obj ? "true" : "false";
    state.writer.String(str);
  }

  inline void native_to_json(const varuint32& obj, native_to_json_state& state) {
    arithmetic_to_json(obj.value, state);
  }

  inline void native_to_json(const eosio::ship_protocol::action& obj, native_to_json_state& state) {
    state.writer.StartObject();
    eosio::for_each_field<eosio::ship_protocol::action>([&](const char* name, auto&& member) {
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
                                                      obj.data.get_pos(), obj.data.remaining());
              if( datajs == nullptr )
                throw std::runtime_error("abieos_bin_to_json returned null");
              state.writer.RawValue(datajs, strlen(datajs), rapidjson::kObjectType);
            }
            catch (...) {
              throw std::runtime_error(abieos_get_error(ctxt));
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
          native_to_json(member(&obj), state);
        }
      });
    state.writer.EndObject();
  }




  inline void native_to_json(const eosio::ship_protocol::contract_row_v0& obj, native_to_json_state& state) {
    state.writer.StartObject();
    eosio::for_each_field<eosio::ship_protocol::contract_row_v0>([&](const char* name, auto&& member) {
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
                                                     obj.value.get_pos(), obj.value.remaining());
              if( valjs == nullptr )
                throw std::runtime_error("abieos_bin_to_json returned null");
              state.writer.RawValue(valjs, strlen(valjs), rapidjson::kObjectType);
            }
            catch (...) {
              throw std::runtime_error(abieos_get_error(ctxt));
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
          native_to_json(member(&obj), state);
        }
      });
    state.writer.EndObject();

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

  inline void native_to_json(const eosio::block_timestamp& obj, native_to_json_state& state) {
    string str = eosio::microseconds_to_str(obj.to_time_point().elapsed.count());
    state.writer.String(str.data(), str.length());
  }

  inline void native_to_json(const eosio::time_point& obj, native_to_json_state& state) {
    string str = eosio::microseconds_to_str(obj.elapsed.count());
    state.writer.String(str.data(), str.length());
  }

  inline void native_to_json(const eosio::public_key& obj, native_to_json_state& state) {
    string str = eosio::public_key_to_string(obj);
    state.writer.String(str.data(), str.length());
  }

  inline void native_to_json(const eosio::signature& obj, native_to_json_state& state) {
    string str = eosio::signature_to_string(obj);
    state.writer.String(str.data(), str.length());
  }

  template <typename T>
  void native_to_json(const T& obj, native_to_json_state& state) {
    if constexpr (std::is_class_v<T>) {
        state.writer.StartObject();
        eosio::for_each_field<T>([&](const char* name, auto&& member) {
            state.writer.Key(name);
            native_to_json(member(&obj), state);
          });
        state.writer.EndObject();
      }
    else {
      static_assert(std::is_arithmetic_v<T>);
      arithmetic_to_json(obj, state);
    }
  }

  template
  void native_to_json<eosio::ship_protocol::action_receipt_v0>(const eosio::ship_protocol::action_receipt_v0&,
                                                               native_to_json_state&);
  
  template
  void native_to_json<eosio::ship_protocol::action_trace_v0>(const eosio::ship_protocol::action_trace_v0&,
                                                             native_to_json_state&);

  template
  void native_to_json<eosio::ship_protocol::partial_transaction_v0>(const eosio::ship_protocol::partial_transaction_v0&,
                                                                    native_to_json_state&);

  template
  void native_to_json<eosio::ship_protocol::transaction_trace_v0>(const eosio::ship_protocol::transaction_trace_v0&,
                                                                  native_to_json_state&);

  template
  void native_to_json<eosio::ship_protocol::packed_transaction>(const eosio::ship_protocol::packed_transaction&,
                                                                native_to_json_state&);
  
  inline void native_to_json(const eosio::ship_protocol::action_receipt& obj, native_to_json_state& state) {
    native_to_json(std::get<eosio::ship_protocol::action_receipt_v0>(obj), state);
  }
  
  inline void native_to_json(const eosio::ship_protocol::action_trace& obj, native_to_json_state& state) {
    native_to_json(std::get<eosio::ship_protocol::action_trace_v0>(obj), state);
  }
  
  inline void native_to_json(const eosio::ship_protocol::partial_transaction& obj, native_to_json_state& state) {
    native_to_json(std::get<eosio::ship_protocol::partial_transaction_v0>(obj), state);
  }
  
  inline void native_to_json(const eosio::ship_protocol::transaction_trace& obj, native_to_json_state& state) {
    native_to_json(std::get<eosio::ship_protocol::transaction_trace_v0>(obj), state);
  }
  
  inline void native_to_json(const eosio::ship_protocol::recurse_transaction_trace& obj, native_to_json_state& state) {
    native_to_json(obj.recurse, state);
  }
  

  inline void native_to_json(const eosio::ship_protocol::transaction_variant& obj, native_to_json_state& state) {
    if( obj.index() == 0 ) {
      const checksum256& v = std::get<checksum256>(obj);
      native_to_json(v, state);
    }
    else {
      const eosio::ship_protocol::packed_transaction& v = std::get<eosio::ship_protocol::packed_transaction>(obj);
      native_to_json(v, state);
    }
  }


  // ABI decoder uses this to produce an independent piece of JSON
  template <typename T>
  void native_to_json(T& v, std::string& dest, vector<string>* encoder_errors=nullptr) {
    rapidjson::StringBuffer buffer(0, 65536);
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
    _js_permission_updates_chan(app().get_channel<chronicle::channels::js_permission_updates>()),
    _js_permission_link_updates_chan(app().get_channel<chronicle::channels::js_permission_link_updates>()),
    _js_account_metadata_updates_chan(app().get_channel<chronicle::channels::js_account_metadata_updates>()),
    _js_receiver_pauses_chan(app().get_channel<chronicle::channels::js_receiver_pauses>()),
    _js_block_completed_chan(app().get_channel<chronicle::channels::js_block_completed>()),
    _js_abi_decoder_errors_chan(app().get_channel<chronicle::channels::js_abi_decoder_errors>()),
    impl_buffer(0, 262144)
  {}

  chronicle::channels::js_forks::channel_type&               _js_forks_chan;
  chronicle::channels::js_blocks::channel_type&              _js_blocks_chan;
  chronicle::channels::js_transaction_traces::channel_type&  _js_transaction_traces_chan;
  chronicle::channels::js_abi_updates::channel_type&         _js_abi_updates_chan;
  chronicle::channels::js_abi_removals::channel_type&        _js_abi_removals_chan;
  chronicle::channels::js_abi_errors::channel_type&          _js_abi_errors_chan;
  chronicle::channels::js_table_row_updates::channel_type&   _js_table_row_updates_chan;
  chronicle::channels::js_permission_updates::channel_type&  _js_permission_updates_chan;
  chronicle::channels::js_permission_link_updates::channel_type&  _js_permission_link_updates_chan;
  chronicle::channels::js_account_metadata_updates::channel_type&  _js_account_metadata_updates_chan;
  chronicle::channels::js_receiver_pauses::channel_type&     _js_receiver_pauses_chan;
  chronicle::channels::js_block_completed::channel_type&     _js_block_completed_chan;
  chronicle::channels::js_abi_decoder_errors::channel_type&  _js_abi_decoder_errors_chan;

  chronicle::channels::forks::channel_type::handle               _forks_subscription;
  chronicle::channels::blocks::channel_type::handle              _blocks_subscription;
  chronicle::channels::transaction_traces::channel_type::handle  _transaction_traces_subscription;
  chronicle::channels::abi_errors::channel_type::handle          _abi_errors_subscription;
  chronicle::channels::abi_removals::channel_type::handle        _abi_removals_subscription;
  chronicle::channels::abi_updates::channel_type::handle         _abi_updates_subscription;
  chronicle::channels::table_row_updates::channel_type::handle   _table_row_updates_subscription;
  chronicle::channels::permission_updates::channel_type::handle  _permission_updates_subscription;
  chronicle::channels::permission_link_updates::channel_type::handle  _permission_link_updates_subscription;
  chronicle::channels::account_metadata_updates::channel_type::handle  _account_metadata_updates_subscription;
  chronicle::channels::receiver_pauses::channel_type::handle     _receiver_pauses_subscription;
  chronicle::channels::block_completed::channel_type::handle     _block_completed_subscription;

  const int channel_priority = 50;

  // A static copy of JSON writer buffer in order to avoid reallocation
  rapidjson::StringBuffer impl_buffer;
  rapidjson::Writer<rapidjson::StringBuffer> impl_writer;

  template <typename T>
  void impl_native_to_json(T& v, std::string& dest, vector<string>* encoder_errors=nullptr) {
    impl_buffer.Clear();
    impl_writer.Reset(impl_buffer);
    json_encoder::native_to_json_state state{impl_writer, encoder_errors};
    json_encoder::native_to_json(v, state);
    dest = impl_buffer.GetString();
  }


  // we only subscribe to receiver channels at startup, assuming our consumers have subscribed at init
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
        ([this](std::shared_ptr<chronicle::channels::abi_removal> ar){
          on_abi_removal(ar);
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
    if (_js_permission_updates_chan.has_subscribers()) {
      _permission_updates_subscription =
        app().get_channel<chronicle::channels::permission_updates>().subscribe
        ([this](std::shared_ptr<chronicle::channels::permission_update> trupd){
          on_permission_update(trupd);
        });
    }
    if (_js_permission_link_updates_chan.has_subscribers()) {
      _permission_link_updates_subscription =
        app().get_channel<chronicle::channels::permission_link_updates>().subscribe
        ([this](std::shared_ptr<chronicle::channels::permission_link_update> trupd){
          on_permission_link_update(trupd);
        });
    }
    if (_js_account_metadata_updates_chan.has_subscribers()) {
      _account_metadata_updates_subscription =
        app().get_channel<chronicle::channels::account_metadata_updates>().subscribe
        ([this](std::shared_ptr<chronicle::channels::account_metadata_update> trupd){
          on_account_metadata_update(trupd);
        });
    }
    if (_js_receiver_pauses_chan.has_subscribers()) {
      _receiver_pauses_subscription =
        app().get_channel<chronicle::channels::receiver_pauses>().subscribe
        ([this](std::shared_ptr<chronicle::channels::receiver_pause> rp){
          on_receiver_pause(rp);
        });
    }
    if (_js_block_completed_chan.has_subscribers()) {
      _block_completed_subscription =
        app().get_channel<chronicle::channels::block_completed>().subscribe
        ([this](std::shared_ptr<block_finished> bf){
          on_block_completed(bf);
        });
    }
  }

  void on_fork(std::shared_ptr<chronicle::channels::fork_event> fe) {
    auto output = make_shared<string>();
    impl_native_to_json(*fe, *output);
    _js_forks_chan.publish(channel_priority, output);
  }

  void on_block(std::shared_ptr<chronicle::channels::block> block_ptr) {
    auto output = make_shared<string>();
    impl_native_to_json(*block_ptr, *output);
    _js_blocks_chan.publish(channel_priority, output);
  }

  void on_transaction_trace(std::shared_ptr<chronicle::channels::transaction_trace> ccttr) {
    vector<string> encoder_errors;
    auto output = make_shared<string>();
    impl_native_to_json(*ccttr, *output, &encoder_errors);
    _js_transaction_traces_chan.publish(channel_priority, output);
    if( encoder_errors.size() > 0 ) {
      map<string, string> attrs;
      attrs["where"] = "transaction_trace";
      attrs["block_num"] = std::to_string(ccttr->block_num);
      attrs["block_timestamp"] = eosio::microseconds_to_str(ccttr->block_timestamp.to_time_point().elapsed.count());
      auto& trace = std::get<eosio::ship_protocol::transaction_trace_v0>(ccttr->trace);
      attrs["trx_id"] = fc::to_hex((const char*)trace.id.value.data(), trace.id.value.size());
      report_encoder_errors(encoder_errors, attrs);
    }
  }

  void on_abi_update(std::shared_ptr<chronicle::channels::abi_update> abiupd) {
    auto output = make_shared<string>();
    impl_native_to_json(*abiupd, *output);
    _js_abi_updates_chan.publish(channel_priority, output);
  }

  void on_abi_removal(std::shared_ptr<chronicle::channels::abi_removal> ar) {
    auto output = make_shared<string>();
    impl_native_to_json(*ar, *output);
    _js_abi_removals_chan.publish(channel_priority, output);
  }

  void on_abi_error(std::shared_ptr<chronicle::channels::abi_error> abierr) {
    auto output = make_shared<string>();
    impl_native_to_json(*abierr, *output);
    _js_abi_errors_chan.publish(channel_priority, output);
  }

  void on_table_row_update(std::shared_ptr<chronicle::channels::table_row_update> trupd) {
    vector<string> encoder_errors;
    auto output = make_shared<string>();
    impl_native_to_json(*trupd, *output, &encoder_errors);
    _js_table_row_updates_chan.publish(channel_priority, output);
    if( encoder_errors.size() > 0 ) {
      map<string, string> attrs;
      attrs["where"] = "table_row_update";
      attrs["block_num"] = std::to_string(trupd->block_num);
      attrs["block_timestamp"] = eosio::microseconds_to_str(trupd->block_timestamp.to_time_point().elapsed.count());
      attrs["added"] = trupd->added ? "true":"false";
      attrs["code"] = string(trupd->kvo.code);
      attrs["scope"] = string(trupd->kvo.scope);
      attrs["table"] = string(trupd->kvo.table);
      attrs["primary_key"] = trupd->kvo.primary_key;
      report_encoder_errors(encoder_errors, attrs);
    }
  }

  void on_permission_update(std::shared_ptr<chronicle::channels::permission_update> pupd) {
    auto output = make_shared<string>();
    impl_native_to_json(*pupd, *output);
    _js_permission_updates_chan.publish(channel_priority, output);
  }

  void on_permission_link_update(std::shared_ptr<chronicle::channels::permission_link_update> plupd) {
    auto output = make_shared<string>();
    impl_native_to_json(*plupd, *output);
    _js_permission_link_updates_chan.publish(channel_priority, output);
  }

  void on_account_metadata_update(std::shared_ptr<chronicle::channels::account_metadata_update> plupd) {
    auto output = make_shared<string>();
    impl_native_to_json(*plupd, *output);
    _js_account_metadata_updates_chan.publish(channel_priority, output);
  }

  void on_receiver_pause(std::shared_ptr<chronicle::channels::receiver_pause> rp) {
    auto output = make_shared<string>();
    impl_native_to_json(*rp, *output);
    _js_receiver_pauses_chan.publish(channel_priority, output);
  }


  void on_block_completed(std::shared_ptr<block_finished> bf) {
    auto output = make_shared<string>();
    impl_native_to_json(*bf, *output);
    _js_block_completed_chan.publish(channel_priority, output);
  }


  inline void report_encoder_errors(vector<string>& encoder_errors, map<string, string>& attrs) {
    rapidjson::StringBuffer buffer(0, 1024);
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
    _js_abi_decoder_errors_chan.publish(channel_priority, output);
  }

};



decoder_plugin::decoder_plugin() :my(new decoder_plugin_impl){
}

decoder_plugin::~decoder_plugin(){
}


void decoder_plugin::set_program_options( options_description& cli, options_description& cfg ) {
}


void decoder_plugin::plugin_initialize( const variables_map& options ) {
  if (is_noexport_opt(options))
    return;
  try {
    donot_start_receiver_before(this, "decoder_plugin");
    ilog("Initialized decoder_plugin");
  }
  FC_LOG_AND_RETHROW();
}


void decoder_plugin::plugin_startup(){
  if (!is_noexport_mode()) {
    my->start();
    ilog("Started decoder_plugin");
  }
}

void decoder_plugin::plugin_shutdown() {
  if (!is_noexport_mode()) {
    ilog("decoder_plugin stopped");
  }
}
