#include <appbase/application.hpp>
#include "chain_state.hpp"

using namespace appbase;

namespace chronicle {  
  namespace channels {
    using namespace chain_state;
    using forks     = channel_decl<struct forks_tag, uint32_t>;
    using blocks    = channel_decl<struct blocks_tag, std::shared_ptr<signed_block>>;

    struct block_table_delta {
      uint32_t        block_num;
      table_delta_v0  table_delta;
    };
    using block_table_deltas  = channel_decl<struct block_table_deltas_tag, std::shared_ptr<block_table_delta>>;
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
  
  // Channels published by receiver_plugin
  
  


  
private:
  std::unique_ptr<class receiver_plugin_impl> my;
};



