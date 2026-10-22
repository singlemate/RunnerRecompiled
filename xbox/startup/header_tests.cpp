#include "xex_headers.h"
#include <vector>
#include <cstdio>
#include <cstdlib>

static void expect(bool ok) { if (!ok) std::exit(1); }
static void put(std::vector<uint8_t>& data, unsigned at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[at + i] = uint8_t(value >> (24 - i * 8));
}
int main(int argc, char** argv) {
    std::vector<uint8_t> data(128);
    std::memcpy(data.data(), "XEX2", 4); put(data, 8, 128); put(data, 20, 3);
    put(data, 24, 0x10100); put(data, 28, 0x829fab98);
    put(data, 32, 0x10201); put(data, 36, 0x82000000);
    put(data, 40, 0x40002); put(data, 44, 120);
    MeXexHeaders h{data.data(), data.size(), 0x60010000};
    uint32_t value = 0;
    expect(h.valid()); expect(h.lookup(0x10100, value) && value == 0x829fab98);
    expect(h.lookup(0x10201, value) && value == 0x60010024);
    expect(h.lookup(0x40002, value) && value == 0x60010078);
    expect(!h.lookup(0xdeadbeef, value) && value == 0);
    put(data, 44, 124); expect(!h.lookup(0x40002, value));
    put(data, 44, UINT32_MAX); expect(!h.lookup(0x40002, value));
    put(data, 40, 0x3ff); put(data, 44, 120); put(data, 120, 8);
    expect(h.lookup(0x3ff, value) && value == 0x60010078);
    put(data, 120, 9); expect(!h.lookup(0x3ff, value));
    put(data, 120, 0); expect(!h.lookup(0x3ff, value));
    put(data, 20, UINT32_MAX); expect(!h.valid()); put(data, 20, 3);
    put(data, 8, 127); expect(!h.valid()); put(data, 8, 128);
    data[0] = 0; expect(!h.valid()); data[0] = 'X';
    MeXexHeaders short_header{data.data(), 23, 0x60010000}; expect(!short_header.valid());
    MeXexHeaders overflowing{data.data(), 128, 0xfffffff0}; expect(!overflowing.valid());
    std::puts("{\"synthetic_checks\":15,\"passed\":true}");
    if (argc != 2) return 0;
    FILE* file = std::fopen(argv[1], "rb"); expect(file != nullptr);
    expect(std::fseek(file, 0, SEEK_END) == 0);
    long size = std::ftell(file); expect(size >= 24 && size <= 1024 * 1024);
    std::rewind(file); data.resize(size); expect(std::fread(data.data(), 1, size, file) == size); std::fclose(file);
    h = {data.data(), data.size(), 0x60010000}; expect(h.valid());
    for (uint32_t i = 0; i < MeXexHeaders::be32(data.data() + 20); ++i) {
        uint32_t key = MeXexHeaders::be32(data.data() + 24 + i * 8);
        bool found = h.lookup(key, value);
        std::printf("{\"key\":%u,\"found\":%s,\"value\":%u}\n", key, found ? "true" : "false", value);
    }
}
