// strata/dictionary.hpp
//
// Dictionary encoding for low-cardinality categorical columns (campaign, cat1..9).
// We map each distinct string to a small integer code exactly once, then store the
// codes (int32) instead of the strings. The wins, per the design notes:
//   * memory: one int32 per row instead of a heap string,
//   * speed:  GROUP BY becomes array indexing, equality becomes int compare,
//   * cache:  codes are contiguous and SIMD-friendly.
//
// The hot path during ingest is `encode(string_view)`: a find-or-insert. We use a
// C++20 *heterogeneous* (transparent) hash map so the lookup takes a string_view that
// points straight into the mmap'd file — no per-row std::string allocation. Only when
// a genuinely new category appears do we allocate (once).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace strata {

// Transparent hash/eq so unordered_map<std::string,...> can be probed with a
// std::string_view key without constructing a temporary std::string.
struct StringHash {
    using is_transparent = void;
    using hash_type = std::hash<std::string_view>;
    std::size_t operator()(std::string_view sv) const noexcept { return hash_type{}(sv); }
    std::size_t operator()(const std::string& s) const noexcept { return hash_type{}(s); }
    std::size_t operator()(const char* s) const noexcept { return hash_type{}(s); }
};

struct StringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

class Dictionary {
public:
    // Find-or-insert. Returns the stable int32 code for `s`. Hot path (per row).
    std::int32_t encode(std::string_view s) {
        if (auto it = index_.find(s); it != index_.end()) return it->second;
        const auto code = static_cast<std::int32_t>(values_.size());
        values_.emplace_back(s);                 // owns the string (code -> string)
        index_.emplace(values_.back(), code);    // owns a second copy (string -> code)
        return code;
    }

    // Look up without inserting. Returns -1 if the value was never seen. Used by the
    // executor to translate a string predicate ("campaign == 'X'") into a code compare.
    std::int32_t lookup(std::string_view s) const {
        if (auto it = index_.find(s); it != index_.end()) return it->second;
        return -1;
    }

    const std::string& decode(std::int32_t code) const { return values_[static_cast<std::size_t>(code)]; }
    std::size_t size() const noexcept { return values_.size(); }
    const std::vector<std::string>& values() const noexcept { return values_; }

private:
    std::vector<std::string> values_;  // code -> string
    std::unordered_map<std::string, std::int32_t, StringHash, StringEq> index_;  // string -> code
};

}  // namespace strata
