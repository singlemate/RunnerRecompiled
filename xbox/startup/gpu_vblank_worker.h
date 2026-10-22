#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

// Monotonic 60 Hz virtual display clock, independent of GPU completion.
// Late wakeups skip missed periods rather than delivering a catch-up burst.
struct MeVblankCadence {
    static constexpr int64_t period_ns=16666667;
    int64_t next_ns;
    bool due(int64_t now) {
        if(now<next_ns)return false;
        next_ns+=((now-next_ns)/period_ns+1)*period_ns;return true;
    }
};
class MeGpuVblankWorker {
 public:
    ~MeGpuVblankWorker(){stop_and_join();}
    bool start(std::function<void()> dispatch) {
        std::lock_guard lock(mutex_);
        if(started_ || stopping_ || !dispatch)return false;
        started_=true;
        try {thread_=std::thread([this,dispatch=std::move(dispatch)]{
            using Clock=std::chrono::steady_clock;
            auto now_ns=[] {return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();};
            MeVblankCadence cadence{now_ns()+MeVblankCadence::period_ns};
            std::unique_lock lock(mutex_);
            while(!stopping_){
                changed_.wait_until(lock,Clock::time_point(std::chrono::nanoseconds(cadence.next_ns)),[&]{return stopping_;});
                if(stopping_)break;
                if(!cadence.due(now_ns()))continue;
                lock.unlock();
                try {dispatch();}catch(...){lock.lock();failure_=std::current_exception();stopping_=true;break;}
                lock.lock();
                const auto resumed=now_ns();
                if(cadence.next_ns<=resumed)cadence.next_ns=resumed+MeVblankCadence::period_ns;
            }
        });}catch(...){started_=false;return false;}
        return true;
    }
    // Must not hold the guest gate while joining.
    void stop_and_join(){
        {std::lock_guard lock(mutex_);stopping_=true;changed_.notify_all();}
        if(thread_.joinable())thread_.join();
    }
    std::exception_ptr failure() const {std::lock_guard lock(mutex_);return failure_;}
 private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    bool started_=false,stopping_=false;
    std::exception_ptr failure_;
};
