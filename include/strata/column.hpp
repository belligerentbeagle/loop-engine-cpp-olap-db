// strata/column.hpp
//
// A Column is one contiguous, primitive array — the whole point of a columnar engine.
// A scan touches only the columns a query needs, and each is laid out so the CPU
// prefetcher and cache lines work *for* the loop (see README "Why columnar beats
// row-major"). We keep three physical representations:
//
//   Int64    -> std::vector<int64_t>   (timestamps, ids, and 0/1 flags)
//   Float64  -> std::vector<double>    (cost, cpo)
//   Dict     -> std::vector<int32_t>   codes + a Dictionary (campaign, cat1..9)
//
// The Column object is a small tagged struct that simply holds whichever vector is
// active. Aggregation reaches straight into the raw vector (`.i64`, `.f64`, `.codes`)
// so the hot loop is a plain `for` over a primitive array with no virtual dispatch.
#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "strata/dictionary.hpp"

namespace strata {

enum class DType : std::uint8_t { Int64, Float64, Dict };

inline const char* dtype_name(DType t) {
    switch (t) {
        case DType::Int64:   return "int64";
        case DType::Float64: return "float64";
        case DType::Dict:    return "dict";
    }
    return "?";
}

class Column {
public:
    DType type{DType::Int64};
    std::string name;

    // Exactly one of these is populated, selected by `type`.
    std::vector<std::int64_t> i64;    // DType::Int64
    std::vector<double>       f64;    // DType::Float64
    std::vector<std::int32_t> codes;  // DType::Dict (dictionary codes)
    Dictionary                dict;   // DType::Dict (code <-> string)

    Column() = default;
    Column(std::string n, DType t) : type(t), name(std::move(n)) {}

    std::size_t size() const noexcept {
        switch (type) {
            case DType::Int64:   return i64.size();
            case DType::Float64: return f64.size();
            case DType::Dict:    return codes.size();
        }
        return 0;
    }

    void reserve(std::size_t n) {
        switch (type) {
            case DType::Int64:   i64.reserve(n);   break;
            case DType::Float64: f64.reserve(n);   break;
            case DType::Dict:    codes.reserve(n); break;
        }
    }

    // Parse one text field and append it to this column. Empty/unparseable fields become a
    // type-default (0, 0.0, or the code for ""), keeping every column exactly num_rows long.
    // Shared by the bulk parser and the streaming MemTable flush.
    void append_parsed(std::string_view f) {
        switch (type) {
            case DType::Int64: {
                std::int64_t v = 0;
                std::from_chars(f.data(), f.data() + f.size(), v);
                i64.push_back(v);
                break;
            }
            case DType::Float64: {
                double v = 0.0;
                std::from_chars(f.data(), f.data() + f.size(), v);
                f64.push_back(v);
                break;
            }
            case DType::Dict:
                codes.push_back(dict.encode(f));
                break;
        }
    }

    // Approximate resident bytes of the column payload (for the ingest memory report).
    std::size_t nbytes() const {
        switch (type) {
            case DType::Int64:   return i64.size() * sizeof(std::int64_t);
            case DType::Float64: return f64.size() * sizeof(double);
            case DType::Dict: {
                std::size_t b = codes.size() * sizeof(std::int32_t);
                for (const auto& s : dict.values()) b += s.size() + sizeof(std::string);
                return b;
            }
        }
        return 0;
    }
};

}  // namespace strata
