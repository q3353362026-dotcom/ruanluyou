#pragma once
#include "capture.hpp"
#include <mutex>
namespace radaer {
struct Ipv4Transport {
    uint32_t source = 0, destination = 0, sequence = 0;
    uint16_t source_port = 0, destination_port = 0;
    uint8_t protocol = 0, tcp_flags = 0;
    size_t ip_length = 0, total_length = 0, payload_offset = 0, payload_length = 0;
};
std::optional<Ipv4Transport> parse_ipv4_transport(View packet);
uint32_t parse_ipv4(const std::string &text);
std::string ipv4_string(uint32_t ip);
void rewrite_tcp_endpoints(Bytes &packet, const Ipv4Transport &t, uint32_t src, uint16_t src_port,
                           uint32_t dst, uint16_t dst_port);
struct SharedRoute {
    uint16_t client_port = 0, server_port = 0, alias_port = 0;
    uint32_t client_ip = 0, server_ip = 0, initial_sequence = 0;
    int64_t last_seen = 0;
    bool retired = false;
};
class SharedRoutes {
    std::mutex mutex_;
    std::map<std::string, uint16_t> routes_;
    std::map<uint16_t, SharedRoute> aliases_;
    uint16_t next_alias_ = 20000;

  public:
    std::optional<SharedRoute> forward(const Ipv4Transport &t, int64_t ts);
    std::optional<SharedRoute> reverse(uint16_t alias, int64_t ts);
    bool original_downstream(const Ipv4Transport &t);
};
// The driver worker only queues copied UDP packets. Broker/decryption stays on
// the capture loop thread; UDP sniff handles never reinject traffic.
class WindowsSharedCapture {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    WindowsSharedCapture(const CaptureOptions &options, Sink events);
    ~WindowsSharedCapture();
    WindowsSharedCapture(const WindowsSharedCapture &) = delete;
    WindowsSharedCapture &operator=(const WindowsSharedCapture &) = delete;
    std::vector<Packet> drain();
    std::optional<std::string> resolve(const std::string &peer_ip, uint16_t alias);
    void check();
    void stop();
};
Json windows_network_inventory();
Json windows_capture_preflight(const CaptureOptions &options);
} // namespace radaer
