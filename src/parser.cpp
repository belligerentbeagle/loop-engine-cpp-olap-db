// strata/parser.cpp — mmap + TSV/CSV parsing, row -> column pivot (Phase 1).
#include "strata/parser.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace strata {

// ----------------------------------------------------------------------------------
// MappedFile
// ----------------------------------------------------------------------------------
MappedFile::MappedFile(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("strata: cannot open '" + path + "': " + std::strerror(errno));

    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        ::close(fd_);
        throw std::runtime_error("strata: fstat failed on '" + path + "'");
    }
    size_ = static_cast<std::size_t>(st.st_size);

    if (size_ == 0) {  // empty file: nothing to map
        data_ = nullptr;
        return;
    }

    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) {
        ::close(fd_);
        throw std::runtime_error("strata: mmap failed on '" + path + "'");
    }
    data_ = static_cast<const char*>(p);

    // We read front-to-back exactly once: tell the kernel so it can read ahead
    // aggressively and drop pages behind us.
    ::madvise(const_cast<char*>(data_), size_, MADV_SEQUENTIAL);
    ::madvise(const_cast<char*>(data_), size_, MADV_WILLNEED);
}

MappedFile::~MappedFile() {
    if (data_) ::munmap(const_cast<char*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
}

MappedFile::MappedFile(MappedFile&& o) noexcept : fd_(o.fd_), data_(o.data_), size_(o.size_) {
    o.fd_ = -1;
    o.data_ = nullptr;
    o.size_ = 0;
}

// ----------------------------------------------------------------------------------
// Parsing helpers
// ----------------------------------------------------------------------------------
namespace {

// A mapped target: which table column (if any) a given file field feeds.
struct FieldTarget {
    int col = -1;  // index into Table::columns(), or -1 to skip this field
};

}  // namespace

// ----------------------------------------------------------------------------------
// Table::from_csv
// ----------------------------------------------------------------------------------
Table Table::from_csv(const std::string& path, const Schema& schema, const LoadOptions& opts) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    MappedFile mf(path);
    const char* const base = mf.data();
    const char* p = base;
    const char* const end = base + mf.size();
    const char delim = opts.delimiter;

    Table table;
    std::vector<FieldTarget> targets;  // file-field-position -> table column

    auto next_line = [&](const char*& cur) -> std::string_view {
        const char* ls = cur;
        const char* nl = static_cast<const char*>(std::memchr(cur, '\n', static_cast<std::size_t>(end - cur)));
        const char* le = nl ? nl : end;
        cur = nl ? nl + 1 : end;
        std::size_t len = static_cast<std::size_t>(le - ls);
        if (len && ls[len - 1] == '\r') --len;  // tolerate CRLF
        return {ls, len};
    };

    // ---- header: decide each file field's target column ----------------------------
    if (opts.has_header) {
        if (p >= end) return table;  // empty file
        std::string_view header = next_line(p);
        std::size_t fs = 0;
        for (std::size_t i = 0; i <= header.size(); ++i) {
            if (i == header.size() || header[i] == delim) {
                std::string_view name = header.substr(fs, i - fs);
                if (const DType* t = schema.find(name)) {
                    Column& c = table.add_column(std::string(name), *t);
                    targets.push_back({static_cast<int>(&c - table.columns().data())});
                } else {
                    targets.push_back({-1});  // present in file, absent from schema -> skip
                }
                fs = i + 1;
            }
        }
    } else {
        // Headerless: schema specs define the columns positionally.
        for (const auto& s : schema.specs()) {
            Column& c = table.add_column(s.name, s.type);
            targets.push_back({static_cast<int>(&c - table.columns().data())});
        }
    }

    // add_column can reallocate the column vector; resolve targets to stable pointers now.
    std::vector<Column*> col_ptr(targets.size(), nullptr);
    for (std::size_t f = 0; f < targets.size(); ++f)
        if (targets[f].col >= 0) col_ptr[f] = &table.column(static_cast<std::size_t>(targets[f].col));

    const std::size_t ncols = table.num_cols();
    // Rough reserve to avoid repeated reallocations. ~110 bytes/row is typical for this
    // schema; vectors still grow (amortized) if the guess is low. Capped by max_rows.
    if (mf.size() > 0) {
        std::size_t approx_rows = mf.size() / 110 + 1;
        if (opts.max_rows) approx_rows = std::min(approx_rows, opts.max_rows);
        for (std::size_t ci = 0; ci < ncols; ++ci) table.column(ci).reserve(approx_rows);
    }

    // ---- body: split each line, feed mapped fields ---------------------------------
    std::size_t row = 0;
    std::vector<std::int8_t> seen(ncols);  // which columns got a value this row
    while (p < end) {
        std::string_view line = next_line(p);
        if (line.empty()) continue;

        std::memset(seen.data(), 0, seen.size());
        std::size_t field = 0;
        std::size_t fs = 0;
        const char* d = line.data();
        const std::size_t n = line.size();
        for (std::size_t i = 0; i <= n; ++i) {
            if (i == n || d[i] == delim) {
                if (field < col_ptr.size()) {
                    if (Column* c = col_ptr[field]) {
                        c->append_parsed(std::string_view(d + fs, i - fs));
                        seen[static_cast<std::size_t>(targets[field].col)] = 1;
                    }
                }
                ++field;
                fs = i + 1;
            }
        }
        // Defensive: pad any column that didn't receive a field (short/ragged line).
        for (std::size_t ci = 0; ci < ncols; ++ci)
            if (!seen[ci]) table.column(ci).append_parsed(std::string_view{});

        if (++row == opts.max_rows) break;
    }

    table.set_num_rows(row);

    const auto t1 = clock::now();
    ParseStats& s = table.stats();
    s.rows = row;
    s.bytes = mf.size();
    s.seconds = std::chrono::duration<double>(t1 - t0).count();
    s.resident_bytes = table.resident_bytes();

    if (opts.verbose) {
        std::fprintf(stderr,
                     "[strata] ingested %zu rows from %s in %.2fs | %.1f Mrows/s | %.0f MB/s | %.0f MB resident\n",
                     s.rows, path.c_str(), s.seconds, s.rows_per_sec() / 1e6, s.mb_per_sec(),
                     s.resident_bytes / 1e6);
    }
    return table;
}

}  // namespace strata
