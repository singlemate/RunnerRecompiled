#include <rex/thread.h>
#include <rex/thread/timer_queue.h>
#include <rex/main_android.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
static unsigned checks;
static void expect(bool value) { ++checks; if(!value){std::fprintf(stderr,"check %u failed\n",checks);std::exit(1);} }
int main() {
    using namespace std::chrono_literals;
    rex::thread::AndroidInitialize();
    expect(rex::GetAndroidApiLevel()>=35);
    auto event=rex::thread::Event::CreateAutoResetEvent(false);expect(bool(event));
    expect(rex::thread::Wait(event.get(),false,0ms)==rex::thread::WaitResult::kTimeout);
    std::atomic<unsigned> message{0};
    rex::thread::Thread::CreationParameters parameters;parameters.stack_size=256*1024;
    auto worker=rex::thread::Thread::Create(parameters,[&]{message.store(42);event->Set();});expect(bool(worker));
    expect(rex::thread::Wait(event.get(),false,1000ms)==rex::thread::WaitResult::kSuccess);
    expect(rex::thread::Wait(worker.get(),false,1000ms)==rex::thread::WaitResult::kSuccess);
    expect(message.load()==42);
    expect(rex::thread::Wait(event.get(),false,0ms)==rex::thread::WaitResult::kTimeout);
    auto timer=rex::thread::QueueTimerOnce([&](void*){message.store(84);event->Set();},nullptr,std::chrono::steady_clock::now()+20ms);
    expect(rex::thread::Wait(event.get(),false,1000ms)==rex::thread::WaitResult::kSuccess);
    if(auto item=timer.lock())item->Disarm();
    expect(message.load()==84);
    worker.reset();
    std::printf("{\"event\":\"rex_host_services_passed\",\"checks\":%u,\"android_api\":%d}\n",checks,rex::GetAndroidApiLevel());
}
