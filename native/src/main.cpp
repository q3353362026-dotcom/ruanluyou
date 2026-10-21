#include "radaer/battle.hpp"
#include "radaer/capture.hpp"
#include "radaer/players.hpp"
#include "radaer/shared_network.hpp"
#include "radaer/webui.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace radaer;
namespace {
struct Args {
    std::map<std::string, std::string> values;
    std::vector<std::string> files;
    Args(int argc, char **argv) {
        for (int i = 2; i < argc; i++) {
            std::string k = argv[i];
            if (!k.starts_with("--")) {
                files.push_back(k);
                continue;
            }
            if (i + 1 >= argc || std::string(argv[i + 1]).starts_with("--"))
                values[k] = "true";
            else
                values[k] = argv[++i];
        }
    }
    std::string get(const std::string &k, const std::string &fallback = "") const {
        auto i = values.find(k);
        return i == values.end() ? fallback : i->second;
    }
    bool has(const std::string &k) const {
        return values.contains(k);
    }
    bool flag(const std::string &k, bool fallback = false) const {
        auto value = get(k, fallback ? "true" : "false");
        if (value != "true" && value != "false")
            throw std::runtime_error(k + " expects true or false");
        return value == "true";
    }
    double real(const std::string &k, const std::string &fallback) const {
        auto value = get(k, fallback);
        size_t used = 0;
        double n = std::stod(value, &used);
        if (used != value.size() || !std::isfinite(n))
            throw std::runtime_error(k + " expects a finite number");
        return n;
    }
    int64_t integer(const std::string &k, const std::string &fallback) const {
        auto value = get(k, fallback);
        size_t used = 0;
        auto n = std::stoll(value, &used);
        if (used != value.size() || n < 0)
            throw std::runtime_error(k + " expects a nonnegative integer");
        return n;
    }
    std::string required(const std::string &k) const {
        auto v = get(k);
        if (v.empty() || v == "true")
            throw std::runtime_error("required: " + k);
        return v;
    }
    void allow(std::initializer_list<std::string> names) const {
        for (auto &[k, v] : values)
            if (std::find(names.begin(), names.end(), k) == names.end())
                throw std::runtime_error("unknown option " + k);
    }
};
size_t journal_limit(const Args &args) {
    auto value = args.integer("--max-log-bytes", std::to_string(default_journal_max_bytes));
    if (value < int64_t(min_journal_max_bytes) || value > int64_t(max_journal_max_bytes))
        throw std::runtime_error("max log bytes must be between " + std::to_string(min_journal_max_bytes) +
                                 " and " + std::to_string(max_journal_max_bytes));
    return size_t(value);
}
void write_json(const std::string &path, const Json &value) {
    if (path.empty()) {
        std::cout << value.dump(2) << '\n';
        return;
    }
    if (std::filesystem::exists(path))
        throw std::runtime_error("output already exists");
    std::ofstream f(path);
    f << value.dump(2) << '\n';
    if (!f)
        throw std::runtime_error("cannot write output");
}
template <class F> void lines(const std::string &path, F fn) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (line.empty())
            continue;
        try {
            fn(Json::parse(line));
        } catch (const std::exception &e) {
            throw std::runtime_error(path + ":" + std::to_string(n) + ": " + e.what());
        }
    }
}
void help() {
    std::cout << R"(radaer-native v0.5.12 - C++ protocol/crypto/capture/player core
  webui [--port 8765] [--lan true --lan-ip IPv4 --lan-if-index INDEX] [--web-root DIRECTORY] [--output-directory DIRECTORY] [--windivert-dll ABS_PATH] [--open-browser true]
  capture --out NEW.jsonl --device ID --seconds 60 [--interface br-lan --client-ip IP]
          --seconds 0 keeps capture running until stopped
          [--relay true --listen IP:PORT --upstream IP:PORT]
          [--original-destination true] [--udp-port PORT]
          [--save-banks DIRECTORY] [--save-tcp-plaintext true] [--save-udp-payload true]
          [--max-log-bytes BYTES] [--synthetic true] [BANK.bin ...]
  network-info
  shared-preflight --client-ip IP --shared-host-ip IP --shared-if-index N --windivert-dll ABS_PATH
                   [--shared-redirect true] [--tcp-port PORT] [--tcp-server IP] [--listen HOST_IP:0]
  capture --shared-windows true --client-ip IP --shared-host-ip IP --shared-if-index N
          --windivert-dll ABS_PATH --out NEW.jsonl --device ID [--seconds 60]
          [--shared-redirect true] [--tcp-port PORT] [--tcp-server IP] [--listen HOST_IP:0]
          [--udp-port PORT] [--save-udp-payload true] [--max-log-bytes BYTES] [BANK.bin ...]
  replay --in capture.jsonl --out NEW.jsonl --device ID [--synthetic true|false] [--max-log-bytes BYTES] BANK.bin ...
  report --out NEW.json --max-gap-ms 5000 [--synthetic-only true] CAPTURE.jsonl ...
  decode-players --in CAPTURE.jsonl --output-directory NEW_DIRECTORY
  validate-players --in players.jsonl --out NEW.jsonl [--max-coordinate N --max-speed N]
                   [--max-age-ms 2000 --max-gap-ms 2000 --min-samples 3]
  check-vectors --in golden.json
Shared TCP defaults: remote port 65010, original destination detection, system-assigned listener port.
Use --tcp-port 0 explicitly to restore all-port interception. DH substitution occurs only at handshake.
Temporary firewall rules are managed after bind; use --manage-firewall false only with manual rules.
Defaults: no live interception and no bank/key material persisted.
Linux packet capture supports IPv4, one LAN interface, one client. TCP relay supports IPv4.
Player movement and explicit identity decoding run natively; Python is used only by reference tests.
)";
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2) {
            help();
            return 0;
        }
        std::string cmd = argv[1];
        Args a(argc, argv);
        if (cmd == "help" || cmd == "--help") {
            help();
            return 0;
        }
        if (cmd == "network-info") {
            a.allow({});
            std::cout << windows_network_inventory().dump(2) << '\n';
            return 0;
        }
        if (cmd == "decode-players") {
            a.allow({"--in", "--output-directory"});
            auto result = decode_player_file(a.required("--in"), a.required("--output-directory"));
            std::cout << result.dump(2) << '\n';
            return result.at("exceptions").get<size_t>() ? 1 : 0;
        }
        if (cmd == "webui") {
            a.allow({"--port", "--web-root", "--output-directory", "--windivert-dll", "--open-browser", "--lan", "--lan-ip", "--lan-if-index"});
            WebOptions options;
            auto port = a.integer("--port", "8765");
            if (port < 1024 || port > 65535)
                throw std::runtime_error("WebUI port must be 1024..65535");
            options.port = unsigned(port);
            options.assets = a.get("--web-root", RADAER_WEB_ROOT);
            options.open_browser = a.flag("--open-browser");
            options.lan = a.flag("--lan");
            options.lan_ip = a.get("--lan-ip");
            const auto lan_index = a.integer("--lan-if-index", "0");
            if (lan_index > 0xffffffffLL || (options.lan && (!lan_index || options.lan_ip.empty())) ||
                (!options.lan && (lan_index || !options.lan_ip.empty())))
                throw std::runtime_error("LAN access requires --lan true, --lan-ip and --lan-if-index together");
            options.lan_if_index = unsigned(lan_index);
            options.output_directory =
                std::filesystem::absolute(a.get("--output-directory", "captures/webui")).string();
            options.windivert_dll =
                std::filesystem::absolute(
                    a.get("--windivert-dll", RADAER_REPO_ROOT "/runtime/release_resources/WinDivert.dll"))
                    .string();
            return webui(options);
        }
        if (cmd == "capture" || cmd == "shared-preflight") {
            a.allow({"--out",
                     "--device",
                     "--seconds",
                     "--interface",
                     "--client-ip",
                     "--relay",
                     "--listen",
                     "--upstream",
                     "--original-destination",
                     "--udp-port",
                     "--save-banks",
                     "--save-tcp-plaintext",
                     "--save-udp-payload",
                     "--max-log-bytes",
                     "--synthetic",
                     "--shared-windows",
                     "--shared-redirect",
                     "--shared-host-ip",
                     "--shared-if-index",
                     "--windivert-dll",
                     "--tcp-port",
                     "--tcp-server",
                     "--manage-firewall"});
            CaptureOptions o;
            if (cmd == "capture") {
                o.output = a.required("--out");
                o.device = a.required("--device");
            }
            o.seconds = a.real("--seconds", "60");
            o.interface = a.get("--interface");
            o.client_ip = a.get("--client-ip");
            o.relay = a.flag("--relay");
            o.shared_windows = a.flag("--shared-windows", cmd == "shared-preflight");
            o.shared_redirect = a.flag("--shared-redirect");
            o.manage_firewall = a.flag("--manage-firewall", true);
            o.shared_host_ip = a.get("--shared-host-ip");
            o.windivert_dll = a.get("--windivert-dll");
            o.tcp_server = a.get("--tcp-server");
            auto tcp_port = a.integer("--tcp-port", o.shared_redirect ? "65010" : "0"),
                 if_index = a.integer("--shared-if-index", "0");
            if (tcp_port > 65535 || if_index > 0xffffffffLL)
                throw std::runtime_error("shared port/interface index out of range");
            o.tcp_port = unsigned(tcp_port);
            o.shared_if_index = unsigned(if_index);
            if (o.shared_redirect)
                o.relay = true;
            o.listen = a.get("--listen", o.shared_redirect ? o.shared_host_ip + ":0" : o.listen);
            o.upstream = a.get("--upstream");
            o.original_destination = a.flag("--original-destination");
            auto port = a.integer("--udp-port", "0");
            if (port > 65535)
                throw std::runtime_error("UDP port exceeds 65535");
            o.udp_port = unsigned(port);
            o.max_log_bytes = journal_limit(a);
            o.save_tcp_plaintext = a.flag("--save-tcp-plaintext");
            o.save_udp_payload = a.flag("--save-udp-payload");
            o.synthetic = a.flag("--synthetic");
            o.bank_files = a.files;
            if (cmd == "shared-preflight") {
                std::cout << windows_capture_preflight(o).dump(2) << '\n';
                return 0;
            }
            return capture(o);
        }
        if (cmd == "report") {
            a.allow({"--out", "--max-gap-ms", "--synthetic-only"});
            if (a.files.empty())
                throw std::runtime_error("report requires capture files");
            auto gap = a.integer("--max-gap-ms", "5000");
            if (gap <= 0)
                throw std::runtime_error("max gap must be positive");
            write_json(a.get("--out"), bank_report(a.files, gap, a.flag("--synthetic-only")));
            return 0;
        }
        if (cmd == "replay") {
            a.allow({"--in", "--out", "--device", "--synthetic", "--max-log-bytes"});
            auto banks = read_bank_files(a.files);
            if (banks.empty())
                throw std::runtime_error("replay requires at least one 128-byte bank file");
            Journal journal(a.required("--out"), a.required("--device"), a.flag("--synthetic", true), {}, true,
                            journal_limit(a));
            int64_t replay_clock = 0;
            BattleTracker battle([&](Json e) { journal.emit(std::move(e)); }, [&] { return replay_clock; });
            auto record = [&](Json e) {
                e["session"] = journal.session();
                e["device"] = a.required("--device");
                e["synthetic"] = a.flag("--synthetic", true);
                if (!e.contains("timestampMs"))
                    e["timestampMs"] = replay_clock;
                journal.emit(e);
                battle.consume(e);
            };
            PlayerValidator validator;
            PlayerDecoder players([&](Json e) {
                if (e.at("type") == "player_sample" || e.at("type") == "self_sample" || e.at("type") == "self_server_sample")
                    record(validator.inspect(e));
                record(std::move(e));
            });
            BankBroker broker([&](Json e) {
                e["session"] = journal.session();
                e["device"] = a.required("--device");
                e["synthetic"] = a.flag("--synthetic", true);
                if (!e.contains("timestampMs"))
                    e["timestampMs"] = now_ms();
                record(e);
                players.consume(e);
            });
            record({{"type", "session_started"},
                    {"data", {{"mode", "offline_replay"}, {"source", a.required("--in")}}}});
            for (auto &b : banks)
                broker.candidate(b, "explicit_bank_file", now_ms());
            uint64_t ordinal = 0;
            lines(a.required("--in"), [&](const Json &e) {
                replay_clock = std::max(replay_clock, e.value("timestampMs", int64_t(0)));
                battle.tick(replay_clock);
                auto type = e.value("type", std::string());
                if (type == "capture_gap") {
                    broker.gap(e.at("timestampMs"), "source_capture_gap",
                               e.at("data").value("sinceMs", int64_t(0)));
                    return;
                }
                if (type != "udp_raw")
                    return;
                const auto &d = e.at("data");
                auto dir = d.at("direction").get<std::string>();
                if (dir != "up" && dir != "down")
                    throw std::runtime_error("invalid direction");
                if (!d.contains("payloadBase64"))
                    throw std::runtime_error("udp_raw payload missing; capture with --save-udp-payload true to replay");
                broker.packet({dir == "up", unbase64(d.at("payloadBase64")), e.at("timestampMs"),
                               d.at("local"), d.at("remote"), ++ordinal, d.value("ipFragmentCount", 1U),
                               d.value("reassemblyDelayMs", int64_t(0))});
            });
            record({{"type", "player_decoder_summary"}, {"data", players.summary()}});
            battle.consume({{"type", "session_stopped"}, {"timestampMs", replay_clock}});
            record({{"type", "session_stopped"}});
            journal.finish();
            return 0;
        }
        if (cmd == "validate-players") {
            a.allow({"--in", "--out", "--max-coordinate", "--max-speed", "--max-age-ms", "--max-gap-ms",
                     "--min-samples"});
            PlayerLimits l;
            l.max_abs_coordinate = a.real("--max-coordinate", "0");
            l.max_speed = a.real("--max-speed", "0");
            l.max_age_ms = a.integer("--max-age-ms", "2000");
            l.max_gap_ms = a.integer("--max-gap-ms", "2000");
            auto minimum = a.integer("--min-samples", "3");
            if (minimum > 100000)
                throw std::runtime_error("minimum samples exceeds 100000");
            l.minimum_samples = unsigned(minimum);
            if (l.max_abs_coordinate < 0 || l.max_speed < 0 || l.max_age_ms < 0 || l.max_gap_ms <= 0 ||
                !l.minimum_samples)
                throw std::runtime_error("invalid player limits");
            PlayerValidator v(l);
            auto path = a.required("--out");
            if (std::filesystem::exists(path))
                throw std::runtime_error("output exists");
            std::ofstream out(path);
            if (!out)
                throw std::runtime_error("cannot create validation output");
            size_t count = 0;
            std::map<std::string, size_t> states;
            lines(a.required("--in"), [&](const Json &e) {
                if (e.value("type", std::string()) != "player_sample")
                    return;
                auto result = v.inspect(e);
                result["session"] = e.value("session", std::string());
                result["synthetic"] = e.value("synthetic", true);
                ++states[result["data"]["status"].get<std::string>()];
                out << result.dump() << '\n';
                ++count;
            });
            if (!out)
                throw std::runtime_error("validation output write failure");
            std::cout << Json({{"samples", count}, {"statuses", states}}).dump(2) << '\n';
            return 0;
        }
        if (cmd == "check-vectors") {
            a.allow({"--in"});
            std::ifstream in(a.required("--in"));
            Json v;
            in >> v;
            size_t n = 0;
            auto check = [&](bool ok, const std::string &label) {
                ++n;
                if (!ok)
                    throw std::runtime_error("vector mismatch: " + label + " #" + std::to_string(n));
            };
            for (auto &t : v.at("aes")) {
                auto p = unhex(t["plain"]), k = unhex(t["key"]), c = unhex(t["cipher"]);
                Key key{};
                if (k.size() != 16)
                    throw std::runtime_error("vector key length");
                std::copy(k.begin(), k.end(), key.begin());
                check(encrypt(p, key) == c, "AES encrypt");
                check(unpad(aes(c, key, false)) == p, "AES decrypt");
            }
            for (auto &t : v.at("udp")) {
                auto p = unhex(t["plain"]), k = unhex(t["bank"]), c = unhex(t["cipher"]);
                Bank bank{};
                if (k.size() != 128)
                    throw std::runtime_error("vector bank length");
                std::copy(k.begin(), k.end(), bank.begin());
                check(udp_crypt(c, bank) == p, "UDP decrypt");
                check(udp_crypt(p, bank, true) == c, "UDP encrypt");
            }
            for (auto &t : v.at("envelopes"))
                check(valid_envelope(unhex(t["payload"])) == t["valid"].get<bool>(), "envelope");
            for (auto &t : v.at("dh")) {
                Dh dh(unhex(t["secret"]));
                check(hex(dh.public_key()) == t["public"].get<std::string>(), "DH public");
                check(hex(dh.derive(unhex(t["peer"]))) == t["key"].get<std::string>(), "DH derive");
            }
            std::cout
                << Json({{"passed", n}, {"scope", "original-managed-assembly synthetic vectors"}}).dump(2)
                << '\n';
            return 0;
        }
        throw std::runtime_error("unknown command " + cmd);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
