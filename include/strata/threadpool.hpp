// strata/threadpool.hpp
//
// A persistent worker pool built on std::jthread / std::stop_token (Phase 3). Workers park
// on a condition variable; `run()` dispatches a batch of tasks and blocks until all finish.
// `map_reduce` splits [0,n) into P partitions, runs each on the pool, and merges partials.
//
// False sharing: each partition accumulates into a thread-local result and writes it into a
// cache-line-aligned slot exactly once, so cores never fight over a shared accumulator line
// (README design note "False sharing in the thread pool").
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <new>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace strata {

// std::hardware_destructive_interference_size is optional in libc++; fall back to 64B.
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

class ThreadPool {
public:
    explicit ThreadPool(unsigned nthreads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned size() const noexcept { return static_cast<unsigned>(workers_.size()); }

    // Enqueue `tasks` and block until every one has run.
    void run(std::vector<std::function<void()>>& tasks);

    // Process-wide pool with `nthreads` workers, created once per distinct size and reused
    // (so a thread-scaling benchmark can request 1,2,4,... without re-spawning each call).
    static ThreadPool& shared(unsigned nthreads);

    // Split [0,n) into P partitions and run `fn(lo,hi)` on each in parallel; block until done.
    template <class Fn>
    void for_ranges(std::size_t n, Fn fn, unsigned nparts = 0) {
        unsigned P = nparts ? nparts : size();
        if (P == 0) P = 1;
        if (n == 0) return;
        P = static_cast<unsigned>(std::min<std::size_t>(P, n));
        const std::size_t chunk = (n + P - 1) / P;
        std::vector<std::function<void()>> tasks;
        tasks.reserve(P);
        for (unsigned k = 0; k < P; ++k) {
            const std::size_t lo = static_cast<std::size_t>(k) * chunk;
            const std::size_t hi = std::min(n, lo + chunk);
            if (lo < hi) tasks.emplace_back([lo, hi, &fn] { fn(lo, hi); });
        }
        run(tasks);
    }

    // Split [0,n) into P partitions, run `map(lo,hi) -> T` on each in parallel, then
    // fold the partials left-to-right with `merge(acc, partial)`. Returns the accumulator.
    template <class T, class MapFn, class MergeFn>
    T map_reduce(std::size_t n, MapFn map, MergeFn merge, unsigned nparts = 0) {
        unsigned P = nparts ? nparts : size();
        if (P == 0) P = 1;
        if (n == 0) return map(std::size_t{0}, std::size_t{0});      // identity
        P = static_cast<unsigned>(std::min<std::size_t>(P, n));      // no empty partitions

        struct alignas(kCacheLine) Slot { T v; };
        std::vector<Slot> slots(P);

        const std::size_t chunk = (n + P - 1) / P;
        std::vector<std::function<void()>> tasks;
        tasks.reserve(P);
        for (unsigned k = 0; k < P; ++k) {
            const std::size_t lo = static_cast<std::size_t>(k) * chunk;
            const std::size_t hi = std::min(n, lo + chunk);
            tasks.emplace_back([&slots, k, lo, hi, &map] { slots[k].v = map(lo, hi); });
        }
        run(tasks);

        T acc = slots[0].v;
        for (unsigned k = 1; k < P; ++k) merge(acc, slots[k].v);
        return acc;
    }

private:
    void worker_loop(std::stop_token st);

    std::vector<std::jthread>          workers_;
    std::queue<std::function<void()>>  tasks_;
    std::mutex                         m_;
    std::condition_variable_any        cv_task_;   // workers wait for work
    std::condition_variable            cv_done_;    // run() waits for completion
    std::size_t                        pending_ = 0;
};

}  // namespace strata
