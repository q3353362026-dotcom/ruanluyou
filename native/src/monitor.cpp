#include "radaer/monitor.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <system_error>
#include <stdexcept>
#include <condition_variable>
#include <thread>
#include <chrono>
namespace radaer {
BankBroker::~BankBroker() {
    for (auto &c : candidates_)
        OPENSSL_cleanse(c.bank.data(), c.bank.size());
    for (auto &[k, f] : flows_)
        if (f.bound)
            OPENSSL_cleanse(f.bound->bank.data(), f.bound->bank.size());
}
Json udp_raw_event(const Packet &p, bool save_payload, const char *timestamp_source) {
    Json data = {{"direction", p.up ? "up" : "down"},
                 {"local", p.local},
                 {"remote", p.remote},
                 {"payloadLength", p.payload.size()},
                 {"timestampSource", timestamp_source},
                 {"ipFragmentCount", p.ip_fragments},
                 {"reassemblyDelayMs", p.reassembly_delay_ms},
                 {"packetOrdinal", p.ordinal}};
    if (save_payload)
        data["payloadBase64"] = base64(p.payload);
    return {{"type", "udp_raw"}, {"timestampMs", p.timestamp}, {"data", std::move(data)}};
}

void BankBroker::candidate(const Bank &bank, const std::string &source, int64_t ts) {
    auto fp = fingerprint(bank);
    if (std::any_of(candidates_.begin(), candidates_.end(), [&](const Candidate &c) { return c.fp == fp; }))
        return;
    if (candidates_.size() == 64) {
        OPENSSL_cleanse(candidates_.front().bank.data(), 128);
        candidates_.pop_front();
    }
    candidates_.push_back({bank, fp});
    sink_({{"type", "bank_candidate"},
           {"timestampMs", ts},
           {"data", {{"fingerprint", fp}, {"source", source}, {"confirmed", false}}}});
    for (auto &[k, f] : flows_)
        if (!f.bound)
            attempt(k, f);
}
void BankBroker::reset(const std::string &key, Flow &f, const std::string &why, int64_t ts) {
    if (f.bound)
        OPENSSL_cleanse(f.bound->bank.data(), 128);
    f.bound.reset();
    f.failures = 0;
    sink_({{"type", "udp_binding_reset"},
           {"timestampMs", ts},
           {"data", {{"flow", key}, {"reason", why}, {"epoch", f.epoch}}}});
}
void BankBroker::gap(int64_t ts, const std::string &reason, int64_t since) {
    for (auto &[k, f] : flows_) {
        reset(k, f, reason, ts);
        f.history.clear();
    }
    sink_({{"type", "capture_gap"},
           {"timestampMs", ts},
           {"data", {{"reason", reason}, {"sinceMs", since > 0 ? since : ts}}}});
}
void BankBroker::emit_plain(const Packet &p, Flow &f) {
    Bytes plain = p.up ? Bytes(p.payload.begin() + 8, p.payload.end()) : udp_crypt(p.payload, f.bound->bank);
    if (!valid_plaintext(plain))
        return;
    bool envelope = valid_envelope(plain);
    Json d = {{"flow", p.local + ">" + p.remote},
              {"local", p.local},
              {"remote", p.remote},
              {"epoch", f.epoch},
              {"direction", p.up ? "up" : "down"},
              {"timestampMs", p.timestamp},
              {"packetOrdinal", p.ordinal},
              {"ipFragmentCount", p.ip_fragments},
              {"reassemblyDelayMs", p.reassembly_delay_ms},
              {"fingerprint", f.bound->fp},
              {"structuralValid", true},
              {"validationLevel", envelope ? "bounded_envelope" : "handshake_shape_only"},
              {"cryptographicAuthenticated", false}};
    // A 25-byte handshake shape alone does not prove the bank is correct.
    if (!p.up && envelope)
        sink_({{"type", "bank_use"}, {"timestampMs", p.timestamp}, {"data", d}});
    d["payloadLength"] = plain.size();
    d["payloadBase64"] = base64(plain);
    sink_({{"type", "udp_plaintext"}, {"timestampMs", p.timestamp}, {"data", d}});
}
void BankBroker::attempt(const std::string &key, Flow &f) {
    std::vector<const Packet *> recent;
    for (auto i = f.history.rbegin(); i != f.history.rend() && recent.size() < 4; ++i)
        if (!i->up && i->payload.size() >= 10)
            recent.push_back(&*i);
    std::set<std::string> distinct;
    for (auto p : recent)
        distinct.insert(fingerprint(p->payload));
    if (distinct.size() < 3)
        return;
    std::optional<Candidate> match;
    unsigned matches = 0;
    for (auto &c : candidates_) {
        bool ok = true;
        for (auto p : recent)
            if (!valid_envelope(udp_crypt(p->payload, c.bank))) {
                ok = false;
                break;
            }
        if (ok) {
            match = c;
            ++matches;
        }
    }
    if (matches != 1)
        return;
    f.bound = match;
    f.epoch = ++next_epoch_;
    f.failures = 0;
    sink_({{"type", "bank_bound"},
           {"timestampMs", f.history.back().timestamp},
           {"data",
            {{"flow", key},
             {"fingerprint", match->fp},
             {"epoch", f.epoch},
             {"distinctEvidence", distinct.size()},
             {"method", "unique_candidate_structural_3_of_4"},
             {"cryptographicAuthenticated", false}}}});
    for (auto &p : f.history)
        if (!p.up || p.payload.size() >= 8)
            emit_plain(p, f);
    f.history.clear();
}
void BankBroker::packet(Packet p) {
    if (p.payload.size() > 65507 || p.timestamp <= 0)
        throw std::runtime_error("invalid UDP input");
    std::string k = p.local + ">" + p.remote;
    if (p.up && p.payload.size() < 8)
        return;
    if (!flows_.contains(k) && flows_.size() >= 256) {
        // LAN capture includes unrelated UDP (DNS, QUIC, discovery). Recycle
        // unbound flows first, and only retire a bound flow after a minute idle.
        auto victim = flows_.end();
        for (auto it = flows_.begin(); it != flows_.end(); ++it) {
            if (it->second.bound && p.timestamp - it->second.last_seen <= 60000)
                continue;
            if (victim == flows_.end() ||
                (victim->second.bound && !it->second.bound) ||
                (bool(victim->second.bound) == bool(it->second.bound) &&
                 it->second.last_seen < victim->second.last_seen))
                victim = it;
        }
        if (victim == flows_.end())
            return; // Preserve active validated flows without ending capture.
        if (victim->second.epoch)
            reset(victim->first, victim->second, "flow_capacity_retired", p.timestamp);
        flows_.erase(victim);
    }
    auto &f = flows_[k];
    f.last_seen = std::max(f.last_seen, p.timestamp);
    if (p.up && initial_handshake(p.payload)) {
        reset(k, f, "initial_handshake", p.timestamp);
        f.history.clear();
    }
    if (f.bound) {
        Bytes plain =
            p.up ? Bytes(p.payload.begin() + 8, p.payload.end()) : udp_crypt(p.payload, f.bound->bank);
        if (valid_plaintext(plain)) {
            if (!p.up && valid_envelope(plain)) {
                f.failures = 0;
                f.history.clear();
            }
            emit_plain(p, f);
            return;
        }
        if (p.up)
            return;
        sink_({{"type", "udp_invalid"},
               {"timestampMs", p.timestamp},
               {"data", {{"flow", k}, {"epoch", f.epoch}, {"reason", "decrypted_envelope_invalid"}}}});
        ++f.failures;
        f.history.push_back(p);
        if (f.failures < 3)
            return;
        reset(k, f, "three_invalid_downstream_packets", p.timestamp);
        attempt(k, f);
        return;
    }
    while (!f.history.empty() &&
           (f.history.size() >= 64 || p.timestamp - f.history.front().timestamp > 10000))
        f.history.pop_front();
    f.history.push_back(std::move(p));
    attempt(k, f);
}
std::string journal_context(const std::filesystem::path &path, const char *action);
std::string journal_limit_context(const std::string &path, size_t used, size_t limit, size_t attempted) {
    return "capture journal size limit reached at '" + path + "' (" + std::to_string(used) + "+" +
           std::to_string(attempted) + " bytes, limit " + std::to_string(limit) + "); start a new session or "
           "increase --max-log-bytes";
}
// Only this worker touches the file after construction. A bounded queue keeps
// USB write stalls off the capture thread; overload is reported, never hidden.
struct Journal::Writer {
    std::ofstream &out;
    std::string path;
    size_t max_bytes;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> queue;
    size_t queued_bytes = 0;
    size_t in_flight_bytes = 0;
    size_t written_bytes = 0;
    bool closing = false;
    bool limit_hit = false;
    std::exception_ptr failure;
    std::thread worker;
    Writer(std::ofstream &stream, std::string file_path, size_t limit)
        : out(stream), path(std::move(file_path)), max_bytes(limit), worker([this] { run(); }) {}
    void push(std::string line) {
        std::lock_guard lock(mutex);
        if (failure)
            std::rethrow_exception(failure);
        if (closing)
            throw std::runtime_error("capture journal already closed");
        // 到上限后静默丢弃，不再抛异常。
        // 原实现 throw 会一路冒到采集 worker 线程把它打死 => phase 掉回 idle，
        // 对局中途丢包废局。这里改成丢弃 + 只记一次标志，采集继续跑。
        const auto used = written_bytes + in_flight_bytes + queued_bytes;
        if (line.size() > max_bytes || used > max_bytes - line.size()) {
            limit_hit = true;
            return;
        }
        if (line.size() > 8 * 1024 * 1024 || queued_bytes > 8 * 1024 * 1024 - line.size()) {
            // 队列满同样只丢弃，不杀 worker
            limit_hit = true;
            return;
        }
        queued_bytes += line.size();
        queue.push_back(std::move(line));
        ready.notify_one();
    }
    void run() noexcept {
        try {
            for (;;) {
                std::string batch;
                {
                    std::unique_lock lock(mutex);
                    ready.wait_for(lock, std::chrono::milliseconds(250), [&] {
                        return closing || queued_bytes >= 64 * 1024;
                    });
                    if (queue.empty() && closing)
                        break;
                    while (!queue.empty() && batch.size() < 256 * 1024) {
                        queued_bytes -= queue.front().size();
                        batch += queue.front();
                        queue.pop_front();
                    }
                    in_flight_bytes += batch.size();
                }
                if (!batch.empty()) {
                    out.write(batch.data(), std::streamsize(batch.size()));
                    out.flush();
                    if (!out) {
                        const auto detail = errno ? std::strerror(errno) : "stream write or flush failed";
                        throw std::runtime_error(journal_context(path, "capture journal write failed") + ": " +
                                                 detail);
                    }
                    std::lock_guard lock(mutex);
                    in_flight_bytes -= batch.size();
                    written_bytes += batch.size();
                }
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            failure = std::current_exception();
        }
    }
    void finish() {
        { std::lock_guard lock(mutex); closing = true; }
        ready.notify_one();
        if (worker.joinable())
            worker.join();
        if (failure)
            std::rethrow_exception(failure);
    }
    ~Writer() { try { finish(); } catch (...) {} }
};
std::string journal_context(const std::filesystem::path &path, const char *action) {
    std::ostringstream message;
    message << action << " '" << path.string() << "'";
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    std::error_code error;
    const auto space = std::filesystem::space(parent, error);
    if (!error)
        message << " (free " << space.available << " bytes in '" << parent.string() << "')";
    else
        message << " (free space unavailable: " << error.message() << ")";
    message << "; choose a writable path, free storage, or disable logging";
    return message.str();
}

Journal::~Journal() = default;
void Journal::finish() {
    std::lock_guard lock(mutex_);
    if (writer_)
        writer_->finish();
}
Journal::Journal(const std::string &path, std::string device, bool synthetic, Sink observer, bool save_logs,
                 size_t max_bytes)
    : device_(std::move(device)), synthetic_(synthetic), observer_(std::move(observer)) {
    if (save_logs && (max_bytes < min_journal_max_bytes || max_bytes > max_journal_max_bytes))
        throw std::runtime_error("capture journal max size must be between " + std::to_string(min_journal_max_bytes) +
                                 " and " + std::to_string(max_journal_max_bytes));
    std::error_code error;
    if (save_logs && std::filesystem::exists(path, error))
        throw std::runtime_error("output exists at '" + path + "'; choose a new session path");
    if (save_logs && error)
        throw std::runtime_error(journal_context(path, "cannot inspect journal path") + ": " + error.message());
    auto parent = std::filesystem::path(path).parent_path();
    if (save_logs && !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error)
            throw std::runtime_error(journal_context(path, "cannot create journal directory") + ": " + error.message());
    }
    std::array<uint8_t, 8> nonce{};
    if (RAND_bytes(nonce.data(), 8) != 1)
        throw std::runtime_error("session random failure");
    session_ = std::to_string(now_ms()) + "-" + hex(nonce);
    if (save_logs) {
        out_.open(path);
        if (!out_) {
            const auto detail = errno ? std::strerror(errno) : "stream open failed";
            throw std::runtime_error(journal_context(path, "cannot create journal") + ": " + detail);
        }
        writer_ = std::make_unique<Writer>(out_, path, max_bytes);
    }
}
void Journal::emit(Json e) {
    std::lock_guard lock(mutex_);
    e["schema"] = "radaer-native/v1";
    e["sequence"] = ++seq_;
    e["session"] = session_;
    e["device"] = device_;
    e["synthetic"] = synthetic_;
    if (!e.contains("timestampMs"))
        e["timestampMs"] = now_ms();
    if (!e.contains("data"))
        e["data"] = Json::object();
    if (writer_) writer_->push(e.dump() + '\n');
    if (observer_)
        observer_(std::move(e));
}
Json PlayerValidator::inspect(const Json &event) {
    const auto &d = event.at("data");
    Json flags = Json::array();
    Json result = {{"type", event.value("type", std::string()) == "self_server_sample" ? "self_server_validation" :
                            event.value("type", std::string()) == "self_sample" ? "self_validation" : "player_validation"},
                   {"data", d}};
    auto &r = result["data"];
    auto key = event.value("session", std::string()) + "|" + d.value("flow", std::string()) + "|" +
               std::to_string(d.value("epoch", 0)) + "|" + d.at("track").get<std::string>();
    auto ts = d.at("timestampMs").get<int64_t>();
    auto seq = d.at("poseSequence").get<uint64_t>();
    auto source_packet = d.value("sourcePacketOrdinal", uint64_t(0));
    double x = d.at("x").get<double>(), y = d.at("y").get<double>(), z = d.at("z").get<double>();
    bool ok = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && ts > 0;
    if (!ok)
        flags.push_back("nonfinite_or_invalid_timestamp");
    if (d.contains("handshakeOk") && !d.at("handshakeOk").get<bool>()) {
        flags.push_back("UE_handshake_unconfirmed");
        ok = false;
    }
    const bool history_errors = d.value("packetErrorsSincePrevious", 0) > 0 ||
                                d.value("semanticErrorsSincePrevious", 0) > 0;
    // These counters cover the interval since this actor's previous position, not
    // the current position block. A complete bounded decode establishes a new
    // continuity baseline; a channel error must not discard every actor's next pose.
    // Prefix-only and legacy samples cannot establish that boundary after an error.
    const bool continuity_reset = history_errors &&
        d.value("decodeCompleteness", std::string()) == "complete" &&
        d.value("movementFrameComplete", true) && d.value("movementBlockComplete", true);
    if (history_errors) {
        flags.push_back(continuity_reset ? "decoder_continuity_reset" : "decoder_error_during_sample");
        if (!continuity_reset)
            ok = false;
    }
    // Known coordinate fields may be complete while a later native object is unsupported.
    // Report coverage separately; handshake, bounds, time and continuity checks still apply.
    if (d.value("decodeCompleteness", std::string()) == "known_prefix")
        flags.push_back("partial_movement_block");
    if (!last_.contains(key) && last_.size() >= 100000)
        throw std::runtime_error("player track limit reached");
    int64_t age = event.at("timestampMs").get<int64_t>() - ts;
    if (age < 0) {
        flags.push_back("future_timestamp");
        ok = false;
    }
    if (age > limits_.max_age_ms) {
        flags.push_back("stale");
        ok = false;
    }
    if (limits_.max_abs_coordinate > 0 &&
        std::max({std::abs(x), std::abs(y), std::abs(z)}) > limits_.max_abs_coordinate) {
        flags.push_back("outside_configured_world");
        ok = false;
    }
    unsigned good = 0;
    auto it = last_.find(key);
    if (it != last_.end()) {
        auto &prev = it->second;
        good = continuity_reset ? 0 : prev.good;
        double dt = double(ts - prev.ts);
        if (dt == 0 && event.value("type", std::string()) == "self_sample" &&
            d.value("gameTime", 0.0) > prev.game_time && prev.game_time > 0) {
            dt = (d.at("gameTime").get<double>() - prev.game_time) * 1000;
            r["intervalSource"] = "upstream_game_clock_same_capture_millisecond";
        }
        // Millisecond capture timestamps can coincide for distinct ordered packets.
        // Keep the real timestamp; packet and pose order must both advance.
        const bool same_millisecond = dt == 0 && seq > prev.seq &&
                                      prev.source_packet > 0 && source_packet > prev.source_packet;
        if (same_millisecond) {
            r["intervalSource"] = "capture_packet_order_same_millisecond";
            r["intervalUpperBoundMs"] = 1;
        }
        if (seq <= prev.seq || dt < 0 || (dt == 0 && !same_millisecond)) {
            flags.push_back(seq == prev.seq ? "duplicate_pose" : "out_of_order");
            ok = false;
        }
        if (dt > limits_.max_gap_ms) {
            flags.push_back("sample_gap");
            good = 0;
        }
        if (dt > 0 || same_millisecond) {
            // For coincident timestamps, 1 ms bounds the elapsed capture time;
            // report only a lower bound, never invent an exact time or speed.
            double speed = std::hypot(x - prev.x, y - prev.y, z - prev.z) * 1000.0 /
                           (same_millisecond ? 1.0 : dt);
            r[same_millisecond ? "speedLowerBoundUnitsPerSecond" : "speedUnitsPerSecond"] = speed;
            if (dt <= limits_.max_gap_ms && limits_.max_speed > 0 && speed > limits_.max_speed) {
                flags.push_back("speed_exceeds_configured_limit");
                ok = false;
            }
        }
    }
    if (ok) {
        ++good;
        last_[key] = {x, y, z, ts, seq, good, d.value("gameTime", 0.0), source_packet};
    } else if (it != last_.end())
        it->second.good = 0;
    r["flags"] = flags;
    r["consecutivePlausibleSamples"] = ok ? good : 0;
    r["status"] = !ok ? "rejected" : good < limits_.minimum_samples ? "warming_up" : "plausible";
    r["cryptographicAuthenticated"] = false;
    r["identityVerified"] = d.value("identitySource", std::string()) == "explicit_protocol";
    r["worldBoundEnabled"] = limits_.max_abs_coordinate > 0;
    r["speedBoundEnabled"] = limits_.max_speed > 0;
    return result;
}
} // namespace radaer
