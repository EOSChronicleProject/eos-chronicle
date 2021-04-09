#include <appbase/application.hpp>
#include <eosio/ship_protocol.hpp>
#include <abieos.hpp>
#include <eosio/abieos.h>
#include <boost/beast/core/flat_buffer.hpp>

using namespace appbase;
using boost::beast::flat_buffer;

namespace chronicle {

  // Channels published by receiver_plugin

  namespace channels {

    using namespace abieos;

    enum class fork_reason_val : uint8_t {
      network  = 1, // fork occurred in the EOSIO network
      restart = 2,  // explicit fork on receiver restart
      resync = 3,   // full resync from the genesis
    };

    inline string to_string(fork_reason_val reason) {
      switch (reason) {
      case fork_reason_val::network: return "network";
      case fork_reason_val::restart: return "restart";
      case fork_reason_val::resync:  return "resync";
      }
      return "unknown";
    }

    struct fork_event {
      uint32_t         block_num;
      uint32_t         depth;
      fork_reason_val  fork_reason;
      uint32_t         last_irreversible;
    };

    EOSIO_REFLECT(fork_event, block_num, depth, fork_reason, last_irreversible);

    using forks     = channel_decl<struct forks_tag, std::shared_ptr<fork_event>>;

    struct block {
      uint32_t                             block_num;
      abieos::checksum256                  block_id;
      uint32_t                             last_irreversible;
      eosio::ship_protocol::signed_block_v0   block;
      std::shared_ptr<flat_buffer>         buffer;
    };

    EOSIO_REFLECT(block, block_num, block_id, last_irreversible, block);

    using blocks    = channel_decl<struct blocks_tag, std::shared_ptr<block>>;

    struct block_table_delta {
      uint32_t                                   block_num;
      eosio::block_timestamp                     block_timestamp;
      eosio::ship_protocol::table_delta_v0       table_delta;
      std::shared_ptr<flat_buffer>               buffer;
    };

    EOSIO_REFLECT(block_table_delta, block_num, block_timestamp, table_delta);

    using block_table_deltas  =
      channel_decl<struct block_table_deltas_tag, std::shared_ptr<block_table_delta>>;

    struct transaction_trace {
      uint32_t                                 block_num;
      eosio::block_timestamp                   block_timestamp;
      eosio::ship_protocol::transaction_trace  trace;
      std::shared_ptr<flat_buffer>             buffer;
    };

    EOSIO_REFLECT(transaction_trace, block_num, block_timestamp, trace);

    using transaction_traces = channel_decl<struct transaction_traces_tag, std::shared_ptr<transaction_trace>>;

    struct abi_update {
      uint32_t                        block_num;
      eosio::block_timestamp          block_timestamp;
      abieos::name                    account;
      abieos::abi_def                 abi;
    };

    EOSIO_REFLECT(abi_update, block_num, block_timestamp, account, abi);

    using abi_updates = channel_decl<struct abi_updates_tag, std::shared_ptr<abi_update>>;

    struct abi_removal {
      uint32_t                        block_num;
      eosio::block_timestamp          block_timestamp;
      abieos::name                    account;
    };

    EOSIO_REFLECT(abi_removal, block_num, block_timestamp, account);

    using abi_removals = channel_decl<struct abi_removals_tag, std::shared_ptr<abi_removal>>;

    struct abi_error {
      uint32_t                        block_num;
      eosio::block_timestamp          block_timestamp;
      abieos::name                    account;
      string                          error;
    };

    EOSIO_REFLECT(abi_error, block_num, block_timestamp, account, error);

    using abi_errors = channel_decl<struct abi_errors_tag, std::shared_ptr<abi_error>>;

    struct table_row_update {
      uint32_t                                 block_num;
      eosio::block_timestamp                   block_timestamp;
      bool                                     added; // false==removed
      eosio::ship_protocol::contract_row_v0    kvo;
      std::shared_ptr<flat_buffer>             buffer;
    };

    EOSIO_REFLECT(table_row_update, block_num, block_timestamp, added, kvo);

    using table_row_updates = channel_decl<struct table_row_updates_tag, std::shared_ptr<table_row_update>>;

    struct permission_update {
      uint32_t                                 block_num;
      eosio::block_timestamp                   block_timestamp;
      bool                                     added; // false==removed
      eosio::ship_protocol::permission_v0      permission;
      std::shared_ptr<flat_buffer>             buffer;
    };

    EOSIO_REFLECT(permission_update, block_num, block_timestamp, added, permission);

    using permission_updates = channel_decl<struct permission_updates_tag, std::shared_ptr<permission_update>>;


    struct permission_link_update {
      uint32_t                                 block_num;
      eosio::block_timestamp                   block_timestamp;
      bool                                     added; // false==removed
      eosio::ship_protocol::permission_link_v0 permission_link;
      std::shared_ptr<flat_buffer>             buffer;
    };

    EOSIO_REFLECT(permission_link_update, block_num, block_timestamp, added, permission_link);

    using permission_link_updates =
      channel_decl<struct permission_link_updates_tag, std::shared_ptr<permission_link_update>>;


    struct account_metadata_update {
      uint32_t                                   block_num;
      eosio::block_timestamp                     block_timestamp;
      eosio::ship_protocol::account_metadata_v0  account_metadata;
      std::shared_ptr<flat_buffer>               buffer;
    };

    EOSIO_REFLECT(account_metadata_update, block_num, block_timestamp, account_metadata);

    using account_metadata_updates =
      channel_decl<struct account_metadata_updates_tag, std::shared_ptr<account_metadata_update>>;

    struct receiver_pause {
      uint32_t                           head;
      uint32_t                           acknowledged;
    };

    EOSIO_REFLECT(receiver_pause, head, acknowledged);

    using receiver_pauses = channel_decl<struct receiver_pauses_tag, std::shared_ptr<receiver_pause>>;

    struct block_finished {
      uint32_t                        block_num;
      abieos::checksum256             block_id;
      eosio::block_timestamp          block_timestamp;
      uint32_t                        last_irreversible;
    };

    EOSIO_REFLECT(block_finished, block_num, block_id, block_timestamp, last_irreversible);

    using block_completed = channel_decl<struct block_completed_tag, std::shared_ptr<block_finished>>;

    struct interactive_request {
      uint32_t                        block_num_start;
      uint32_t                        block_num_end;
    };

    using interactive_requests = channel_decl<struct interactive_requests_tag, std::shared_ptr<interactive_request>>;
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

  bool is_interactive();
  void request_block(uint32_t block_num);

  bool is_noexport();

  void exporter_will_ack_blocks(uint32_t max_unconfirmed);
  void ack_block(uint32_t block_num);
  void slowdown(bool pause);
  abieos_context* get_contract_abi_ctxt(abieos::name account);
  void add_dependency(appbase::abstract_plugin* plug, string plugname);
  void abort_receiver();
private:
  std::unique_ptr<class receiver_plugin_impl> my;
  std::vector<std::tuple<appbase::abstract_plugin*, std::string>> dependent_plugins;
  void start_after_dependencies();
};

// Global functions

bool is_noexport_opt(const variables_map& options);

extern receiver_plugin* receiver_plug;

void exporter_initialized();

inline bool is_interactive_mode() {
  return receiver_plug->is_interactive();
}

inline bool is_noexport_mode() {
  return receiver_plug->is_noexport();
}


void exporter_will_ack_blocks(uint32_t max_unconfirmed);

inline void ack_block(uint32_t block_num) {
  receiver_plug->ack_block(block_num);
}

inline void slowdown_receiver(bool pause) {
  receiver_plug->slowdown(pause);
}


void donot_start_receiver_before(appbase::abstract_plugin* plug, string plugname);
void abort_receiver();

inline abieos_context* get_contract_abi_ctxt(abieos::name account) {
  return receiver_plug->get_contract_abi_ctxt(account);
}
