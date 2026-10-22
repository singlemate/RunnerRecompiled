#pragma once
#include <mutex>
#include <condition_variable>
#include <cstdint>
// FIFO serialization for cooperative guest execution. This does not preempt a
// running guest function; every unlock transfers to the oldest queued ticket.
class MeExecutionGate {
    std::mutex mutex_;
    std::condition_variable changed_;
    uint64_t next_=0,serving_=0;
public:
    uint64_t queued_waiters(){std::lock_guard<std::mutex> lock(mutex_);return next_>serving_?next_-serving_-1:0;}
    void lock(){std::unique_lock<std::mutex> lock(mutex_);const auto ticket=next_++;changed_.wait(lock,[&]{return serving_==ticket;});}
    void unlock(){std::lock_guard<std::mutex> lock(mutex_);++serving_;changed_.notify_all();}
};
