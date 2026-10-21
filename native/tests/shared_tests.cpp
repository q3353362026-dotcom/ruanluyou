#include "radaer/shared_network.hpp"
#include "radaer/webui.hpp"
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
void put16(Bytes &b, size_t p, uint16_t v) {
    b[p] = uint8_t(v >> 8);
    b[p + 1] = uint8_t(v);
}
void put32(Bytes &b, size_t p, uint32_t v) {
    put16(b, p, uint16_t(v >> 16));
    put16(b, p + 2, uint16_t(v));
}
Bytes packet(bool tcp = true) {
    Bytes b(tcp ? 45 : 33, 0);
    b[0] = 0x45;
    b[8] = 64;
    b[9] = tcp ? 6 : 17;
    put16(b, 2, uint16_t(b.size()));
    put32(b, 12, parse_ipv4("192.168.137.2"));
    put32(b, 16, parse_ipv4("198.51.100.7"));
    put16(b, 20, 40123);
    put16(b, 22, 22102);
    if (tcp) {
        put32(b, 24, 12345);
        b[32] = 0x50;
        b[33] = 2;
    } else
        put16(b, 24, 13);
    for (size_t i = b.size() - 5; i < b.size(); ++i)
        b[i] = uint8_t(i);
    return b;
}
} // namespace
int main() {
    try {
        check(ipv4_string(parse_ipv4("192.168.137.2")) == "192.168.137.2", "IPv4 roundtrip");
        for (auto s : {"", "1.2.3", "1.2.3.256", "-1.2.3.4", "1.2.3.4.5", "1..3.4", "1.2.3.4x", " 1.2.3.4",
                       "1.2.3."}) {
            bool caught = false;
            try {
                parse_ipv4(s);
            } catch (...) {
                caught = true;
            }
            check(caught, "reject invalid IPv4");
        }
        auto tcp = packet();
        auto t = parse_ipv4_transport(tcp);
        check(t && t->source_port == 40123 && t->destination_port == 22102 && t->sequence == 12345 &&
                  t->payload_length == 5,
              "TCP parse");
        auto udp = packet(false);
        auto u = parse_ipv4_transport(udp);
        check(u && u->payload_offset == 28 && u->payload_length == 5, "UDP parse");
        for (size_t n = 0; n < tcp.size(); ++n)
            check(!parse_ipv4_transport(View(tcp).first(n)), "reject truncated TCP");
        for (size_t n = 0; n < udp.size(); ++n)
            check(!parse_ipv4_transport(View(udp).first(n)), "reject truncated UDP");
        for (auto fragment : {uint16_t(0x2000), uint16_t(1), uint16_t(0x3fff)}) {
            auto b = udp;
            put16(b, 6, fragment);
            check(!parse_ipv4_transport(b), "reject IP fragments");
        }
        for (auto ihl : {0x40, 0x44, 0x4f, 0x65}) {
            auto b = tcp;
            b[0] = uint8_t(ihl);
            check(!parse_ipv4_transport(b), "invalid IP header");
        }
        for (auto offset : {0x00, 0x40, 0xf0}) {
            auto b = tcp;
            b[32] = uint8_t(offset);
            check(!parse_ipv4_transport(b), "invalid TCP header");
        }
        for (auto len : {7, 14, 65535}) {
            auto b = udp;
            put16(b, 24, uint16_t(len));
            check(!parse_ipv4_transport(b), "invalid UDP length");
        }
        auto rewritten = tcp;
        rewrite_tcp_endpoints(rewritten, *t, parse_ipv4("192.168.137.2"), 20000, parse_ipv4("192.168.137.1"),
                              19001);
        auto rt = parse_ipv4_transport(rewritten);
        check(rt && rt->destination == parse_ipv4("192.168.137.1") && rt->source_port == 20000 &&
                  rt->destination_port == 19001,
              "rewritten tuple");
        check(rt->sequence == t->sequence &&
                  std::equal(tcp.begin() + 24, tcp.begin() + 36, rewritten.begin() + 24),
              "sequence flags ACK untouched");
        check(std::equal(tcp.begin() + 40, tcp.end(), rewritten.begin() + 40), "payload untouched");
        check(rewritten[10] == 0 && rewritten[11] == 0 && rewritten[36] == 0 && rewritten[37] == 0,
              "checksums marked for repair");
        SharedRoutes routes;
        auto syn = *t;
        auto ack = syn;
        ack.tcp_flags = 0x10;
        check(!routes.forward(ack, 0), "midstream bypass");
        auto first = routes.forward(syn, 1);
        check(first.has_value(), "first SYN route");
        check(routes.forward(syn, 2)->alias_port == first->alias_port, "SYN retransmission stable alias");
        check(routes.forward(ack, 3)->alias_port == first->alias_port, "ACK maps existing route");
        auto remote = syn;
        remote.destination = parse_ipv4("198.51.100.8");
        auto second = routes.forward(remote, 4);
        check(second && second->alias_port != first->alias_port,
              "same source port different remote unique alias");
        auto client = syn;
        client.source = parse_ipv4("192.168.137.3");
        check(routes.forward(client, 5)->alias_port != first->alias_port, "client addresses separated");
        auto reply = syn;
        std::swap(reply.source, reply.destination);
        std::swap(reply.source_port, reply.destination_port);
        check(routes.original_downstream(reply), "mapped original server reply suppressed");
        ++reply.source_port;
        check(!routes.original_downstream(reply), "unrelated server reply passed");
        ++syn.sequence;
        auto newer = routes.forward(syn, 6);
        check(newer && newer->alias_port != first->alias_port, "new SYN sequence uses new alias");
        auto retired = routes.reverse(first->alias_port, 7);
        check(retired && retired->retired && retired->initial_sequence == 12345,
              "old alias retained and retired");
        check(!routes.reverse(newer->alias_port, 8)->retired, "new connection active");
        check(routes.forward(ack, 9)->alias_port == newer->alias_port, "forward maps new epoch");
        auto next = syn;
        ++next.destination_port;
        routes.forward(next, 400000);
        check(!routes.reverse(first->alias_port, 400001), "idle mappings expire");
        check(!routes.reverse(1, 400002), "unknown alias rejected");
        SharedRoutes capacity;
        for (unsigned i = 0; i < 8192; ++i) {
            auto p = *t;
            p.source_port = uint16_t(i + 1);
            check(capacity.forward(p, 0).has_value(), "bounded route allocation");
        }
        auto excess = *t;
        excess.source_port = 10000;
        check(!capacity.forward(excess, 0), "route capacity fails open for new flow");
        check(capacity.forward(excess, 300001).has_value(), "expired routes free capacity");
        auto range = discovery_range(parse_ipv4("192.168.10.233"), 24);
        check(range.first == parse_ipv4("192.168.10.1") && range.last == parse_ipv4("192.168.10.254") &&
                  !range.limited,
              "LAN /24 bounds");
        range = discovery_range(parse_ipv4("10.20.30.40"), 16);
        check(range.first == parse_ipv4("10.20.30.1") && range.last == parse_ipv4("10.20.30.254") &&
                  range.limited,
              "large subnet capped to local /24");
        range = discovery_range(parse_ipv4("172.31.224.1"), 20);
        check(range.last - range.first + 1 == 4094 && !range.limited, "bounded /20 discovery");
        range = discovery_range(parse_ipv4("192.0.2.10"), 31);
        check(range.first == parse_ipv4("192.0.2.10") && range.last == parse_ipv4("192.0.2.11"),
              "point to point range");
        range = discovery_range(parse_ipv4("192.0.2.10"), 32);
        check(range.first == range.last, "single address range");
        bool invalid = false;
        try {
            discovery_range(parse_ipv4("127.0.0.1"), 24);
        } catch (...) {
            invalid = true;
        }
        check(invalid, "loopback discovery rejected");
        std::cout << "passed " << checks << " shared packet assertions (synthetic only)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED " << checks << ": " << e.what() << '\n';
        return 1;
    }
}
