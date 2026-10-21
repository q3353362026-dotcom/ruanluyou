#include "radaer/battle.hpp"
#include "radaer/player_view.hpp"
#include "radaer/loot.hpp"
#include <algorithm>
#include <chrono>

namespace radaer {
BattleTracker::BattleTracker(Sink sink, std::function<int64_t()> clock)
    : sink_(std::move(sink)), clock_(std::move(clock)) {
    if (!clock_)
        clock_ = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
}
Json BattleTracker::snapshot() const {
    Json state = {{"status", status_},        {"liveActive", status_ == "live"},
                  {"idleTimeoutMs", idle_ms}, {"flow", nullptr},
                  {"epoch", nullptr},         {"map", nullptr},
                  {"flowSession", nullptr},   {"self", nullptr}, {"serverSelf", nullptr},
                  {"players", Json::array()}, {"loot",Json::object()}};
    if (selected_) {
        const auto &f = flows_.at(*selected_);
        state["flowSession"] = std::get<0>(*selected_);
        state["flow"] = std::get<1>(*selected_);
        state["epoch"] = std::get<2>(*selected_);
        state["map"] = f.map;
        state["lastPlaintextAtMs"] = f.timestamp;
        if (status_ == "live") {
            state["players"] = f.players;
            state["loot"] = f.loot;
            state["self"] = f.self;
            state["serverSelf"] = f.server_self;
        }
    }
    return state;
}
void BattleTracker::update(const std::string &reason, int64_t timestamp) {
    selected_.reset();
    uint64_t latest = 0;
    for (const auto &[key, f] : flows_)
        if (!f.map.empty() && f.order > latest) {
            selected_ = key;
            latest = f.order;
        }
    status_ = stopped_                                              ? "stopped"
              : !selected_                 ? (recovery_required_ ? "recovery_required" : "waiting")
              : clock_() - flows_.at(*selected_).received > idle_ms ? "idle"
                                                                    : "live";
    Json signature = {{"status", status_},
                      {"flow", selected_ ? Json(std::get<1>(*selected_)) : Json()},
                      {"epoch", selected_ ? Json(std::get<2>(*selected_)) : Json()},
                      {"map", selected_ ? Json(flows_.at(*selected_).map) : Json()},
                      {"flowSession", selected_ ? Json(std::get<0>(*selected_)) : Json()}};
    if (signature == last_transition_)
        return;
    auto state = snapshot();
    state["reason"] = reason;
    if (status_ == "idle")
        state["reason"] = "udp_silent_15s";
    else if (last_transition_.is_object() && status_ == "live") {
        if (last_transition_.value("status", std::string()) == "idle" &&
            last_transition_["flow"] == state["flow"] && last_transition_["epoch"] == state["epoch"])
            state["reason"] = "udp_resumed";
        else if (!last_transition_["flow"].is_null() &&
                 (last_transition_["flow"] != state["flow"] || last_transition_["epoch"] != state["epoch"]))
            state["reason"] = "flow_changed";
    }
    state["previousFlow"] = last_transition_.is_object() ? last_transition_["flow"] : Json();
    state["previousEpoch"] = last_transition_.is_object() ? last_transition_["epoch"] : Json();
    state["previousStatus"] = last_transition_.is_object() ? last_transition_["status"] : Json();
    last_transition_ = std::move(signature);
    Json event = identity_;
    event["type"] = "battle_state";
    event["timestampMs"] = timestamp;
    event["data"] = std::move(state);
    if (sink_)
        sink_(std::move(event));
}
void BattleTracker::tick(int64_t timestamp) {
    if (!identity_.empty())
        update("timer", timestamp);
}
void BattleTracker::consume(const Json &event) {
    const auto type = event.value("type", std::string());
    if (type != "session_started" && type != "session_stopped" && type != "capture_gap" &&
        type != "udp_binding_reset" && type != "udp_plaintext" && type != "battle_welcome" &&
        type != "player_validation" && type != "self_validation" &&
        type != "self_server_validation" && type != "player_motion" && type != "player_state" && type != "loot_state")
        return;
    for (const auto key : {"session", "device", "synthetic"})
        if (event.contains(key))
            identity_[key] = event[key];
    auto timestamp = event.value("timestampMs", now_ms());
    if (type == "session_started" || type == "capture_gap") {
        recovery_required_ = type == "capture_gap";
        flows_.clear();
        epochs_.clear();
        retired_.clear();
        stopped_ = false;
        update(type, timestamp);
        return;
    }
    if (type == "session_stopped") {
        stopped_ = true;
        update(type, timestamp);
        return;
    }
    if (stopped_)
        return;
    const auto &d = event.at("data");
    auto session = event.value("session", std::string());
    auto flow = type == "udp_plaintext"
                    ? d.at("local").get<std::string>() + ">" + d.at("remote").get<std::string>()
                    : d.at("flow").get<std::string>();
    auto pair = std::pair{session, flow};
    if (type == "udp_binding_reset") {
        // 旧实现 retired_[pair] = max(epochs_[pair], 事件里的 epoch)。
        // 事件里的 epoch 就是当前 epoch => retired_[pair] == 当前 epoch
        // => 下面 epoch <= retired_[pair] 恒为真 => 该 flow 所有 udp_plaintext
        // 被永久拒收 => f.received 不再更新 => 15 秒后判 idle =>
        // players / loot 全被清空 => 雷达上人和盒子全没了。
        //
        // 正确语义：retired_ 只用来挡住「已经过期的那一代 epoch」。
        // 记录为「当前 epoch - 1」，这样当前 epoch 的包照常接收，
        // 只有更早的迟到包会被丢弃。
        const auto cur = epochs_[pair];
        const auto ev = d.value("epoch", uint64_t(0));
        retired_[pair] = (cur > 0 ? cur : ev) > 0 ? (cur > 0 ? cur : ev) - 1 : 0;
        std::erase_if(flows_, [&](const auto &entry) {
            return std::get<0>(entry.first) == session && std::get<1>(entry.first) == flow;
        });
        update(type, timestamp);
        return;
    }
    auto epoch = d.at("epoch").get<uint64_t>();
    Key key{session, flow, epoch};
    if (type == "udp_plaintext") {
        if (!d.value("structuralValid", false) ||
            (d.at("direction") != "up" && d.at("direction") != "down") || epoch < epochs_[pair] ||
            (retired_.contains(pair) && epoch <= retired_[pair]))
            return;
        if (epoch > epochs_[pair]) {
            std::erase_if(flows_, [&](const auto &entry) {
                return std::get<0>(entry.first) == session && std::get<1>(entry.first) == flow;
            });
            epochs_[pair] = epoch;
        }
        if (!flows_.contains(key) && flows_.size() >= 64)
            throw std::runtime_error("battle tracker flow limit");
        auto &f = flows_[key];
        f.received = clock_();
        f.timestamp = d.at("timestampMs").get<int64_t>();
        f.order = ++order_;
        update("latest_plaintext", timestamp);
    } else if (type == "battle_welcome") {
        auto it = flows_.find(key);
        auto map = d.at("map").get<std::string>();
        if (it == flows_.end() || !map.starts_with("/Game/"))
            return;
        // 原实现只在 map 为空时赋值 => 同一 flow key 换图后永远停在第一张图。
        // 每次 welcome 都更新；相同值赋值无副作用。
        it->second.map = std::move(map);
        recovery_required_ = false;
        update("welcome_confirmed", timestamp);
    } else {
        auto it = flows_.find(key);
        if (it == flows_.end())
            return;
        if (type == "loot_state") { apply_loot_state(it->second.loot,d); return; }
        if (type == "player_state") {
            apply_player_state(it->second.players,it->second.self,it->second.server_self,d);
            return;
        }
        if (type == "player_motion") {
            apply_player_motion(it->second.players, d);
            return;
        }
        if (type == "self_server_validation") {
            it->second.server_self = d;
            return;
        }
        if (type == "self_validation") {
            it->second.self = d;
            return;
        }
        Json row = Json::object();
        for (auto name : {"flow",
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
            if (d.contains(name))
                row[name] = d[name];
        upsert_player_row(it->second.players, std::move(row));
    }
}
} // namespace radaer
