// strata/threadpool.cpp — jthread worker pool with clean stop_token shutdown (Phase 3).
#include "strata/threadpool.hpp"

#include <map>
#include <memory>

namespace strata {

ThreadPool::ThreadPool(unsigned nthreads) {
    if (nthreads == 0) nthreads = 1;
    workers_.reserve(nthreads);
    for (unsigned i = 0; i < nthreads; ++i)
        workers_.emplace_back([this](std::stop_token st) { worker_loop(st); });
}

ThreadPool::~ThreadPool() {
    // Ask workers to stop, wake them, and join *here* — while m_/cv_ are still alive.
    // (Members are torn down after this body but before workers_; joining now avoids a
    // worker touching a destroyed mutex during shutdown.)
    for (auto& w : workers_) w.request_stop();
    cv_task_.notify_all();
    for (auto& w : workers_)
        if (w.joinable()) w.join();
}

void ThreadPool::worker_loop(std::stop_token st) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lk(m_);
            // condition_variable_any can wait on a stop_token: wakes on stop request too.
            cv_task_.wait(lk, st, [this] { return !tasks_.empty(); });
            if (st.stop_requested() && tasks_.empty()) return;
            job = std::move(tasks_.front());
            tasks_.pop();
        }
        job();
        {
            std::lock_guard lk(m_);
            if (--pending_ == 0) cv_done_.notify_all();
        }
    }
}

void ThreadPool::run(std::vector<std::function<void()>>& tasks) {
    if (tasks.empty()) return;
    {
        std::lock_guard lk(m_);
        pending_ += tasks.size();
        for (auto& t : tasks) tasks_.push(std::move(t));
    }
    cv_task_.notify_all();
    std::unique_lock lk(m_);
    cv_done_.wait(lk, [this] { return pending_ == 0; });
}

ThreadPool& ThreadPool::shared(unsigned nthreads) {
    if (nthreads == 0) nthreads = 1;
    static std::mutex reg_mu;
    static std::map<unsigned, std::unique_ptr<ThreadPool>> registry;
    std::lock_guard lk(reg_mu);
    auto it = registry.find(nthreads);
    if (it == registry.end())
        it = registry.emplace(nthreads, std::make_unique<ThreadPool>(nthreads)).first;
    return *it->second;
}

}  // namespace strata
