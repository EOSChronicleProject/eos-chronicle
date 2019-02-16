#include <appbase/application.hpp>
#include "chain_state_types.hpp"
#include <abieos.h>

using namespace appbase;

namespace chronicle {

  // Channels published by receiver_plugin

  namespace channels {

    using namespace abieos;
    
    struct fork_event {
      uint32_t    fork_block_num;
      uint32_t    depth;
    };
    
    template <typename F>
    constexpr void for_each_field(fork_event*, F f) {
      f("block_num", member_ptr<&fork_event::fork_block_num>{});
      f("depth", member_ptr<&fork_event::depth>{});
    }
    
    using forks     = channel_decl<struct forks_tag, std::shared_ptr<fork_event>>;

    struct block {
      uint32_t                        block_num;
      uint32_t                        last_irreversible;
      chain_state::signed_block       block;
    };

    template <typename F>
    constexpr void for_each_field(block*, F f) {
      f("block_num", member_ptr<&block::block_num>{});
      f("last_irreversible", member_ptr<&block::last_irreversible>{});
      f("block", member_ptr<&block::block>{});
    }
    
    using blocks    = channel_decl<struct blocks_tag, std::shared_ptr<block>>;

    struct block_table_delta {
      uint32_t                     block_num;
      abieos::block_timestamp      block_timestamp;
      chain_state::table_delta_v0  table_delta;
    };

    template <typename F>
    constexpr void for_each_field(block_table_delta*, F f) {
      f("block_num", member_ptr<&block_table_delta::block_num>{});
      f("block_timestamp", member_ptr<&block_table_delta::block_timestamp>{});
      f("table_delta", member_ptr<&block_table_delta::table_delta>{});
    }
    
    using block_table_deltas  =
      channel_decl<struct block_table_deltas_tag, std::shared_ptr<block_table_delta>>;    

    struct transaction_trace {
      uint32_t                        block_num;
      abieos::block_timestamp         block_timestamp;
      chain_state::transaction_trace  trace;
    };

    template <typename F>
    constexpr void for_each_field(transaction_trace*, F f) {
      f("block_num", member_ptr<&transaction_trace::block_num>{});
      f("block_timestamp", member_ptr<&transaction_trace::block_timestamp>{});
      f("trace", member_ptr<&transaction_trace::trace>{});
    }
    
    using transaction_traces = channel_decl<struct transaction_traces_tag, std::shared_ptr<transaction_trace>>;

    struct abi_update {
      uint32_t                        block_num;
      abieos::block_timestamp         block_timestamp;
      abieos::name                    account;
      abieos::bytes                   abi_bytes;
      abieos::abi_def                 abi;
    };

    template <typename F>
    constexpr void for_each_field(abi_update*, F f) {
      f("block_num", member_ptr<&abi_update::block_num>{});
      f("block_timestamp", member_ptr<&abi_update::block_timestamp>{});
      f("account", member_ptr<&abi_update::account>{});
      f("abi_bytes", member_ptr<&abi_update::abi_bytes>{});
      f("abi", member_ptr<&abi_update::abi>{});
    }
    
    using abi_updates = channel_decl<struct abi_updates_tag, std::shared_ptr<abi_update>>;

    struct abi_removal {
      uint32_t                        block_num;
      abieos::block_timestamp         block_timestamp;
      abieos::name                    account;
    };

    template <typename F>
    constexpr void for_each_field(abi_removal*, F f) {
      f("block_num", member_ptr<&abi_removal::block_num>{});
      f("block_timestamp", member_ptr<&abi_removal::block_timestamp>{});
      f("account", member_ptr<&abi_removal::account>{});
    }

    using abi_removals = channel_decl<struct abi_removals_tag, std::shared_ptr<abi_removal>>;

    struct abi_error {
      uint32_t                        block_num;
      abieos::block_timestamp         block_timestamp;
      abieos::name                    account;
      string                          error; 
    };

    template <typename F>
    constexpr void for_each_field(abi_error*, F f) {
      f("block_num", member_ptr<&abi_error::block_num>{});
      f("block_timestamp", member_ptr<&abi_error::block_timestamp>{});
      f("account", member_ptr<&abi_error::account>{});
      f("error", member_ptr<&abi_error::error>{});
    }
    
    using abi_errors = channel_decl<struct abi_errors_tag, std::shared_ptr<abi_error>>;

    struct table_row_update {
      uint32_t                           block_num;
      abieos::block_timestamp            block_timestamp;
      bool                               added; // false==removed
      chain_state::key_value_object      kvo;
    };

    template <typename F>
    constexpr void for_each_field(table_row_update*, F f) {
      f("block_num", member_ptr<&table_row_update::block_num>{});
      f("block_timestamp", member_ptr<&table_row_update::block_timestamp>{});
      f("added", member_ptr<&table_row_update::added>{});
      f("kvo", member_ptr<&table_row_update::kvo>{});
    }
    
    using table_row_updates = channel_decl<struct table_row_updates_tag, std::shared_ptr<table_row_update>>;
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

  abieos_context* get_contract_abi_ctxt(abieos::name account);
  void add_dependency(appbase::abstract_plugin* plug, string plugname);
  
private:
  std::unique_ptr<class receiver_plugin_impl> my;
  std::vector<std::tuple<appbase::abstract_plugin*, std::string>> dependent_plugins;
  void start_after_dependencies();
};


abieos_context* get_contract_abi_ctxt(abieos::name account);
void donot_start_receiver_before(appbase::abstract_plugin* plug, string plugname);
