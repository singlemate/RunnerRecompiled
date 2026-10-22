#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

// Callback delivery worker only, NOT a PM4/graphics command processor.
// Stop/join must be called without holding the loader execution gate.
class MeGpuInterruptWorker {
 public:
    struct Request { uint32_t callback, argument, source, cpu; };
    using Dispatch = std::function<void(const Request&)>;
    ~MeGpuInterruptWorker() { stop_and_join(); }
    MeGpuInterruptWorker() = default;
    MeGpuInterruptWorker(const MeGpuInterruptWorker&) = delete;
    bool start(Dispatch dispatch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ || stopping_ || !dispatch) return false;
        started_ = true;
        try {
            thread_ = std::thread([this, dispatch = std::move(dispatch)] {
                for (;;) {
                    Request request;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                        if (queue_.empty()) return;
                        request = queue_.front(); queue_.pop_front();
                    }
                    try { dispatch(request); }
                    catch (...) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        failure_ = std::current_exception(); stopping_ = true;
                        queue_.clear(); return;
                    }
                }
            });
        } catch (...) { started_ = false; return false; }
        return true;
    }
    bool submit(Request request) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopping_ || !request.callback || (request.callback & 3) ||
            (request.cpu != UINT32_MAX && request.cpu >= 6) || queue_.size() >= 64) return false;
        queue_.push_back(request); changed_.notify_one(); return true;
    }
    // Drains accepted requests unless dispatch faults; faults discard the remainder.
    void stop_and_join() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; changed_.notify_all(); }
        if (thread_.joinable()) thread_.join();
    }
    std::exception_ptr failure() const {
        std::lock_guard<std::mutex> lock(mutex_); return failure_;
    }
 private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Request> queue_;
    std::thread thread_;
    bool started_ = false, stopping_ = false;
    std::exception_ptr failure_;
};
