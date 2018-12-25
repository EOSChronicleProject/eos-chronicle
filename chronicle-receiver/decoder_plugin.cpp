// copyright defined in LICENSE.txt

#include "decoder_plugin.hpp"
#include "receiver_plugin.hpp"

#include <iostream>
#include <string>



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
  chronicle::channels::table_row_updates::channel_type::handle   _table_row_updates_subscription;
  chronicle::channels::abi_errors::channel_type::handle          _abi_errors_subscription;
  chronicle::channels::abi_removals::channel_type::handle        _abi_removals_subscription;
  chronicle::channels::abi_updates::channel_type::handle         _abi_updates_subscription;
  chronicle::channels::transaction_traces::channel_type::handle  _transaction_traces_subscription;

  // we only siubscribe to receiver channels at startup, assuming our consumers have subscribed at init
  void start() {
    if (_js_forks_chan.has_subscribers()) {
      _forks_subscription =
        app().get_channel<chronicle::channels::forks>().subscribe([this](uint32_t block_num){
            on_fork(block_num);
          });
    }
    
    if (_js_blocks_chan.has_subscribers()) {
      _blocks_subscription =
        app().get_channel<chronicle::channels::blocks>().subscribe([this](std::shared_ptr<chain_state::signed_block> block_ptr){
            on_block(block_ptr);
          });
    }
  }

  void on_fork(uint32_t block_num) {
  }

  void on_block(std::shared_ptr<chain_state::signed_block> block_ptr) {
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

