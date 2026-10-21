#include "radaer/ue.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace radaer::ue {
Bits::Bits(View d, size_t count, size_t start) : data(d), pos(start), end(count) {
    if (count > d.size() * 8 || start > count)
        throw Error("invalid bit interval");
}
uint64_t Bits::read(unsigned n) {
    if (n > 64 || n > left())
        throw Error("truncated bitstream at bit " + std::to_string(pos));
    if (!n)
        return 0;
    size_t byte = pos / 8;
    unsigned offset = unsigned(pos % 8);
    uint64_t v = data[byte] >> offset;
    // A 64-bit unaligned read can span nine bytes. Only load bytes within the checked interval.
    for (unsigned filled = 8 - offset; filled < n; filled += 8)
        v |= uint64_t(data[++byte]) << filled;
    pos += n;
    return n == 64 ? v : v & ((uint64_t(1) << n) - 1);
}
void Bits::skip(size_t n) {
    if (n > left())
        throw Error("truncated skipped field");
    pos += n;
}
uint32_t Bits::packed() {
    uint64_t v = 0;
    for (unsigned i = 0; i < 5; i++) {
        auto b = read(8);
        v |= (b >> 1) << (i * 7);
        if (v > UINT32_MAX)
            throw Error("packed integer overflow");
        if (!(b & 1))
            return uint32_t(v);
    }
    throw Error("unterminated packed integer");
}
uint64_t Bits::integer(uint64_t maximum) {
    if (maximum < 1 || maximum > (uint64_t(1) << 32))
        throw Error("invalid serialized integer maximum");
    uint64_t v = 0;
    for (uint64_t mask = 1; v + mask < maximum; mask <<= 1)
        if (bit())
            v |= mask;
    return v;
}
double Bits::floating() {
    return std::bit_cast<float>(uint32_t(read(32)));
}
namespace {
void utf8(std::string &s, uint32_t c) {
    if (c < 0x80)
        s += char(c);
    else if (c < 0x800) {
        s += char(0xc0 | (c >> 6));
        s += char(0x80 | (c & 63));
    } else if (c < 0x10000) {
        s += char(0xe0 | (c >> 12));
        s += char(0x80 | ((c >> 6) & 63));
        s += char(0x80 | (c & 63));
    } else {
        s += char(0xf0 | (c >> 18));
        s += char(0x80 | ((c >> 12) & 63));
        s += char(0x80 | ((c >> 6) & 63));
        s += char(0x80 | (c & 63));
    }
}
} // namespace
std::string Bits::string() {
    auto count = std::bit_cast<int32_t>(uint32_t(read(32)));
    if (count > 4096 || count < -4096)
        throw Error("FString size exceeds limit");
    if (!count)
        return {};
    std::string s;
    if (count > 0) {
        auto raw = take(size_t(count) * 8);
        if (raw.back())
            throw Error("FString missing terminator");
        s.assign(raw.begin(), raw.end() - 1);
        // Positive-length network FStrings are UTF-8 in the recovered protocol.
        // Validate now, before retaining a name or path in decoder state.
        try {
            (void)Json(s).dump();
        } catch (const Json::exception &) {
            throw Error("invalid UTF8 FString");
        }
    } else {
        const auto units = unsigned(-count);
        for (unsigned i = 0; i < units; i++) {
            uint32_t c = uint32_t(read(16));
            if (i + 1 == units) {
                if (c)
                    throw Error("UTF16 FString missing terminator");
                break;
            }
            if (c >= 0xd800 && c <= 0xdbff) {
                if (i + 2 >= units)
                    throw Error("truncated UTF16 surrogate");
                auto low = uint32_t(read(16));
                ++i;
                if (low < 0xdc00 || low > 0xdfff)
                    throw Error("invalid UTF16 surrogate");
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
            } else if (c >= 0xdc00 && c <= 0xdfff)
                throw Error("unpaired UTF16 surrogate");
            utf8(s, c);
        }
    }
    return s;
}
Bytes Bits::take(size_t n) {
    if (n > left())
        throw Error("truncated bit payload");
    auto b = slice(data, pos, n);
    pos += n;
    return b;
}
size_t termination(View d) {
    for (size_t i = d.size(); i; i--)
        if (d[i - 1])
            return (i - 1) * 8 + std::bit_width(unsigned(d[i - 1])) - 1;
    throw Error("missing termination bit");
}
Bytes slice(View d, size_t start, size_t count) {
    if (start > d.size() * 8 || count > d.size() * 8 - start)
        throw Error("invalid slice");
    Bytes b((count + 7) / 8);
    size_t first = start / 8;
    unsigned shift = unsigned(start % 8);
    if (!shift)
        std::copy_n(d.begin() + first, b.size(), b.begin());
    else
        for (size_t i = 0; i < b.size(); ++i) {
            unsigned v = d[first + i] >> shift;
            if (first + i + 1 < d.size())
                v |= unsigned(d[first + i + 1]) << (8 - shift);
            b[i] = uint8_t(v);
        }
    if (count % 8)
        b.back() &= uint8_t((1u << (count % 8)) - 1);
    return b;
}
void append(Bytes &to, size_t &bits, View from, size_t count) {
    if (count > from.size() * 8 || bits > 8 * 1024 * 1024 || count > 8 * 1024 * 1024 - bits)
        throw Error("partial bunch exceeds 1 MiB");
    to.resize((bits + count + 7) / 8);
    size_t first = bits / 8;
    unsigned shift = unsigned(bits % 8);
    for (size_t i = 0; i < (count + 7) / 8; ++i) {
        unsigned v = from[i];
        if (i == count / 8 && count % 8)
            v &= (1u << (count % 8)) - 1;
        to[first + i] |= uint8_t(v << shift);
        if (shift && first + i + 1 < to.size())
            to[first + i + 1] |= uint8_t(v >> (8 - shift));
    }
    bits += count;
}
Json fname(Bits &r) {
    auto start = r.pos;
    Json j = {{"hardcoded_index", nullptr}, {"text", nullptr}, {"number", nullptr}};
    if (r.bit()) {
        auto index = r.packed();
        if (index > 604)
            throw Error("FName index exceeds catalog");
        j["hardcoded"] = true;
        j["hardcoded_index"] = index;
    } else {
        auto text = r.string();
        auto number = std::bit_cast<int32_t>(uint32_t(r.read(32)));
        j["hardcoded"] = false;
        j["text"] = text;
        j["number"] = number;
    }
    j["interval"] = {start, r.pos};
    return j;
}
Json vector(Bits &r, double scale, unsigned maximum) {
    auto h = r.integer(maximum);
    auto bias = int64_t(1) << (h + 1);
    Json v = Json::array();
    for (int i = 0; i < 3; i++)
        v.push_back((int64_t(r.read(unsigned(h + 2))) - bias) / scale);
    return v;
}
Json rotation(Bits &r, unsigned width) {
    Json v = Json::array();
    for (int i = 0; i < 3; i++)
        v.push_back(r.bit() ? r.read(width) * 360.0 / double(uint64_t(1) << width) : 0.0);
    return v;
}
Packet packet(View data, bool up) {
    if (data.size() > 65535)
        throw Error("oversize datagram");
    Bits outer(data, termination(data));
    Packet result;
    if (outer.bit()) {
        result.control = true;
        return result;
    }
    auto notify = uint32_t(outer.read(32));
    result.sequence = (notify >> 18) & 16383;
    outer.skip(((notify & 15) + 1) * 32);
    auto body = outer.take(outer.left());
    Bits r(body, termination(body));
    bool time = r.bit();
    if (time && !up)
        r.skip(8);
    r.skip(8);
    while (r.left()) {
        if (result.bunches.size() >= 256)
            throw Error("bunch count limit");
        Bunch b;
        if (r.bit()) {
            b.open = r.bit();
            b.close = r.bit();
            if (b.close)
                b.close_reason = unsigned(r.integer(16));
        }
        r.bit();
        b.reliable = r.bit();
        r.bit();
        b.channel = r.packed();
        if (b.channel >= 32767)
            throw Error("channel index limit");
        b.exports = r.bit();
        b.must = r.bit();
        b.partial = r.bit();
        if (b.reliable)
            b.sequence = uint32_t(r.integer(1024));
        if (b.partial) {
            b.initial = r.bit();
            b.final = r.bit();
        }
        if (b.open || b.reliable)
            b.name = fname(r);
        b.bits = size_t(r.integer(8192));
        // Never promote truncated packets into apparently valid partial chains.
        b.payload = r.take(b.bits);
        b.fragments.push_back({0, b.exports, b.must});
        result.bunches.push_back(std::move(b));
    }
    return result;
}
Json debug_packet(View d, bool up) {
    auto p = packet(d, up);
    Json out = {{"control", p.control}, {"sequence", p.sequence}, {"bunches", Json::array()}};
    for (auto &b : p.bunches)
        out["bunches"].push_back({{"channel", b.channel},
                                  {"sequence", b.sequence},
                                  {"open", b.open},
                                  {"close", b.close},
                                  {"reliable", b.reliable},
                                  {"partial", b.partial},
                                  {"initial", b.initial},
                                  {"final", b.final},
                                  {"exports", b.exports},
                                  {"must", b.must},
                                  {"bits", b.bits},
                                  {"payload", hex(b.payload)}});
    return out;
}
} // namespace radaer::ue
