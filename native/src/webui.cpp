#include "radaer/webui.hpp"
#include "httplib/httplib.h"
#include "radaer/rates.hpp"
#include "radaer/player_view.hpp"
#include "radaer/loot.hpp"
#include "radaer/radar_replay.hpp"
#include <algorithm>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/rand.h>
#include <regex>
#include <thread>
#include <tuple>
#ifdef __linux__
#include "radaer/linux_network.hpp"
#endif
#ifdef _WIN32
#include <shellapi.h>
#endif
namespace radaer {
namespace {
volatile std::sig_atomic_t web_stopped = 0;
void web_signal(int) {
    web_stopped = 1;
}
std::string nonce() {
    std::array<uint8_t, 16> bytes{};
    if (RAND_bytes(bytes.data(), int(bytes.size())) != 1)
        throw std::runtime_error("random generation failed");
    return hex(bytes);
}
std::string read_asset(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("WebUI asset missing: " + path.string());
    return std::string(std::istreambuf_iterator<char>(file), {});
}
unsigned number(const Json &j, const char *key, unsigned fallback, unsigned minimum, unsigned maximum) {
    if (!j.contains(key))
        return fallback;
    if (!j[key].is_number_integer())
        throw std::runtime_error(std::string(key) + " 必须是整数");
    auto n = j[key].get<int64_t>();
    if (n < minimum || n > maximum)
        throw std::runtime_error(std::string(key) + " 超出有效范围");
    return unsigned(n);
}
std::string string_value(const Json &j, const char *key, const std::string &fallback = "") {
    auto value = j.value(key, fallback);
    if (value.size() > 256)
        throw std::runtime_error(std::string(key) + " 过长");
    return value;
}
struct Captures {
    WebOptions options;
    std::mutex control, mutex;
    std::atomic<bool> stop_requested{false};
    std::thread worker;
    LiveRates rates;
    uint64_t revision = 0;
    Json state = {{"running", false},        {"phase", "idle"},          {"error", ""},
                  {"events", Json::array()}, {"players", Json::array()}, {"counts", Json::object()}};
    explicit Captures(WebOptions o) : options(std::move(o)) {}
    ~Captures() {
        stop_requested = true;
        if (worker.joinable())
            worker.join();
    }
    CaptureOptions parse(const Json &j) {
        CaptureOptions o;
        o.shared_windows = true;
        o.save_logs = j.value("saveLogs", true);
        o.save_udp_payload = j.value("saveUdpPayload", false);
        o.max_log_bytes = number(j, "maxLogBytes", unsigned(default_journal_max_bytes),
                                 unsigned(min_journal_max_bytes), unsigned(max_journal_max_bytes));
        o.shared_host_ip = string_value(j, "hostIp");
        o.shared_if_index = number(j, "ifIndex", 0, 0, 4294967295u);
        o.client_ip = string_value(j, "clientIp");
        o.device = string_value(j, "device", o.client_ip);
        if (o.device.empty() || o.device.size() > 80)
            throw std::runtime_error("设备标识需为 1–80 个字节");
        o.seconds = number(j, "seconds", 0, 0, 604800);
        o.udp_port = number(j, "udpPort", 0, 0, 65535);
        o.shared_redirect = j.value("redirectTcp", true);
        o.relay = o.shared_redirect;
        o.tcp_port = number(j, "tcpPort", o.shared_redirect ? 65010 : 0, 0, 65535);
        o.tcp_server = string_value(j, "tcpServer");
        o.listen = o.shared_host_ip + ":" + std::to_string(number(j, "listenPort", 0, 0, 65535));
        o.windivert_dll = options.windivert_dll;
#ifdef __linux__
        o.interface = linux_interface(o.shared_if_index, o.shared_host_ip);
        o.shared_windows = false;
        o.linux_redirect = o.shared_redirect;
        o.original_destination = o.shared_redirect;
        o.shared_redirect = false;
        o.shared_if_index = 0;
        o.shared_host_ip.clear();
        o.windivert_dll.clear();
        if (!o.linux_redirect) o.tcp_port = 0;
        if (!o.tcp_server.empty()) throw std::runtime_error("Linux WebUI does not accept tcpServer");
#endif
        if (o.save_logs && j.value("saveBanks", false))
            o.save_banks = (std::filesystem::path(options.output_directory) / "banks").string();
        if (j.contains("bankIds")) {
            if (!j["bankIds"].is_array() || j["bankIds"].size() > 64)
                throw std::runtime_error("bank 列表无效");
            for (auto &value : j["bankIds"]) {
                auto id = value.get<std::string>();
                if (!std::regex_match(id, std::regex("[a-f0-9]{64}")))
                    throw std::runtime_error("无效 bank 标识");
                auto file = std::filesystem::path(options.output_directory) / "banks" / (id + ".bin");
                if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file) != 128)
                    throw std::runtime_error("bank 文件不存在或长度错误");
                o.bank_files.push_back(file.string());
            }
        }
        return o;
    }
    Json banks() {
        Json result = Json::array();
        auto dir = std::filesystem::path(options.output_directory) / "banks";
        if (!std::filesystem::exists(dir))
            return result;
        for (auto &entry : std::filesystem::directory_iterator(dir)) {
            auto id = entry.path().stem().string();
            if (entry.is_regular_file() && entry.path().extension() == ".bin" && entry.file_size() == 128 &&
                std::regex_match(id, std::regex("[a-f0-9]{64}")))
                result.push_back(id);
        }
        return result;
    }
    Json preflight(const Json &j) {
        return windows_capture_preflight(parse(j));
    }
    void observe(Json event) {
        std::lock_guard lock(mutex);
        ++revision;
        rates.consume(event);
        auto type = event.value("type", std::string());
        auto &count = state["counts"][type];
        count = count.is_null() ? 1 : count.get<uint64_t>() + 1;
        state["lastEventAtMs"] = event.value("timestampMs", now_ms());
        if (type == "capture_stats")
            state["stats"] = event["data"];
        if (type == "udp_reassembly_summary") {
            state["reassembly"] = event["data"];
            return;
        }
        if (type == "tcp_listener_started")
            state["listener"] = event["data"];
        if (type == "player_decoder_summary")
            state["playerSummary"] = event["data"];
        if (type == "battle_state") {
            state["battle"] = event.at("data");
            state["players"] = state["battle"].at("players");
            state["loot"] = state["battle"].value("loot",Json::object());
            state["battle"].erase("loot");
            state["self"] = state["battle"].value("self", Json());
            state["serverSelf"] = state["battle"].value("serverSelf", Json());
            state["battle"].erase("players");
            state["battle"].erase("self");
            state["battle"].erase("serverSelf");
        }
        if (type == "capture_gap") {
            state["loot"] = Json::object();
            state["players"] = Json::array();
            state["self"] = nullptr;
            state["serverSelf"] = nullptr;
        }
        if (type == "udp_binding_reset") {
            if(state.value("battle",Json::object()).value("flow",Json())==event["data"].value("flow",Json()))state["loot"]=Json::object();
            auto flow = event["data"].value("flow", std::string());
            if (state.contains("serverSelf") && state["serverSelf"].is_object() &&
                state["serverSelf"].value("flow", std::string()) == flow)
                state["serverSelf"] = nullptr;
            if (state.contains("self") && state["self"].is_object() &&
                state["self"].value("flow", std::string()) == flow)
                state["self"] = nullptr;
            auto &rows = state["players"];
            for (size_t i = rows.size(); i > 0; --i)
                if (rows[i - 1].value("flow", std::string()) == flow)
                    rows.erase(i - 1);
        }
        if (type == "player_validation" || type == "self_validation" || type == "self_server_validation" || type == "player_motion" || type == "player_state" || type == "loot_state") {
            const auto &d = event.at("data");
            if (!state.contains("battle") || state["battle"].value("status", std::string()) != "live" ||
                state["battle"]["flow"] != d["flow"] || state["battle"]["epoch"] != d["epoch"] ||
                state["battle"]["flowSession"] != event.value("session", std::string()))
                return;
            if (type == "loot_state") {apply_loot_state(state["loot"],d);return;}
            if (type == "player_state") {
                apply_player_state(state["players"],state["self"],state["serverSelf"],d);
                return;
            }
            if (type == "player_motion") {
                apply_player_motion(state["players"], d);
                return;
            }
            if (type == "self_server_validation") {
                state["serverSelf"] = d;
                return;
            }
            if (type == "self_validation") {
                state["self"] = d;
                return;
            }
            Json row = Json::object();
            for (auto key : {"flow",
                             "epoch",
                             "channel",
                             "track",
                             "poseSequence",
                             "slot",
                             "name",
                             "teamId",
                             "teamSource",
                             "camp",
                             "x",
                             "y",
                             "z",
                             "yaw",
                             "motion",
                             "attributes",
                             "vitals",
                             "equipment",
                             "lifecycle",
                             "timestampMs",
                             "status",
                             "flags",
                             "identitySource",
                             "actorGuid",
                             "playerStateGuid",
                             "identityKey",
                             "channelSource",
                             "decodeCompleteness",
                             "movementFrameComplete",
                             "movementBlockComplete",
                             "decodeStopReason"})
                if (d.contains(key))
                    row[key] = d[key];
            upsert_player_row(state["players"], std::move(row));
            return;
        }
        if (type == "udp_raw" || type == "udp_plaintext" || type == "tcp_plaintext" || type == "bank_use" ||
            type == "capture_stats" || type == "player_sample" || type == "self_sample" ||
            type == "self_server_sample" ||
            type == "player_identity")
            return;
        Json safe = {
            {"type", type}, {"timestampMs", event.value("timestampMs", now_ms())}, {"data", Json::object()}};
        for (auto key : {"reason",           "error",         "fingerprint",    "flow",       "local",
                         "remote",           "client",        "server",         "connection", "tcpConnection",
                         "listen",           "tcpMode",       "status",         "epoch",      "map",
                         "previousFlow",     "previousEpoch", "previousStatus", "channel",    "scope",
                         "affectsContinuity"})
            if (event["data"].contains(key))
                safe["data"][key] = event["data"][key];
        state["events"].push_back(std::move(safe));
        if (state["events"].size() > 80)
            state["events"].erase(0);
    }
    Json start(const Json &j) {
        std::lock_guard serial(control);
        {
            std::lock_guard lock(mutex);
            if (state["running"].get<bool>())
                throw std::runtime_error("已有采集运行，请先停止");
        }
        if (worker.joinable())
            worker.join();
        auto o = parse(j);
        Json configuration = j;
        configuration["maxLogBytes"] = o.max_log_bytes;
        auto pre = windows_capture_preflight(o);
        if (!pre["elevated"].get<bool>())
            throw std::runtime_error("启动采集需要以管理员身份运行 WebUI");
        if (o.save_logs) {
            std::filesystem::create_directories(options.output_directory);
            o.output = (std::filesystem::path(options.output_directory) /
                        ("web-" + std::to_string(now_ms()) + "-" + nonce().substr(0, 8) + ".jsonl")).string();
        }
        stop_requested = false;
        {
            std::lock_guard lock(mutex);
            rates.reset();
            state = {
                {"running", true},          {"phase", "starting"},      {"error", ""},
                {"saveLogs", o.save_logs},  {"maxLogBytes", o.max_log_bytes}, {"output", o.output},
                {"startedAtMs", now_ms()},  {"clientIp", o.client_ip},
                {"events", Json::array()},  {"counts", Json::object()},
                {"redirectTcp", o.shared_redirect || o.linux_redirect},
                {"players", Json::array()}, {"seconds", o.seconds},     {"configuration", configuration}};
        }
        try {
            worker = std::thread([this, o] {
                std::string error;
                try {
                    {
                        std::lock_guard lock(mutex);
                        state["phase"] = "running";
                    }
                    capture(o, &stop_requested, [this](Json e) { observe(std::move(e)); });
                } catch (const std::exception &e) {
                    error = e.what();
                }
                std::lock_guard lock(mutex);
                state["running"] = false;
                state["finishedAtMs"] = now_ms();
                state["phase"] = error.empty() ? "completed" : "failed";
                state["error"] = error;
                rates.reset();
                if (!error.empty()) {
                    state["players"] = Json::array();
                    state["self"] = nullptr;
                    state["serverSelf"] = nullptr;
                    state["battle"]["status"] = "error";
                    state["battle"]["liveActive"] = false;
                }
            });
        } catch (...) {
            std::lock_guard lock(mutex);
            state["running"] = false;
            state["phase"] = "failed";
            throw;
        }
        return status();
    }
    Json status() {
        std::lock_guard lock(mutex);
        auto result = state;
        result["revision"] = revision;
        result["rates"] = rates.snapshot();
        for (auto &row : result["players"])
            row.update(rates.slot(row.at("channel").get<uint32_t>(), row.at("slot").get<unsigned>()));
        return result;
    }
    Json live_status() {
        std::lock_guard lock(mutex);
        // Frequent radar polls do not copy device discovery, logs, or configuration.
        return {{"revision", revision},
                {"serverTimeMs", now_ms()},
                {"running", state.value("running", false)},
                {"battle", state.value("battle", Json::object())},
                {"players", state.at("players")},
                {"loot", state.value("loot",Json::object())},
                {"self", state.value("self", Json())},
                {"serverSelf", state.value("serverSelf", Json())},
                {"rates", rates.snapshot()}};
    }
    void stop() {
        stop_requested = true;
        std::lock_guard lock(mutex);
        if (state["running"].get<bool>())
            state["phase"] = "stopping";
    }
    Json report() {
        std::lock_guard serial(control);
        auto snapshot = status();
        if (snapshot["running"].get<bool>())
            throw std::runtime_error("请先停止采集再生成报告");
        auto path = snapshot.value("output", std::string());
        if (path.empty() || !std::filesystem::exists(path))
            throw std::runtime_error("尚无采集文件");
        return bank_report({path});
    }
    Json clear_logs() {
        std::lock_guard serial(control);
        const auto snapshot = status();
        const auto active = snapshot.value("running", false) ? snapshot.value("output", std::string()) : "";
        auto result = radar_clear_recordings(options.output_directory, active);
        const auto output = snapshot.value("output", std::string());
        if (active.empty() && !output.empty() && !std::filesystem::exists(output)) {
            std::lock_guard lock(mutex);
            state["output"] = "";
        }
        return result;
    }
};
} // namespace
int webui(const WebOptions &options) {
#if !defined(_WIN32) && !defined(__linux__)
    (void)options;
    throw std::runtime_error("WebUI discovery currently supports Windows only");
#else
    if (options.port < 1024 || options.port > 65535)
        throw std::runtime_error("WebUI port must be 1024..65535");
    std::map<std::string, std::pair<std::string, std::string>> assets;
    for (auto [url, file, mime] : {std::tuple{"/", "index.html", "text/html; charset=utf-8"},
                                   std::tuple{"/app.js", "app.js", "text/javascript; charset=utf-8"},
                                   std::tuple{"/radar.js", "radar.js", "text/javascript; charset=utf-8"},
                                   std::tuple{"/radar", "radar/index.html", "text/html; charset=utf-8"},
                                   std::tuple{"/radar/", "radar/index.html", "text/html; charset=utf-8"},
                                   std::tuple{"/radar/view.css", "radar/view.css", "text/css; charset=utf-8"},
                                   std::tuple{"/radar/model.js", "radar/model.js", "text/javascript; charset=utf-8"},
                                   std::tuple{"/radar/replay.js", "radar/replay.js", "text/javascript; charset=utf-8"},
                                   std::tuple{"/radar/view.js", "radar/view.js", "text/javascript; charset=utf-8"},
                                   std::tuple{"/radar/maps.json", "radar/maps.json", "application/json; charset=utf-8"},
                                   std::tuple{"/radar/operators.json", "radar/operators.json", "application/json; charset=utf-8"},
                                   std::tuple{"/radar/items.json", "radar/items.json", "application/json; charset=utf-8"},
                                   std::tuple{"/vendor/leaflet/leaflet.js", "vendor/leaflet/leaflet.js", "text/javascript; charset=utf-8"},
                                   std::tuple{"/vendor/leaflet/leaflet.css", "vendor/leaflet/leaflet.css", "text/css; charset=utf-8"},
                                   std::tuple{"/styles.css", "styles.css", "text/css; charset=utf-8"}})
        assets[url] = {read_asset(std::filesystem::path(options.assets) / file), mime};
    // Register only catalogued numeric portrait names; never turn a request into a filesystem path.
    const auto catalog = Json::parse(assets.at("/radar/operators.json").first);
    for (const auto &[id, entry] : catalog.at("operators").items()) {
        if (id.empty() || id.size() > 15 || !std::all_of(id.begin(),id.end(),[](char c){return c >= '0' && c <= '9';}))
            throw std::runtime_error("invalid operator catalog ID");
        if (!entry.value("portrait",Json()).is_string()) continue;
        const auto url = "/radar/portraits/" + id + ".png";
        if (entry.at("portrait") != url) throw std::runtime_error("invalid operator portrait path");
        assets[url] = {read_asset(std::filesystem::path(options.assets) / "radar" / "portraits" / (id + ".png")),"image/png"};
    }
    Captures captures(options);
    std::mutex replay_mutex;
    LanDiscovery discovery;
    httplib::Server server;
    const auto token = nonce();
    const auto bind_ip = options.lan ? options.lan_ip : "127.0.0.1";
    const auto host = bind_ip + ":" + std::to_string(options.port),
               localhost = "localhost:" + std::to_string(options.port);
    // Bind only the explicitly selected address. Never fall back to all interfaces.
    std::map<std::string, std::pair<uint32_t, uint32_t>> lan_hosts;
    Json lan_urls = Json::array();
    if (options.lan) {
        const auto inventory = windows_network_inventory();
        for (const auto &adapter : inventory.at("adapters")) {
            if (!adapter.value("up", false) || adapter.at("ifIndex") != options.lan_if_index) continue;
            for (const auto &address : adapter.at("ipv4")) {
                const auto ip_text = address.at("address").get<std::string>();
                if (ip_text != options.lan_ip) continue;
                const auto ip = parse_ipv4(ip_text);
                const auto prefix = address.at("prefixLength").get<unsigned>();
                const bool private_ip = (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8;
                if (!private_ip || prefix == 0 || prefix > 32) continue;
                const auto mask = uint32_t(0xffffffffU << (32 - prefix));
                const auto authority = ip_text + ":" + std::to_string(options.port);
                if (lan_hosts.emplace(authority, std::pair{ip & mask, mask}).second)
                    lan_urls.push_back("http://" + authority);
            }
        }
        if (lan_hosts.size() != 1)
            throw std::runtime_error("Selected LAN IPv4 is not assigned to an active private interface; select again");
    }
    server.new_task_queue = [] { return new httplib::ThreadPool(4); };
    server.set_payload_max_length(16384);
    server.set_read_timeout(5);
    server.set_write_timeout(5);
    server.set_default_headers(
        {{"Cache-Control", "no-store"},
         {"X-Content-Type-Options", "nosniff"},
         {"Referrer-Policy", "no-referrer"},
         {"Content-Security-Policy",
          "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data: https://game.gtimg.cn; connect-src "
          "'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"}});
    server.set_pre_routing_handler([&](const httplib::Request &req, httplib::Response &res) {
        auto request_host = req.get_header_value("Host"), origin = req.get_header_value("Origin");
        bool allowed = false;
        try {
            const auto peer = parse_ipv4(req.remote_addr);
            const bool loopback = (peer >> 24) == 127;
            allowed = !options.lan && loopback && (request_host == host || request_host == localhost);
            if (const auto found = lan_hosts.find(request_host); found != lan_hosts.end()) {
                const auto [network, mask] = found->second;
                allowed = loopback || (peer & mask) == network;
            }
        } catch (const std::exception &) { /* Invalid peer addresses are rejected. */ }
        allowed = allowed && req.get_header_value("Sec-Fetch-Site") != "cross-site";
        if (!origin.empty() && origin != "http://" + request_host)
            allowed = false;
        if (req.method == "POST")
            allowed = allowed && req.get_header_value("X-Radaer-Token") == token &&
                      req.get_header_value("Content-Type").starts_with("application/json");
        if (!allowed) {
            res.status = 403;
            res.set_content(Json({{"error", "请求地址、来源或令牌无效，请使用启动时显示的地址打开 WebUI。"}}).dump(),
                            "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    auto api = [&](auto fn) {
        return [fn](const httplib::Request &req, httplib::Response &res) {
            try {
                auto body = req.method == "POST" ? Json::parse(req.body) : Json::object();
                if (!body.is_object())
                    throw std::runtime_error("请求必须是 JSON 对象");
                res.set_content(fn(body).dump(), "application/json; charset=utf-8");
            } catch (const std::exception &e) {
                res.status = 400;
                res.set_content(Json({{"error", e.what()}}).dump(), "application/json; charset=utf-8");
            }
        };
    };
    for (auto &[url, asset] : assets)
        server.Get(url, [asset](const httplib::Request &, httplib::Response &res) {
            res.set_content(asset.first, asset.second);
        });
    server.Get("/api/bootstrap", api([&](const Json &) {
                   return Json{{"token", token},
                               {"network", windows_network_inventory()},
                               {"outputDirectory", options.output_directory},
                               {"bankIds", captures.banks()},
                               {"listen", "http://" + host},
                               {"lanEnabled", options.lan}, {"lanUrls", lan_urls},
                               {"lanIfIndex", options.lan_if_index}};
               }));
    server.Get("/api/state", api([&](const Json &) {
                   return Json{{"scan", discovery.status()}, {"capture", captures.status()}};
               }));
    server.Get("/api/live", api([&](const Json &) { return captures.live_status(); }));
    server.Get("/api/replays", api([&](const Json &) {
        const auto state = captures.status();
        return radar_recordings(options.output_directory, state.value("running", false) ? state.value("output", std::string()) : "");
    }));
    server.Post("/api/replay/load", api([&](const Json &body) {
        std::unique_lock lock(replay_mutex, std::try_to_lock);
        if (!lock.owns_lock()) throw std::runtime_error("正在读取另一条回放，请稍后重试");
        const auto state = captures.status();
        return radar_replay(options.output_directory, string_value(body,"id"), state.value("running", false) ? state.value("output", std::string()) : "");
    }));
    server.Post("/api/logs/clear", api([&](const Json &) {
        std::unique_lock lock(replay_mutex, std::try_to_lock);
        if (!lock.owns_lock()) throw std::runtime_error("正在读取回放，请稍后清理历史日志");
        return captures.clear_logs();
    }));
    server.Get("/api/network", api([&](const Json &) { return windows_network_inventory(); }));
    server.Get("/api/banks", api([&](const Json &) { return captures.banks(); }));
    server.Post("/api/scan/start", api([&](const Json &j) {
                    discovery.start(number(j, "ifIndex", 0, 1, 0xffffffffU), string_value(j, "hostIp"),
                                    j.value("active", true));
                    return discovery.status();
                }));
    server.Post("/api/scan/stop", api([&](const Json &) {
                    discovery.cancel();
                    return Json{{"stopping", true}};
                }));
    server.Post("/api/capture/preflight", api([&](const Json &j) { return captures.preflight(j); }));
    server.Post("/api/capture/start", api([&](const Json &j) { return captures.start(j); }));
    server.Post("/api/capture/stop", api([&](const Json &) {
                    captures.stop();
                    return captures.status();
                }));
    server.Post("/api/capture/report", api([&](const Json &) { return captures.report(); }));
    server.Post("/api/server/stop", api([&](const Json &) {
                    captures.stop();
                    discovery.cancel();
                    server.stop();
                    return Json{{"stopping", true}};
                }));
    if (!server.bind_to_port(bind_ip, int(options.port)))
        throw std::runtime_error("Cannot bind WebUI to " + host + "; check the selected address and port");
    web_stopped = 0;
    auto old_int = std::signal(SIGINT, web_signal), old_term = std::signal(SIGTERM, web_signal);
    std::jthread watcher([&](std::stop_token stop) {
        while (!stop.stop_requested() && !web_stopped)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (web_stopped)
            server.stop();
    });
    std::cout << "Radaer WebUI: http://" << host
              << (options.lan ? "\nLAN access enabled on the selected interface only.\n" : "\nLocal access only.\n")
              << "Ctrl+C stops the server and active capture.\n"
              << std::flush;
    for (const auto &url : lan_urls)
        std::cout << "LAN radar: " << url.get<std::string>() << "/radar\n" << std::flush;
#ifdef _WIN32
    if (options.open_browser) {
        auto url = "http://" + host;
        std::wstring wide(url.begin(), url.end());
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
#endif
    bool ok = server.listen_after_bind();
    watcher.request_stop();
    watcher.join();
    std::signal(SIGINT, old_int);
    std::signal(SIGTERM, old_term);
    discovery.cancel();
    captures.stop();
    return ok ? 0 : 1;
#endif
}
} // namespace radaer
