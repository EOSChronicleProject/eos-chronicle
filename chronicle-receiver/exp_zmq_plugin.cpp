// copyright defined in LICENSE.txt

#include "exp_zmq_plugin.hpp"
#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"
#include "chronicle_msgtypes.h"
#include <azmq/socket.hpp>
#include <boost/beast/core.hpp>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>


using boost::beast::flat_buffer;
using boost::system::error_code;

static appbase::abstract_plugin& _exp_zmq_plugin = app().register_plugin<exp_zmq_plugin>();

namespace {
  const char* SENDER_BIND_OPT = "exp-zmq-sender";
  const char* SENDER_BIND_DEFAULT = "tcp://127.0.0.1:5557";

  const char* RECEICER_CN_OPT = "exp-zmq-receiver";
  const char* RECEICER_CN_DEFAULT = "tcp://127.0.0.1:5558";

  const char* ZMQ_MAXUNACK_OPT = "exp-zmq-max-unack";
  const uint32_t ZMQ_MAXUNACK_DEFAULT = 1000;
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
  chronicle::channels::js_receiver_pauses::channel_type::handle     _js_receiver_pauses_subscription;


  const int zmq_priority = 60;
  
  std::shared_ptr<azmq::push_socket> sender_socket;
  string sender_bind_str;

  std::shared_ptr<azmq::sub_socket> receiver_socket;
  string receiver_connect_str;
  std::array<char, 256> rcvbuf;
  
  exp_zmq_plugin_impl()
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

    _js_receiver_pauses_subscription =
      app().get_channel<chronicle::channels::js_receiver_pauses>().subscribe
      ([this](std::shared_ptr<string> event){ on_event(CHRONICLE_MSGTYPE_RCVR_PAUSE, 0, event); });
  }

  void start() {
    sender_socket = std::make_shared<azmq::push_socket>(std::ref(app().get_io_service()));
    receiver_socket = std::make_shared<azmq::sub_socket>(std::ref(app().get_io_service()));
    
    ilog("Binding to ZMQ PUSH socket ${u}", ("u", sender_bind_str));
    sender_socket->bind(sender_bind_str);
    ilog("Binding to ZMQ SUB socket ${u}", ("u", receiver_connect_str));
    receiver_socket->connect(receiver_connect_str);
    receiver_socket->set_option(azmq::socket::subscribe(""));
    async_read_acks();
  }
  

  void on_event(int32_t msgtype, int32_t msgopts, std::shared_ptr<string> event) {
    try {
      try {
        auto buf = std::make_shared<std::vector<unsigned char>>(event->length()+sizeof(msgtype)+sizeof(msgopts));
        unsigned char *ptr = buf->data();
        memcpy(ptr, &msgtype, sizeof(msgtype));
        ptr += sizeof(msgtype);
        memcpy(ptr, &msgopts, sizeof(msgopts));
        ptr += sizeof(msgopts);
        memcpy(ptr, event->data(), event->length());
        sender_socket->async_send
          (boost::asio::buffer(buf->data(), buf->size()),
           app().get_priority_queue().wrap(zmq_priority, [this](error_code ec, size_t) {
             if (ec) {
               elog("ERROR writing to ZMQ socket: ${e}", ("e",ec.message()));
               abort_receiver();
             }}));
      }
      FC_LOG_AND_RETHROW();
    }
    catch (...) {
      abort_receiver();
    }
  }


  void async_read_acks() {
    try {
      try {
        receiver_socket->async_receive
          (boost::asio::buffer(rcvbuf),
           app().get_priority_queue().wrap(zmq_priority, [this](error_code ec, size_t bytes_transferred) {
               if (ec) {
                 abort_receiver();
               }
               else {
                 uint64_t ack = std::stoul(string((const char*)rcvbuf.data(), bytes_transferred));
                 if( ack > UINT32_MAX )
                   throw std::runtime_error("Consumer acknowledged block number higher than UINT32_MAX");
                 ack_block(ack);
                 async_read_acks();
               }
             }));
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
     "ZMQ Sender Socket binding")
    (RECEICER_CN_OPT, bpo::value<string>()->default_value(RECEICER_CN_DEFAULT),
     "ZMQ Receiver Socket connect string")
    (ZMQ_MAXUNACK_OPT, bpo::value<uint32_t>()->default_value(ZMQ_MAXUNACK_DEFAULT),
     "Receiver will pause at so many unacknowledged blocks");
}

  
void exp_zmq_plugin::plugin_initialize( const variables_map& options ) {
  try {
    donot_start_receiver_before(this, "exp_zmq_plugin");

    my->sender_bind_str = options.at(SENDER_BIND_OPT).as<string>();
    if (my->sender_bind_str.empty()) {
      wlog("${o} not specified => exp_zmq_plugin disabled.", ("o",SENDER_BIND_OPT));
      return;
    }

    my->receiver_connect_str = options.at(RECEICER_CN_OPT).as<string>();
    if (my->receiver_connect_str.empty()) {
      wlog("${o} not specified => exp_zmq_plugin disabled.", ("o",RECEICER_CN_OPT));
      return;
    }

    uint32_t maxunack = options.at(ZMQ_MAXUNACK_OPT).as<uint32_t>();
    if( maxunack == 0 )
      throw std::runtime_error("Maximum unacked blocks must be a positive integer");
    exporter_will_ack_blocks(maxunack);
    
    my->init();
    ilog("Initialized exp_zmq_plugin");
    exporter_initialized();
  }
  FC_LOG_AND_RETHROW();
}


void exp_zmq_plugin::plugin_startup(){
  my->start();
  ilog("Started exp_zmq_plugin");
}

void exp_zmq_plugin::plugin_shutdown() {
  if( ! my->sender_bind_str.empty() ) {
    my->sender_socket->disconnect(my->sender_bind_str);
  }
  if( ! my->receiver_connect_str.empty() ) {
    my->receiver_socket->disconnect(my->receiver_connect_str);
  }
  ilog("exp_zmq_plugin stopped");
}



