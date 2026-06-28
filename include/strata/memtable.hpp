// strata/memtable.hpp
//
// Phase 3 ingestion-while-querying. Incoming events land in a row-oriented buffer (the
// MemTable). When it reaches a threshold, the batch is handed to a background flush thread
// over a lock-free single-producer/single-consumer queue; the flush thread pivots the rows
// into the columnar store. Queries run against the columnar store the whole time, guarded
// by a shared_mutex (many readers, one appending writer). Clean shutdown via std::stop_token.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "strata/table.hpp"
#include "strata/threadpool.hpp"  // kCacheLine

namespace strata {

// Lock-free bounded ring buffer for one producer + one consumer. The hot push/pop paths use
// only atomic loads/stores with acquire/release ordering — no mutex. head_/tail_ sit on
// separate cache lines so the two threads don't ping-pong a shared line.
template <class T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity) : buf_(capacity ? capacity : 2), cap_(buf_.size()) {}

    bool push(T&& v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t next = inc(t);
        if (next == head_.load(std::memory_order_acquire)) return false;  // full
        buf_[t] = std::move(v);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;  // empty
        out = std::move(buf_[h]);
        head_.store(inc(h), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    std::size_t inc(std::size_t i) const { return (i + 1 == cap_) ? 0 : i + 1; }

    std::vector<T>                        buf_;
    std::size_t                           cap_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer index
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer index
};

// A batch of raw delimited rows awaiting flush (the row-oriented MemTable hand-off unit).
using RowBatch = std::vector<std::string>;

class StreamingTable {
public:
    // `schema` defines the columns (in order). `flush_threshold` rows buffer before a batch
    // is handed off. The background flush thread starts immediately.
    explicit StreamingTable(Schema schema, std::size_t flush_threshold = 10000,
                            char delimiter = '\t', std::size_t queue_capacity = 256);
    ~StreamingTable();

    StreamingTable(const StreamingTable&) = delete;
    StreamingTable& operator=(const StreamingTable&) = delete;

    // Producer side. Buffer one raw row; hands off a batch when the threshold is hit.
    void insert(std::string_view raw_row);

    // Drain everything buffered/in-flight and block until it is visible in the store.
    void flush_sync();

    std::size_t visible_rows() const { return rows_flushed_.load(std::memory_order_acquire); }
    std::size_t pending_rows() const {
        return rows_inserted_.load(std::memory_order_acquire) - rows_flushed_.load(std::memory_order_acquire);
    }
    std::size_t batches_flushed() const { return batches_flushed_.load(std::memory_order_relaxed); }

    // Run `fn(const Table&)` against the flushed columnar store under a shared lock, so it is
    // safe to query concurrently with ingest. Returns whatever `fn` returns.
    template <class Fn>
    auto query(Fn&& fn) const {
        std::shared_lock lk(table_mu_);
        return fn(table_);
    }

private:
    void enqueue(RowBatch&& batch);     // hand a batch to the flush thread
    void flush_loop(std::stop_token st);
    void apply_batch(const RowBatch& batch);  // parse+append under the writer lock

    Schema                    schema_;
    char                      delim_;
    std::size_t               threshold_;

    Table                     table_;                 // the columnar store (the queryable side)
    mutable std::shared_mutex table_mu_;              // readers share; flush takes unique

    RowBatch                  buffer_;                 // current producer-side MemTable
    SpscRing<RowBatch>        queue_;                  // producer -> flush hand-off (lock-free)

    std::mutex                wake_mu_;                // only for parking the idle flush thread
    std::condition_variable_any wake_cv_;

    std::atomic<std::size_t>  rows_inserted_{0};
    std::atomic<std::size_t>  rows_flushed_{0};
    std::atomic<std::size_t>  batches_flushed_{0};

    std::jthread              flusher_;                // declared last: stops/joins first
};

}  // namespace strata
