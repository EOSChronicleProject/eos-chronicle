#include "chain_state_types.hpp"
#include <abieos.h>

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

  bool bin_to_native(transaction_status& obj, bin_to_native_state& state, bool start) {
    uint8_t& o = (uint8_t&) obj;
    return bin_to_native(o, state, start);
  }

  bool json_to_native(transaction_status& obj, json_to_native_state& state, event_type event, bool start) {
    uint8_t& o = (uint8_t&) obj;
    return json_to_native(o, state, event, start);
  }

  bool bin_to_native(variant_header_zero& obj, bin_to_native_state& state, bool start) {
    varuint32 o;
    return bin_to_native(o, state, start);
    return true;
  }

  bool json_to_native(variant_header_zero& obj, json_to_native_state& state, event_type event, bool start) {
    varuint32 o;
    return json_to_native(o, state, event, start);
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
