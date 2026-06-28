// strata/sql.hpp
//
// A tiny SQL-ish frontend so queries can be strings instead of builder calls (README
// stretch goal). It parses a practical subset and maps it onto the existing Executor:
//
//   SELECT [<dim>,] <agg> FROM <table>
//   [WHERE <col> <op> <val> [AND <col> <op> <val> ...]]
//   [GROUP BY <col>]
//   [ORDER BY <col|N> [ASC|DESC]]
//   [LIMIT <n>]
//
//   <agg> := COUNT(*) | COUNT(col) | SUM(col) | AVG(col) | MIN(col) | MAX(col)
//   <op>  := = | == | != | <> | < | <= | > | >=        (conditions are AND-only)
//
// Examples:
//   SELECT COUNT(*) FROM events WHERE click = 1
//   SELECT campaign, SUM(cost) FROM events WHERE click = 1 GROUP BY campaign ORDER BY 2 DESC LIMIT 10
//   SELECT cat1, AVG(cpo) FROM events WHERE conversion = 1 GROUP BY cat1
//
// Not supported (and rejected with a clear message): joins, OR/NOT, projections without an
// aggregate, HAVING, sub-selects. The table name after FROM is accepted but ignored — a Strata
// query always runs against the one table you call .sql() on.
#pragma once

#include <string>

#include "strata/executor.hpp"
#include "strata/table.hpp"

namespace strata {

// Parse and run `sql` against `table`. ORDER BY / LIMIT are applied to the result (the
// returned QueryResult is already sorted and truncated). Throws std::runtime_error with a
// human-readable message on a parse or binding error. `threads` sets aggregation parallelism.
QueryResult run_sql(const Table& table, const std::string& sql, unsigned threads = 1);

}  // namespace strata
