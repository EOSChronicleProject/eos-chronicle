#include "receiver_plugin.hpp"

namespace chain_state {

  string to_string(transaction_status status) {
    switch (status) {
    case transaction_status::executed: return "executed";
    case transaction_status::soft_fail: return "soft_fail";
    case transaction_status::hard_fail: return "hard_fail";
    case transaction_status::delayed: return "delayed";
    case transaction_status::expired: return "expired";
    }
    throw runtime_error("unknown status: " + to_string((uint8_t)status));
  }

  bool bin_to_native(transaction_status& status, bin_to_native_state& state, bool) {
    status = transaction_status(read_bin<uint8_t>(state.bin));
    return true;
  }

  bool json_to_native(transaction_status&, json_to_native_state&, event_type, bool) {
    throw error("json_to_native: transaction_status unsupported");
  }

  bool bin_to_native(variant_header_zero&, bin_to_native_state& state, bool) {
    if (read_varuint32(state.bin))
      throw std::runtime_error("unexpected variant value");
    return true;
  }

  bool json_to_native(variant_header_zero&, json_to_native_state&, event_type, bool) {
    return true;
  }

  bool bin_to_native(recurse_action_trace& obj, bin_to_native_state& state, bool start) {
    action_trace& o = obj;
    return bin_to_native(o, state, start);
  }

  bool json_to_native(recurse_action_trace& obj, json_to_native_state& state, event_type event, bool start) {
    action_trace& o = obj;
    return json_to_native(o, state, event, start);
  }

  bool bin_to_native(recurse_transaction_trace& obj, bin_to_native_state& state, bool start) {
    transaction_trace& o = obj;
    return bin_to_native(o, state, start);
  }

  bool json_to_native(recurse_transaction_trace& obj, json_to_native_state& state,
                      event_type event, bool start) {
    transaction_trace& o = obj;
    return json_to_native(o, state, event, start);
  }

}
