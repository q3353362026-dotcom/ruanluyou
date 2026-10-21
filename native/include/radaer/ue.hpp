#pragma once
#include "core.hpp"
#include <stdexcept>
namespace radaer::ue {
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Bits {
    View data;
    size_t pos = 0, end = 0;
    Bits(View d, size_t count, size_t start = 0);
    size_t left() const {
        return end - pos;
    }
    uint64_t read(unsigned count);
    bool bit() {
        return read(1) != 0;
    }
    void skip(size_t count);
    uint32_t packed();
    uint64_t integer(uint64_t maximum);
    double floating();
    std::string string();
    Bytes take(size_t count);
};
size_t termination(View data);
Bytes slice(View data, size_t start, size_t count);
void append(Bytes &to, size_t &bits, View from, size_t count);
Json fname(Bits &r);
Json vector(Bits &r, double scale = 1, unsigned maximum = 24);
Json rotation(Bits &r, unsigned width = 8);
struct Bunch {
    uint32_t channel = 0, sequence = 0;
    bool open = false, close = false, reliable = false, partial = false, initial = false, final = false,
         exports = false, must = false;
    unsigned close_reason = 0;
    Json name;
    Bytes payload;
    size_t bits = 0;
    struct Fragment {
        size_t start;
        bool exports, must;
    };
    std::vector<Fragment> fragments;
};
struct Packet {
    bool control = false;
    uint32_t sequence = 0;
    std::vector<Bunch> bunches;
};
Packet packet(View data, bool up);
Json movement(Bits &r);
Json health(Bits &r);
// Channel 3 contains several message families. A nonmatching body returns null silently.
Json self_movement(Bits &r);
uint32_t object(Bits &r, bool exporting, Json &exports, unsigned depth = 0);
void exports(Bits &r, Json &out);
Json prefix(Bits &r, bool has_exports, bool has_must, bool quantized_bit,
            const std::vector<Bunch::Fragment> &fragments = {}, bool identity_only = false);
struct Block {
    bool rep = false, actor = false, deleted = false;
    uint32_t sub = 0, cls = 0;
    size_t start = 0, end = 0;
};
std::vector<Block> blocks(Bits &r, std::string *tail_error = nullptr);
// Embedded catalog data; no Python or external schema lookup at runtime.
Json properties(Bits &r, const std::string &profile);
Json debug_packet(View data, bool up);
} // namespace radaer::ue
