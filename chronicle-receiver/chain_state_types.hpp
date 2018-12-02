#include <string>
#include <memory>
#include <stdexcept>
#include <optional>
#include <abieos.hpp>


namespace chain_state {
  using std::string;
  using std::variant;
  using std::vector;
  using std::to_string;
  using std::runtime_error;
  using std::optional;

  using namespace abieos;

  enum class transaction_status : uint8_t {
    executed  = 0, // succeed, no error handler executed
      soft_fail = 1, // objectively failed (not executed), error handler executed
      hard_fail = 2, // objectively failed and error handler objectively failed thus no state change
      delayed   = 3, // transaction delayed/deferred/scheduled for future execution
      expired   = 4, // transaction expired and storage space refuned to user
      };

  string to_string(transaction_status status);
  bool bin_to_native(transaction_status& status, bin_to_native_state& state, bool);
  bool json_to_native(transaction_status&, json_to_native_state&, event_type, bool);

  struct variant_header_zero {};

  bool bin_to_native(variant_header_zero&, bin_to_native_state& state, bool);
  bool json_to_native(variant_header_zero&, json_to_native_state&, event_type, bool);

  struct block_position {
    uint32_t    block_num = 0;
    checksum256 block_id  = {};
  };

  template <typename F>
  constexpr void for_each_field(block_position*, F f) {
    f("block_num", member_ptr<&block_position::block_num>{});
    f("block_id", member_ptr<&block_position::block_id>{});
  }

  struct get_blocks_result_v0 {
    block_position           head;
    block_position           last_irreversible;
    optional<block_position> this_block;
    optional<block_position> prev_block;
    optional<input_buffer>   block;
    optional<input_buffer>   traces;
    optional<input_buffer>   deltas;
  };

  template <typename F>
  constexpr void for_each_field(get_blocks_result_v0*, F f) {
    f("head", member_ptr<&get_blocks_result_v0::head>{});
    f("last_irreversible", member_ptr<&get_blocks_result_v0::last_irreversible>{});
    f("this_block", member_ptr<&get_blocks_result_v0::this_block>{});
    f("prev_block", member_ptr<&get_blocks_result_v0::prev_block>{});
    f("block", member_ptr<&get_blocks_result_v0::block>{});
    f("traces", member_ptr<&get_blocks_result_v0::traces>{});
    f("deltas", member_ptr<&get_blocks_result_v0::deltas>{});
  }

  struct row {
    bool         present;
    input_buffer data;
  };

  template <typename F>
  constexpr void for_each_field(row*, F f) {
    f("present", member_ptr<&row::present>{});
    f("data", member_ptr<&row::data>{});
  }

  struct table_delta_v0 {
    string      name;
    vector<row> rows;
  };

  template <typename F>
  constexpr void for_each_field(table_delta_v0*, F f) {
    f("name", member_ptr<&table_delta_v0::name>{});
    f("rows", member_ptr<&table_delta_v0::rows>{});
  }

  struct action_trace_authorization {
    name actor;
    name permission;
  };

  template <typename F>
  constexpr void for_each_field(action_trace_authorization*, F f) {
    f("actor", member_ptr<&action_trace_authorization::actor>{});
    f("permission", member_ptr<&action_trace_authorization::permission>{});
  }

  struct action_trace_auth_sequence {
    name     account;
    uint64_t sequence;
  };

  template <typename F>
  constexpr void for_each_field(action_trace_auth_sequence*, F f) {
    f("account", member_ptr<&action_trace_auth_sequence::account>{});
    f("sequence", member_ptr<&action_trace_auth_sequence::sequence>{});
  }

  struct action_trace_ram_delta {
    name    account;
    int64_t delta;
  };

  template <typename F>
  constexpr void for_each_field(action_trace_ram_delta*, F f) {
    f("account", member_ptr<&action_trace_ram_delta::account>{});
    f("delta", member_ptr<&action_trace_ram_delta::delta>{});
  }

  struct recurse_action_trace;

  struct action_trace {
    variant_header_zero                dummy;
    variant_header_zero                receipt_dummy;
    abieos::name                       receipt_receiver;
    checksum256                        receipt_act_digest;
    uint64_t                           receipt_global_sequence;
    uint64_t                           receipt_recv_sequence;
    vector<action_trace_auth_sequence> receipt_auth_sequence;
    varuint32                          receipt_code_sequence;
    varuint32                          receipt_abi_sequence;
    abieos::name                       account;
    abieos::name                       name;
    vector<action_trace_authorization> authorization;
    input_buffer                       data;
    bool                               context_free;
    int64_t                            elapsed;
    string                             console;
    vector<action_trace_ram_delta>     account_ram_deltas;
    optional<string>                   except;
    vector<recurse_action_trace>       inline_traces;
  };

  template <typename F>
  constexpr void for_each_field(action_trace*, F f) {
    f("dummy", member_ptr<&action_trace::dummy>{});
    f("receipt_dummy", member_ptr<&action_trace::receipt_dummy>{});
    f("receipt_receiver", member_ptr<&action_trace::receipt_receiver>{});
    f("receipt_act_digest", member_ptr<&action_trace::receipt_act_digest>{});
    f("receipt_global_sequence", member_ptr<&action_trace::receipt_global_sequence>{});
    f("receipt_recv_sequence", member_ptr<&action_trace::receipt_recv_sequence>{});
    f("receipt_auth_sequence", member_ptr<&action_trace::receipt_auth_sequence>{});
    f("receipt_code_sequence", member_ptr<&action_trace::receipt_code_sequence>{});
    f("receipt_abi_sequence", member_ptr<&action_trace::receipt_abi_sequence>{});
    f("account", member_ptr<&action_trace::account>{});
    f("name", member_ptr<&action_trace::name>{});
    f("authorization", member_ptr<&action_trace::authorization>{});
    f("data", member_ptr<&action_trace::data>{});
    f("context_free", member_ptr<&action_trace::context_free>{});
    f("elapsed", member_ptr<&action_trace::elapsed>{});
    f("console", member_ptr<&action_trace::console>{});
    f("account_ram_deltas", member_ptr<&action_trace::account_ram_deltas>{});
    f("except", member_ptr<&action_trace::except>{});
    f("inline_traces", member_ptr<&action_trace::inline_traces>{});
  }

  struct recurse_action_trace : action_trace {};

  bool bin_to_native(recurse_action_trace& obj, bin_to_native_state& state, bool start);
  bool json_to_native(recurse_action_trace& obj, json_to_native_state& state, event_type event, bool start);

  struct recurse_transaction_trace;

  struct transaction_trace {
    variant_header_zero               dummy;
    checksum256                       id;
    transaction_status                status;
    uint32_t                          cpu_usage_us;
    varuint32                         net_usage_words;
    int64_t                           elapsed;
    uint64_t                          net_usage;
    bool                              scheduled;
    vector<action_trace>              action_traces;
    optional<string>                  except;
    vector<recurse_transaction_trace> failed_dtrx_trace;
  };

  template <typename F>
  constexpr void for_each_field(transaction_trace*, F f) {
    f("dummy", member_ptr<&transaction_trace::dummy>{});
    f("transaction_id", member_ptr<&transaction_trace::id>{});
    f("status", member_ptr<&transaction_trace::status>{});
    f("cpu_usage_us", member_ptr<&transaction_trace::cpu_usage_us>{});
    f("net_usage_words", member_ptr<&transaction_trace::net_usage_words>{});
    f("elapsed", member_ptr<&transaction_trace::elapsed>{});
    f("net_usage", member_ptr<&transaction_trace::net_usage>{});
    f("scheduled", member_ptr<&transaction_trace::scheduled>{});
    f("action_traces", member_ptr<&transaction_trace::action_traces>{});
    f("except", member_ptr<&transaction_trace::except>{});
    f("failed_dtrx_trace", member_ptr<&transaction_trace::failed_dtrx_trace>{});
  }

  struct recurse_transaction_trace : transaction_trace {};

  bool bin_to_native(recurse_transaction_trace& obj, bin_to_native_state& state, bool start);
  bool json_to_native(recurse_transaction_trace& obj, json_to_native_state& state, event_type event, bool start);

  struct producer_key {
    name       producer_name;
    public_key block_signing_key;
  };

  template <typename F>
  constexpr void for_each_field(producer_key*, F f) {
    f("producer_name", member_ptr<&producer_key::producer_name>{});
    f("block_signing_key", member_ptr<&producer_key::block_signing_key>{});
  }

  struct extension {
    uint16_t type;
    bytes    data;
  };

  template <typename F>
  constexpr void for_each_field(extension*, F f) {
    f("type", member_ptr<&extension::type>{});
    f("data", member_ptr<&extension::data>{});
  }

  struct producer_schedule {
    uint32_t             version;
    vector<producer_key> producers;
  };

  template <typename F>
  constexpr void for_each_field(producer_schedule*, F f) {
    f("version", member_ptr<&producer_schedule::version>{});
    f("producers", member_ptr<&producer_schedule::producers>{});
  }

  struct transaction_receipt_header {
    uint8_t   status;
    uint32_t  cpu_usage_us;
    varuint32 net_usage_words;
  };

  template <typename F>
  constexpr void for_each_field(transaction_receipt_header*, F f) {
    f("status", member_ptr<&transaction_receipt_header::status>{});
    f("cpu_usage_us", member_ptr<&transaction_receipt_header::cpu_usage_us>{});
    f("net_usage_words", member_ptr<&transaction_receipt_header::net_usage_words>{});
  }

  struct packed_transaction {
    vector<signature> signatures;
    uint8_t           compression;
    bytes             packed_context_free_data;
    bytes             packed_trx;
  };

  template <typename F>
  constexpr void for_each_field(packed_transaction*, F f) {
    f("signatures", member_ptr<&packed_transaction::signatures>{});
    f("compression", member_ptr<&packed_transaction::compression>{});
    f("packed_context_free_data", member_ptr<&packed_transaction::packed_context_free_data>{});
    f("packed_trx", member_ptr<&packed_transaction::packed_trx>{});
  }

  using transaction_variant = variant<checksum256, packed_transaction>;

  struct transaction_receipt : transaction_receipt_header {
    transaction_variant trx;
  };

  template <typename F>
  constexpr void for_each_field(transaction_receipt*, F f) {
    for_each_field((transaction_receipt_header*)nullptr, f);
    f("trx", member_ptr<&transaction_receipt::trx>{});
  }

  struct block_header {
    block_timestamp             timestamp;
    name                        producer;
    uint16_t                    confirmed;
    checksum256                 previous;
    checksum256                 transaction_mroot;
    checksum256                 action_mroot;
    uint32_t                    schedule_version;
    optional<producer_schedule> new_producers;
    vector<extension>           header_extensions;
  };

  template <typename F>
  constexpr void for_each_field(block_header*, F f) {
    f("timestamp", member_ptr<&block_header::timestamp>{});
    f("producer", member_ptr<&block_header::producer>{});
    f("confirmed", member_ptr<&block_header::confirmed>{});
    f("previous", member_ptr<&block_header::previous>{});
    f("transaction_mroot", member_ptr<&block_header::transaction_mroot>{});
    f("action_mroot", member_ptr<&block_header::action_mroot>{});
    f("schedule_version", member_ptr<&block_header::schedule_version>{});
    f("new_producers", member_ptr<&block_header::new_producers>{});
    f("header_extensions", member_ptr<&block_header::header_extensions>{});
  }

  struct signed_block_header : block_header {
    signature producer_signature;
  };

  template <typename F>
  constexpr void for_each_field(signed_block_header*, F f) {
    for_each_field((block_header*)nullptr, f);
    f("producer_signature", member_ptr<&signed_block_header::producer_signature>{});
  }

  struct signed_block : signed_block_header {
    vector<transaction_receipt> transactions;
    vector<extension>           block_extensions;
  };

  template <typename F>
  constexpr void for_each_field(signed_block*, F f) {
    for_each_field((signed_block_header*)nullptr, f);
    f("transactions", member_ptr<&signed_block::transactions>{});
    f("block_extensions", member_ptr<&signed_block::block_extensions>{});
  }


  struct account_object {
    abieos::name         name;
    uint8_t              vm_type;
    uint8_t              vm_version;
    bool                 privileged;
    time_point           last_code_update;
    checksum256          code_version;
    block_timestamp      creation_date;
    bytes                code;
    bytes                abi;
  };

  template <typename F>
  constexpr void for_each_field(account_object*, F f) {
    f("name", member_ptr<&account_object::name>{});
    f("vm_type", member_ptr<&account_object::vm_type>{});
    f("vm_version", member_ptr<&account_object::vm_version>{});
    f("privileged", member_ptr<&account_object::privileged>{});
    f("last_code_update", member_ptr<&account_object::last_code_update>{});
    f("code_version", member_ptr<&account_object::code_version>{});
    f("creation_date", member_ptr<&account_object::creation_date>{});
    f("code", member_ptr<&account_object::code>{});
    f("abi", member_ptr<&account_object::abi>{});
  }
}

namespace abieos {
  template <typename F>
  constexpr void for_each_field(time_point*, F f) {
    f("microseconds", member_ptr<&time_point::microseconds>{});
  }
}
