#include <appbase/application.hpp>

using namespace appbase;

namespace chronicle {
  // Channels published by decoder_plugin
  namespace channels {
    using js_forks               = channel_decl<struct js_forks_tag, std::shared_ptr<string>>;
    using js_block_started       = channel_decl<struct js_block_started_tag, std::shared_ptr<string>>;
    using js_blocks              = channel_decl<struct js_blocks_tag, std::shared_ptr<string>>;
    using js_transaction_traces  = channel_decl<struct js_transaction_traces_tag, std::shared_ptr<string>>;
    using js_abi_updates         = channel_decl<struct js_abi_updates_tag, std::shared_ptr<string>>;
    using js_abi_removals        = channel_decl<struct js_abi_removals_tag, std::shared_ptr<string>>;
    using js_abi_errors          = channel_decl<struct js_abi_errors_tag, std::shared_ptr<string>>;
    using js_table_row_updates   = channel_decl<struct js_table_row_updates_tag, std::shared_ptr<string>>;
    using js_permission_updates  = channel_decl<struct js_permission_updates_tag, std::shared_ptr<string>>;
    using js_permission_link_updates  = channel_decl<struct js_permission_link_updates_tag, std::shared_ptr<string>>;
    using js_account_metadata_updates  = channel_decl<struct js_account_metadata_updates_tag, std::shared_ptr<string>>;
    using js_abi_decoder_errors  = channel_decl<struct js_abi_decoder_errors_tag, std::shared_ptr<string>>;
    using js_receiver_pauses     = channel_decl<struct js_receiver_pauses_tag, std::shared_ptr<string>>;
    using js_block_completed     = channel_decl<struct js_block_completed_tag, std::shared_ptr<string>>;
  }
}


class decoder_plugin : public appbase::plugin<decoder_plugin>
{
public:
  APPBASE_PLUGIN_REQUIRES();
  decoder_plugin();
  virtual ~decoder_plugin();
  virtual void set_program_options(options_description& cli, options_description& cfg) override;
  void plugin_initialize(const variables_map& options);
  void plugin_startup();
  void plugin_shutdown();

private:
  std::unique_ptr<class decoder_plugin_impl> my;
};
