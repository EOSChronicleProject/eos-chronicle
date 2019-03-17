// copyright defined in LICENSE.txt

#include "exp_ws_plugin.hpp"
#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"

#include <queue>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core.hpp>
#include <boost/lockfree/queue.hpp>
#include <stdexcept>
	
#include "rapidjson/reader.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"


#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

using boost::beast::flat_buffer;
using boost::system::error_code;
using std::make_shared;


static appbase::abstract_plugin& _exp_ws_plugin = app().register_plugin<exp_ws_plugin>();

namespace {
  const char* WS_HOST_OPT = "exp-ws-host";
  const char* WS_PORT_OPT = "exp-ws-port";
  const char* WS_MAXUNACK_OPT = "exp-ws-max-unack";
  const char* WS_MAXQUEUE_OPT = "exp-ws-max-queue";
}

class exp_ws_plugin_impl : std::enable_shared_from_this<exp_ws_plugin_impl> {
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

  string ws_host;
  string ws_port;
  boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws;
  const int ws_priority = 60;

  rapidjson::StringBuffer json_buffer;
  rapidjson::Writer<rapidjson::StringBuffer> json_writer;

  std::queue<string> async_queue;
  uint32_t queue_maxsize;
  string async_msg;
  boost::asio::const_buffer async_out_buffer;
  boost::asio::deadline_timer mytimer;
  uint32_t pause_time_msec = 0;
  uint32_t prev_ack_reported = 0;

  exp_ws_plugin_impl():
    ws(std::ref(app().get_io_service())),
    mytimer(std::ref(app().get_io_service()))
  {};

  void init() {
    json_buffer.Reserve(1024*256);
      

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
    ilog("Connecting to websocket server ${h}:${p}", ("h",ws_host)("p",ws_port));
    boost::asio::ip::tcp::resolver r(std::ref(app().get_io_service()));
    auto const results = r.resolve(ws_host, ws_port);
    ws.binary(true);
    boost::asio::connect(ws.next_layer(), results.begin(), results.end());
    ws.handshake(ws_host, "/");
    ilog("Connected");
    async_read_acks();
    async_send_events();
  }


  void stop() {
    close_ws(boost::beast::websocket::close_code::normal);
  }
  
  
  void close_ws(boost::beast::websocket::close_reason reason) {
    ws.next_layer().cancel();
    if( ws.is_open() ) {
      ilog("Closing websocket connection to ${h}:${p}", ("h",ws_host)("p",ws_port));    
      ws.async_close(reason, app().get_priority_queue().wrap(ws_priority, [&](error_code ec) {
            if (ec) elog(ec.message());
            abort_receiver();
          }));
    }
    else {
      abort_receiver();
    }
  }
    
      
  
  void async_read_acks() {
    auto in_buffer = std::make_shared<flat_buffer>();
    ws.async_read
      (*in_buffer,
       app().get_priority_queue().wrap(ws_priority, [this, in_buffer](error_code ec, size_t) {
           if (ec) {
             close_ws(boost::beast::websocket::close_code::unknown_data);
           }
           else {
             const auto in_data = in_buffer->data();
             uint64_t ack = std::stoul(string((const char*)in_data.data(), in_data.size()));
             if( ack > UINT32_MAX )
               throw std::runtime_error("Consumer acknowledged block number higher than UINT32_MAX");
             ack_block(ack);
             if( ack - prev_ack_reported > 10000 ) {
               ilog("exp_ws_plugin queue_size=${q}", ("q",async_queue.size()));
               prev_ack_reported = ack;
             }
             async_read_acks();
           }
         }));
  }


  void async_send_events() {
    if( async_queue.empty() ) {
      if( pause_time_msec == 0 ) {
        pause_time_msec = 5;
      }
      else if( pause_time_msec < 256 ) {
        pause_time_msec *= 2;
      }

      mytimer.expires_from_now(boost::posix_time::milliseconds(pause_time_msec));
      mytimer.async_wait(app().get_priority_queue().wrap(ws_priority, [this](const error_code ec) {
            async_send_events();
          }));
    }
    else {
      pause_time_msec = 0;
      if( async_queue.size() > queue_maxsize )
        slowdown_receiver();
      async_msg = async_queue.front();
      async_queue.pop();
      async_out_buffer = boost::asio::const_buffer(async_msg.data(), async_msg.size());
      ws.async_write
        (async_out_buffer,
         app().get_priority_queue().wrap(ws_priority, [this](error_code ec, size_t) {
             if (ec) {
               elog("ERROR writing to websocket: ${e}", ("e",ec.message()));
               close_ws(boost::beast::websocket::close_code::unknown_data);
             }
             else {
               async_send_events();
             }
           }));
    }
  }
            
          
      
  
  void on_event(const char* msgtype, std::shared_ptr<string> event) {
    try {
      try {
        json_buffer.Clear();
        json_writer.Reset(json_buffer);
        json_writer.StartObject();
        json_writer.Key("msgtype");
        json_writer.String(msgtype);
        json_writer.Key("data");
        json_writer.RawValue(event->data(), event->length(), rapidjson::kObjectType);
        json_writer.EndObject();

        string msg(json_buffer.GetString());
        async_queue.push(msg);
      }
      FC_LOG_AND_RETHROW();
    }
    catch (...) {
      abort_receiver();
    }
  }
};



exp_ws_plugin::exp_ws_plugin() :my(new exp_ws_plugin_impl){
}

exp_ws_plugin::~exp_ws_plugin(){
}


void exp_ws_plugin::set_program_options( options_description& cli, options_description& cfg ) {
  cfg.add_options()
    (WS_HOST_OPT, bpo::value<string>(), "Websocket server host to connect to")
    (WS_PORT_OPT, bpo::value<string>(), "Websocket server port to connect to")
    (WS_MAXUNACK_OPT, bpo::value<uint32_t>()->default_value(1000), "Receiver will pause at so many unacknowledged blocks")
    (WS_MAXQUEUE_OPT, bpo::value<uint32_t>()->default_value(10000), "Receiver will pause if outbound queue exceeds this limit")
    ;
}

  
void exp_ws_plugin::plugin_initialize( const variables_map& options ) {
  try {
    donot_start_receiver_before(this, "exp_ws_plugin");

    bool opt_missing = false;
    if( options.count(WS_HOST_OPT) != 1 ) {
      elog("${o} not specified => exp_ws_plugin disabled.", ("o",WS_HOST_OPT));
      opt_missing = true;
    }
    if( options.count(WS_PORT_OPT) != 1 ) {
      elog("${o} not specified => exp_ws_plugin disabled.", ("o",WS_PORT_OPT));
      opt_missing = true;
    }

    if( opt_missing )
      throw std::runtime_error("Mandatory option missing");

    my->ws_host = options.at(WS_HOST_OPT).as<string>();
    my->ws_port = options.at(WS_PORT_OPT).as<string>();

    uint32_t maxunack = options.at(WS_MAXUNACK_OPT).as<uint32_t>();
    if( maxunack == 0 )
      throw std::runtime_error("Maximum unacked blocks must be a positive integer");
    exporter_will_ack_blocks(maxunack);
    my->queue_maxsize = options.at(WS_MAXQUEUE_OPT).as<uint32_t>();
    if( my->queue_maxsize == 0 )
      throw std::runtime_error("Maximum queue size must be a positive integer");
    
    my->init();    
    ilog("Initialized exp_ws_plugin");
    exporter_initialized();
  }
  FC_LOG_AND_RETHROW();
}


void exp_ws_plugin::plugin_startup(){
  my->start();
  ilog("Started exp_ws_plugin");
}

void exp_ws_plugin::plugin_shutdown() {
  my->stop();
  ilog("exp_ws_plugin stopped");
}



