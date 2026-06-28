// strata/table.hpp
//
// The columnar store: a set of equal-length Columns plus a name->index map. This is the
// in-memory layout every query scans. Rows never exist as objects here; a "row" is just
// the same index `i` read out of each column it touches.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "strata/column.hpp"
#include "strata/schema.hpp"

namespace strata {

struct ParseStats {
    std::size_t rows = 0;
    std::size_t bytes = 0;        // bytes of source consumed
    double      seconds = 0.0;
    std::size_t resident_bytes = 0;  // approximate column payload in memory

    double rows_per_sec() const { return seconds > 0 ? rows / seconds : 0; }
    double mb_per_sec()   const { return seconds > 0 ? (bytes / 1e6) / seconds : 0; }
};

struct LoadOptions {
    char        delimiter = '\t';
    bool        has_header = true;
    std::size_t max_rows = 0;   // 0 = no limit
    bool        verbose = false;
};

class Table {
public:
    // mmap + parse a CSV/TSV file into a columnar Table, driven by `schema`.
    // Implemented in src/parser.cpp.
    static Table from_csv(const std::string& path,
                          const Schema& schema,
                          const LoadOptions& opts = {});

    // Convenience: the Criteo schema with tab delimiter.
    static Table from_criteo(const std::string& path, std::size_t max_rows = 0) {
        LoadOptions o;
        o.delimiter = '\t';
        o.max_rows = max_rows;
        return from_csv(path, criteo_schema(), o);
    }

    // ---- schema construction (used by parser and memtable) -------------------------
    // NOTE: the returned reference is invalidated by a subsequent add_column() (the column
    // vector may reallocate). Use it immediately, or re-fetch via column(name) afterwards.
    Column& add_column(const std::string& name, DType type) {
        name_to_idx_[name] = columns_.size();
        columns_.emplace_back(name, type);
        return columns_.back();
    }

    // ---- access --------------------------------------------------------------------
    std::size_t num_rows() const noexcept { return num_rows_; }
    std::size_t num_cols() const noexcept { return columns_.size(); }
    void set_num_rows(std::size_t n) noexcept { num_rows_ = n; }

    bool has_column(std::string_view name) const { return name_to_idx_.contains(std::string(name)); }

    const Column& column(std::string_view name) const { return columns_.at(index_of(name)); }
    Column&       column(std::string_view name)       { return columns_.at(index_of(name)); }

    const Column& column(std::size_t i) const { return columns_.at(i); }
    Column&       column(std::size_t i)       { return columns_.at(i); }

    const std::vector<Column>& columns() const noexcept { return columns_; }

    std::size_t index_of(std::string_view name) const {
        auto it = name_to_idx_.find(std::string(name));
        if (it == name_to_idx_.end())
            throw std::out_of_range("strata::Table: no column named '" + std::string(name) + "'");
        return it->second;
    }

    const ParseStats& stats() const noexcept { return stats_; }
    ParseStats&       stats()       noexcept { return stats_; }

    std::size_t resident_bytes() const {
        std::size_t b = 0;
        for (const auto& c : columns_) b += c.nbytes();
        return b;
    }

private:
    std::vector<Column>                          columns_;
    std::unordered_map<std::string, std::size_t> name_to_idx_;
    std::size_t                                  num_rows_ = 0;
    ParseStats                                   stats_;
};

// Debug/CLI helpers (src/table.cpp).
std::string Table_describe(const Table& t);
std::string Table_head(const Table& t, std::size_t n = 5);

}  // namespace strata
