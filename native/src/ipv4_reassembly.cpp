#include "radaer/ipv4_reassembly.hpp"
#include <algorithm>
#include <stdexcept>

namespace radaer {
namespace {
uint16_t u16(View b, size_t i) {
    return uint16_t((unsigned(b[i]) << 8) | b[i + 1]);
}
uint32_t u32(View b, size_t i) {
    return (uint32_t(u16(b, i)) << 16) | u16(b, i + 2);
}
void put16(Bytes &b, size_t i, size_t n) {
    b[i] = uint8_t(n >> 8);
    b[i + 1] = uint8_t(n);
}
} // namespace
Ipv4UdpReassembler::Ipv4UdpReassembler() : Ipv4UdpReassembler(Limits{}) {}
Ipv4UdpReassembler::Ipv4UdpReassembler(Limits limits) : limits_(limits) {
    if (!limits.datagrams || !limits.bytes || !limits.fragments_per_datagram || limits.timeout_ms <= 0)
        throw std::invalid_argument("invalid IPv4 reassembly limits");
}
void Ipv4UdpReassembler::erase(const Key &key) {
    auto it = pending_.find(key);
    if (it != pending_.end()) {
        bytes_ -= it->second.bytes;
        pending_.erase(it);
    }
}
void Ipv4UdpReassembler::forget_recent(const Key &key) {
    auto it = recent_.find(key);
    if (it != recent_.end()) {
        recent_bytes_ -= it->second.payload.size();
        recent_.erase(it);
    }
}
Ipv4UdpReassembler::Result Ipv4UdpReassembler::reject(const Key &key, int64_t now, const char *reason) {
    erase(key);
    // An ambiguous datagram stays rejected for the reassembly lifetime.
    if (rejected_.size() >= limits_.datagrams && !rejected_.contains(key)) {
        auto oldest = std::min_element(rejected_.begin(), rejected_.end(),
                                       [](const auto &a, const auto &b) { return a.second < b.second; });
        rejected_.erase(oldest);
    }
    rejected_[key] = now;
    ++dropped_;
    return {Status::rejected, {}, reason};
}
size_t Ipv4UdpReassembler::expire(int64_t now) {
    std::lock_guard lock(mutex_);
    size_t count = 0;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.created >= limits_.timeout_ms) {
            bytes_ -= it->second.bytes;
            it = pending_.erase(it);
            ++count;
        } else
            ++it;
    }
    for (auto it = rejected_.begin(); it != rejected_.end();)
        if (now - it->second >= limits_.timeout_ms)
            it = rejected_.erase(it);
        else
            ++it;
    for (auto it = recent_.begin(); it != recent_.end();)
        if (now - it->second.completed >= limits_.timeout_ms) {
            recent_bytes_ -= it->second.payload.size();
            it = recent_.erase(it);
        } else
            ++it;
    timed_out_ += count;
    count += unreported_timeouts_;
    unreported_timeouts_ = 0;
    return count;
}
Ipv4UdpReassembler::Result Ipv4UdpReassembler::push(View b, int64_t now, int64_t timestamp) {
    std::lock_guard lock(mutex_);
    if (b.size() < 20 || (b[0] >> 4) != 4 || b[9] != 17)
        return {Status::rejected, {}, "not_ipv4_udp"};
    const Key key{u32(b, 12), u32(b, 16), u16(b, 4)};
    const size_t header = (b[0] & 15) * 4, total = u16(b, 2);
    const unsigned flags = u16(b, 6);
    const bool fragmented = (flags & 0x3fff) != 0, more = (flags & 0x2000) != 0;
    if (fragmented)
        ++fragments_;
    if (header < 20 || total < header || total > b.size() || (flags & 0x8000) ||
        (fragmented && (flags & 0x4000)))
        return reject(key, now, "invalid_ipv4_fragment_header");
    if (!fragmented) {
        // Atomic datagrams may reuse IDs (often zero) while a fragmented
        // datagram is in flight. They must not cancel that reassembly state.
        return {Status::complete, Bytes(b.begin(), b.begin() + total), {}, timestamp, 1};
    }
    if (auto it = rejected_.find(key); it != rejected_.end()) {
        if (now - it->second < limits_.timeout_ms)
            return {Status::rejected, {}, "quarantined_ipv4_datagram"};
        rejected_.erase(it);
    }
    const size_t offset = (flags & 0x1fff) * 8, length = total - header, end = offset + length;
    if (!length || (more && length % 8) || end > 65535 - 20)
        return reject(key, now, "invalid_ipv4_fragment_length");
    // Suppress late identical copies without creating an incomplete set that
    // would later generate a false timeout. A differing fragment permits ID reuse.
    if (auto previous = recent_.find(key); previous != recent_.end()) {
        const auto &payload = previous->second.payload;
        if (now - previous->second.completed < limits_.timeout_ms && end <= payload.size() &&
            more == (end < payload.size()) &&
            std::equal(b.begin() + header, b.begin() + total, payload.begin() + offset)) {
            ++duplicates_;
            return {};
        }
        forget_recent(key);
    }
    auto it = pending_.find(key);
    if (it != pending_.end() && now - it->second.created >= limits_.timeout_ms) {
        erase(key);
        ++timed_out_;
        ++unreported_timeouts_;
        it = pending_.end();
    }
    if (it == pending_.end()) {
        if (pending_.size() >= limits_.datagrams)
            return reject(key, now, "ipv4_reassembly_datagram_limit");
        it = pending_.try_emplace(key).first;
        it->second.created = now;
        it->second.timestamp = timestamp;
    }
    auto &d = it->second;
    for (const auto &[start, part] : d.parts) {
        if (start == offset && part.bytes.size() == length && part.more == more &&
            std::equal(part.bytes.begin(), part.bytes.end(), b.begin() + header)) {
            ++duplicates_;
            return {}; // Identical duplicate does not extend the lifetime.
        }
        if (start < end && offset < start + part.bytes.size())
            return reject(key, now, "overlapping_ipv4_fragments");
    }
    if ((d.end && (end > *d.end || (!more && end != *d.end) || (more && end >= *d.end))) ||
        (!more && !d.parts.empty() && d.parts.rbegin()->first + d.parts.rbegin()->second.bytes.size() > end))
        return reject(key, now, "conflicting_ipv4_fragment_end");
    if (d.parts.size() >= limits_.fragments_per_datagram || length > limits_.bytes - bytes_)
        return reject(key, now, "ipv4_reassembly_memory_or_fragment_limit");
    if (!offset) {
        d.header.assign(b.begin(), b.begin() + header);
        if (end > 65535 - header || (d.end && *d.end > 65535 - header))
            return reject(key, now, "oversized_reassembled_ipv4_packet");
    }
    if (!d.header.empty() && end > 65535 - d.header.size())
        return reject(key, now, "oversized_reassembled_ipv4_packet");
    d.timestamp = std::min(d.timestamp, timestamp);
    d.parts.emplace(offset, Part{Bytes(b.begin() + header, b.begin() + total), more});
    d.bytes += length;
    bytes_ += length;
    if (!more)
        d.end = end;
    if (d.header.empty() || !d.end)
        return {};
    size_t contiguous = 0;
    for (const auto &[start, part] : d.parts) {
        if (start != contiguous)
            return {};
        contiguous += part.bytes.size();
    }
    if (contiguous != *d.end)
        return {};
    Result result{Status::complete, d.header, {}, d.timestamp, unsigned(d.parts.size())};
    for (const auto &[start, part] : d.parts)
        result.packet.insert(result.packet.end(), part.bytes.begin(), part.bytes.end());
    const auto h = d.header.size();
    if (*d.end < 8 || u16(result.packet, h + 4) != *d.end)
        return reject(key, now, "reassembled_udp_length_mismatch");
    put16(result.packet, 2, result.packet.size());
    put16(result.packet, 6, 0);
    put16(result.packet, 10, 0);
    unsigned checksum = 0;
    for (size_t p = 0; p < h; p += 2)
        checksum += u16(result.packet, p);
    while (checksum >> 16)
        checksum = (checksum & 65535) + (checksum >> 16);
    put16(result.packet, 10, uint16_t(~checksum));
    erase(key);
    const size_t payload_size = result.packet.size() - h;
    if (payload_size <= limits_.bytes) {
        while (!recent_.empty() &&
               (recent_.size() >= limits_.datagrams || recent_bytes_ + payload_size > limits_.bytes)) {
            auto oldest = std::min_element(recent_.begin(), recent_.end(), [](const auto &a, const auto &b) {
                return a.second.completed < b.second.completed;
            });
            forget_recent(oldest->first);
        }
        recent_[key] = {Bytes(result.packet.begin() + h, result.packet.end()), now};
        recent_bytes_ += payload_size;
    }
    ++completed_;
    return result;
}
Json Ipv4UdpReassembler::stats() const {
    std::lock_guard lock(mutex_);
    return {{"ipv4FragmentsReceived", fragments_},   {"ipv4DatagramsReassembled", completed_},
            {"ipv4DuplicateFragments", duplicates_}, {"ipv4ReassemblyRejected", dropped_},
            {"ipv4ReassemblyTimeouts", timed_out_},  {"ipv4ReassemblyPending", pending_.size()},
            {"ipv4ReassemblyBufferedBytes", bytes_}, {"ipv4ReassemblyRecentBytes", recent_bytes_}};
}
} // namespace radaer
