#include "radaer/capture.hpp"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <netfw.h>
#include <windows.h>
#undef interface
namespace radaer {
namespace {
void checked(HRESULT result, const char *what) {
    if (FAILED(result))
        throw std::runtime_error(std::string(what) + " HRESULT=" + std::to_string(uint32_t(result)));
}
template <class T> struct Com {
    T *p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T *operator->() {
        return p;
    }
};
struct Bstr {
    BSTR p;
    explicit Bstr(const std::wstring &value) : p(SysAllocString(value.c_str())) {
        if (!p)
            throw std::bad_alloc();
    }
    ~Bstr() {
        SysFreeString(p);
    }
    operator BSTR() const {
        return p;
    }
};
std::wstring wide(const std::string &s) {
    return std::wstring(s.begin(), s.end());
}
} // namespace
struct CaptureFirewall::Impl {
    bool initialized = false, added = false;
    Com<INetFwPolicy2> policy;
    Com<INetFwRules> rules;
    Com<INetFwRule> rule;
    std::wstring name;
    BSTR rule_name = nullptr;
    Impl() {
        auto rc = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        checked(rc, "COM initialization");
        initialized = true;
    }
    void add(const CaptureOptions &o) {
        checked(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                 __uuidof(INetFwPolicy2), reinterpret_cast<void **>(&policy.p)),
                "Firewall policy");
        checked(policy->get_Rules(&rules.p), "Firewall rules");
        checked(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule),
                                 reinterpret_cast<void **>(&rule.p)),
                "Firewall rule");
        GUID guid{};
        checked(CoCreateGuid(&guid), "Rule GUID");
        wchar_t guid_text[40]{};
        StringFromGUID2(guid, guid_text, 40);
        name = L"RadaerNativeShared-" + std::wstring(guid_text);
        rule_name = SysAllocString(name.c_str());
        if (!rule_name)
            throw std::bad_alloc();
        wchar_t exe[32768]{};
        auto length = GetModuleFileNameW(nullptr, exe, 32768);
        if (!length || length == 32768)
            throw std::runtime_error("Cannot resolve executable path");
        auto port = o.listen.substr(o.listen.rfind(':') + 1);
        checked(rule->put_Name(Bstr(name)), "Rule name");
        checked(rule->put_ApplicationName(Bstr(exe)), "Rule executable");
        checked(rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP), "Rule protocol");
        checked(rule->put_LocalAddresses(Bstr(wide(o.shared_host_ip))), "Rule local IP");
        checked(rule->put_RemoteAddresses(Bstr(wide(o.client_ip))), "Rule client IP");
        checked(rule->put_LocalPorts(Bstr(wide(port))), "Rule port");
        checked(rule->put_Direction(NET_FW_RULE_DIR_IN), "Rule direction");
        checked(rule->put_Profiles(NET_FW_PROFILE2_ALL), "Rule profile");
        checked(rule->put_Action(NET_FW_ACTION_ALLOW), "Rule action");
        checked(rule->put_Enabled(VARIANT_TRUE), "Rule enabled");
        checked(rules->Add(rule.p), "Add capture firewall rule");
        added = true;
    }
    ~Impl() {
        if (added && FAILED(rules->Remove(rule_name)))
            std::cerr << "Warning: cannot remove temporary RadaerNativeShared firewall rule. Check Windows "
                         "Firewall.\n";
        if (rule_name)
            SysFreeString(rule_name);
        // Release COM interfaces before uninitializing their apartment.
        if (rule.p) {
            rule.p->Release();
            rule.p = nullptr;
        }
        if (rules.p) {
            rules.p->Release();
            rules.p = nullptr;
        }
        if (policy.p) {
            policy.p->Release();
            policy.p = nullptr;
        }
        if (initialized)
            CoUninitialize();
    }
};
CaptureFirewall::CaptureFirewall(const CaptureOptions &o) {
    if (o.shared_redirect && o.manage_firewall) {
        p_ = std::make_unique<Impl>();
        p_->add(o);
    }
}
CaptureFirewall::~CaptureFirewall() = default;
} // namespace radaer
#elif defined(__linux__)
#include "radaer/linux_network.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <regex>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
namespace radaer {
namespace {
// TPROXY 用的 fwmark 与策略路由表号。选一个不和 OpenClash（0x162/354）冲突的值。
constexpr unsigned kTproxyMark = 0x1a2b;
constexpr unsigned kTproxyTable = 421;

// Feed a file descriptor to nft directly; never interpret interface/IP values through a shell.
bool nft(const std::string &rules) {
    char filename[] = "/tmp/radaer-nft-XXXXXX";
    int fd = mkstemp(filename);
    if (fd < 0) throw std::runtime_error("Cannot create nft input");
    unlink(filename);
    size_t offset = 0;
    while (offset < rules.size()) {
        auto count = write(fd, rules.data() + offset, rules.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); throw std::runtime_error("Cannot write nft input"); }
        offset += size_t(count);
    }
    lseek(fd, 0, SEEK_SET);
    const char *exe = access("/usr/sbin/nft", X_OK) == 0 ? "/usr/sbin/nft" : "/sbin/nft";
    pid_t child = fork();
    if (child == 0) {
        if (dup2(fd, STDIN_FILENO) < 0) _exit(126);
        close(fd);
        execl(exe, exe, "-f", "-", static_cast<char *>(nullptr));
        _exit(127);
    }
    close(fd);
    if (child < 0) throw std::runtime_error("Cannot start nft");
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}
struct CaptureFirewall::Impl {
    int lock = -1;
    bool added = false;
    void add(const CaptureOptions &o) {
        linux_capture_preflight(o);
        if (!std::regex_match(o.interface, std::regex("[A-Za-z0-9_.:-]{1,15}")))
            throw std::runtime_error("Unsupported capture interface name");
        const auto separator = o.listen.rfind(':');
        if (separator == std::string::npos) throw std::runtime_error("Missing bound TCP listener");
        const auto host = parse_ipv4(o.listen.substr(0, separator));
        const auto port = o.listen.substr(separator + 1);
        if (!std::regex_match(port, std::regex("[0-9]{1,5}")) || std::stoul(port) == 0 || std::stoul(port) > 65535)
            throw std::runtime_error("Invalid bound TCP port");
        unsigned prefix = 0;
        const auto inventory = linux_network_inventory();
        for (const auto &a : inventory.at("adapters")) {
            if (a.at("name") != o.interface) continue;
            for (const auto &ip : a.at("ipv4"))
                if (parse_ipv4(ip.at("address").get<std::string>()) == host)
                    prefix = ip.at("prefixLength").get<unsigned>();
        }
        if (!prefix || prefix > 30) throw std::runtime_error("TCP listener must belong to selected LAN");
        lock = open("/var/run/radaer-firewall.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("Another Radaer TCP capture owns the firewall");
        // The table is dedicated to this application. Atomic replacement also repairs a killed capture.
        //
        // 【TPROXY 而非 REDIRECT】
        // 旧实现用 `redirect to :port`（NAT 链），内核会把目标地址改写成网关本地地址，
        // 客户端 getpeername() 得到 192.168.5.1:port —— 游戏客户端会 inet_ntop() 取对端 IP
        // 并上报，发现「连的不是游戏服务器」→ 判定为异常 → 退出对局后被丢进观察局。
        //
        // 改用 tproxy：内核保留原始目标地址，客户端 getpeername() 仍是真实服务器 IP；
        // relay 侧用 IP_TRANSPARENT socket + getsockname() 拿到原始目标。
        // 需要配套策略路由（见 Impl::add 里 ip rule/ip route 的设置）。
        const auto network = ipv4_string(host & (0xffffffffU << (32 - prefix))) + "/" + std::to_string(prefix);
        const auto rules = "add table ip radaer_native\ndelete table ip radaer_native\n"
            "table ip radaer_native {\n chain prerouting { type filter hook prerouting priority mangle; policy accept;\n"
            "iifname \"" + o.interface + "\" ip saddr " + ipv4_string(parse_ipv4(o.client_ip)) +
            " ip daddr != " + network + " fib daddr type != local tcp dport " + std::to_string(o.tcp_port) +
            " counter meta mark set 0x" + std::to_string(kTproxyMark) +
            " tproxy ip to :" + port + " accept\n }\n}\n";
        if (!nft(rules)) throw std::runtime_error("Cannot install Radaer TCP tproxy rule");

        // 策略路由：把打了 mark 的包交给本地回环，让 TPROXY 能投递给 relay 的监听 socket。
        // 这些命令幂等（重复执行不报错），失败只告警不中断（某些系统已有等价规则）。
        if (!nft("add table ip radaer_native\n")) { /* 已存在则忽略 */ }
        std::string route_cmds =
            "ip rule add fwmark " + std::to_string(kTproxyMark) + " lookup " + std::to_string(kTproxyTable) +
            " 2>/dev/null; "
            "ip route add local 0.0.0.0/0 dev lo table " + std::to_string(kTproxyTable) + " 2>/dev/null; true";
        if (std::system(route_cmds.c_str()) != 0)
            std::cerr << "Warning: cannot install TPROXY policy routing (ip rule/route)\n";
        added = true;
    }
    ~Impl() {
        if (added) {
            try { if (!nft("delete table ip radaer_native\n")) std::cerr << "Cannot remove Radaer nft table\n"; }
            catch (...) { std::cerr << "Cannot run nft cleanup\n"; }
        }
        if (lock >= 0) close(lock);
    }
};
CaptureFirewall::CaptureFirewall(const CaptureOptions &o) {
    if (o.linux_redirect) { p_ = std::make_unique<Impl>(); p_->add(o); }
}
CaptureFirewall::~CaptureFirewall() = default;
} // namespace radaer
#else
namespace radaer {
struct CaptureFirewall::Impl {};
CaptureFirewall::CaptureFirewall(const CaptureOptions &) {}
CaptureFirewall::~CaptureFirewall() = default;
} // namespace radaer
#endif
