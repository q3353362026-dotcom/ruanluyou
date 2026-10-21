#include "radaer/ue.hpp"
#include <iostream>
#include <random>

using namespace radaer;
size_t checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const ue::Error &) {
        rejected = true;
    }
    check(rejected, "invalid interval accepted");
}
uint64_t reference_read(View bytes, size_t start, unsigned n) {
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i)
        value |= uint64_t((bytes[(start + i) / 8] >> ((start + i) % 8)) & 1) << i;
    return value;
}
Bytes reference_slice(View bytes, size_t start, size_t n) {
    Bytes result((n + 7) / 8);
    for (size_t i = 0; i < n; ++i)
        result[i / 8] |= uint8_t(((bytes[(start + i) / 8] >> ((start + i) % 8)) & 1) << (i % 8));
    return result;
}
int main() {
    try {
        std::mt19937_64 random(0x17345);
        for (size_t size = 0; size <= 18; ++size) {
            Bytes bytes(size);
            for (auto &b : bytes)
                b = uint8_t(random());
            for (size_t start = 0; start <= size * 8; ++start) {
                for (unsigned n = 0; n <= 64; ++n) {
                    ue::Bits r(bytes, size * 8, start);
                    if (n > r.left()) {
                        rejects([&] { r.read(n); });
                        check(r.pos == start, "failed read advanced cursor");
                    } else {
                        // Restrict the logical end, including when more backing bytes exist.
                        r.end = start + n;
                        check(r.read(n) == reference_read(bytes, start, n),
                              "read differs from bit reference");
                        check(r.pos == start + n && !r.left(), "read cursor mismatch");
                        rejects([&] { r.read(1); });
                    }
                }
                for (size_t n = 0; n <= size * 8 - start; ++n) {
                    auto expected = reference_slice(bytes, start, n);
                    check(ue::slice(bytes, start, n) == expected, "slice differs from bit reference");
                    ue::Bits r(bytes, start + n, start);
                    check(r.take(n) == expected && r.pos == start + n, "take mismatch");
                }
            }
            ue::Bits r(bytes, size * 8);
            rejects([&] { r.read(65); });
            rejects([&] { ue::slice(bytes, size * 8 + 1, 0); });
            rejects([&] { ue::slice(bytes, 0, size * 8 + 1); });
            rejects([&] { r.skip(size * 8 + 1); });
            rejects([&] { r.take(size * 8 + 1); });
        }
        for (size_t bits = 0; bits <= 40; ++bits)
            for (size_t count = 0; count <= 128; ++count) {
                Bytes to((bits + 7) / 8), from((count + 7) / 8);
                for (auto &b : to)
                    b = uint8_t(random());
                for (auto &b : from)
                    b = uint8_t(random());
                // Retain the old OR semantics even for nonzero unused bits in the destination.
                auto expected = to;
                expected.resize((bits + count + 7) / 8);
                for (size_t i = 0; i < count; ++i)
                    expected[(bits + i) / 8] |= uint8_t(((from[i / 8] >> (i % 8)) & 1) << ((bits + i) % 8));
                auto length = bits;
                ue::append(to, length, from, count);
                check(to == expected && length == bits + count, "append differs from bit reference");
            }
        Bytes data(20, 0);
        rejects([&] { ue::termination(data); });
        rejects([&] { ue::termination({}); });
        for (size_t bit = 0; bit < data.size() * 8; ++bit) {
            data[bit / 8] |= uint8_t(1u << (bit % 8));
            check(ue::termination(data) == bit, "termination mismatch");
        }
        size_t count = 0;
        rejects([&] { ue::append(data, count, View{}, 1); });
        count = 8 * 1024 * 1024;
        rejects([&] { ue::append(data, count, View(data), 1); });
        std::cout << Json({{"bitBoundaryChecks", checks},
                           {"allOffsetsAndWidths0To64", true},
                           {"sliceAppendMatchSlowReference", true}})
                         .dump()
                  << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
