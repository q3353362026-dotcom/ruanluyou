#include "radaer/webui.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <netioapi.h>
// clang-format on
#undef interface
namespace radaer {
namespace {
std::string mac_string(const BYTE *bytes, ULONG count) {
    std::string mac;
    char part[4];
    for (ULONG i = 0; i < count; ++i) {
        std::snprintf(part, sizeof(part), "%02X", bytes[i]);
        if (i)
            mac += ':';
        mac += part;
    }
    return mac;
}
std::string neighbor_state(NL_NEIGHBOR_STATE state) {
    switch (state) {
    case NlnsReachable:
        return "reachable";
    case NlnsStale:
        return "stale";
    case NlnsDelay:
        return "delay";
    case NlnsProbe:
        return "probe";
    case NlnsPermanent:
        return "permanent";
    default:
        return "unconfirmed";
    }
}
} // namespace
struct LanDiscovery::Impl {
    std::mutex control, mutex;
    std::atomic<bool> cancelled{false};
    std::thread thread;
    Json state = {
        {"running", false}, {"completed", 0}, {"total", 0}, {"devices", Json::array()}, {"error", ""}};
    std::map<uint32_t, Json> devices;
    unsigned selected = 0;
    uint32_t host = 0;
    DiscoveryRange range;
    std::vector<std::string> gateways;
    void neighbors() {
        PMIB_IPNET_TABLE2 table = nullptr;
        auto rc = GetIpNetTable2(AF_INET, &table);
        if (rc != NO_ERROR)
            throw std::runtime_error("GetIpNetTable2 failed: " + std::to_string(rc));
        std::unique_ptr<MIB_IPNET_TABLE2, decltype(&FreeMibTable)> owned(table, FreeMibTable);
        std::lock_guard lock(mutex);
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            auto &row = table->Table[i];
            auto ip = ntohl(row.Address.Ipv4.sin_addr.s_addr);
            if (row.InterfaceIndex != selected || ip < range.first || ip > range.last || ip == host ||
                row.State == NlnsUnreachable || row.State == NlnsIncomplete || !row.PhysicalAddressLength ||
                row.PhysicalAddressLength > sizeof(row.PhysicalAddress))
                continue;
            auto address = ipv4_string(ip);
            auto &d = devices[ip];
            if (d.is_null())
                d = {{"ip", address}, {"name", ""}, {"self", false}, {"online", false}, {"rttMs", nullptr}};
            d["mac"] = mac_string(row.PhysicalAddress, row.PhysicalAddressLength);
            d["neighborState"] = neighbor_state(row.State);
            d["gateway"] = std::find(gateways.begin(), gateways.end(), address) != gateways.end();
            if (d.value("evidence", std::string()) != "icmp_reply") {
                d["online"] = row.State == NlnsReachable;
                d["evidence"] = row.State == NlnsReachable ? "neighbor_reachable" : "neighbor_cache";
            }
        }
    }
    void run(bool active) noexcept {
        try {
            neighbors();
            if (active) {
                std::atomic<uint32_t> next{range.first};
                std::atomic<unsigned> done{0};
                std::atomic<unsigned> failures{0};
                std::vector<std::jthread> workers;
                for (unsigned worker = 0; worker < 12; ++worker)
                    workers.emplace_back([&] {
                        HANDLE handle = IcmpCreateFile();
                        if (handle == INVALID_HANDLE_VALUE) {
                            ++failures;
                            return;
                        }
                        std::unique_ptr<void, decltype(&IcmpCloseHandle)> owned(handle, IcmpCloseHandle);
                        while (!cancelled) {
                            auto ip = next.fetch_add(1);
                            if (ip > range.last)
                                break;
                            if (ip == host)
                                continue;
                            char payload[] = "radaer-discovery";
                            alignas(ICMP_ECHO_REPLY) std::array<unsigned char, 1024> buffer{};
                            DWORD replies = IcmpSendEcho2Ex(handle, nullptr, nullptr, nullptr, htonl(host),
                                                            htonl(ip), payload, sizeof(payload), nullptr,
                                                            buffer.data(), DWORD(buffer.size()), 250);
                            auto *reply = reinterpret_cast<ICMP_ECHO_REPLY *>(buffer.data());
                            if (replies && reply->Status == IP_SUCCESS && ntohl(reply->Address) == ip) {
                                std::lock_guard lock(mutex);
                                auto address = ipv4_string(ip);
                                auto &d = devices[ip];
                                if (d.is_null())
                                    d = Json::object();
                                d["ip"] = address;
                                d["name"] = "";
                                d["self"] = false;
                                d["online"] = true;
                                d["evidence"] = "icmp_reply";
                                d["rttMs"] = reply->RoundTripTime;
                                d["seenAtMs"] = now_ms();
                                d["gateway"] =
                                    std::find(gateways.begin(), gateways.end(), address) != gateways.end();
                            }
                            auto completed = ++done;
                            {
                                std::lock_guard lock(mutex);
                                state["completed"] = done.load();
                            }
                            if (completed % 32 == 0) {
                                try {
                                    neighbors();
                                } catch (...) {
                                    ++failures;
                                }
                            }
                        }
                    });
                for (auto &worker : workers)
                    worker.join();
                if (failures) {
                    std::lock_guard lock(mutex);
                    state["warning"] = "部分探测失败；邻居缓存仍可作为发现线索。";
                }
            }
            neighbors();
        } catch (const std::exception &e) {
            std::lock_guard lock(mutex);
            state["error"] = e.what();
        }
        std::lock_guard lock(mutex);
        state["running"] = false;
        state["cancelled"] = cancelled.load();
        state["finishedAtMs"] = now_ms();
    }
    void cancel_and_join() {
        cancelled = true;
        if (thread.joinable())
            thread.join();
    }
    ~Impl() {
        cancel_and_join();
    }
};
LanDiscovery::LanDiscovery() : p_(std::make_unique<Impl>()) {}
LanDiscovery::~LanDiscovery() = default;
void LanDiscovery::start(unsigned index, const std::string &address, bool active) {
    auto inventory = windows_network_inventory();
    uint32_t host = parse_ipv4(address);
    std::optional<unsigned> prefix;
    std::vector<std::string> gateways;
    for (auto &a : inventory["adapters"])
        if (a["ifIndex"] == index && a["up"].get<bool>()) {
            for (auto &ip : a["ipv4"])
                if (ip["address"] == address)
                    prefix = ip["prefixLength"].get<unsigned>();
            if (a.contains("gateways"))
                gateways = a["gateways"].get<std::vector<std::string>>();
        }
    if (!prefix)
        throw std::runtime_error("请选择当前启用网卡的本机 IPv4 地址");
    auto range = discovery_range(host, *prefix);
    std::lock_guard control(p_->control);
    {
        std::lock_guard lock(p_->mutex);
        if (p_->state["running"].get<bool>())
            throw std::runtime_error("扫描正在进行，请等待或停止当前扫描");
    }
    p_->cancel_and_join();
    p_->cancelled = false;
    p_->selected = index;
    p_->host = host;
    p_->range = range;
    p_->gateways = gateways;
    {
        std::lock_guard lock(p_->mutex);
        p_->devices.clear();
        p_->devices[host] = {{"ip", address},   {"name", "采集主机"}, {"self", true},
                             {"online", true},  {"mac", ""},          {"evidence", "local_interface"},
                             {"rttMs", nullptr}};
        p_->state = {{"running", true},
                     {"active", active},
                     {"ifIndex", index},
                     {"hostIp", address},
                     {"completed", 0},
                     {"total", active ? range.last - range.first : 0},
                     {"limited", range.limited},
                     {"prefixLength", range.prefix},
                     {"rangeStart", ipv4_string(range.first)},
                     {"rangeEnd", ipv4_string(range.last)},
                     {"startedAtMs", now_ms()},
                     {"error", ""},
                     {"warning", ""}};
    }
    try {
        p_->thread = std::thread([this, active] { p_->run(active); });
    } catch (...) {
        std::lock_guard lock(p_->mutex);
        p_->state["running"] = false;
        throw;
    }
}
void LanDiscovery::cancel() {
    p_->cancelled = true;
}
Json LanDiscovery::status() {
    std::lock_guard lock(p_->mutex);
    Json result = p_->state;
    result["devices"] = Json::array();
    for (auto &[ip, d] : p_->devices)
        result["devices"].push_back(d);
    return result;
}
} // namespace radaer
#elif defined(__linux__)
#include "radaer/linux_network.hpp"
#include <fstream>
#include <sstream>
namespace radaer {
struct LanDiscovery::Impl {
    std::mutex mutex;
    Json state = {{"running", false}, {"devices", Json::array()}};
};
LanDiscovery::LanDiscovery() : p_(std::make_unique<Impl>()) {}
LanDiscovery::~LanDiscovery() = default;
void LanDiscovery::start(unsigned index, const std::string &host, bool) {
    const auto iface = linux_interface(index, host);
    Json devices = Json::array();
    devices.push_back({{"ip", host}, {"name", iface}, {"self", true}, {"online", true}});
    std::map<std::string, std::string> names;
    std::ifstream leases("/tmp/dhcp.leases");
    std::string line;
    while (std::getline(leases, line)) {
        std::istringstream row(line);
        std::string expires, mac, ip, name;
        if (row >> expires >> mac >> ip >> name && name != "*") names[ip] = name;
    }
    std::ifstream arp("/proc/net/arp");
    std::getline(arp, line);
    while (std::getline(arp, line)) {
        std::istringstream row(line);
        std::string ip, type, flags, mac, mask, device;
        if (!(row >> ip >> type >> flags >> mac >> mask >> device) || device != iface || ip == host) continue;
        if ((std::stoul(flags, nullptr, 16) & 2) == 0) continue;
        devices.push_back({{"ip", ip}, {"mac", mac}, {"name", names[ip]}, {"self", false},
                           {"online", false}, {"evidence", "neighbor_cache"}, {"rttMs", nullptr}});
    }
    std::lock_guard lock(p_->mutex);
    p_->state = {{"running", false}, {"ifIndex", index}, {"hostIp", host}, {"devices", devices},
                 {"completed", devices.size()}, {"total", devices.size()}, {"finishedAtMs", now_ms()},
                 {"error", ""}, {"warning", "已读取 LAN 邻居缓存；未出现的设备可手动输入 IP，缓存不代表当前在线。"}};
}
void LanDiscovery::cancel() {}
Json LanDiscovery::status() { std::lock_guard lock(p_->mutex); return p_->state; }
} // namespace radaer
#else
namespace radaer {
struct LanDiscovery::Impl {};
LanDiscovery::LanDiscovery() : p_(std::make_unique<Impl>()) {}
LanDiscovery::~LanDiscovery() = default;
void LanDiscovery::start(unsigned, const std::string &, bool) {
    throw std::runtime_error("LAN discovery currently requires Windows");
}
void LanDiscovery::cancel() {}
Json LanDiscovery::status() {
    return {{"running", false}, {"devices", Json::array()}};
}
} // namespace radaer
#endif
