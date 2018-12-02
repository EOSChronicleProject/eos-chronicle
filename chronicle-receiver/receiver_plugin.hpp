#include <appbase/application.hpp>
#include "chain_state_types.hpp"

using namespace appbase;

namespace chronicle {

  // Channels published by receiver_plugin

  namespace channels {
    using forks     = channel_decl<struct forks_tag, uint32_t>;
    using blocks    = channel_decl<struct blocks_tag, std::shared_ptr<chain_state::signed_block>>;

    struct block_table_delta {
      uint32_t                     block_num;
      chain_state::table_delta_v0  table_delta;
    };
    using block_table_deltas  = channel_decl<struct block_table_deltas_tag, std::shared_ptr<block_table_delta>>;

    struct transaction_trace {
      uint32_t                        block_num;
      chain_state::transaction_trace  trace;
    };
    using transaction_traces = channel_decl<struct transaction_traces_tag, std::shared_ptr<transaction_trace>>;
  }
}
  

class receiver_plugin : public appbase::plugin<receiver_plugin>
{
public:
  APPBASE_PLUGIN_REQUIRES();
  receiver_plugin();
  virtual ~receiver_plugin();
  virtual void set_program_options(options_description& cli, options_description& cfg) override;  
  void plugin_initialize(const variables_map& options);
  void plugin_startup();
  void plugin_shutdown();
  
private:
  std::unique_ptr<class receiver_plugin_impl> my;
};



