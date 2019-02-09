// copyright defined in LICENSE.txt

#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"

#include <iostream>
#include <string>
#include <fc/io/json.hpp>


namespace json_encoder {
  inline constexpr bool trace_native_to_json = false;
  
  struct native_to_json_state : json_reader_handler<native_to_json_state> {
    rapidjson::Writer<rapidjson::StringBuffer>& writer;
    
    native_to_json_state(rapidjson::Writer<rapidjson::StringBuffer>& writer)
      : writer{writer} {}
  };

  struct json_serializer {
    virtual void native_to_json(native_to_json_state& state) const = 0;
  };
  
  template <typename T>
  struct native_serializer_impl : native_serializer {
    void native_to_json(native_to_json_state& state) const override {
      using ::json_encoder::native_to_json;
      return native_to_json(*reinterpret_cast<T*>(v), state);
    }
  }
  
  struct json_field_serializer_methods {
    virtual void native_to_json(native_to_json_state& state) const = 0;
  };
  
  struct json_field_serializer {
    const json_field_serializer_methods* methods = nullptr;
  };

  template <typename T>
  inline constexpr auto json_serializer_for = json_serializer_impl<T>{};

  template <typename member_ptr>
  constexpr auto create_json_field_serializer_methods_impl() {
    struct impl : json_field_serializer_methods {
      void void native_to_json(native_to_json_state& state) const override {
        using ::json_encoder::native_to_json;
        return native_to_json(member_from_void(member_ptr{}, v), state);
      }
    };
    return impl{};
  }
  
  template <typename member_ptr>
  inline constexpr auto field_serializer_methods_for = create_json_field_serializer_methods_impl<member_ptr>();
  
  template <typename T>
  constexpr auto create_json_field_serializers() {
    constexpr auto num_fields = ([&]() constexpr {
        int num_fields = 0;
        for_each_field((T*)nullptr, [&](auto, auto) { ++num_fields; });
        return num_fields;
      }());
    std::array<json_field_serializer, num_fields> fields;
    int i = 0;
    for_each_field((T*)nullptr, [&](auto* name, auto member_ptr) {
        fields[i++] = {name, &field_serializer_methods_for<decltype(member_ptr)>};
      });
    return fields;
  }

  template <typename T>
  inline constexpr auto json_field_serializers_for = create_json_field_serializers<T>();
  
  template <typename T>
  void native_to_json(T*, native_to_json_state& state) -> std::enable_if_t<std::is_arithmetic_v<T>, bool>;

  inline void native_to_json(std::string* str, native_to_json_state& state) {
    state.writer.String(str.c_str(), str.size());
  }
  
  inline void native_to_json(bytes v, native_to_json_state& state) {
    state.writer.String(v.data.c_str(), v.data.size());
  }


  template <typename T>
  void native_to_json(T* v, std::string& dest) {
    rapidjson::StringBuffer buffer{};
    rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};
    native_to_json_state state{writer};
    native_to_json(v, native_to_json_state);
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
    
    _js_forks_chan.publish()
  }

  void on_block(std::shared_ptr<chronicle::channels::block> block_ptr) {
  }

  void on_transaction_trace(std::shared_ptr<chronicle::channels::transaction_trace> ccttr) {
  }

  void on_abi_update(std::shared_ptr<chronicle::channels::abi_update> abiupd) {
  }

  void on_abi_removal(abieos::name conrtract) {
  }

  void on_abi_error(std::shared_ptr<chronicle::channels::abi_error> abierr) {
  }
  
  void on_table_row_update(std::shared_ptr<chronicle::channels::table_row_update> trupd) {
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



