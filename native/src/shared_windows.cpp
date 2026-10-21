#include "radaer/ipv4_reassembly.hpp"
#include "radaer/shared_network.hpp"
#include <atomic>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: IP Helper requires Winsock types before its declarations.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "windivert/windivert.h"
// clang-format on
#undef interface
static_assert(sizeof(WINDIVERT_ADDRESS) == 80, "WinDivert 2.x ABI");
namespace radaer {
namespace {
std::runtime_error winerr(const std::string &what) {
    return std::runtime_error(what + " (Windows error " + std::to_string(GetLastError()) + ")");
}
std::string utf8(const wchar_t *s) {
    if (!s)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    out.pop_back();
    return out;
}
bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) != 0;
    CloseHandle(token);
    return ok && e.TokenIsElevated;
}
struct Driver {
    HMODULE module = nullptr;
    decltype(&WinDivertOpen) open = nullptr;
    decltype(&WinDivertRecv) recv = nullptr;
    decltype(&WinDivertSend) send = nullptr;
    decltype(&WinDivertClose) close = nullptr;
    decltype(&WinDivertShutdown) shutdown = nullptr;
    decltype(&WinDivertSetParam) set = nullptr;
    decltype(&WinDivertGetParam) get = nullptr;
    decltype(&WinDivertHelperCalcChecksums) checksums = nullptr;
    decltype(&WinDivertHelperCompileFilter) compile = nullptr;
    template <class F> void symbol(F &f, const char *name) {
        auto p = GetProcAddress(module, name);
        if (!p)
            throw std::runtime_error(std::string("WinDivert missing symbol: ") + name);
        static_assert(sizeof(f) == sizeof(p));
        std::memcpy(&f, &p, sizeof(f));
    }
    explicit Driver(const std::string &file) {
        auto path = std::filesystem::path(reinterpret_cast<const char8_t *>(file.c_str()));
        if (!path.is_absolute() || !std::filesystem::is_regular_file(path))
            throw std::runtime_error("--windivert-dll requires an existing absolute DLL path");
        if (!std::filesystem::is_regular_file(path.parent_path() / "WinDivert64.sys"))
            throw std::runtime_error("WinDivert64.sys must be next to WinDivert.dll");
        if (sizeof(void *) != 8)
            throw std::runtime_error("shared capture requires a 64-bit build");
        module = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            throw winerr("cannot load WinDivert DLL");
        try {
            symbol(open, "WinDivertOpen");
            symbol(recv, "WinDivertRecv");
            symbol(send, "WinDivertSend");
            symbol(close, "WinDivertClose");
            symbol(shutdown, "WinDivertShutdown");
            symbol(set, "WinDivertSetParam");
            symbol(get, "WinDivertGetParam");
            symbol(checksums, "WinDivertHelperCalcChecksums");
            symbol(compile, "WinDivertHelperCompileFilter");
        } catch (...) {
            FreeLibrary(module);
            module = nullptr;
            throw;
        }
    }
    ~Driver() {
        if (module)
            FreeLibrary(module);
    }
    void validate(const std::string &filter, WINDIVERT_LAYER layer) {
        const char *error = nullptr;
        UINT position = 0;
        if (!compile(filter.c_str(), layer, nullptr, 0, &error, &position))
            throw std::runtime_error("invalid WinDivert filter at " + std::to_string(position) + ": " +
                                     (error ? error : "unknown"));
    }
};
struct Settings {
    uint32_t client = 0, host = 0, mask = 0;
    uint16_t listen_port = 0;
    std::string udp, forward, reply;
};
Settings settings(const CaptureOptions &o) {
    if (!o.shared_windows || !o.interface.empty() || o.original_destination || !o.upstream.empty())
        throw std::runtime_error(
            "shared capture cannot combine Linux interface/original-destination or explicit upstream");
    Settings s;
    s.client = parse_ipv4(o.client_ip);
    s.host = parse_ipv4(o.shared_host_ip);
    auto unicast = [](uint32_t ip) { return (ip >> 24) != 0 && (ip >> 24) != 127 && (ip >> 24) < 224; };
    if (!unicast(s.client) || !unicast(s.host) || s.client == s.host || !o.shared_if_index)
        throw std::runtime_error("supply distinct client/host unicast IPv4 addresses and --shared-if-index");
    if (o.udp_port > 65535 || o.tcp_port > 65535)
        throw std::runtime_error("port exceeds 65535");
    if (o.shared_redirect != o.relay)
        throw std::runtime_error("shared TCP redirect requires relay enabled (set automatically by CLI)");
    if (!o.shared_redirect && (o.tcp_port || !o.tcp_server.empty()))
        throw std::runtime_error("TCP filter requires --shared-redirect true");
    auto inventory = windows_network_inventory();
    bool matched = false;
    for (auto &adapter : inventory["adapters"])
        for (auto &a : adapter["ipv4"])
            if (parse_ipv4(a["address"]) == s.client)
                throw std::runtime_error(
                    "client IP belongs to this host; select the downstream customer device");
    for (auto &adapter : inventory["adapters"]) {
        if (adapter["ifIndex"] != o.shared_if_index)
            continue;
        if (!adapter["up"].get<bool>())
            throw std::runtime_error("selected sharing adapter is down");
        for (auto &a : adapter["ipv4"]) {
            if (parse_ipv4(a["address"]) != s.host)
                continue;
            auto bits = a["prefixLength"].get<unsigned>();
            uint32_t mask = bits == 0 ? 0 : 0xffffffffU << (32 - bits);
            s.mask = mask;
            if ((s.host & mask) != (s.client & mask))
                throw std::runtime_error("client must be on selected private sharing subnet");
            if (bits < 31 && ((s.client & ~mask) == 0 || (s.client & ~mask) == ~mask))
                throw std::runtime_error("client is subnet network/broadcast address");
            matched = true;
        }
    }
    if (!matched)
        throw std::runtime_error("--shared-host-ip is not assigned to --shared-if-index");
    auto client = ipv4_string(s.client), host = ipv4_string(s.host);
    // NETWORK_FORWARD interface metadata can describe different legs of ICS.
    // Scope both directions by client address; use private IfIdx for inbound injection.
    s.udp = "ip and ip.Protocol == 17 and (ip.SrcAddr == " + client + " or ip.DstAddr == " + client + ")";
    if (o.shared_redirect) {
        auto colon = o.listen.rfind(':');
        if (colon == std::string::npos || parse_ipv4(o.listen.substr(0, colon)) != s.host)
            throw std::runtime_error("shared relay must listen on --shared-host-ip:PORT");
        auto p = o.listen.substr(colon + 1);
        unsigned n = 0;
        auto [end, ec] = std::from_chars(p.data(), p.data() + p.size(), n);
        if (ec != std::errc() || end != p.data() + p.size() || n > 65535)
            throw std::runtime_error("invalid listen port");
        s.listen_port = uint16_t(n);
        auto first = ipv4_string(s.host & s.mask), last = ipv4_string((s.host & s.mask) | ~s.mask);
        std::string up =
            "ip.SrcAddr == " + client + " and (ip.DstAddr < " + first + " or ip.DstAddr > " + last + ")";
        std::string down =
            "ip.DstAddr == " + client + " and (ip.SrcAddr < " + first + " or ip.SrcAddr > " + last + ")";
        if (o.tcp_port) {
            up += " and tcp.DstPort == " + std::to_string(o.tcp_port);
            down += " and tcp.SrcPort == " + std::to_string(o.tcp_port);
        }
        if (!o.tcp_server.empty()) {
            auto server = parse_ipv4(o.tcp_server);
            if (!unicast(server) || server == s.client || server == s.host)
                throw std::runtime_error("invalid TCP server");
            up += " and ip.DstAddr == " + ipv4_string(server);
            down += " and ip.SrcAddr == " + ipv4_string(server);
        }
        s.forward = "ip and tcp and ((" + up + ") or (" + down + "))";
        for (auto &adapter : inventory["adapters"])
            for (auto &a : adapter["ipv4"])
                s.forward += " and ip.SrcAddr != " + a["address"].get<std::string>() +
                             " and ip.DstAddr != " + a["address"].get<std::string>();
        if (s.listen_port)
            s.reply = "outbound and ip and tcp and ip.SrcAddr == " + host +
                      " and tcp.SrcPort == " + std::to_string(s.listen_port) + " and ip.DstAddr == " + client;
    }
    return s;
}
Json filters(const Settings &s) {
    return {{"udp", s.udp},
            {"tcpForward", s.forward},
            {"tcpReturn", s.reply.empty() ? Json(nullptr) : Json(s.reply)}};
}
} // namespace
Json windows_network_inventory() {
    ULONG size = 16384;
    std::vector<uint8_t> memory(size);
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 4 && rc == ERROR_BUFFER_OVERFLOW; ++i) {
        memory.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data()), &size);
    }
    if (rc != NO_ERROR)
        throw std::runtime_error("GetAdaptersAddresses failed: " + std::to_string(rc));
    Json adapters = Json::array();
    for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(memory.data()); a; a = a->Next) {
        Json ips = Json::array();
        for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            auto *v = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
            ips.push_back({{"address", ipv4_string(ntohl(v->sin_addr.s_addr))},
                           {"prefixLength", u->OnLinkPrefixLength}});
        }
        Json gateways = Json::array();
        for (auto *g = a->FirstGatewayAddress; g; g = g->Next)
            if (g->Address.lpSockaddr->sa_family == AF_INET) {
                auto *v = reinterpret_cast<sockaddr_in *>(g->Address.lpSockaddr);
                gateways.push_back(ipv4_string(ntohl(v->sin_addr.s_addr)));
            }
        adapters.push_back({{"ifIndex", a->IfIndex},
                            {"gateways", gateways},
                            {"name", utf8(a->FriendlyName)},
                            {"description", utf8(a->Description)},
                            {"up", a->OperStatus == IfOperStatusUp},
                            {"type", a->IfType},
                            {"ipv4", ips}});
    }
    return {{"platform", "windows"},
            {"elevated", elevated()},
            {"adapters", adapters},
            {"sharingStateVerified", false},
            {"note", "Use shared-network.ps1 -Mode Info for ICS and neighbor inventory."}};
}
Json windows_capture_preflight(const CaptureOptions &o) {
    auto s = settings(o);
    Driver d(o.windivert_dll);
    d.validate(s.udp, WINDIVERT_LAYER_NETWORK_FORWARD);
    if (o.shared_redirect) {
        d.validate(s.forward, WINDIVERT_LAYER_NETWORK_FORWARD);
        if (!s.reply.empty())
            d.validate(s.reply, WINDIVERT_LAYER_NETWORK);
    }
    return {{"configurationValid", true},
            {"elevated", elevated()},
            {"filters", filters(s)},
            {"driverOpened", false},
            {"driverVersionVerified", false},
            {"sharingConnectivityVerified", false},
            {"firewallRuleRequired", o.shared_redirect},
            {"firewallManaged", o.shared_redirect && o.manage_firewall},
            {"tcpPortFilter", o.shared_redirect ? o.tcp_port : 0},
            {"tcpMode", !o.shared_redirect                      ? "disabled"
                        : (o.tcp_port || !o.tcp_server.empty()) ? "filtered"
                                                                : "automatic_all_ports"},
            {"listen", o.shared_redirect ? Json(o.listen) : Json(nullptr)},
            {"returnFilterPendingBind", o.shared_redirect && !s.listen_port},
            {"udpMode", "sniff_copy_no_reinject"},
            {"next", "Confirm client gateway/connectivity, run elevated, then start capture before opening "
                     "the game connection."}};
}
struct WindowsSharedCapture::Impl {
    CaptureOptions o;
    Settings s;
    Driver api;
    Sink events;
    HANDLE udp = INVALID_HANDLE_VALUE, forward = INVALID_HANDLE_VALUE, network = INVALID_HANDLE_VALUE;
    std::vector<std::thread> threads;
    std::atomic<bool> stopping{false};
    SharedRoutes routes;
    Ipv4UdpReassembler reassembler;
    std::mutex mutex;
    std::vector<Packet> queued;
    size_t queued_bytes = 0;
    uint64_t gap_count = 0;
    int64_t gap_since = 0, gap_until = 0;
    std::string gap_reason;
    std::string failure;
    std::atomic<uint64_t> received{0}, redirect_up{0}, redirect_down{0}, suppressed{0};
    std::atomic<uint64_t> discarded{0}, invalid_packets{0}, queue_overflows{0};
    LARGE_INTEGER anchor{}, frequency{};
    int64_t wall = 0, last_stats = 0;
    Impl(const CaptureOptions &options, Sink sink)
        : o(options), s(settings(o)), api(o.windivert_dll), events(std::move(sink)) {}
    ~Impl() {
        stop();
    }
    int64_t stamp(INT64 tick) const {
        return wall + int64_t((static_cast<long double>(tick) - anchor.QuadPart) * 1000 / frequency.QuadPart);
    }
    HANDLE open(const std::string &filter, WINDIVERT_LAYER layer, UINT64 flags) {
        api.validate(filter, layer);
        auto h = api.open(filter.c_str(), layer, 0, flags);
        if (h == INVALID_HANDLE_VALUE)
            throw winerr("WinDivertOpen; administrator rights and compatible driver required");
        try {
            UINT64 major = 0, minor = 0;
            if (!api.get(h, WINDIVERT_PARAM_VERSION_MAJOR, &major) ||
                !api.get(h, WINDIVERT_PARAM_VERSION_MINOR, &minor) || major != 2 || minor < 2)
                throw std::runtime_error("WinDivert 2.2+ driver required");
            if (!api.set(h, WINDIVERT_PARAM_QUEUE_LENGTH, 8192) ||
                !api.set(h, WINDIVERT_PARAM_QUEUE_TIME, 2000) ||
                !api.set(h, WINDIVERT_PARAM_QUEUE_SIZE, 16777216))
                throw winerr("WinDivert queue setup failed");
        } catch (...) {
            api.close(h);
            throw;
        }
        return h;
    }
    void start() {
        if (o.shared_redirect && !s.listen_port)
            throw std::runtime_error("TCP listener must be bound before starting shared capture");
        if (!elevated())
            throw std::runtime_error("shared capture requires an elevated Administrator terminal");
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&anchor))
            throw winerr("QPC unavailable");
        wall = now_ms();
        last_stats = wall;
        if (o.shared_redirect)
            network = open(s.reply, WINDIVERT_LAYER_NETWORK, 0);
        udp = open(s.udp, WINDIVERT_LAYER_NETWORK_FORWARD, WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
        if (o.shared_redirect)
            forward = open(s.forward, WINDIVERT_LAYER_NETWORK_FORWARD, 0);
        events({{"type", "shared_capture_started"},
                {"data",
                 {{"filters", filters(s)},
                  {"ifIndex", o.shared_if_index},
                  {"listen", o.shared_redirect ? Json(o.listen) : Json(nullptr)},
                  {"udpMode", "sniff_copy_no_reinject"},
                  {"timestampSource", "WinDivert_QPC"},
                  {"kernelDropCountAvailable", false}}}});
        threads.emplace_back([this] { worker(udp, 0); });
        if (o.shared_redirect) {
            threads.emplace_back([this] { worker(network, 2); });
            threads.emplace_back([this] { worker(forward, 1); });
        }
    }
    void shutdown() noexcept {
        for (auto h : {forward, udp, network})
            if (h != INVALID_HANDLE_VALUE)
                api.shutdown(h, WINDIVERT_SHUTDOWN_RECV);
    }
    void stop() noexcept {
        stopping = true;
        shutdown();
        for (auto &thread : threads)
            if (thread.joinable())
                thread.join();
        threads.clear();
        for (auto *h : {&forward, &udp, &network})
            if (*h != INVALID_HANDLE_VALUE) {
                api.close(*h);
                *h = INVALID_HANDLE_VALUE;
            }
    }
    void gap(int64_t ts, const std::string &reason) {
        std::lock_guard lock(mutex);
        if (!gap_count) {
            gap_since = queued.empty() ? ts : std::min(ts, queued.front().timestamp);
            gap_reason = reason;
        } else if (gap_reason != reason)
            gap_reason = "shared_multiple_capture_gaps";
        gap_until = std::max(ts, gap_until);
        gap_count += queued.size() + 1;
        discarded += queued.size() + 1;
        queued.clear();
        queued_bytes = 0;
    }
    void inject(HANDLE handle, Bytes &b, WINDIVERT_ADDRESS &a, bool changed) {
        if (changed && !api.checksums(b.data(), UINT(b.size()), &a, 0))
            throw winerr("checksum repair failed");
        UINT written = 0;
        if (!api.send(handle, b.data(), UINT(b.size()), &written, &a) || written != b.size())
            throw winerr("packet reinjection failed");
    }
    void worker(HANDLE handle, int mode) noexcept {
        try {
            while (true) {
                Bytes b(WINDIVERT_MTU_MAX);
                WINDIVERT_ADDRESS a{};
                UINT length = 0;
                if (!api.recv(handle, b.data(), UINT(b.size()), &length, &a)) {
                    if (stopping)
                        break;
                    throw winerr("WinDivert receive failed");
                }
                b.resize(length);
                std::optional<Ipv4Transport> t;
                if (mode == 0) {
                    ++received;
                    auto ts = stamp(a.Timestamp);
                    auto rebuilt = reassembler.push(b, int64_t(GetTickCount64()), ts);
                    if (rebuilt.status == Ipv4UdpReassembler::Status::pending)
                        continue;
                    if (rebuilt.status == Ipv4UdpReassembler::Status::rejected) {
                        ++invalid_packets;
                        gap(ts, rebuilt.reason);
                        continue;
                    }
                    auto completed_ts = ts;
                    ts = rebuilt.timestamp;
                    b = std::move(rebuilt.packet);
                    t = parse_ipv4_transport(b);
                    if (!t || t->protocol != 17) {
                        ++invalid_packets;
                        gap(ts, "invalid_udp_packet");
                        continue;
                    }
                    bool up = t->source == s.client;
                    if (!up && t->destination != s.client)
                        continue;
                    if (o.udp_port && (up ? t->destination_port : t->source_port) != o.udp_port)
                        continue;
                    auto src = ipv4_string(t->source) + ":" + std::to_string(t->source_port);
                    auto dst = ipv4_string(t->destination) + ":" + std::to_string(t->destination_port);
                    Packet p{up,
                             Bytes(b.begin() + t->payload_offset,
                                   b.begin() + t->payload_offset + t->payload_length),
                             ts,
                             up ? src : dst,
                             up ? dst : src,
                             0,
                             rebuilt.fragments,
                             std::max(int64_t(0), completed_ts - ts)};
                    bool overflow = false;
                    {
                        std::lock_guard lock(mutex);
                        overflow =
                            queued.size() >= 8192 || queued_bytes + p.payload.size() > 32 * 1024 * 1024;
                        if (!overflow) {
                            queued_bytes += p.payload.size();
                            queued.push_back(std::move(p));
                        }
                    }
                    if (overflow) {
                        ++queue_overflows;
                        gap(ts, "shared_queue_overflow");
                    }
                    continue; // SNIFF copies must never be sent back to the network.
                }
                t = parse_ipv4_transport(b);
                if (!t || t->protocol != 6) {
                    inject(handle, b, a, false);
                    continue;
                }
                auto tick = int64_t(GetTickCount64());
                if (mode == 1) {
                    auto remote = t->source == s.client ? t->destination : t->source;
                    if ((remote & s.mask) == (s.host & s.mask)) {
                        inject(handle, b, a, false);
                        continue;
                    }
                    if (t->destination == s.client) {
                        if (routes.original_downstream(*t)) {
                            ++suppressed;
                            continue;
                        }
                        inject(handle, b, a, false);
                        continue;
                    }
                    if (t->source != s.client) {
                        inject(handle, b, a, false);
                        continue;
                    }
                    auto route = routes.forward(*t, tick);
                    if (!route) {
                        inject(handle, b, a, false);
                        continue;
                    }
                    rewrite_tcp_endpoints(b, *t, s.client, route->alias_port, s.host, s.listen_port);
                    a.Layer = WINDIVERT_LAYER_NETWORK;
                    a.Event = WINDIVERT_EVENT_NETWORK_PACKET;
                    a.Outbound = 0;
                    a.Loopback = 0;
                    a.Sniffed = 0;
                    a.Impostor = 0;
                    a.Network.IfIdx = o.shared_if_index;
                    a.Network.SubIfIdx = 0;
                    inject(network, b, a, true);
                    ++redirect_up;
                } else {
                    auto route = routes.reverse(t->destination_port, tick);
                    if (!route) {
                        inject(handle, b, a, false);
                        continue;
                    }
                    if (route->retired) {
                        ++suppressed;
                        continue;
                    }
                    rewrite_tcp_endpoints(b, *t, route->server_ip, route->server_port, s.client,
                                          route->client_port);
                    a.Outbound = 1;
                    inject(handle, b, a, true);
                    ++redirect_down;
                }
            }
        } catch (const std::exception &e) {
            {
                std::lock_guard lock(mutex);
                if (failure.empty())
                    failure = e.what();
            }
            stopping = true;
            shutdown();
        }
    }
};
WindowsSharedCapture::WindowsSharedCapture(const CaptureOptions &o, Sink events)
    : p_(std::make_unique<Impl>(o, std::move(events))) {
    p_->start();
}
WindowsSharedCapture::~WindowsSharedCapture() = default;
std::vector<Packet> WindowsSharedCapture::drain() {
    std::vector<Packet> packets;
    uint64_t gaps = 0;
    int64_t since = 0, until = 0;
    std::string reason;
    {
        std::lock_guard lock(p_->mutex);
        packets.swap(p_->queued);
        p_->queued_bytes = 0;
        gaps = p_->gap_count;
        since = p_->gap_since;
        until = p_->gap_until;
        reason = std::move(p_->gap_reason);
        p_->gap_count = 0;
        p_->gap_since = p_->gap_until = 0;
    }
    if (gaps)
        p_->events({{"type", "capture_gap"},
                    {"timestampMs", until},
                    {"data", {{"reason", reason}, {"sinceMs", since}, {"discardedPackets", gaps}}}});
    return packets;
}
std::optional<std::string> WindowsSharedCapture::resolve(const std::string &peer, uint16_t alias) {
    if (parse_ipv4(peer) != p_->s.client)
        return {};
    auto r = p_->routes.reverse(alias, int64_t(GetTickCount64()));
    if (!r || r->retired)
        return {};
    return ipv4_string(r->server_ip) + ":" + std::to_string(r->server_port);
}
void WindowsSharedCapture::check() {
    if (p_->reassembler.expire(int64_t(GetTickCount64())))
        p_->gap(now_ms(), "ipv4_fragment_timeout");
    std::string error;
    {
        std::lock_guard lock(p_->mutex);
        error = p_->failure;
    }
    if (!error.empty()) {
        p_->events({{"type", "capture_gap"},
                    {"timestampMs", now_ms()},
                    {"data", {{"reason", "shared_driver_error"}, {"sinceMs", p_->last_stats}}}});
        throw std::runtime_error(error);
    }
    if (now_ms() - p_->last_stats >= 1000) {
        auto data = p_->reassembler.stats();
        data.update(Json{{"source", "WinDivert"},
                         {"udpReceived", p_->received.load()},
                         {"redirectedUp", p_->redirect_up.load()},
                         {"redirectedDown", p_->redirect_down.load()},
                         {"suppressed", p_->suppressed.load()},
                         {"discardedByApplication", p_->discarded.load()},
                         {"invalidPackets", p_->invalid_packets.load()},
                         {"queueOverflowEvents", p_->queue_overflows.load()},
                         {"kernelDrops", nullptr}});
        p_->events({{"type", "capture_stats"}, {"data", std::move(data)}});
        p_->last_stats = now_ms();
    }
}
void WindowsSharedCapture::stop() {
    p_->stop();
    p_->events({{"type", "udp_reassembly_summary"}, {"data", p_->reassembler.stats()}});
}
} // namespace radaer
#else
#ifdef __linux__
#include "radaer/linux_network.hpp"
#endif
namespace radaer {
struct WindowsSharedCapture::Impl {};
WindowsSharedCapture::WindowsSharedCapture(const CaptureOptions &, Sink) {
    throw std::runtime_error("shared-windows requires Windows");
}
WindowsSharedCapture::~WindowsSharedCapture() = default;
std::vector<Packet> WindowsSharedCapture::drain() {
    return {};
}
std::optional<std::string> WindowsSharedCapture::resolve(const std::string &, uint16_t) {
    return {};
}
void WindowsSharedCapture::check() {}
void WindowsSharedCapture::stop() {}
Json windows_network_inventory() {
#ifdef __linux__
    return linux_network_inventory();
#else
    throw std::runtime_error("network-info requires Windows");
#endif
}
Json windows_capture_preflight(const CaptureOptions &options) {
#ifdef __linux__
    return linux_capture_preflight(options);
#else
    (void)options;
    throw std::runtime_error("shared preflight requires Windows");
#endif
}
} // namespace radaer
#endif
