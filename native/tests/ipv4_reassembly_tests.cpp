#include "radaer/ipv4_reassembly.hpp"
#include "radaer/shared_network.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace radaer;
namespace {
unsigned checks = 0;
void check(bool value, const char *why) {
    ++checks;
    if (!value)
        throw std::runtime_error(why);
}
void put16(Bytes &b, size_t p, size_t v) {
    b[p] = uint8_t(v >> 8);
    b[p + 1] = uint8_t(v);
}
Bytes packet(unsigned id = 7, size_t payload = 61, size_t header = 20) {
    Bytes b(header + 8 + payload, 0);
    b[0] = uint8_t(0x40 | header / 4);
    b[8] = 64;
    b[9] = 17;
    b[12] = 192;
    b[13] = 0;
    b[14] = 2;
    b[15] = 1;
    b[16] = 198;
    b[17] = 51;
    b[18] = 100;
    b[19] = 2;
    put16(b, 2, b.size());
    put16(b, 4, id);
    put16(b, header, 40000);
    put16(b, header + 2, 29982);
    put16(b, header + 4, payload + 8);
    for (size_t i = header + 8; i < b.size(); ++i)
        b[i] = uint8_t(i * 37);
    return b;
}
Bytes fragment(const Bytes &b, size_t offset, size_t count, bool more, bool copy_options = true) {
    const size_t original_header = (b[0] & 15) * 4, header = copy_options ? original_header : 20;
    Bytes out(b.begin(), b.begin() + header);
    out[0] = uint8_t(0x40 | header / 4);
    out.insert(out.end(), b.begin() + original_header + offset, b.begin() + original_header + offset + count);
    put16(out, 2, out.size());
    put16(out, 6, (more ? 0x2000 : 0) | offset / 8);
    return out;
}
void verify(const Ipv4UdpReassembler::Result &r, const Bytes &b, unsigned parts) {
    check(r.status == Ipv4UdpReassembler::Status::complete, "datagram completes");
    auto t = parse_ipv4_transport(r.packet), expected = parse_ipv4_transport(b);
    check(t && expected && t->source_port == expected->source_port &&
              t->destination_port == expected->destination_port &&
              t->payload_length == expected->payload_length,
          "reassembled UDP endpoints and length");
    check(std::equal(r.packet.begin() + t->payload_offset, r.packet.end(),
                     b.begin() + expected->payload_offset),
          "all payload bytes preserved");
    check(r.fragments == parts, "unique fragment count");
    unsigned sum = 0;
    for (size_t i = 0; i < t->ip_length; i += 2)
        sum += (unsigned(r.packet[i]) << 8) | r.packet[i + 1];
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    check(sum == 65535, "reconstructed IPv4 header checksum");
}
} // namespace
int main() {
    try {
        using Status = Ipv4UdpReassembler::Status;
        const auto b = packet();
        const auto first = fragment(b, 0, 24, true), middle = fragment(b, 24, 24, true),
                   last = fragment(b, 48, 21, false);
        std::array<Bytes, 3> parts{first, middle, last};
        std::array<unsigned, 3> order{0, 1, 2};
        do {
            Ipv4UdpReassembler r;
            check(r.push(parts[order[0]], 0, 1000).status == Status::pending, "first arrival pending");
            check(r.push(parts[order[0]], 1, 1001).status == Status::pending, "duplicate pending");
            check(r.push(parts[order[1]], 2, 1002).status == Status::pending, "second arrival pending");
            auto done = r.push(parts[order[2]], 3, 1003);
            verify(done, b, 3);
            check(done.timestamp == 1000, "earliest fragment timestamp retained");
            check(r.stats()["ipv4DuplicateFragments"] == 1 && r.stats()["ipv4ReassemblyBufferedBytes"] == 0,
                  "duplicates counted and memory released");
        } while (std::next_permutation(order.begin(), order.end()));
        {
            Ipv4UdpReassembler r;
            r.push(first, 0, 1000);
            r.push(middle, 1, 1001);
            r.push(last, 2, 1002);
            for (const auto &part : parts)
                check(r.push(part, 3, 1003).status == Status::pending, "late duplicate suppressed");
            check(r.stats()["ipv4DuplicateFragments"] == 3 && r.stats()["ipv4ReassemblyPending"] == 0,
                  "late copies do not create a false incomplete set");
            check(r.expire(5002) == 0 && r.stats()["ipv4ReassemblyRecentBytes"] == 0,
                  "recent cache expires without reporting data loss");
            auto changed = b;
            changed.back() ^= 1;
            r.push(fragment(changed, 48, 21, false), 5003, 6003);
            r.push(fragment(changed, 0, 24, true), 5004, 6004);
            verify(r.push(fragment(changed, 24, 24, true), 5005, 6005), changed, 3);
        }
        {
            Ipv4UdpReassembler r;
            auto single = r.push(b, 0, 1000);
            check(single.status == Status::complete && single.packet == b && single.fragments == 1,
                  "unfragmented datagram passes unchanged");
            r.push(first, 1, 1001);
            auto atomic = b;
            put16(atomic, 6, 0x4000);
            check(r.push(atomic, 2, 1002).packet == atomic && r.stats()["ipv4ReassemblyPending"] == 1,
                  "atomic packet reusing the IP ID leaves fragment state intact");
            r.push(last, 5001, 6001);
            check(r.expire(5002) == 1 && r.expire(5003) == 0, "push-detected timeout notified exactly once");
        }
        {
            Ipv4UdpReassembler r;
            r.push(first, 0, 1000);
            auto overlap = fragment(b, 16, 24, true);
            check(r.push(overlap, 1, 1001).status == Status::rejected, "overlapping ranges rejected");
            check(r.push(first, 2, 1002).status == Status::rejected, "ambiguous ID quarantined");
            check(r.stats()["ipv4ReassemblyPending"] == 0, "conflict purges entire datagram");
            r.expire(5001);
            check(r.push(first, 5002, 6002).status == Status::pending, "ID reusable after quarantine");
        }
        {
            Ipv4UdpReassembler r;
            r.push(first, 0, 1000);
            auto changed = first;
            changed.back() ^= 1;
            check(r.push(changed, 1, 1001).status == Status::rejected, "conflicting duplicate rejected");
        }
        {
            Ipv4UdpReassembler r;
            r.push(first, 0, 1000);
            r.push(last, 1, 1001);
            check(r.expire(4999) == 0 && r.expire(5000) == 1, "missing middle times out");
            check(r.stats()["ipv4ReassemblyBufferedBytes"] == 0, "timeout releases memory");
        }
        {
            Ipv4UdpReassembler r;
            r.push(first, 0, 1000);
            r.push(first, 4999, 5999);
            check(r.expire(5000) == 1, "duplicate cannot extend timeout forever");
        }
        {
            Ipv4UdpReassembler r;
            auto other = packet(8);
            r.push(first, 0, 1000);
            r.push(fragment(other, 0, 24, true), 0, 1000);
            check(r.stats()["ipv4ReassemblyPending"] == 2, "IP identifiers isolated");
            auto different_source = first;
            different_source[15] = 3;
            r.push(different_source, 0, 1000);
            check(r.stats()["ipv4ReassemblyPending"] == 3, "source addresses isolated");
            auto different_destination = first;
            different_destination[19] = 3;
            r.push(different_destination, 0, 1000);
            check(r.stats()["ipv4ReassemblyPending"] == 4, "destination addresses isolated");
        }
        {
            Ipv4UdpReassembler r({1, 40, 2, 100});
            r.push(first, 0, 1000);
            check(r.push(fragment(packet(8), 0, 24, true), 1, 1001).status == Status::rejected,
                  "datagram capacity enforced");
            check(r.push(middle, 2, 1002).status == Status::rejected, "byte capacity enforced");
        }
        {
            Ipv4UdpReassembler r({4, 1024, 1, 100});
            r.push(first, 0, 1000);
            check(r.push(last, 1, 1001).status == Status::rejected, "fragment capacity enforced");
        }
        for (auto invalid : {fragment(b, 0, 23, true), fragment(b, 48, 0, false)}) {
            Ipv4UdpReassembler r;
            check(r.push(invalid, 0, 1000).status == Status::rejected, "invalid fragment length rejected");
        }
        {
            Ipv4UdpReassembler r;
            auto invalid = first;
            invalid.resize(21);
            check(r.push(invalid, 0, 1000).status == Status::rejected, "truncated fragment rejected");
        }
        {
            Ipv4UdpReassembler r;
            auto options = packet(7, 61, 24);
            r.push(fragment(options, 24, 45, false, false), 0, 1000);
            verify(r.push(fragment(options, 0, 24, true), 1, 1001), options, 2);
        }
        {
            Ipv4UdpReassembler r;
            auto wrong = first;
            put16(wrong, 24, 68);
            r.push(wrong, 0, 1000);
            r.push(middle, 1, 1001);
            check(r.push(last, 2, 1002).status == Status::rejected, "UDP length checked after assembly");
        }
        {
            Ipv4UdpReassembler r;
            r.push(last, 0, 1000);
            check(r.push(fragment(b, 24, 8, false), 1, 1001).status == Status::rejected,
                  "contradictory final boundaries rejected");
        }
        for (unsigned flags : {0x6000, 0xa000, 0x3fff}) {
            Ipv4UdpReassembler r;
            auto wrong = first;
            put16(wrong, 6, flags);
            check(r.push(wrong, 0, 1000).status == Status::rejected,
                  "DF/reserved flag or oversized offset rejected");
        }
        {
            Ipv4UdpReassembler r;
            auto large = packet(9, 65507);
            r.push(fragment(large, 32768, 32747, false), 0, 1000);
            verify(r.push(fragment(large, 0, 32768, true), 1, 1001), large, 2);
        }
        {
            Ipv4UdpReassembler r({1, 100, 3, 5000});
            for (unsigned id : {7, 8}) {
                auto p = packet(id);
                r.push(fragment(p, 0, 24, true), id, 1000);
                r.push(fragment(p, 24, 45, false), id, 1000);
            }
            check(r.stats()["ipv4DatagramsReassembled"] == 2 && r.stats()["ipv4ReassemblyRecentBytes"] == 69,
                  "recent cache obeys byte/context bounds");
        }
        std::cout << "passed " << checks << " IPv4 UDP reassembly assertions\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED " << checks << ": " << e.what() << '\n';
        return 1;
    }
}
