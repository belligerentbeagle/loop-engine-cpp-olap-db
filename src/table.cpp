// strata/table.cpp — Table utilities: schema description and row preview (debugging).
#include "strata/table.hpp"

#include <cstdio>
#include <string>

namespace strata {

namespace {
std::string cell(const Column& c, std::size_t row) {
    switch (c.type) {
        case DType::Int64:   return std::to_string(c.i64[row]);
        case DType::Float64: return std::to_string(c.f64[row]);
        case DType::Dict:    return c.dict.decode(c.codes[row]);
    }
    return "";
}
}  // namespace

// Human-readable schema + cardinality summary, e.g. for a CLI `describe`.
std::string Table_describe(const Table& t) {
    std::string out;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Table: %zu rows x %zu cols, %.1f MB resident\n",
                  t.num_rows(), t.num_cols(), t.resident_bytes() / 1e6);
    out += buf;
    for (const auto& c : t.columns()) {
        if (c.type == DType::Dict)
            std::snprintf(buf, sizeof(buf), "  %-22s %-8s  (%zu distinct)\n",
                          c.name.c_str(), dtype_name(c.type), c.dict.size());
        else
            std::snprintf(buf, sizeof(buf), "  %-22s %-8s\n", c.name.c_str(), dtype_name(c.type));
        out += buf;
    }
    return out;
}

// First `n` rows as tab-separated text (sanity-checking ingest against the source file).
std::string Table_head(const Table& t, std::size_t n) {
    std::string out;
    for (const auto& c : t.columns()) {
        out += c.name;
        out += '\t';
    }
    out += '\n';
    const std::size_t rows = std::min(n, t.num_rows());
    for (std::size_t r = 0; r < rows; ++r) {
        for (const auto& c : t.columns()) {
            out += cell(c, r);
            out += '\t';
        }
        out += '\n';
    }
    return out;
}

}  // namespace strata
