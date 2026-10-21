#include "radaer/rates.hpp"
#include <algorithm>
#include <chrono>

namespace radaer {
uint64_t LiveRates::Window::count(int64_t now) {
    while (!buckets.empty() && buckets.front().first <= now - window_ms) {
        total -= buckets.front().second;
        buckets.pop_front();
    }
    return total;
}
void LiveRates::Window::add(int64_t now) {
    count(now);
    if (!buckets.empty() && buckets.back().first == now)
        ++buckets.back().second;
    else
        buckets.emplace_back(now, 1);
    ++total;
}
LiveRates::LiveRates(std::function<int64_t()> clock) : clock_(std::move(clock)) {
    if (!clock_)
        clock_ = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
}
void LiveRates::clear_coordinates() {
    coordinate_packets_ = {};
    coordinate_samples_ = {};
    self_samples_ = {};
    last_coordinate_.reset();
    slots_.clear();
    packets_seen_.clear();
    packet_times_.clear();
}
void LiveRates::reset() {
    running_ = false;
    session_.clear();
    current_.reset();
    decrypted_ = {};
    last_decrypted_.reset();
    clear_coordinates();
}
void LiveRates::prune_packets(int64_t now) {
    while (!packet_times_.empty() && packet_times_.front().first <= now - window_ms) {
        packets_seen_.erase(packet_times_.front().second);
        packet_times_.pop_front();
    }
}
void LiveRates::consume(const Json &event) {
    const auto type = event.value("type", std::string());
    if (type == "session_started") {
        reset();
        running_ = true;
        session_ = event.value("session", std::string());
        return;
    }
    if (!running_ || event.value("session", std::string()) != session_)
        return;
    if (type == "session_stopped") {
        reset();
        return;
    }
    if (type == "capture_gap") {
        current_.reset();
        decrypted_ = {};
        last_decrypted_.reset();
        clear_coordinates();
        return;
    }
    if (type != "battle_state" && type != "udp_binding_reset" && type != "udp_plaintext" &&
        type != "player_sample" && type != "self_sample")
        return;
    const auto &d = event.at("data");
    if (type == "battle_state") {
        std::optional<FlowKey> next;
        if (d.value("status", std::string()) == "live")
            next = FlowKey{d.at("flowSession").get<std::string>(), d.at("flow").get<std::string>(),
                           d.at("epoch").get<uint64_t>()};
        if (next != current_)
            clear_coordinates();
        current_ = std::move(next);
        return;
    }
    if (type == "udp_binding_reset") {
        if (current_ && std::get<1>(*current_) == d.at("flow").get<std::string>()) {
            current_.reset();
            clear_coordinates();
        }
        return;
    }
    const auto now = clock_();
    if (type == "udp_plaintext") {
        // Upstream packets have their clear envelope stripped; they are not successful decryptions.
        if (d.at("direction") == "down" && d.value("structuralValid", false)) {
            decrypted_.add(now);
            last_decrypted_ = now;
        }
        return;
    }
    if (!current_ ||
        *current_ != FlowKey{session_, d.at("flow").get<std::string>(), d.at("epoch").get<uint64_t>()})
        return;
    if (type == "self_sample") {
        self_samples_.add(now);
        return;
    }
    coordinate_samples_.add(now);
    last_coordinate_ = now;
    prune_packets(now);
    auto ordinal = d.value("sourcePacketOrdinal", uint64_t(0));
    if (ordinal && packets_seen_.insert(ordinal).second) {
        packet_times_.emplace_back(now, ordinal);
        coordinate_packets_.add(now);
    }
    auto key = std::pair{d.at("channel").get<uint32_t>(), d.at("slot").get<unsigned>()};
    if (!slots_.contains(key) && slots_.size() >= 128) {
        auto oldest = std::min_element(slots_.begin(), slots_.end(), [](const auto &a, const auto &b) {
            return a.second.received < b.second.received;
        });
        slots_.erase(oldest);
    }
    auto &s = slots_[key];
    auto track = d.value("track", std::string());
    if (s.track != track)
        s = {};
    auto source_time = d.at("timestampMs").get<int64_t>();
    s.source_interval = s.source_time > 0 && source_time >= s.source_time
                            ? std::optional<int64_t>(source_time - s.source_time)
                            : std::nullopt;
    s.track = std::move(track);
    s.source_time = source_time;
    s.received = now;
    s.updates.add(now);
}
Json LiveRates::snapshot() {
    const auto now = clock_();
    prune_packets(now);
    return {{"windowMs", window_ms},
            {"udpDecryptPacketsPerSecond", decrypted_.count(now)},
            {"coordinatePacketsPerSecond", coordinate_packets_.count(now)},
            {"coordinateSamplesPerSecond", coordinate_samples_.count(now)},
            {"selfSamplesPerSecond", self_samples_.count(now)},
            {"lastDecryptAgeMs", last_decrypted_ ? Json(now - *last_decrypted_) : Json()},
            {"lastCoordinateAgeMs", last_coordinate_ ? Json(now - *last_coordinate_) : Json()}};
}
Json LiveRates::slot(uint32_t channel, unsigned index) {
    Json result = {{"updatesPerSecond", 0}, {"lastUpdateAgeMs", nullptr}, {"sourceIntervalMs", nullptr}};
    auto it = slots_.find({channel, index});
    if (it != slots_.end()) {
        const auto now = clock_();
        auto &s = it->second;
        result["updatesPerSecond"] = s.updates.count(now);
        result["lastUpdateAgeMs"] = now - s.received;
        if (s.source_interval)
            result["sourceIntervalMs"] = *s.source_interval;
    }
    return result;
}
} // namespace radaer
