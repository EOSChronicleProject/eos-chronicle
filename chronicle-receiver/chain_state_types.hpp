#pragma once
#include <string>
#include <memory>
#include <stdexcept>
#include <optional>
#include <abieos.hpp>
#include <fc/reflect/variant.hpp>



namespace chain_state {
  using std::string;
  using std::variant;
  using std::vector;
  using std::to_string;
  using std::runtime_error;
  using std::optional;

  using namespace abieos;

  struct account_object {
    abieos::name         name;
    block_timestamp      creation_date;
    bytes                abi;
  };

  template <typename F>
  constexpr void for_each_field(account_object*, F f) {
    f("name", member_ptr<&account_object::name>{});
    f("creation_date", member_ptr<&account_object::creation_date>{});
    f("abi", member_ptr<&account_object::abi>{});
  }

  // representation of table rows for JSON export

  struct table_row_colval {
    string column;
    string value;
  };

  template <typename F>
  constexpr void for_each_field(struct table_row_colval*, F f) {
    f("column", member_ptr<&table_row_colval::column>{});
    f("value", member_ptr<&table_row_colval::value>{});
  };


  struct table_row {
    bool           added; // false==removed
    abieos::name   code;
    abieos::name   scope;
    abieos::name   table;
    abieos::name   table_payer;
    uint64_t       primary_key;
    abieos::name   row_payer;
    vector<table_row_colval> columns;
  };

  template <typename F>
  constexpr void for_each_field(struct table_row*, F f) {
    f("added", member_ptr<&table_row::added>{});
    f("code", member_ptr<&table_row::code>{});
    f("scope", member_ptr<&table_row::scope>{});
    f("table", member_ptr<&table_row::table>{});
    f("table_payer", member_ptr<&table_row::table_payer>{});
    f("primary_key", member_ptr<&table_row::primary_key>{});
    f("row_payer", member_ptr<&table_row::row_payer>{});
    f("columns", member_ptr<&table_row::columns>{});
  };


  // representation of tables and rows for binary decoding

  struct table_id_object {
    abieos::name   code;
    abieos::name   scope;
    abieos::name   table;
    abieos::name   payer;
  };

  template <typename F>
  constexpr void for_each_field(struct table_id_object*, F f) {
    f("code", member_ptr<&table_id_object::code>{});
    f("scope", member_ptr<&table_id_object::scope>{});
    f("table", member_ptr<&table_id_object::table>{});
    f("payer", member_ptr<&table_id_object::payer>{});
  };

  struct key_value_object {
    abieos::name          code;
    abieos::name          scope;
    abieos::name          table;
    uint64_t              primary_key;
    abieos::name          payer;
    abieos::input_buffer  value;
  };

  template <typename F>
  constexpr void for_each_field(struct key_value_object*, F f) {
    f("code", member_ptr<&table_id_object::code>{});
    f("scope", member_ptr<&table_id_object::scope>{});
    f("table", member_ptr<&table_id_object::table>{});
    f("primary_key", member_ptr<&key_value_object::primary_key>{});
    f("payer", member_ptr<&key_value_object::payer>{});
    f("value", member_ptr<&key_value_object::value>{});
  };

  struct permission_level {
    abieos::name    actor;
    abieos::name    permission;
  };

  template <typename F>
  constexpr void for_each_field(permission_level*, F f) {
    f("actor", abieos::member_ptr<&permission_level::actor>{});
    f("permission", abieos::member_ptr<&permission_level::permission>{});
  }

  using weight_type = uint16_t;

  struct permission_level_weight {
    permission_level  permission;
    weight_type       weight;
  };

  template <typename F>
  constexpr void for_each_field(permission_level_weight*, F f) {
    f("permission", abieos::member_ptr<&permission_level_weight::permission>{});
    f("weight", abieos::member_ptr<&permission_level_weight::weight>{});
  }

  struct key_weight {
    abieos::public_key    key;
    weight_type           weight;
  };

  template <typename F>
  constexpr void for_each_field(key_weight*, F f) {
    f("key", abieos::member_ptr<&key_weight::key>{});
    f("weight", abieos::member_ptr<&key_weight::weight>{});
  }

  struct wait_weight {
    uint32_t     wait_sec;
    weight_type  weight;
  };

  template <typename F>
  constexpr void for_each_field(wait_weight*, F f) {
    f("wait_sec", abieos::member_ptr<&wait_weight::wait_sec>{});
    f("weight", abieos::member_ptr<&wait_weight::weight>{});
  }

  struct shared_authority {
    uint32_t                            threshold;
    vector<key_weight>                  keys;
    vector<permission_level_weight>     accounts;
    vector<wait_weight>                 waits;
  };


  template <typename F>
  constexpr void for_each_field(shared_authority*, F f) {
    f("threshold", abieos::member_ptr<&shared_authority::threshold>{});
    f("keys", abieos::member_ptr<&shared_authority::keys>{});
    f("accounts", abieos::member_ptr<&shared_authority::accounts>{});
    f("waits", abieos::member_ptr<&shared_authority::waits>{});
  }

  struct permission_object {
    abieos::name          owner;
    abieos::name          name;
    abieos::name          parent;
    time_point            last_updated;
    shared_authority      auth;

  };

  template <typename F>
  constexpr void for_each_field(permission_object*, F f) {
    f("owner", abieos::member_ptr<&permission_object::owner>{});
    f("name", abieos::member_ptr<&permission_object::name>{});
    f("parent", abieos::member_ptr<&permission_object::parent>{});
    f("last_updated", abieos::member_ptr<&permission_object::last_updated>{});
    f("auth", abieos::member_ptr<&permission_object::auth>{});
  }

  struct permission_link_object {
    abieos::name    account;
    abieos::name    code;
    abieos::name    message_type;
    abieos::name    required_permission;
  };

  template <typename F>
  constexpr void for_each_field(permission_link_object*, F f) {
    f("account", abieos::member_ptr<&permission_link_object::account>{});
    f("code", abieos::member_ptr<&permission_link_object::code>{});
    f("message_type", abieos::member_ptr<&permission_link_object::message_type>{});
    f("required_permission", abieos::member_ptr<&permission_link_object::required_permission>{});
  }
}
