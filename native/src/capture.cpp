#include "radaer/capture.hpp"
#include "radaer/battle.hpp"
#include "radaer/ipv4_reassembly.hpp"
#include "radaer/players.hpp"
#include "radaer/shared_network.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using socklen_type = int;
constexpr socket_t bad_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/filter.h>
#include <linux/netfilter_ipv4.h>
#include <net/if.h>
#endif
using socket_t = int;
using socklen_type = socklen_t;
constexpr socket_t bad_socket = -1;
#endif
namespace radaer {
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop_handler(int) {
    stopped = 1;
}
void close_socket(socket_t s) {
    if (s == bad_socket)
        return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
struct Socket {
    socket_t fd = bad_socket;
    Socket() = default;
    explicit Socket(socket_t s) : fd(s) {}
    ~Socket() {
        close_socket(fd);
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
};
bool would_block() {
#ifdef _WIN32
    auto e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
}
void nonblock(socket_t s) {
#ifdef _WIN32
    u_long v = 1;
    if (ioctlsocket(s, FIONBIO, &v) != 0)
        throw std::runtime_error("nonblocking socket failed");
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("nonblocking socket failed");
    if (s >= FD_SETSIZE)
        throw std::runtime_error("descriptor exceeds select limit");
#endif
}
sockaddr_in endpoint(const std::string &name, bool allow_zero = false) {
    auto p = name.rfind(':');
    if (p == std::string::npos)
        throw std::runtime_error("expected numeric IPv4:port");
    auto value = name.substr(p + 1);
    unsigned port = 0;
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
    if (ec != std::errc() || end != value.data() + value.size() || (!port && !allow_zero) || port > 65535)
        throw std::runtime_error("invalid port");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, name.substr(0, p).c_str(), &a.sin_addr) != 1)
        throw std::runtime_error("numeric IPv4 address required");
    return a;
}
std::string address(sockaddr_in a) {
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip)))
        throw std::runtime_error("IP formatting failure");
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}
[[maybe_unused]] uint16_t be16(View b, size_t i) {
    return uint16_t(uint16_t(b[i]) << 8 | b[i + 1]);
}
void save_bank(const std::string &dir, const Bank &b) {
    if (dir.empty())
        return;
    std::filesystem::create_directories(dir);
    auto path = std::filesystem::path(dir) / (fingerprint(b) + ".bin");
    if (std::filesystem::exists(path))
        return;
#ifdef _WIN32
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(b.data()), 128);
    if (!out)
        throw std::runtime_error("bank file write failed");
#else
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot create bank file");
    auto n = write(fd, b.data(), 128);
    close(fd);
    if (n != 128)
        throw std::runtime_error("bank file write failed");
#endif
}
struct Connection {
    Socket client, server;
    std::unique_ptr<TgcpSession> protocol;
    Bytes to_client, to_server;
    bool connecting = true, dead = false;
    std::string id;
    int64_t started = 0, last = 0, sniff_started = 0, handshake_started = 0;
    bool eof_client = false, eof_server = false;
};
void queue(Bytes &dst, const Bytes &src) {
    if (dst.size() + src.size() > 20 * 1024 * 1024)
        throw std::runtime_error("relay backpressure buffer exceeded");
    dst.insert(dst.end(), src.begin(), src.end());
}
void flush(socket_t fd, Bytes &q) {
    if (q.empty())
        return;
#ifdef _WIN32
    auto n = send(fd, reinterpret_cast<const char *>(q.data()), int(q.size()), 0);
#else
    auto n = send(fd, q.data(), q.size(), MSG_NOSIGNAL);
#endif
    if (n > 0)
        q.erase(q.begin(), q.begin() + n);
    else if (n == 0 || !would_block())
        throw std::runtime_error("relay write failed");
}
} // namespace
std::vector<Bank> read_bank_files(const std::vector<std::string> &paths) {
    std::vector<Bank> banks;
    for (auto &p : paths) {
        std::ifstream f(p, std::ios::binary);
        if (!f)
            throw std::runtime_error("cannot read bank file " + p);
        Bytes b((std::istreambuf_iterator<char>(f)), {});
        if (b.size() != 128)
            throw std::runtime_error("bank file must contain exactly 128 raw bytes");
        Bank k{};
        std::copy(b.begin(), b.end(), k.begin());
        banks.push_back(k);
    }
    return banks;
}
int capture(const CaptureOptions &options, std::atomic<bool> *cancellation, Sink observer) {
    CaptureOptions o = options;
    if (!std::isfinite(o.seconds) || o.seconds < 0 || o.seconds > 86400 * 7)
        throw std::runtime_error("seconds must be in [0,604800]; 0 means unlimited");
    if (o.udp_port > 65535)
        throw std::runtime_error("UDP port outside range");
    if (!o.relay && o.interface.empty() && !o.shared_windows)
        throw std::runtime_error("choose --interface, --relay true, or --shared-windows true");
    if (o.relay && (int(o.original_destination) + int(!o.upstream.empty()) + int(o.shared_redirect) != 1))
        throw std::runtime_error(
            "relay needs exactly one upstream/original-destination/shared-redirect mode");
    if (!o.shared_windows && (o.shared_redirect || o.shared_if_index || !o.shared_host_ip.empty() ||
                              !o.windivert_dll.empty() || (o.tcp_port && !o.linux_redirect) || !o.tcp_server.empty()))
        throw std::runtime_error("sharing options require --shared-windows true");
    if (o.shared_windows)
        windows_capture_preflight(o);
    if (o.linux_redirect)
        windows_capture_preflight(o);
    if (!o.interface.empty() && o.client_ip.empty())
        throw std::runtime_error("--interface requires --client-ip");
#ifndef __linux__
    if (!o.interface.empty() || o.original_destination)
        throw std::runtime_error("passive interface capture/original destination requires Linux; use Windows "
                                 "replay or explicit TCP relay");
#endif
#ifdef _WIN32
    struct Wsa {
        Wsa() {
            WSADATA w{};
            if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
                throw std::runtime_error("WSAStartup failed");
        }
        ~Wsa() {
            WSACleanup();
        }
    } wsa;
#endif
    auto banks = read_bank_files(o.bank_files);
    Socket listener, udp;
    in_addr client_ip{};
    int receive_buffer = 0;
    uint64_t dropped_total = 0, packets_total = 0;
    int64_t max_queue_delay = 0;
    if (!o.client_ip.empty() && inet_pton(AF_INET, o.client_ip.c_str(), &client_ip) != 1)
        throw std::runtime_error("invalid client IP");
    if (o.relay) {
        auto a = endpoint(o.listen, true);
        listener.fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listener.fd == bad_socket)
            throw std::runtime_error("listener creation failed");
        int one = 1;
#ifdef _WIN32
        setsockopt(listener.fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&one),
                   sizeof(one));
#else
        setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof(one));
#endif
        if (bind(listener.fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 ||
            listen(listener.fd, SOMAXCONN) != 0)
            throw std::runtime_error("relay bind/listen failed");
        socklen_type length = sizeof(a);
        if (getsockname(listener.fd, reinterpret_cast<sockaddr *>(&a), &length) != 0 || !a.sin_port)
            throw std::runtime_error("cannot obtain bound TCP listener port");
        o.listen = address(a);
        nonblock(listener.fd);
    }
#ifdef __linux__
    if (!o.interface.empty()) {
        auto index = if_nametoindex(o.interface.c_str());
        if (!index)
            throw std::runtime_error("unknown capture interface");
        // Protocol-specific packet sockets miss the transmit tap. Server replies
        // leave the LAN interface, so subscribe to ETH_P_ALL and filter IPv4 below.
        udp.fd = socket(AF_PACKET, SOCK_DGRAM, 0);
        if (udp.fd < 0)
            throw std::runtime_error("AF_PACKET requires root or CAP_NET_RAW");
        // SOCK_DGRAM exposes the IP header at offset zero. Filter before the
        // socket queue, keeping both directions and all IPv4 UDP fragments.
        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, uint32_t(SKF_AD_OFF + SKF_AD_PROTOCOL)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_IP, 0, 7),
            BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 9),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_UDP, 0, 5),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 12),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohl(client_ip.s_addr), 2, 0),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 16),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohl(client_ip.s_addr), 0, 1),
            BPF_STMT(BPF_RET | BPF_K, 65535),
            BPF_STMT(BPF_RET | BPF_K, 0),
        };
        sock_fprog program{static_cast<unsigned short>(std::size(filter)), filter};
        if (setsockopt(udp.fd, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) < 0)
            throw std::runtime_error("cannot install client UDP capture filter");
        sockaddr_ll a{};
        a.sll_family = AF_PACKET;
        a.sll_protocol = htons(ETH_P_ALL);
        a.sll_ifindex = int(index);
        nonblock(udp.fd);
        int timestamps = 1;
        if (setsockopt(udp.fd, SOL_SOCKET, SO_TIMESTAMPNS, &timestamps, sizeof(timestamps)) < 0)
            throw std::runtime_error("kernel packet timestamps unavailable");
        // 环形缓冲大小：旧值 64MB 是致命的。
        // SO_RCVBUFFORCE 会强制内核分配 64MB 大块内存（kmalloc），AX3600 只有 400MB
        // 总内存，这块内存锁死不参与回收；绝密模式 UDP 爆量时缓冲被反复填满，
        // 内存带宽和 cache 全被冲掉，还会触发内核 compaction 停顿
        // => 整机卡死（WiFi/SSH/转发全挂）。
        // 8MB 足够吸收换图瞬间的 burst（实测峰值 ~2MB/s * 2s），且不会压垮内存。
        int size = 8 * 1024 * 1024;
        // CAP_NET_ADMIN can raise this socket's limit without changing global sysctls.
        if (setsockopt(udp.fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size)) < 0 &&
            setsockopt(udp.fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)
            throw std::runtime_error("cannot configure packet receive buffer");
        socklen_t size_length = sizeof(receive_buffer);
        if (getsockopt(udp.fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, &size_length) < 0)
            throw std::runtime_error("cannot read packet receive buffer");
        if (bind(udp.fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0)
            throw std::runtime_error("packet interface bind failed");
    }
#endif
    #ifdef __linux__
    // CPU/IO 优先级降到最低：radaer 是旁路观测进程，绝不能和路由转发抢资源。
    // AX3600 是 4 核 A53 @1.0GHz，绝密模式 UDP 爆量时 radaer 单核吃满会拖垮整机
    // （WiFi/SSH/转发全卡）。nice 19 + idle IO 让内核调度永远优先保证转发。
    // 代价：雷达数据可能滞后几百毫秒，但网络绝不断。
    setpriority(PRIO_PROCESS, 0, 19);
    {
        // IOPRIO_CLASS_IDLE = 3，最低 IO 优先级
        int ioprio = (3 << 13);
        syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, ioprio);
    }
#endif
    Journal journal(o.output, o.device, o.synthetic, std::move(observer), o.save_logs, o.max_log_bytes);
    BattleTracker battle([&](Json e) { journal.emit(std::move(e)); });
    auto record = [&](Json e) {
        e["session"] = journal.session();
        e["device"] = o.device;
        e["synthetic"] = o.synthetic;
        if (!e.contains("timestampMs"))
            e["timestampMs"] = now_ms();
        journal.emit(e);
        battle.consume(e);
    };
    PlayerValidator player_validator;
    PlayerDecoder players([&](Json e) {
        if (e.at("type") == "player_sample" || e.at("type") == "self_sample" || e.at("type") == "self_server_sample")
            record(player_validator.inspect(e));
        record(std::move(e));
    });
    BankBroker broker([&](Json e) {
        e["session"] = journal.session();
        e["device"] = o.device;
        e["synthetic"] = o.synthetic;
        if (!e.contains("timestampMs"))
            e["timestampMs"] = now_ms();
        Json persisted = e;
        if (!o.save_udp_payload && persisted["type"] == "udp_plaintext")
            persisted["data"].erase("payloadBase64");
        record(std::move(persisted));
        players.consume(e);
    });
    record({{"type", "session_started"},
            {"data",
             {{"mode", "capture"},
              {"interface", o.interface},
              {"relay", o.relay},
              {"sharedWindows", o.shared_windows},
              {"sharedRedirect", o.shared_redirect},
              {"sharedHostIp", o.shared_host_ip},
              {"clientIp", o.client_ip},
              {"udpPortFilter", o.udp_port},
              {"tcpPortFilter", o.tcp_port},
              {"savesBankMaterial", !o.save_banks.empty()},
              {"savesTcpPlaintext", o.save_tcp_plaintext},
              {"savesUdpPayload", o.save_udp_payload},
              {"maxLogBytes", o.max_log_bytes}}}});
    if (o.relay)
        record(
            {{"type", "tcp_listener_started"},
             {"data",
              {{"listen", o.listen},
               {"automaticPort", endpoint(options.listen, true).sin_port == 0},
               {"tcpMode", o.shared_redirect
                               ? ((o.tcp_port || !o.tcp_server.empty()) ? "filtered" : "automatic_all_ports")
                               : "relay"}}}});
    for (auto &b : banks)
        broker.candidate(b, "explicit_bank_file", now_ms());
    // Hold the bound listener throughout firewall setup and packet interception.
    // Destruction stops capture before removing the rule and closing the listener.
    CaptureFirewall firewall(o);
    std::unique_ptr<WindowsSharedCapture> shared;
    if (o.shared_windows)
        shared = std::make_unique<WindowsSharedCapture>(o, [&](Json e) {
            if (e.value("type", std::string()) == "capture_gap")
                broker.gap(e.at("timestampMs"), e.at("data").at("reason"),
                           e.at("data").value("sinceMs", int64_t(0)));
            else
                record(std::move(e));
        });
    std::vector<std::unique_ptr<Connection>> connections;
    uint64_t conn_id = 0, ordinal = 0;
    int64_t last_stats = now_ms();
#ifdef __linux__
    Ipv4UdpReassembler reassembler;
    auto steady_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
#endif
    // Embedded capture uses cooperative cancellation and leaves WebUI signals alone.
    struct Signals {
        bool enabled;
        decltype(&stop_handler) previous_int = nullptr, previous_term = nullptr;
        explicit Signals(bool enabled) : enabled(enabled) {
            if (enabled) {
                stopped = 0;
                previous_int = std::signal(SIGINT, stop_handler);
                previous_term = std::signal(SIGTERM, stop_handler);
            }
        }
        ~Signals() {
            if (enabled) {
                std::signal(SIGINT, previous_int);
                std::signal(SIGTERM, previous_term);
            }
        }
    } signals(cancellation == nullptr);
    auto cancelled = [&] { return cancellation ? cancellation->load() : stopped != 0; };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(o.seconds * 1000));
    std::cerr << "capture session " << journal.session() << " -> " << o.output << '\n';
    auto drain_shared = [&] {
        if (!shared)
            return;
        for (auto &p : shared->drain()) {
            p.ordinal = ++ordinal;
            record(udp_raw_event(p, o.save_udp_payload, "WinDivert_QPC"));
            broker.packet(std::move(p));
        }
        shared->check();
    };
    while (!cancelled() && (o.seconds == 0 || std::chrono::steady_clock::now() < deadline)) {
        drain_shared();
        battle.tick();
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        socket_t maxfd = 0;
        auto watch = [&](socket_t s, bool r, bool w) {
            if (s == bad_socket)
                return;
            if (r)
                FD_SET(s, &rd);
            if (w)
                FD_SET(s, &wr);
            maxfd = std::max(maxfd, s);
        };
        watch(listener.fd, true, false);
        watch(udp.fd, true, false);
        for (auto &c : connections) {
            watch(c->client.fd, !c->eof_client && c->to_server.size() < 1024 * 1024, !c->to_client.empty());
            watch(c->server.fd, !c->connecting && !c->eof_server && c->to_client.size() < 1024 * 1024,
                  c->connecting || !c->to_server.empty());
        }
        timeval timeout{0, shared ? 20000 : 200000};
        int ready = 0;
#ifdef _WIN32
        if (rd.fd_count == 0 && wr.fd_count == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        else
#endif
            ready = select(int(maxfd + 1), &rd, &wr, nullptr, &timeout);
        if (ready < 0) {
            if (cancelled())
                break;
#ifndef _WIN32
            if (errno == EINTR)
                continue;
#endif
            throw std::runtime_error("select failed");
        }
        int64_t ts = now_ms();
        if (listener.fd != bad_socket && FD_ISSET(listener.fd, &rd)) {
            sockaddr_in source{};
            socklen_type sl = sizeof(source);
            Socket incoming(accept(listener.fd, reinterpret_cast<sockaddr *>(&source), &sl));
            if (incoming.fd != bad_socket) {
                if (connections.size() >= std::min<size_t>(256, (FD_SETSIZE - 2) / 2) ||
                    (!o.client_ip.empty() && source.sin_addr.s_addr != client_ip.s_addr)) {
                    record({{"type", "tcp_rejected"},
                            {"data", {{"reason", "client_filter_or_connection_limit"}}}});
                } else {
                    auto c = std::make_unique<Connection>();
                    c->client.fd = incoming.fd;
                    incoming.fd = bad_socket;
                    c->id = std::to_string(++conn_id);
                    c->started = c->last = ts;
                    try {
                        nonblock(c->client.fd);
                        sockaddr_in dest{};
#ifdef __linux__
                        if (o.original_destination) {
                            socklen_t len = sizeof(dest);
                            if (getsockopt(c->client.fd, SOL_IP, SO_ORIGINAL_DST, &dest, &len) < 0)
                                throw std::runtime_error("SO_ORIGINAL_DST failed");
                            sockaddr_in local{};
                            socklen_t ll = sizeof(local);
                            getsockname(c->client.fd, reinterpret_cast<sockaddr *>(&local), &ll);
                            if (dest.sin_addr.s_addr == local.sin_addr.s_addr &&
                                dest.sin_port == local.sin_port)
                                throw std::runtime_error("original destination points back to relay");
                        } else
#endif
                        {
                            if (o.shared_redirect) {
                                auto peer = ipv4_string(ntohl(source.sin_addr.s_addr));
                                auto original = shared->resolve(peer, ntohs(source.sin_port));
                                if (!original)
                                    throw std::runtime_error("no sharing route for accepted connection");
                                dest = endpoint(*original);
                            } else
                                dest = endpoint(o.upstream);
                        }
                        c->server.fd = socket(AF_INET, SOCK_STREAM, 0);
                        if (c->server.fd == bad_socket)
                            throw std::runtime_error("upstream socket failed");
                        int no_delay = 1;
                        for (auto fd : {c->client.fd, c->server.fd})
                            if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                                           reinterpret_cast<const char *>(&no_delay), sizeof(no_delay)) != 0)
                                throw std::runtime_error("cannot set TCP_NODELAY");
                        nonblock(c->server.fd);
                        int rc = connect(c->server.fd, reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
                        if (rc == 0)
                            c->connecting = false;
                        else if (!would_block())
                            throw std::runtime_error("upstream connect failed");
                        if (!c->connecting)
                            c->sniff_started = ts;
                        std::string id = c->id;
                        c->protocol = std::make_unique<TgcpSession>(
                            [&, id](Json e) {
                                e["data"]["tcpConnection"] = id;
                                if (e["type"] == "tcp_plaintext" && !o.save_tcp_plaintext)
                                    e["data"].erase("payloadBase64");
                                record(std::move(e));
                            },
                            [&, id](const Bank &bank) {
                                save_bank(o.save_banks, bank);
                                broker.candidate(bank, "tcp:" + id, now_ms());
                            });
                        record(
                            {{"type", "tcp_connected"},
                             {"data",
                              {{"connection", id}, {"client", address(source)}, {"server", address(dest)}}}});
                        connections.push_back(std::move(c));
                    } catch (const std::exception &e) {
                        record(
                            {{"type", "tcp_error"}, {"data", {{"connection", c->id}, {"error", e.what()}}}});
                    }
                }
            }
        }
        for (auto &c : connections) {
            try {
                if (c->connecting && FD_ISSET(c->server.fd, &wr)) {
                    int error = 0;
                    socklen_type len = sizeof(error);
                    if (getsockopt(c->server.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error),
                                   &len) != 0 ||
                        error)
                        throw std::runtime_error("upstream connect completion failed");
                    c->connecting = false;
                    c->sniff_started = ts;
                }
                if (c->connecting && ts - c->started > 10000)
                    throw std::runtime_error("connect timeout");
                if (!c->connecting && !c->protocol->committed() && !c->protocol->bypass() &&
                    ts - c->sniff_started >= 1000) {
                    auto result = c->protocol->pass_through("prefix_timeout");
                    queue(c->to_server, result.to_server);
                    queue(c->to_client, result.to_client);
                }
                if (c->protocol->committed() && !c->protocol->established() &&
                    ts - c->handshake_started > 10000)
                    throw std::runtime_error("handshake timeout");
                if (!c->protocol->bypass() && ts - c->last > 120000)
                    throw std::runtime_error("relay idle timeout");
                for (int direction = 0; direction < 2; direction++) {
                    bool up = direction == 0;
                    socket_t fd = up ? c->client.fd : c->server.fd;
                    if ((!up && c->connecting) || !FD_ISSET(fd, &rd))
                        continue;
                    std::array<uint8_t, 65536> buf{};
                    auto n = recv(fd, reinterpret_cast<char *>(buf.data()), int(buf.size()), 0);
                    if (n == 0) {
                        if (!c->protocol->committed() && !c->protocol->bypass()) {
                            auto result = c->protocol->pass_through(up ? "client_eof" : "server_first");
                            queue(c->to_server, result.to_server);
                            queue(c->to_client, result.to_client);
                        }
                        if (c->protocol->buffered())
                            throw std::runtime_error("EOF with incomplete TGCP frame");
                        (up ? c->eof_client : c->eof_server) = true;
                    } else if (n < 0) {
                        if (!would_block())
                            throw std::runtime_error("relay read failed");
                    } else {
                        c->last = ts;
                        auto result = c->protocol->feed(up, View(buf).first(size_t(n)));
                        if (c->protocol->committed() && !c->handshake_started)
                            c->handshake_started = ts;
                        queue(c->to_server, result.to_server);
                        queue(c->to_client, result.to_client);
                    }
                }
                if (!c->connecting && FD_ISSET(c->server.fd, &wr))
                    flush(c->server.fd, c->to_server);
                if (FD_ISSET(c->client.fd, &wr))
                    flush(c->client.fd, c->to_client);
                if (c->eof_client && c->to_server.empty())
                    shutdown(c->server.fd, 1);
                if (c->eof_server && c->to_client.empty())
                    shutdown(c->client.fd, 1);
                if (c->eof_client && c->eof_server && c->to_server.empty() && c->to_client.empty())
                    c->dead = true;
            } catch (const std::exception &e) {
                c->dead = true;
                record({{"type", "tcp_error"},
                        {"data",
                         {{"connection", c->id},
                          {"error", e.what()},
                          {"dhCommitted", c->protocol->committed()}}}});
            }
        }
        connections.erase(
            std::remove_if(connections.begin(), connections.end(), [](auto &c) { return c->dead; }),
            connections.end());
#ifdef __linux__
        if (reassembler.expire(steady_ms()))
            broker.gap(ts, "ipv4_fragment_timeout");
        if (udp.fd != bad_socket && FD_ISSET(udp.fd, &rd)) {
            // 时间预算：换图/开局时游戏会瞬间爆量（重载地图 + 全实体同步），
            // 旧实现每轮死循环处理 128 个包，每个包都要 XTEA 解密 + UE 解析，
            // 单线程把 CPU 吃满 => AX3600 整体无响应（WiFi/SSH/转发全卡）。
            // 改成：每轮最多 8ms 或 32 个包，剩下的留给下一轮，保证主循环能喘气。
            const auto budget_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
            for (int batch = 0; batch < 32; batch++) {
                if (std::chrono::steady_clock::now() >= budget_end)
                    break;
                std::array<uint8_t, 65536> buffer{};
                alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(timespec))> control{};
                iovec io{buffer.data(), buffer.size()};
                sockaddr_ll packet_address{};
                msghdr message{};
                message.msg_name = &packet_address;
                message.msg_namelen = sizeof(packet_address);
                message.msg_iov = &io;
                message.msg_iovlen = 1;
                message.msg_control = control.data();
                message.msg_controllen = control.size();
                auto n = recvmsg(udp.fd, &message, 0);
                if (n < 0) {
                    if (would_block())
                        break;
                    throw std::runtime_error("packet capture receive failed");
                }
                if (message.msg_namelen < sizeof(packet_address) ||
                    packet_address.sll_protocol != htons(ETH_P_IP))
                    continue;
                int64_t packet_ts = 0;
                for (auto *cm = CMSG_FIRSTHDR(&message); cm; cm = CMSG_NXTHDR(&message, cm)) {
                    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPNS &&
                        cm->cmsg_len >= CMSG_LEN(sizeof(timespec))) {
                        timespec stamp{};
                        std::memcpy(&stamp, CMSG_DATA(cm), sizeof(stamp));
                        packet_ts = int64_t(stamp.tv_sec) * 1000 + stamp.tv_nsec / 1000000;
                    }
                }
                if (!packet_ts || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
                    broker.gap(ts, "packet_or_timestamp_truncated", last_stats);
                    continue;
                }
                View b(buffer.data(), size_t(n));
                max_queue_delay = std::max(max_queue_delay, std::max(int64_t(0), now_ms() - packet_ts));
                if (b.size() < 20 || (b[0] >> 4) != 4 || b[9] != 17)
                    continue;
                size_t h = (b[0] & 15) * 4, total = be16(b, 2);
                if (h < 20 || total < h || total > b.size())
                    continue;
                in_addr src{}, dst{};
                std::memcpy(&src, b.data() + 12, 4);
                std::memcpy(&dst, b.data() + 16, 4);
                bool up = src.s_addr == client_ip.s_addr;
                if (!up && dst.s_addr != client_ip.s_addr)
                    continue;
                auto rebuilt = reassembler.push(b, steady_ms(), packet_ts);
                if (rebuilt.status == Ipv4UdpReassembler::Status::pending)
                    continue;
                if (rebuilt.status == Ipv4UdpReassembler::Status::rejected) {
                    broker.gap(ts, rebuilt.reason);
                    continue;
                }
                const auto completed_ts = packet_ts;
                packet_ts = rebuilt.timestamp;
                b = View(rebuilt.packet);
                h = (b[0] & 15) * 4;
                total = b.size();
                if (total < h + 8)
                    continue;
                auto sport = be16(b, h), dport = be16(b, h + 2), len = be16(b, h + 4);
                if (len < 8 || h + len > total)
                    continue;
                if (o.udp_port && (up ? dport : sport) != o.udp_port)
                    continue;
                sockaddr_in sa{}, da{};
                sa.sin_family = da.sin_family = AF_INET;
                sa.sin_addr = src;
                da.sin_addr = dst;
                sa.sin_port = htons(sport);
                da.sin_port = htons(dport);
                Packet p{up,
                         Bytes(b.begin() + h + 8, b.begin() + h + len),
                         packet_ts,
                         address(up ? sa : da),
                         address(up ? da : sa),
                         ++ordinal,
                         rebuilt.fragments,
                         std::max(int64_t(0), completed_ts - packet_ts)};
                record(udp_raw_event(p, o.save_udp_payload, "kernel_SO_TIMESTAMPNS"));
                broker.packet(std::move(p));
            }
        }
        if (udp.fd != bad_socket && ts - last_stats >= 1000) {
            tpacket_stats stats{};
            socklen_t len = sizeof(stats);
            if (getsockopt(udp.fd, SOL_PACKET, PACKET_STATISTICS, &stats, &len) == 0) {
                dropped_total += stats.tp_drops;
                packets_total += stats.tp_packets;
                // 内核环形缓冲丢包只记录、不废局。
                // 原因：UDP 本身无重传，丢几个包由上层 UE 协议自行恢复；
                // 换图/加载高峰必然丢包，若触发 capture_gap 会清空全部流，
                // 导致整局雷达失效（battle.cpp:97 只有 battle_welcome 能恢复）。
                // 真正的数据损坏由 MSG_TRUNC / IPv4 重组失败判定（见上方）。
                auto data = reassembler.stats();
                data.update(Json{{"packets", stats.tp_packets}, {"drops", stats.tp_drops},
                                 {"packetsTotal", packets_total}, {"dropsTotal", dropped_total},
                                 {"receiveBufferBytes", receive_buffer}, {"maxQueueDelayMs", max_queue_delay},
                                 {"kernelFilter", "client_ipv4_udp"}});
                record({{"type", "capture_stats"}, {"timestampMs", ts}, {"data", std::move(data)}});
                max_queue_delay = 0;
            }
            last_stats = ts;
        }
#else
        (void)ordinal;
        (void)last_stats;
#endif
    }
    connections.clear();
#ifdef __linux__
    if (udp.fd != bad_socket)
        record({{"type", "udp_reassembly_summary"}, {"data", reassembler.stats()}});
#endif
    if (shared) {
        shared->stop();
        drain_shared();
    }
    record({{"type", "player_decoder_summary"}, {"data", players.summary()}});
    battle.consume({{"type", "session_stopped"}});
    record({{"type", "session_stopped"},
            {"data", {{"reason", cancelled() ? "requested_stop" : "duration_elapsed"}}}});
    journal.finish();
    return 0;
}
} // namespace radaer
