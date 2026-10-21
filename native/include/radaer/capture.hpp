#pragma once
#include "monitor.hpp"
#include <atomic>
namespace radaer {
struct CaptureOptions {
    std::string listen = "127.0.0.1:19001", upstream, interface, client_ip, device, output, save_banks;
    std::vector<std::string> bank_files;
    double seconds = 60; // 0 keeps capture running until cancellation or a fatal error.
    bool relay = false, original_destination = false, synthetic = false, save_tcp_plaintext = false;
    bool save_udp_payload = false, save_logs = true;
    size_t max_log_bytes = default_journal_max_bytes;
    // Windows ICS / Mobile Hotspot: capture forwarded client traffic.
    bool shared_windows = false, shared_redirect = false, manage_firewall = true;
    bool linux_redirect = false;
    std::string windivert_dll, shared_host_ip, tcp_server;
    unsigned shared_if_index = 0, tcp_port = 0;
};
class CaptureFirewall {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    explicit CaptureFirewall(const CaptureOptions &options);
    ~CaptureFirewall();
};
int capture(const CaptureOptions &options, std::atomic<bool> *cancellation = nullptr, Sink observer = {});
std::vector<Bank> read_bank_files(const std::vector<std::string> &paths);
} // namespace radaer
