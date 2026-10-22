#include "virtual_memory.h"
#include <cstdio>
#include <cstdlib>
static unsigned calls;
static bool succeeds(uint32_t, uint32_t) { ++calls; return true; }
static bool fails(uint32_t, uint32_t) { return false; }
static unsigned checks;
static void expect(bool ok) { ++checks; if (!ok) { std::fprintf(stderr, "check %u failed\n", checks); std::exit(1); } }
int main() {
    MeVirtualMemory vm; vm.commit_pages = succeeds;
    expect(vm.supports(0x60002000, 4, 0));
    expect(!vm.supports(0x80002000, 4, 0));
    expect(!vm.supports(0x3000, 2, 0));
    uint32_t base = 0, size = 0x100000;
    expect(vm.allocate(base, size, 0x60002000) == 0 && base == 0x10000000 && size == 0x100000 && calls == 0);
    expect(vm.regions[0].committed[0] == 0 && vm.regions[0].page == 65536);
    uint32_t committed = base, amount = 1;
    expect(vm.allocate(committed, amount, 0x1000) == 0 && amount == 65536 && calls == 1);
    expect(vm.regions[0].committed[0] == 1 && vm.regions[0].committed[1] == 0);
    amount = 1; expect(vm.allocate(committed, amount, 0x1000) == 0 && calls == 1);
    committed = base + 0x100000; amount = 65536;
    expect(vm.allocate(committed, amount, 0x1000) == MeVirtualMemory::conflict);
    committed = base; amount = 65536;
    expect(vm.allocate(committed, amount, 0x2000) == MeVirtualMemory::conflict);
    committed = base + 0xf0000; amount = 0x20000;
    expect(vm.allocate(committed, amount, 0x1000) == MeVirtualMemory::conflict);
    MeVirtualMemory fail; fail.commit_pages = fails; base = 0; size = 4096;
    expect(fail.allocate(base, size, 0x3000) == MeVirtualMemory::no_memory && base == 0 && size == 4096 && fail.regions.empty());
    MeVirtualMemory fixed; fixed.commit_pages = succeeds; base = 0x10000001; size = 1;
    expect(fixed.allocate(base, size, 0x2000) == 0 && base == 0x10000000 && size == 4096);
    base = 0; size = 1;
    expect(fixed.allocate(base, size, 0x3000) == 0 && base == 0x10010000 && size == 65536);
    base = 0; size = 0; expect(fixed.allocate(base, size, 0x3000) == MeVirtualMemory::invalid);
    base = 0x3ffff000; size = 8192; expect(fixed.allocate(base, size, 0x2000) == MeVirtualMemory::no_memory);
    MeVirtualMemory freed; freed.commit_pages = succeeds; freed.discard_pages = fails;
    base = 0; size = 8192; expect(freed.allocate(base, size, 0x3000) == 0);
    auto original = base; auto original_size = size;
    expect(freed.free(base, size, 0x8000) == MeVirtualMemory::no_memory && freed.regions.size() == 1 && freed.regions[0].committed[0]);
    freed.discard_pages = succeeds;
    base = original + 1; size = 0;
    expect(freed.free(base, size, 0x8000) == MeVirtualMemory::invalid && freed.regions.size() == 1);
    base = original; size = original_size + 1;
    expect(freed.free(base, size, 0x4000) == MeVirtualMemory::invalid);
    base = original + 1; size = 1;
    expect(freed.free(base, size, 0x4000) == 0 && base == original && size == 4096 && !freed.regions[0].committed[0] && freed.regions[0].committed[1]);
    base = original; size = 4096;
    expect(freed.allocate(base, size, 0x1000) == 0 && freed.regions[0].committed[0]);
    base = original; size = 0;
    expect(freed.free(base, size, 0x8000) == 0 && size == original_size && freed.regions.empty());
    base = 0; size = 1;
    expect(freed.allocate(base, size, 0x3000) == 0 && base == original);
    expect(freed.free(base, size, 0x1000) == MeVirtualMemory::invalid);
    base = original + original_size; size = 0;
    expect(freed.free(base, size, 0x8000) == 0xc00000a0);
    std::printf("{\"passed\":true,\"checks\":%u}\n", checks);
}
