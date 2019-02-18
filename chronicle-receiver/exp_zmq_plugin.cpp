// copyright defined in LICENSE.txt

#include "exp_zmq_plugin.hpp"
#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"
#include "chronicle_msgtypes.h"
#include <zmq.hpp>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

static appbase::abstract_plugin& _exp_zmq_plugin = app().register_plugin<exp_zmq_plugin>();

namespace {
  const char* SENDER_BIND_OPT = "exp-zmq-bind";
  const char* SENDER_BIND_DEFAULT = "tcp://127.0.0.1:5557";
}

class exp_zmq_plugin_impl : std::enable_shared_from_this<exp_zmq_plugin_impl> {
public:

  chronicle::channels::js_forks::channel_type::handle               _js_forks_subscription;
  chronicle::channels::js_blocks::channel_type::handle              _js_blocks_subscription;
  chronicle::channels::js_transaction_traces::channel_type::handle  _js_transaction_traces_subscription;
  chronicle::channels::js_abi_updates::channel_type::handle         _js_abi_updates_subscription;
  chronicle::channels::js_abi_removals::channel_type::handle        _js_abi_removals_subscription;
  chronicle::channels::js_abi_errors::channel_type::handle          _js_abi_errors_subscription;
  chronicle::channels::js_table_row_updates::channel_type::handle   _js_table_row_updates_subscription;
  chronicle::channels::js_abi_decoder_errors::channel_type::handle  _js_abi_decoder_errors_subscription;


  zmq::context_t context;
  zmq::socket_t sender_socket;
  string socket_bind_str;
  
  exp_zmq_plugin_impl():
    context(1),
    sender_socket(context, ZMQ_PUSH)
  {}

  void init() {
    _js_forks_subscription =
      app().get_channel<chronicle::channels::js_forks>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_FORK, 0, event); });
    
    _js_blocks_subscription =
        app().get_channel<chronicle::channels::js_blocks>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_BLOCK, 0, event); });
    
    _js_transaction_traces_subscription =
      app().get_channel<chronicle::channels::js_transaction_traces>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_TX_TRACE, 0, event); });

    _js_abi_updates_subscription =
      app().get_channel<chronicle::channels::js_abi_updates>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_ABI_UPD, 0, event); });
    
    _js_abi_removals_subscription =
      app().get_channel<chronicle::channels::js_abi_removals>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_ABI_REM, 0, event); });

    _js_abi_errors_subscription =
      app().get_channel<chronicle::channels::js_abi_errors>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_ABI_ERR, 0, event); });

    _js_table_row_updates_subscription =
      app().get_channel<chronicle::channels::js_table_row_updates>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_TBL_ROW, 0, event); });

    _js_abi_decoder_errors_subscription =
      app().get_channel<chronicle::channels::js_abi_decoder_errors>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_ENCODER_ERR, 0, event); });
}

  void start() {}
  
  void on_event(int32_t msgtype, int32_t msgopts, std::shared_ptr<string> event) {
    try {
      try {
        zmq::message_t message(event->length()+sizeof(msgtype)+sizeof(msgopts));
        unsigned char* ptr = (unsigned char*) message.data();
        memcpy(ptr, &msgtype, sizeof(msgtype));
        ptr += sizeof(msgtype);
        memcpy(ptr, &msgopts, sizeof(msgopts));
        ptr += sizeof(msgopts);
        memcpy(ptr, event->data(), event->length());
        sender_socket.send(message);
      }
      FC_LOG_AND_RETHROW();
    }
    catch (...) {
      abort_receiver();
    }
  }
};



exp_zmq_plugin::exp_zmq_plugin() :my(new exp_zmq_plugin_impl){
}

exp_zmq_plugin::~exp_zmq_plugin(){
}


void exp_zmq_plugin::set_program_options( options_description& cli, options_description& cfg ) {
  cfg.add_options()
    (SENDER_BIND_OPT, bpo::value<string>()->default_value(SENDER_BIND_DEFAULT),
     "ZMQ Sender Socket binding");
}

  
void exp_zmq_plugin::plugin_initialize( const variables_map& options ) {
  try {
    donot_start_receiver_before(this, "exp_zmq_plugin");

    my->socket_bind_str = options.at(SENDER_BIND_OPT).as<string>();
    if (my->socket_bind_str.empty()) {
      wlog("${o} not specified => exp_zmq_plugin disabled.", ("o",SENDER_BIND_OPT));
      return;
    }

    ilog("Binding to ZMQ PUSH socket ${u}", ("u", my->socket_bind_str));
    my->sender_socket.bind(my->socket_bind_str);
    
    my->init();
    ilog("Initialized exp_zmq_plugin");
  }
  FC_LOG_AND_RETHROW();
}


void exp_zmq_plugin::plugin_startup(){
  my->start();
  ilog("Started exp_zmq_plugin");
}

void exp_zmq_plugin::plugin_shutdown() {
  if( ! my->socket_bind_str.empty() ) {
    my->sender_socket.disconnect(my->socket_bind_str);
    my->sender_socket.close();
  }
  ilog("exp_zmq_plugin stopped");
}



