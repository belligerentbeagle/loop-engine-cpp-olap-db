// strata/memtable.cpp — streaming ingest with a background flush thread (Phase 3).
#include "strata/memtable.hpp"

#include <cstring>

namespace strata {

StreamingTable::StreamingTable(Schema schema, std::size_t flush_threshold, char delimiter,
                               std::size_t queue_capacity)
    : schema_(std::move(schema)),
      delim_(delimiter),
      threshold_(flush_threshold ? flush_threshold : 1),
      queue_(queue_capacity) {
    // Build the columnar store: one column per schema spec, in order. Rows are parsed
    // positionally on flush (field i -> column i).
    for (const auto& s : schema_.specs()) table_.add_column(s.name, s.type);
    table_.set_num_rows(0);
    buffer_.reserve(threshold_);

    // Start the background flush thread.
    flusher_ = std::jthread([this](std::stop_token st) { flush_loop(st); });
}

StreamingTable::~StreamingTable() {
    // Stop the consumer and join it (it drains the queue on stop). Then flush whatever is
    // still buffered directly on this thread — no consumer is running, so it's safe.
    flusher_.request_stop();
    { std::lock_guard lk(wake_mu_); }
    wake_cv_.notify_all();
    if (flusher_.joinable()) flusher_.join();

    if (!buffer_.empty()) {
        apply_batch(buffer_);
        rows_flushed_.fetch_add(buffer_.size(), std::memory_order_release);
        buffer_.clear();
    }
    RowBatch leftover;
    while (queue_.pop(leftover)) {
        apply_batch(leftover);
        rows_flushed_.fetch_add(leftover.size(), std::memory_order_release);
    }
}

// Producer thread only.
void StreamingTable::insert(std::string_view raw_row) {
    buffer_.emplace_back(raw_row);
    rows_inserted_.fetch_add(1, std::memory_order_release);
    if (buffer_.size() >= threshold_) {
        enqueue(std::move(buffer_));
        buffer_.clear();
        buffer_.reserve(threshold_);
    }
}

// Producer-side hand-off. If the ring is momentarily full, nudge the consumer and yield.
void StreamingTable::enqueue(RowBatch&& batch) {
    while (!queue_.push(std::move(batch))) {
        wake_cv_.notify_all();
        std::this_thread::yield();
    }
    { std::lock_guard lk(wake_mu_); }
    wake_cv_.notify_all();
}

void StreamingTable::flush_loop(std::stop_token st) {
    RowBatch batch;
    for (;;) {
        if (queue_.pop(batch)) {
            apply_batch(batch);
            rows_flushed_.fetch_add(batch.size(), std::memory_order_release);
            batches_flushed_.fetch_add(1, std::memory_order_relaxed);
            batch.clear();
            { std::lock_guard lk(wake_mu_); }
            wake_cv_.notify_all();  // wake any flush_sync() waiter
            continue;
        }
        if (st.stop_requested() && queue_.empty()) return;
        std::unique_lock lk(wake_mu_);
        wake_cv_.wait(lk, st, [&] { return !queue_.empty(); });
    }
}

// Parse one batch of raw rows and append them to the columnar store under the writer lock.
void StreamingTable::apply_batch(const RowBatch& batch) {
    const std::size_t ncols = table_.num_cols();
    std::unique_lock lk(table_mu_);
    for (const std::string& line : batch) {
        std::size_t field = 0, fs = 0;
        const char* d = line.data();
        const std::size_t n = line.size();
        for (std::size_t i = 0; i <= n; ++i) {
            if (i == n || d[i] == delim_) {
                if (field < ncols) table_.column(field).append_parsed(std::string_view(d + fs, i - fs));
                ++field;
                fs = i + 1;
            }
        }
        for (std::size_t c = field; c < ncols; ++c)  // pad ragged/short lines
            table_.column(c).append_parsed(std::string_view{});
    }
    table_.set_num_rows(table_.num_rows() + batch.size());
}

void StreamingTable::flush_sync() {
    // Hand off whatever is buffered (producer-side), then wait until it is all visible.
    if (!buffer_.empty()) {
        enqueue(std::move(buffer_));
        buffer_.clear();
        buffer_.reserve(threshold_);
    }
    { std::lock_guard lk(wake_mu_); }
    wake_cv_.notify_all();
    std::unique_lock lk(wake_mu_);
    wake_cv_.wait(lk, [&] {
        return rows_flushed_.load(std::memory_order_acquire) ==
               rows_inserted_.load(std::memory_order_acquire);
    });
}

}  // namespace strata
