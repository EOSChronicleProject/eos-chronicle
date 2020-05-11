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


  // representation of table rows for JSON export

  struct table_row_colval {
    string column;
    string value;
  };

  EOSIO_REFLECT(table_row_colval, column, value);

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

  EOSIO_REFLECT(table_row, added, code, scope, table, table_payer, primary_key, row_payer, columns);
}
