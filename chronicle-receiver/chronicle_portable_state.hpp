#include <fc/reflect/reflect.hpp>
#include <eosio/ship_protocol.hpp>

namespace chronicle_portable_state {

  const uint32_t MIN_VERSION_SUPPORTED = 0;
  const uint32_t MAX_VERSION_SUPPORTED = 0;
  const uint32_t CURRENT_VERSION = 0;
  const uint64_t MAGIC_NUMBER = 0x67c7d12ce9fc56c1;

  using checksum256 = eosio::checksum256;

  struct portable_format_version {
    uint32_t    format_version;
    std::string chronicle_version;
  };

  struct state_object_v0 {
    uint32_t    head;
    checksum256 head_id;
    uint32_t    irreversible;
    checksum256 irreversible_id;
  };


  struct received_block_object_v0 {
    uint32_t     block_index;
    checksum256  block_id;
  };


  struct contract_abi_object_v0 {
    uint64_t                  account;
    std::vector<char>         abi;
  };


  struct contract_abi_history_v0 {
    uint64_t                  account;
    uint32_t                  block_index;
    std::vector<char>         abi;
  };

  struct stream_end_v0 {
    uint64_t                  magic_number;
  };

  using row = std::variant<state_object_v0, received_block_object_v0, contract_abi_object_v0, contract_abi_history_v0,
                           stream_end_v0>;
}


FC_REFLECT(eosio::checksum256, (value));
FC_REFLECT(chronicle_portable_state::portable_format_version, (format_version)(chronicle_version));
FC_REFLECT(chronicle_portable_state::state_object_v0, (head)(head_id)(irreversible)(irreversible_id));
FC_REFLECT(chronicle_portable_state::received_block_object_v0, (block_index)(block_id));
FC_REFLECT(chronicle_portable_state::contract_abi_object_v0, (account)(abi));
FC_REFLECT(chronicle_portable_state::contract_abi_history_v0, (account)(block_index)(abi));
FC_REFLECT(chronicle_portable_state::stream_end_v0, (magic_number));
