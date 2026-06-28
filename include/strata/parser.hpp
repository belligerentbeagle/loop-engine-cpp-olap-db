// strata/parser.hpp
//
// Phase 1 ingestion: memory-map a CSV/TSV file and pivot its rows into the columnar
// Table. mmap lets the kernel page the file in lazily and serve hot pages from the page
// cache, so we never buffer the whole 2.4 GB file in user space (README "Why columnar",
// design note "mmap vs read()"). Parsing reads field bytes straight out of the mapping as
// string_views — no per-field std::string is constructed on the hot path.
#pragma once

#include <cstddef>
#include <string>

#include "strata/table.hpp"

namespace strata {

// RAII wrapper around a read-only mmap of a whole file.
class MappedFile {
public:
    explicit MappedFile(const std::string& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept;
    MappedFile& operator=(MappedFile&&) = delete;

    const char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    int         fd_   = -1;
    const char* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace strata
