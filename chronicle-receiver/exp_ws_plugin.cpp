

// copyright defined in LICENSE.txt

#include "exp_ws_plugin.hpp"
#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"
#include "chronicle_msgtypes.h"

#include <queue>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core.hpp>
#include <stdexcept>
#include <limits>

#include "rapidjson/reader.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"


#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

using boost::beast::flat_buffer;
using boost::system::error_code;


static auto _exp_ws_plugin = app().register_plugin<exp_ws_plugin>();

namespace {
  const char* WS_HOST_OPT = "exp-ws-host";
  const char* WS_PORT_OPT = "exp-ws-port";
  const char* WS_PATH_OPT = "exp-ws-path";
  const char* WS_MAXUNACK_OPT = "exp-ws-max-unack";
  const char* WS_MAXQUEUE_OPT = "exp-ws-max-queue";
  const char* WS_BINHDR = "exp-ws-bin-header";
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
  chronicle::channels::js_permission_updates::channel_type::handle  _js_permission_updates_subscription;
  chronicle::channels::js_permission_link_updates::channel_type::handle  _js_permission_link_updates_subscription;
  chronicle::channels::js_account_metadata_updates::channel_type::handle  _js_account_metadata_updates_subscription;
  chronicle::channels::js_receiver_pauses::channel_type::handle     _js_receiver_pauses_subscription;
  chronicle::channels::js_block_completed::channel_type::handle     _js_block_completed_subscription;
  chronicle::channels::js_abi_decoder_errors::channel_type::handle  _js_abi_decoder_errors_subscription;

  chronicle::channels::interactive_requests::channel_type&          _interactive_requests_chan;

  string ws_host;
  string ws_port;
  string ws_path;
  bool use_bin_headers;
  uint32_t maxunack;

  using wstream = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;
  std::shared_ptr<wstream> ws;
  const int ws_priority = 60;
  const int ws_order = 1000;

  rapidjson::StringBuffer json_buffer;
  rapidjson::Writer<rapidjson::StringBuffer> json_writer;

  using msgbuf = std::vector<unsigned char>;
  std::queue<std::shared_ptr<msgbuf>> async_queue;
  std::shared_ptr<msgbuf> async_msg; // this to prevent deallocation during async write
  uint32_t queue_hwm;
  uint32_t queue_lwm;
  boost::asio::const_buffer async_out_buffer;
  std::shared_ptr<boost::asio::deadline_timer> mytimer;

  uint32_t pause_time_msec = 0;
  uint32_t msg_report_counter = 1000;

  exp_ws_plugin_impl() :
    _interactive_requests_chan(app().get_channel<chronicle::channels::interactive_requests>())
  {};

  void init() {
    ws = std::make_shared<wstream>(app().get_io_service());
    mytimer = std::make_shared<boost::asio::deadline_timer>(app().get_io_service());

    if (use_bin_headers) {
      _js_forks_subscription =
        app().get_channel<chronicle::channels::js_forks>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_FORK, 0, event); });

      _js_blocks_subscription =
        app().get_channel<chronicle::channels::js_blocks>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_BLOCK, 0, event); });

      _js_transaction_traces_subscription =
        app().get_channel<chronicle::channels::js_transaction_traces>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_TX_TRACE, 0, event); });

      _js_abi_updates_subscription =
        app().get_channel<chronicle::channels::js_abi_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_ABI_UPD, 0, event); });

      _js_abi_removals_subscription =
        app().get_channel<chronicle::channels::js_abi_removals>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_ABI_REM, 0, event); });

      _js_abi_errors_subscription =
        app().get_channel<chronicle::channels::js_abi_errors>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_ABI_ERR, 0, event); });

      _js_table_row_updates_subscription =
        app().get_channel<chronicle::channels::js_table_row_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_TBL_ROW, 0, event); });

      _js_permission_updates_subscription =
        app().get_channel<chronicle::channels::js_permission_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_PERMISSION, 0, event); });

      _js_permission_link_updates_subscription =
        app().get_channel<chronicle::channels::js_permission_link_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_PERMISSION_LINK, 0, event); });

      _js_account_metadata_updates_subscription =
        app().get_channel<chronicle::channels::js_account_metadata_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_ACC_METADATA, 0, event); });

      _js_abi_decoder_errors_subscription =
        app().get_channel<chronicle::channels::js_abi_decoder_errors>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_ENCODER_ERR, 0, event); });

      _js_receiver_pauses_subscription =
        app().get_channel<chronicle::channels::js_receiver_pauses>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_RCVR_PAUSE, 0, event); });

      _js_block_completed_subscription =
        app().get_channel<chronicle::channels::js_block_completed>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_bin(CHRONICLE_MSGTYPE_BLOCK_COMPLETED, 0, event); });
    }
    else {
      json_buffer.Reserve(1024*256);

      _js_forks_subscription =
        app().get_channel<chronicle::channels::js_forks>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("FORK", event); });

      _js_blocks_subscription =
        app().get_channel<chronicle::channels::js_blocks>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("BLOCK", event); });

      _js_transaction_traces_subscription =
        app().get_channel<chronicle::channels::js_transaction_traces>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("TX_TRACE", event); });

      _js_abi_updates_subscription =
        app().get_channel<chronicle::channels::js_abi_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("ABI_UPD", event); });

      _js_abi_removals_subscription =
        app().get_channel<chronicle::channels::js_abi_removals>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("ABI_REM", event); });

      _js_abi_errors_subscription =
        app().get_channel<chronicle::channels::js_abi_errors>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("ABI_ERR", event); });

      _js_table_row_updates_subscription =
        app().get_channel<chronicle::channels::js_table_row_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("TBL_ROW", event); });

      _js_permission_updates_subscription =
        app().get_channel<chronicle::channels::js_permission_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("PERMISSION", event); });

      _js_permission_link_updates_subscription =
        app().get_channel<chronicle::channels::js_permission_link_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("PERMISSION_LINK", event); });

      _js_account_metadata_updates_subscription =
        app().get_channel<chronicle::channels::js_account_metadata_updates>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("ACC_METADATA", event); });

      _js_receiver_pauses_subscription =
        app().get_channel<chronicle::channels::js_receiver_pauses>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("RCVR_PAUSE", event); });

      _js_block_completed_subscription =
        app().get_channel<chronicle::channels::js_block_completed>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("BLOCK_COMPLETED", event); });

      _js_abi_decoder_errors_subscription =
        app().get_channel<chronicle::channels::js_abi_decoder_errors>().subscribe
        ([this](std::shared_ptr<string> event){ on_event_json("ENCODER_ERR", event); });
    }
  }


  void start() {
    if (!is_interactive_mode())
      exporter_will_ack_blocks(maxunack);

    ilog("Connecting to websocket server ${h}:${p}", ("h",ws_host)("p",ws_port));
    boost::asio::ip::tcp::resolver r(app().get_io_service());
    auto const results = r.resolve(ws_host, ws_port);
    ws->binary(true);
    ws->auto_fragment(true);
    boost::asio::connect(ws->next_layer(), results.begin(), results.end());
    ws->handshake(ws_host, ws_path);
    ilog("Connected");
    if (is_interactive_mode()) {
      async_read_interactive_reqs();
    }
    else {
      async_read_acks();
    }
    async_send_events();
  }


  void stop() {
    close_ws(boost::beast::websocket::close_code::normal);
  }


  void close_ws(boost::beast::websocket::close_reason reason) {
    if( ws->is_open() ) {
      ilog("Closing the export websocket connection to ${h}:${p}", ("h",ws_host)("p",ws_port));
      ws->next_layer().cancel();
      ws->async_close(reason, app().executor().get_priority_queue().wrap(ws_priority, ws_order, [&](error_code ec) {
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
    ws->async_read
      (*in_buffer,
       app().executor().get_priority_queue().wrap(ws_priority, ws_order, [this, in_buffer](error_code ec, size_t) {
           if (ec) {
             close_ws(boost::beast::websocket::close_code::unknown_data);
           }
           else {
             const auto in_data = in_buffer->data();
             uint64_t ack = std::stoul(string((const char*)in_data.data(), in_data.size()));
             if( ack > std::numeric_limits<uint32_t>::max() ) {
               elog("Wrong data in acknowledgement: ${s}",
                    ("s",string((const char*)in_data.data(), in_data.size())));
               throw std::runtime_error("Consumer acknowledged block number higher than UINT32_MAX");
             }
             ack_block(ack);
             async_read_acks();
           }
         }));
  }


  void async_read_interactive_reqs() {
    auto in_buffer = std::make_shared<flat_buffer>();
    ws->async_read
      (*in_buffer,
       app().executor().get_priority_queue().wrap(ws_priority, ws_order, [this, in_buffer](error_code ec, size_t) {
           if (ec) {
             close_ws(boost::beast::websocket::close_code::unknown_data);
           }
           else {
             auto req = std::make_shared<chronicle::channels::interactive_request>();
             const auto in_data = in_buffer->data();
             string reqstr((const char*)in_data.data(), in_data.size());
             auto pos = reqstr.find('-');
             if (pos == string::npos) {
               req->block_num_start = std::stoul(reqstr);
               req->block_num_end = req->block_num_start+1;
             } else {
               req->block_num_start = std::stoul(string(reqstr, 0, pos));
               req->block_num_end = std::stoul(string(reqstr, pos+1));
             }
             if (req->block_num_end <= req->block_num_start) {
               elog("Wrong interactive request: start=${s}, end=${e}", ("s",req->block_num_start)("e",req->block_num_end));
               throw std::runtime_error("End block in interactive request not higher than start block");
             }
             ilog("Interactive request: start=${s}, end=${e}", ("s",req->block_num_start)("e",req->block_num_end));
             _interactive_requests_chan.publish(ws_priority, req);
             async_read_interactive_reqs();
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

      mytimer->expires_from_now(boost::posix_time::milliseconds(pause_time_msec));
      mytimer->async_wait(app().executor().get_priority_queue().wrap(ws_priority, ws_order, [this](const error_code ec) {
            async_send_events();
          }));
    }
    else {
      pause_time_msec = 0;
      if( async_queue.size() >= queue_hwm ) {
        slowdown_receiver(true);
      }
      else if( async_queue.size() < queue_lwm ) {
        slowdown_receiver(false);
      }
      async_msg = async_queue.front();
      async_queue.pop();
      async_out_buffer = boost::asio::const_buffer(async_msg->data(), async_msg->size());
      ws->async_write
        (async_out_buffer,
         app().executor().get_priority_queue().wrap(ws_priority, ws_order, [this](error_code ec, size_t) {
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


  inline void push_msg(std::shared_ptr<msgbuf> buf) {
    async_queue.push(buf);
    if( pause_time_msec > 0 )
      mytimer->cancel();
    msg_report_counter--;
    if( msg_report_counter == 0 ) {
      ilog("exp_ws_plugin queue_size=${q}", ("q",async_queue.size()));
      msg_report_counter = 10000;
    }
  }


  void on_event_json(const char* msgtype, std::shared_ptr<string> event) {
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

        size_t sz = json_buffer.GetSize();
        string msg(json_buffer.GetString());
        auto buf = std::make_shared<msgbuf>(sz);
        memcpy(buf->data(), msg.data(), sz);
        push_msg(buf);
      }
      FC_LOG_AND_RETHROW();
    }
    catch (...) {
      abort_receiver();
    }
  }


  void on_event_bin(int32_t msgtype, int32_t msgopts, std::shared_ptr<string> event) {
    try {
      try {
        auto buf = std::make_shared<msgbuf>(event->length()+sizeof(msgtype)+sizeof(msgopts));
        unsigned char *ptr = buf->data();
        memcpy(ptr, &msgtype, sizeof(msgtype));
        ptr += sizeof(msgtype);
        memcpy(ptr, &msgopts, sizeof(msgopts));
        ptr += sizeof(msgopts);
        memcpy(ptr, event->data(), event->length());
        push_msg(buf);
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
    (WS_PATH_OPT, bpo::value<string>()->default_value("/"), "Websocket server URL path")
    (WS_MAXUNACK_OPT, bpo::value<uint32_t>()->default_value(1000),
     "Receiver will pause at so many unacknowledged blocks")
    (WS_MAXQUEUE_OPT, bpo::value<uint32_t>()->default_value(10000),
     "Receiver will pause if outbound queue exceeds this limit")
    (WS_BINHDR, bpo::value<bool>()->default_value(false),
     "Start export messages with 32-bit native msgtype,msgopt")
    ;
}


void exp_ws_plugin::plugin_initialize( const variables_map& options ) {
  if (is_noexport_opt(options))
    return;

  try {
    app().get_plugin("decoder_plugin").initialize(options);
    donot_start_receiver_before(this, "exp_ws_plugin");

    bool opt_missing = false;
    if( options.count(WS_HOST_OPT) != 1 ) {
      elog("${o} not specified, as required by exp_ws_plugin", ("o",WS_HOST_OPT));
      opt_missing = true;
    }
    if( options.count(WS_PORT_OPT) != 1 ) {
      elog("${o} not specified, as required by exp_ws_plugin", ("o",WS_PORT_OPT));
      opt_missing = true;
    }

    if( opt_missing )
      throw std::runtime_error("Mandatory option missing");

    my->ws_host = options.at(WS_HOST_OPT).as<string>();
    my->ws_port = options.at(WS_PORT_OPT).as<string>();
    my->ws_path = options.at(WS_PATH_OPT).as<string>();

    my->maxunack = options.at(WS_MAXUNACK_OPT).as<uint32_t>();
    if( my->maxunack == 0 )
      throw std::runtime_error("Maximum unacked blocks must be a positive integer");

    my->queue_hwm = options.at(WS_MAXQUEUE_OPT).as<uint32_t>();
    if( my->queue_hwm == 0 )
      throw std::runtime_error("Maximum queue size must be a positive integer");
    my->queue_lwm = my->queue_hwm * 3 / 4;

    my->use_bin_headers = options.at(WS_BINHDR).as<bool>();

    my->init();
    ilog("Initialized exp_ws_plugin");
    exporter_initialized();
  }
  FC_LOG_AND_RETHROW();
}


void exp_ws_plugin::plugin_startup(){
  if (!is_noexport_mode()) {
    my->start();
    ilog("Started exp_ws_plugin");
  }
}

void exp_ws_plugin::plugin_shutdown() {
  if (!is_noexport_mode()) {
    my->stop();
    ilog("exp_ws_plugin stopped");
  }
}
