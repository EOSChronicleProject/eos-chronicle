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
  
}

namespace abieos {
  template <typename F>
  constexpr void for_each_field(time_point*, F f) {
    f("microseconds", member_ptr<&time_point::microseconds>{});
  }
}

