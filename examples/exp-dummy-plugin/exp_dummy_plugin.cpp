// demo third-party plugin 

#include "exp_dummy_plugin.hpp"
#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"

#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

static appbase::abstract_plugin& _exp_dummy_plugin = app().register_plugin<exp_dummy_plugin>();

namespace {
  const char* DUMMY_FOO_OPT = "exp-dummy-foo";
}

class exp_dummy_plugin_impl : std::enable_shared_from_this<exp_dummy_plugin_impl> {
public:
  chronicle::channels::js_forks::channel_type::handle               _js_forks_subscription;
  chronicle::channels::js_blocks::channel_type::handle              _js_blocks_subscription;
  chronicle::channels::js_transaction_traces::channel_type::handle  _js_transaction_traces_subscription;
  chronicle::channels::js_abi_updates::channel_type::handle         _js_abi_updates_subscription;
  chronicle::channels::js_abi_removals::channel_type::handle        _js_abi_removals_subscription;
  chronicle::channels::js_abi_errors::channel_type::handle          _js_abi_errors_subscription;
  chronicle::channels::js_table_row_updates::channel_type::handle   _js_table_row_updates_subscription;
  chronicle::channels::js_receiver_pauses::channel_type::handle     _js_receiver_pauses_subscription;
  chronicle::channels::js_abi_decoder_errors::channel_type::handle  _js_abi_decoder_errors_subscription;

  exp_dummy_plugin_impl()
  {};

  void init() {
    _js_forks_subscription =
      app().get_channel<chronicle::channels::js_forks>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("FORK", event); });
    
    _js_blocks_subscription =
        app().get_channel<chronicle::channels::js_blocks>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("BLOCK", event); });
    
    _js_transaction_traces_subscription =
      app().get_channel<chronicle::channels::js_transaction_traces>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("TX_TRACE", event); });

    _js_abi_updates_subscription =
      app().get_channel<chronicle::channels::js_abi_updates>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("ABI_UPD", event); });
    
    _js_abi_removals_subscription =
      app().get_channel<chronicle::channels::js_abi_removals>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("ABI_REM", event); });

    _js_abi_errors_subscription =
      app().get_channel<chronicle::channels::js_abi_errors>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("ABI_ERR", event); });

    _js_table_row_updates_subscription =
      app().get_channel<chronicle::channels::js_table_row_updates>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("TBL_ROW", event); });

    _js_receiver_pauses_subscription =
      app().get_channel<chronicle::channels::js_receiver_pauses>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("RCVR_PAUSE", event); });
    
    _js_abi_decoder_errors_subscription =
      app().get_channel<chronicle::channels::js_abi_decoder_errors>().subscribe
      ([this](std::shared_ptr<string> event){ on_event("ENCODER_ERR", event); });
  }


  void start() {
  }


  void stop() {
  }
  
  void on_event(const char* msgtype, std::shared_ptr<string> event) {
    ilog("Event: ${i}", ("i", msgtype));
  }
};



exp_dummy_plugin::exp_dummy_plugin() :my(new exp_dummy_plugin_impl){
}

exp_dummy_plugin::~exp_dummy_plugin(){
}


void exp_dummy_plugin::set_program_options( options_description& cli, options_description& cfg ) {
  cfg.add_options()
    (DUMMY_FOO_OPT, bpo::value<string>(), "Dumy option that does notning")
    ;
}

  
void exp_dummy_plugin::plugin_initialize( const variables_map& options ) {
  try {
    donot_start_receiver_before(this, "exp_dummy_plugin");

    if( options.count(DUMMY_FOO_OPT) > 0 ) {
      ilog("foo is ${f}", ("f",options.at(DUMMY_FOO_OPT).as<string>()));
    }
    my->init();    
    ilog("Initialized exp_dummy_plugin");
    exporter_initialized();
  }
  FC_LOG_AND_RETHROW();
}


void exp_dummy_plugin::plugin_startup(){
  my->start();
  ilog("Started exp_dummy_plugin");
}

void exp_dummy_plugin::plugin_shutdown() {
  my->stop();
  ilog("exp_dummy_plugin stopped");
}



