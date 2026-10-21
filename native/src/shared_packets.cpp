#include "radaer/shared_network.hpp"
#include <charconv>
#include <stdexcept>
namespace radaer {
namespace {
uint16_t u16(View b, size_t p) {
    return uint16_t((unsigned(b[p]) << 8) | b[p + 1]);
}
uint32_t u32(View b, size_t p) {
    return (uint32_t(u16(b, p)) << 16) | u16(b, p + 2);
}
void put16(Bytes &b, size_t p, uint16_t v) {
    b[p] = uint8_t(v >> 8);
    b[p + 1] = uint8_t(v);
}
void put32(Bytes &b, size_t p, uint32_t v) {
    put16(b, p, uint16_t(v >> 16));
    put16(b, p + 2, uint16_t(v));
}
std::string key(uint32_t src, uint16_t sp, uint32_t dst, uint16_t dp) {
    return ipv4_string(src) + ":" + std::to_string(sp) + ">" + ipv4_string(dst) + ":" + std::to_string(dp);
}
} // namespace
std::optional<Ipv4Transport> parse_ipv4_transport(View b) {
    if (b.size() < 20 || (b[0] >> 4) != 4)
        return {};
    Ipv4Transport t;
    t.ip_length = (b[0] & 15) * 4;
    t.total_length = u16(b, 2);
    t.protocol = b[9];
    // Fragment reassembly belongs to the IP stack. Never decrypt partial datagrams.
    if (t.ip_length < 20 || t.total_length < t.ip_length || t.total_length > b.size() || (u16(b, 6) & 0x3fff))
        return {};
    t.source = u32(b, 12);
    t.destination = u32(b, 16);
    auto h = t.ip_length;
    if (t.protocol == 6) {
        if (t.total_length < h + 20)
            return {};
        size_t tcp = (b[h + 12] >> 4) * 4;
        if (tcp < 20 || h + tcp > t.total_length)
            return {};
        t.sequence = u32(b, h + 4);
        t.tcp_flags = b[h + 13];
        t.payload_offset = h + tcp;
        t.payload_length = t.total_length - t.payload_offset;
    } else if (t.protocol == 17) {
        if (t.total_length < h + 8)
            return {};
        size_t len = u16(b, h + 4);
        if (len < 8 || h + len > t.total_length)
            return {};
        t.payload_offset = h + 8;
        t.payload_length = len - 8;
    } else
        return {};
    t.source_port = u16(b, h);
    t.destination_port = u16(b, h + 2);
    return t;
}
uint32_t parse_ipv4(const std::string &s) {
    uint32_t ip = 0;
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        size_t end = s.find('.', start);
        if ((i < 3 && end == std::string::npos) || (i == 3 && end != std::string::npos))
            throw std::runtime_error("invalid IPv4: " + s);
        if (end == std::string::npos)
            end = s.size();
        unsigned n = 0;
        auto [p, ec] = std::from_chars(s.data() + start, s.data() + end, n);
        if (start == end || ec != std::errc() || p != s.data() + end || n > 255)
            throw std::runtime_error("invalid IPv4: " + s);
        ip = (ip << 8) | n;
        start = end + 1;
    }
    return ip;
}
std::string ipv4_string(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}
void rewrite_tcp_endpoints(Bytes &b, const Ipv4Transport &t, uint32_t src, uint16_t sp, uint32_t dst,
                           uint16_t dp) {
    auto actual = parse_ipv4_transport(b);
    if (!actual || actual->protocol != 6 || actual->ip_length != t.ip_length ||
        actual->total_length != t.total_length)
        throw std::runtime_error("invalid TCP packet for rewrite");
    put32(b, 12, src);
    put32(b, 16, dst);
    put16(b, t.ip_length, sp);
    put16(b, t.ip_length + 2, dp);
    // Caller must recalculate IP/TCP checksums before injection.
    put16(b, 10, 0);
    put16(b, t.ip_length + 16, 0);
}
std::optional<SharedRoute> SharedRoutes::forward(const Ipv4Transport &t, int64_t ts) {
    if (t.protocol != 6)
        return {};
    std::lock_guard lock(mutex_);
    auto k = key(t.source, t.source_port, t.destination, t.destination_port);
    bool syn = (t.tcp_flags & 0x12) == 2;
    auto existing = routes_.find(k);
    if (existing != routes_.end()) {
        auto &r = aliases_.at(existing->second);
        if (!syn || r.initial_sequence == t.sequence) {
            r.last_seen = ts;
            return r;
        }
    }
    if (!syn)
        return {}; // Do not capture already-established connections midstream.
    for (auto i = aliases_.begin(); i != aliases_.end();) {
        const auto &r = i->second;
        if (ts - r.last_seen > 300000) {
            auto old = key(r.client_ip, r.client_port, r.server_ip, r.server_port);
            auto f = routes_.find(old);
            if (f != routes_.end() && f->second == i->first)
                routes_.erase(f);
            i = aliases_.erase(i);
        } else
            ++i;
    }
    if (aliases_.size() >= 8192)
        return {};
    uint16_t alias;
    do {
        alias = next_alias_++;
        if (next_alias_ > 59999)
            next_alias_ = 20000;
    } while (aliases_.contains(alias));
    existing = routes_.find(k);
    if (existing != routes_.end())
        aliases_.at(existing->second).retired = true;
    SharedRoute r{t.source_port, t.destination_port, alias, t.source, t.destination, t.sequence, ts, false};
    aliases_[alias] = r;
    routes_[k] = alias;
    return r;
}
std::optional<SharedRoute> SharedRoutes::reverse(uint16_t alias, int64_t ts) {
    std::lock_guard lock(mutex_);
    auto i = aliases_.find(alias);
    if (i == aliases_.end())
        return {};
    i->second.last_seen = ts;
    return i->second;
}
bool SharedRoutes::original_downstream(const Ipv4Transport &t) {
    std::lock_guard lock(mutex_);
    return routes_.contains(key(t.destination, t.destination_port, t.source, t.source_port));
}
} // namespace radaer
