#include "events.h"
#include <cstdio>
#include <cstdlib>
static unsigned checks;
static void expect(bool value) {
    ++checks;
    if (!value) { std::fprintf(stderr, "check %u failed\n", checks); std::exit(1); }
}
int main() {
    MeEvents events;
    uint32_t automatic = 0, manual = 0, status = 0, previous = 99;
    expect(events.create(2, false, automatic) == 0xc000000d && !automatic);
    expect(events.create(1, false, automatic) == 0 && automatic == 0x10000);
    expect(events.wait(automatic, true, status) && status == 0x102);
    expect(!events.wait(automatic, false, status));
    expect(events.set(automatic, previous) == 0 && previous == 0);
    expect(events.set(automatic, previous) == 0 && previous == 1);
    expect(events.wait(automatic, false, status) && status == 0);
    expect(events.wait(automatic, true, status) && status == 0x102);
    expect(events.create(0, true, manual) == 0 && manual != automatic);
    expect(events.wait(manual, false, status) && status == 0);
    expect(events.wait(manual, false, status) && status == 0);
    expect(events.clear(manual) == 0);
    expect(events.wait(manual, true, status) && status == 0x102);
    expect(events.close(automatic) == 0);
    expect(events.set(automatic, previous) == 0xc0000008);
    expect(events.wait(automatic, true, status) && status == 0xc0000008);
    expect(events.close(automatic) == 0xc0000008);
    expect(events.close(manual + 1) == 0xc0000008);
    uint32_t next = 0;
    expect(events.create(1, true, next) == 0 && next != automatic);
    expect(!events.find(automatic));
    expect(events.find(manual) && !events.find(manual)->signaled);
    for (unsigned i = 3; i < 4096; ++i)
        if (events.create(1, false, next)) return 1;
    expect(events.create(1, false, next) == 0xc000009a);
    std::printf("{\"passed\":true,\"checks\":%u}\n", checks);
}
