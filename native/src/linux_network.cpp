#ifdef __linux__
#include "radaer/linux_network.hpp"
#include <arpa/inet.h>
#include <bit>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <stdexcept>

namespace radaer {
Json linux_network_inventory() {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) throw std::runtime_error("Cannot enumerate Linux interfaces");
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> owned(list, freeifaddrs);
    std::map<unsigned, Json> rows;
    for (auto *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_netmask) continue;
        auto index = if_nametoindex(a->ifa_name);
        if (!index) continue;
        auto &row = rows[index];
        if (row.is_null())
            row = {{"ifIndex", index}, {"name", a->ifa_name}, {"description", a->ifa_name},
                   {"up", bool(a->ifa_flags & IFF_UP)}, {"type", 6},
                   {"gateways", Json::array()}, {"ipv4", Json::array()}};
        auto ip = ntohl(reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr.s_addr);
        auto mask = ntohl(reinterpret_cast<sockaddr_in *>(a->ifa_netmask)->sin_addr.s_addr);
        row["ipv4"].push_back({{"address", ipv4_string(ip)}, {"prefixLength", std::popcount(mask)}});
    }
    Json adapters = Json::array();
    for (auto &[index, row] : rows) adapters.push_back(std::move(row));
    return {{"platform", "linux"}, {"elevated", geteuid() == 0}, {"adapters", adapters},
            {"sharingStateVerified", false}};
}

std::string linux_interface(unsigned index, const std::string &address) {
    const auto inventory = linux_network_inventory();
    for (const auto &a : inventory.at("adapters")) {
        if (a.at("ifIndex") != index || !a.at("up").get<bool>()) continue;
        for (const auto &ip : a.at("ipv4"))
            if (ip.at("address") == address) return a.at("name").get<std::string>();
    }
    throw std::runtime_error("所选 IPv4 地址不属于启用的采集接口");
}

Json linux_capture_preflight(const CaptureOptions &o) {
    const auto client = parse_ipv4(o.client_ip);
    bool valid = false;
    const auto inventory = linux_network_inventory();
    for (const auto &a : inventory.at("adapters")) {
        if (a.at("name") != o.interface || !a.at("up").get<bool>()) continue;
        for (const auto &address : a.at("ipv4")) {
            const auto host = parse_ipv4(address.at("address").get<std::string>());
            const auto prefix = address.at("prefixLength").get<unsigned>();
            if (!prefix || prefix > 30) continue;
            const auto mask = uint32_t(0xffffffffU << (32 - prefix));
            const bool private_host = (host >> 24) == 10 || (host >> 20) == 0xac1 || (host >> 16) == 0xc0a8;
            if (private_host && client != host && (client & mask) == (host & mask) &&
                (client & ~mask) != 0 && (client & ~mask) != ~mask) valid = true;
        }
    }
    if (!valid) throw std::runtime_error("客户机必须是所选 LAN 接口同网段的其他 IPv4 设备");
    if (o.linux_redirect && (!o.original_destination || !o.relay || o.tcp_port == 0 || o.tcp_port > 65535))
        throw std::runtime_error("Linux TCP 接入需要指定目标端口和 original-destination 中继");
    if (o.linux_redirect && access("/usr/sbin/nft", X_OK) != 0 && access("/sbin/nft", X_OK) != 0)
        throw std::runtime_error("TCP 接入需要安装 nftables（nft 命令）");
    return {{"configurationValid", true}, {"elevated", geteuid() == 0},
            {"interface", o.interface}, {"firewallManaged", o.linux_redirect},
            {"sharingConnectivityVerified", false}, {"udpMode", "af_packet_copy"}};
}
} // namespace radaer
#endif
