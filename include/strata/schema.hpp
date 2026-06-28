// strata/schema.hpp
//
// A Schema maps column *names* to physical types. The parser is header-driven: it reads
// the TSV header row, then for each named column looks up its type here. Columns present
// in the file but absent from the schema are skipped (the field is stepped over, never
// allocated); columns in the schema but absent from the file are simply not built. This
// makes the real Criteo file and the synthetic generator interchangeable, and tolerant of
// column-order differences between mirrors.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "strata/column.hpp"

namespace strata {

struct ColumnSpec {
    std::string name;
    DType       type;
};

class Schema {
public:
    Schema() = default;
    Schema(std::initializer_list<ColumnSpec> specs) {
        for (const auto& s : specs) add(s.name, s.type);
    }

    Schema& add(std::string name, DType type) {
        index_[name] = specs_.size();
        specs_.push_back({std::move(name), type});
        return *this;
    }

    // Returns the type for `name`, or nullptr if the column is not in the schema.
    const DType* find(std::string_view name) const {
        if (auto it = index_.find(std::string(name)); it != index_.end())
            return &specs_[it->second].type;
        return nullptr;
    }

    const std::vector<ColumnSpec>& specs() const noexcept { return specs_; }
    bool empty() const noexcept { return specs_.empty(); }

private:
    std::vector<ColumnSpec>                 specs_;
    std::unordered_map<std::string, std::size_t> index_;
};

// The Criteo "Attribution Modeling for Bidding" schema. Tab-separated, header row present.
// Binary flags and ids are kept as Int64 (uniform numeric path, trivially vectorizable);
// cost/cpo are Float64; campaign and the contextual cat1..cat9 are dictionary-encoded.
// `uid` is parsed as Int64 (an anonymized numeric id) but isn't used by any demo query.
inline Schema criteo_schema() {
    return Schema{
        {"timestamp",             DType::Int64},
        {"uid",                   DType::Int64},
        {"campaign",              DType::Dict},
        {"conversion",            DType::Int64},
        {"conversion_timestamp",  DType::Int64},
        {"conversion_id",         DType::Int64},
        {"attribution",           DType::Int64},
        {"click",                 DType::Int64},
        {"cost",                  DType::Float64},
        {"cpo",                   DType::Float64},
        {"time_since_last_click", DType::Int64},
        {"click_pos",             DType::Int64},
        {"click_nb",              DType::Int64},
        {"cat1",                  DType::Dict},
        {"cat2",                  DType::Dict},
        {"cat3",                  DType::Dict},
        {"cat4",                  DType::Dict},
        {"cat5",                  DType::Dict},
        {"cat6",                  DType::Dict},
        {"cat7",                  DType::Dict},
        {"cat8",                  DType::Dict},
        {"cat9",                  DType::Dict},
    };
}

}  // namespace strata
