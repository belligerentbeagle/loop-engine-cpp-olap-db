// strata/executor.hpp
//
// Phase 2: column-at-a-time vectorized execution. Filters and aggregations operate on raw
// contiguous arrays, never on per-row objects. The shapes here are deliberately the ones a
// compiler can auto-vectorize under -O3 -march=native: branchless predicate evaluation into
// a byte mask, then a masked reduction. GROUP BY over a dictionary column becomes indexing
// into a small array of accumulators.
//
//   Executor(table)
//     .filter("click", Cmp::Eq, 1)
//     .group_by("campaign")
//     .agg(Agg::Sum, "cost");
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "strata/table.hpp"

namespace strata {

enum class Agg : std::uint8_t { Count, Sum, Avg, Min, Max };
enum class Cmp : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

const char* agg_name(Agg a);
const char* cmp_name(Cmp c);

// A single filter predicate: `column <cmp> value`. For Dict columns, an Eq/Ne predicate is
// resolved to a dictionary code once at execution time, so the per-row test is an int compare.
struct Predicate {
    std::string         column;
    Cmp                 cmp = Cmp::Eq;
    double              value = 0;            // numeric comparison constant
    std::optional<std::string> str_value;     // set for string equality on a Dict column
};

struct QueryResult {
    bool                     grouped = false;
    std::string              group_col;       // dimension name (if grouped)
    Agg                      agg = Agg::Count;
    std::string              metric_col;       // measure column (empty for Count)

    std::vector<std::string> keys;            // decoded group key, one per group
    std::vector<std::int64_t> keys_num;       // raw numeric key (for numeric/bucketed group-by)
    std::vector<std::int64_t> counts;         // rows per group after filtering
    std::vector<double>      values;          // aggregated measure per group

    // Stats for the milestone numbers.
    double      seconds = 0.0;
    std::size_t rows_scanned = 0;
    std::size_t bytes_scanned = 0;

    double rows_per_sec() const { return seconds > 0 ? rows_scanned / seconds : 0; }
    double gb_per_sec()   const { return seconds > 0 ? (bytes_scanned / 1e9) / seconds : 0; }

    // Sort groups by measure (or count for Count) descending — convenience for "top N".
    void sort_by_value_desc();
    std::string to_string(std::size_t max_rows = 20) const;
};

class Executor {
public:
    explicit Executor(const Table& table) : table_(table) {}

    Executor& filter(const std::string& column, Cmp cmp, double value);
    Executor& filter(const std::string& column, Cmp cmp, std::string_view value);  // Dict
    Executor& filter_eq(const std::string& column, double value) { return filter(column, Cmp::Eq, value); }
    Executor& filter_eq(const std::string& column, std::string_view v) { return filter(column, Cmp::Eq, v); }

    // Group by a Dict column, or a numeric column. `bucket` (>0) buckets a numeric key,
    // e.g. group_by("timestamp", 86400) aggregates per day.
    Executor& group_by(const std::string& column, std::int64_t bucket = 0);

    // Run the query. metric_col is required for Sum/Avg/Min/Max, ignored for Count.
    QueryResult agg(Agg a, const std::string& metric_col = "");
    QueryResult count() { return agg(Agg::Count); }

    // Number of threads to use (Phase 3). 0/1 = single-threaded.
    Executor& threads(unsigned n) { threads_ = n; return *this; }

private:
    const Table&           table_;
    std::vector<Predicate> preds_;
    std::string            group_col_;
    std::int64_t           group_bucket_ = 0;
    unsigned               threads_ = 1;
};

}  // namespace strata
