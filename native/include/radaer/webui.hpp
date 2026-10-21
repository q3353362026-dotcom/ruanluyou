#pragma once
#include "shared_network.hpp"
#include <atomic>
namespace radaer {
struct DiscoveryRange {
    uint32_t first = 0, last = 0;
    unsigned prefix = 0;
    bool limited = false;
};
DiscoveryRange discovery_range(uint32_t host, unsigned prefix);
class LanDiscovery {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    LanDiscovery();
    ~LanDiscovery();
    void start(unsigned if_index, const std::string &host, bool active = true);
    void cancel();
    Json status();
};
struct WebOptions {
    unsigned port = 8765;
    bool open_browser = false, lan = false;
    unsigned lan_if_index = 0;
    std::string lan_ip;
    std::string assets, output_directory, windivert_dll;
};
int webui(const WebOptions &options);
} // namespace radaer
