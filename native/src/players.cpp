#include "radaer/players.hpp"
#include "radaer/battle.hpp"
#include "radaer/ue.hpp"
#include "radaer/health.hpp"
#include "radaer/equipment.hpp"
#include "radaer/loot.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <tuple>
namespace radaer {
namespace {
using ue::Bunch;
bool world(const Json &p) {
    if (!p.is_array() || p.size() != 3)
        return false;
    bool nonzero = false;
    for (auto &c : p) {
        if (!c.is_number())
            return false;
        double n = c.get<double>();
        if (!std::isfinite(n) || std::abs(n) > 2097152)
            return false;
        nonzero |= n != 0;
    }
    return nonzero;
}
int64_t unwrap(uint32_t n, int64_t last, unsigned mod) {
    int64_t v = last - last % mod + n;
    if (v - last > mod / 2)
        v -= mod;
    else if (last - v >= mod / 2)
        v += mod;
    return v;
}
uint32_t guid(const Json &j) {
    return j.is_object() ? j.value("guid", uint32_t(0)) : j.is_number_unsigned() ? j.get<uint32_t>() : 0;
}
struct Queued {
    Bunch bunch;
    Json event;
    int64_t sequence = 0;
};
struct Channel {
    uint32_t actor = 0, arch = 0;
    std::string profile;
    std::optional<int64_t> delivered;
    std::map<int64_t, Queued> queue;
    std::optional<Bunch> partial;
    int64_t partial_sequence = 0, last_time = 0, last_unreliable = -1;
    uint64_t generation = 0;
    size_t errors = 0;
    size_t server_self_errors = 0;
    bool movement_confirmed = false;
};
struct Actor {
    Json fields = Json::object();
    std::string identity_key;
    uint32_t owner = 0, ps = 0;
    int slot = -1;
    bool exact = false;
    bool player_state = false;
    HealthTracker::Stamp life_stamp{};
    Json live_status;
};
struct Track {
    uint64_t poses = 0, generation = 0, revision = 0;
    size_t errors = 0, packet_errors = 0;
    uint32_t actor = 0, ps = 0;
    Json motion = Json::object();
};
struct Flow {
    LootTracker loot;
    HealthTracker health;
    EquipmentTracker equipment;
    uint64_t logical_order = 0;
    uint32_t owning_actor = 0;
    bool state_bindings_dirty = false;
    std::map<uint32_t, Json> lifecycle;
    std::map<uint32_t, Channel> channels;
    std::map<uint32_t, Json> paths;
    std::map<uint32_t, Actor> actors;
    // slot -> actor 索引。position() 每个移动包都要按 slot 找 actor，
    // 旧实现遍历整个 actors 表（最多 8192 项），绝密模式每秒几十个移动包
    // => 每秒数百万次比较 => 单线程 CPU 吃满 => 数据积压 => 雷达位置滞后。
    // 用索引把查找降到 O(log n)，并在 actors 增删时同步维护。
    std::map<int, uint32_t> slot_index;   // slot -> actor id（0 表示冲突/无效）
    std::set<int> slot_dup;               // 出现过多个 actor 的 slot（冲突）
    std::map<std::pair<uint32_t, unsigned>, Track> slots;
    std::map<int, std::pair<int64_t, uint64_t>> claim_time;
    std::set<int> claim_conflicts;
    uint64_t self_poses = 0;
    uint64_t server_self_poses = 0;
    size_t self_errors = 0, up_packet_errors = 0, self_packet_errors = 0;
    size_t server_packet_errors = 0;
    std::map<std::string, int64_t> packet_high;
    std::set<std::pair<std::string, int64_t>> seen;
    std::deque<std::pair<std::string, int64_t>> seen_order;
    bool welcome = false;
    size_t errors = 0, packet_errors = 0;
    // 队列/分片缓冲的近似字节数，增量维护（见 buffered()）。
    size_t buffered_bytes = 0;
    size_t buffered() const {
        // 旧实现每个包都遍历 channels × queue（通道上限 2048，队列 64 => 13 万次
        // std::map 节点遍历），而 consume() 里每包要调两次 => 每秒百万级指针追逐。
        // 改成增量维护：只在这两个上限判断里用到「是否超过 8MB」，用近似值足够。
        return buffered_bytes;
    }
};
std::string profile_for(const std::string &s) {
    auto loot=LootTracker::profile(s);if(!loot.empty())return loot;
    if (s.find("BP_PropertyReplicationCharacterHealth") != std::string::npos)
        return "health";
    if (s.find("MovementReplicationActor") != std::string::npos)
        return "movement";
    if (s.find("BP_SOLPlayerState") != std::string::npos)
        return "current";
    if (s.find("BP_DFMPlayerState") != std::string::npos)
        return "legacy";
    // AI 角色类有 270 条属性（Character 只有 200 条），必须走独立表，
    // 否则 handle 越界 => "replication handle outside catalog" => 身份链断裂。
    if (s.find("BP_DFMAICharacter") != std::string::npos || s.find("BP_DFMCharacter_AI") != std::string::npos)
        return "character_ai";
    if (s.find("BP_DFMCharacter") != std::string::npos || s.find("BP_SOLCharacter") != std::string::npos)
        return "character";
    // AZ3 地图的真人角色换用 BP_AZCharacter_C（不带 BP_DFM 前缀）。
    // 不加这条规则 -> profile 为空 -> 通道被跳过 -> MoveHandle 永不解析 ->
    // actor.slot 恒为 -1 -> identitySource=unassociated_slot -> 队伍全空 -> 敌人显示成队友。
    // 属性布局与 BP_DFMCharacter_C 一致（Python 参考实现 liveview.py 也是共用 char_cmds）。
    // 必须精确匹配：同图还有 BP_InteractorContainer_AZ_NukesBackpack_C /
    // BP_DynamicWaterSystem_AZ_C 等非角色类，宽泛匹配会把它们误判成角色。
    if (s.find("BP_AZCharacter") != std::string::npos)
        return "character";
    return {};
}
Json vitals_for(const Flow &f,uint32_t actor){
    auto result=f.health.snapshot(actor);auto maximum=f.equipment.max_health(actor);
    if(!maximum.is_null())result["health"]["MaxHealth"]=maximum;
    return result;
}
std::string path_for(const Flow &f, uint32_t id, unsigned depth = 0) {
    if (depth > 16)
        return {};
    auto it = f.paths.find(id);
    if (it == f.paths.end())
        return {};
    auto path = it->second.value("path", Json());
    if (!path.is_string())
        return {};
    std::string s = path.get<std::string>();
    auto outer = it->second.value("outer", Json());
    if (outer.is_number_unsigned() && outer != 0)
        s = path_for(f, outer, depth + 1) + "/" + s;
    return s;
}
void update_exports(Flow &f, const Json &entries) {
    f.health.exports(entries);
    f.loot.exports(entries);
    for (auto &x : entries)
        if (x.at("path").is_string()) {
            auto id = x.at("guid").get<uint32_t>();
            if (!f.paths.contains(id) && f.paths.size() >= 8192)
                throw ue::Error("NetGUID cache capacity");
            f.paths[id] = x;
        }
}
uint32_t player_state_for(const Flow &f, const Actor &actor) {
    if (actor.ps) {
        auto it = f.actors.find(actor.ps);
        return it != f.actors.end() && it->second.player_state ? actor.ps : 0;
    }
    // Owner is a Controller, not a PlayerState. Resolve only a unique shared Controller.
    uint32_t match = 0;
    if (actor.owner)
        for (const auto &[id, ps] : f.actors)
            if (ps.player_state && ps.owner == actor.owner) {
                if (match)
                    return 0;
                match = id;
            }
    return match;
}
std::string welcome_map(const Bunch &b) {
    if (b.bits < 8 || b.payload.empty() || b.payload[0] != 1)
        return {};
    try {
        ue::Bits r(b.payload, b.bits, 8);
        auto map = r.string();
        if (map.starts_with("/Game/"))
            return map;
    } catch (const ue::Error &) {
        // The original scanner also accepts a bounded /Game/ string when FString framing differs.
    }
    std::string bytes(b.payload.begin(), b.payload.begin() + b.bits / 8);
    auto start = bytes.find("/Game/");
    if (start == std::string::npos || start >= 128)
        return {};
    auto end = bytes.find('\0', start);
    auto map = bytes.substr(start, end == std::string::npos ? end : end - start);
    for (auto &c : map)
        if (static_cast<unsigned char>(c) >= 128)
            c = '?';
    return map;
}
Json attributes(const Json &fields) {
    Json out = Json::object();
    for (auto [wire, name] : {std::pair{"HeroId", "heroId"}, {"ArmForceID", "armForceId"},
             {"bIsABot", "isBot"}, {"CurrentCharacterLiveStatus", "liveStatus"},
             {"bDead", "dead"}, {"bFinishGame", "finished"}, {"bIsDeadBox", "deadBox"},
             {"DeathWaitRescueTime", "rescueSeconds"}, {"PlayerGamePlayState", "gameplayState"}})
        if (fields.contains(wire)) out[name] = fields.at(wire);
    return out;
}
bool bounded_vector(const Json &v) {
    if (!v.is_array() || v.size() != 3) return false;
    for (auto &n : v)
        if (!n.is_number() || !std::isfinite(n.get<double>()) || std::abs(n.get<double>()) > 100000)
            return false;
    return true;
}
void extend_motion(Json &out, const Json &item, int64_t timestamp) {
    if (timestamp < out.value("updatedAtMs", int64_t(0))) return;
    auto angle = [](uint16_t value) { double d = value * 360.0 / 65536; return d > 180 ? d - 360 : d; };
    if (item.contains("pv") && bounded_vector(item.at("pv"))) {
        out["velocity"] = item.at("pv");
        out["velocityAtMs"] = timestamp;
    }
    if (item.contains("rot")) out["bodyRotation"] = item.at("rot");
    if (item.contains("scalar") && item.at("scalar").is_number() &&
        item.at("scalar").get<double>() >= 0 && item.at("scalar").get<double>() <= 1000000) {
        out["timeSeconds"] = item.at("scalar");
        out["timeAtMs"] = timestamp;
    }
    if (item.contains("u160")) {
        auto packed = item.at("u160").get<uint32_t>();
        out["viewYaw"] = angle(uint16_t(packed >> 16));
        out["viewPitch"] = angle(uint16_t(packed));
        out["viewAtMs"] = timestamp;
    }
    out["updatedAtMs"] = timestamp;
    out["source"] = "downstream_h21";
    out["semanticConfidence"] = "cross_validated";
}
} // namespace
struct PlayerDecoder::Impl {
    Sink sink;
    std::vector<Json> emitted;
    std::map<std::tuple<std::string, std::string, uint64_t>, Flow> flows;
    std::map<std::pair<std::string, std::string>, uint64_t> epochs, retired;
    uint64_t packets = 0, samples = 0, errors = 0, duplicates = 0, identities = 0, reset_generation = 0;
    uint64_t warnings = 0, self_samples = 0, server_self_samples = 0, motion_updates = 0;
    bool synthetic = false;
    explicit Impl(Sink s) : sink(std::move(s)) {}
    void loot_event(Flow &f,const Json &event,uint32_t actor) {
        auto row=f.loot.snapshot(actor);if(row.is_null())return;
        const auto &d=event.at("data");
        row["flow"]=d.at("local").get<std::string>()+">"+d.at("remote").get<std::string>();
        row["epoch"]=d.at("epoch");emit(event,"loot_state",std::move(row));
    }
    void emit(const Json &event, std::string type, Json data) {
        if (emitted.size() >= 4096) {
            if (type == "player_decode_error" || type == "player_decode_warning")
                return;
            throw ue::Error("player events per packet limit");
        }
        Json e = Json::object();
        e["type"] = std::move(type);
        e["session"] = event.value("session", std::string());
        e["synthetic"] = event.value("synthetic", true);
        e["timestampMs"] = event.value("timestampMs", int64_t(0));
        e["data"] = std::move(data);
        if (event.contains("device"))
            e["device"] = event["device"];
        emitted.push_back(std::move(e));
    }
    void diagnostic(Flow &f, const Json &e, const std::string &why,
                    std::optional<uint32_t> channel = std::nullopt, bool warning = false) {
        const auto direction = e.at("data").value("direction", std::string());
        if (warning)
            ++warnings;
        else {
            ++errors;
            ++f.errors;
            if (channel) {
                auto it = f.channels.find(*channel + (direction == "up" ? 32768 : 0));
                if (it != f.channels.end())
                    ++it->second.errors;
            } else if (direction != "up")
                ++f.packet_errors;
            else
                ++f.up_packet_errors;
        }
        emit(e, warning ? "player_decode_warning" : "player_decode_error",
             {{"error", why},
              {"channel", channel ? Json(*channel) : Json(nullptr)},
              {"scope", channel ? "channel" : "packet"},
              {"direction", direction},
              {"affectsContinuity", !warning},
              {"epoch", e.at("data").at("epoch")},
              {"flow", e.at("data").at("local").get<std::string>() + ">" +
                           e.at("data").at("remote").get<std::string>()},
              {"packetOrdinal", e.at("data").value("packetOrdinal", uint64_t(0))}});
    }
    void state_event(Flow &f, const Json &e, uint32_t actor) {
        const auto &d = e.at("data");
        Json state = {{"flow",d.at("local").get<std::string>() + ">" + d.at("remote").get<std::string>()},
            {"epoch",d.at("epoch")},{"actorGuid",actor},{"timestampMs",d.at("timestampMs")},
            {"vitals",vitals_for(f,actor)},{"equipment",f.equipment.snapshot(actor)}};
        if (f.lifecycle.contains(actor)) state["lifecycle"] = f.lifecycle.at(actor);
        emit(e,"player_state",std::move(state));
    }
    void lifecycle(Flow &f, const Json &e, uint32_t actor, bool present, const std::string &reason) {
        if (!actor) return;
        if (!f.lifecycle.contains(actor) && f.lifecycle.size() >= 8192) throw ue::Error("lifecycle capacity");
        f.lifecycle[actor] = {{"present",present},{"reason",reason},{"timestampMs",e.at("data").at("timestampMs")}};
        state_event(f,e,actor);
    }
    void flush_state(Flow &f, const Json &e) {
        if (f.state_bindings_dirty) for (const auto &[id,actor] : f.actors) if (!actor.player_state) {
            auto ps = player_state_for(f,actor);
            if (ps) {
                const auto &state = f.actors.at(ps);
                f.health.life(id,state.live_status,state.life_stamp,"playerstate");
            }
        }
        f.state_bindings_dirty = false;
        for (auto actor : f.health.drain()) state_event(f,e,actor);
    }
    void position(Flow &f, Channel &channel, const Bunch &b, const Json &e, unsigned slot, const Json &item,
                  bool exported, bool frame_complete, bool block_complete, const std::string &stop) {
        auto pos = item.find("pos");
        const bool has_position = pos != item.end() && world(*pos);
        if ((pos != item.end() && !has_position) || (!has_position && !block_complete))
            return;
        auto &d = e.at("data");
        auto flow = d.at("local").get<std::string>() + ">" + d.at("remote").get<std::string>();
        auto key = std::pair{b.channel, slot};
        if (!has_position && !f.slots.contains(key)) return;
        if (!f.slots.contains(key) && f.slots.size() >= 8192)
            throw ue::Error("movement track capacity");
        Json name = nullptr, team = nullptr, fields = Json::object();
        uint32_t actor_id = 0, ps_id = 0;
        bool conflict = f.claim_conflicts.contains(int(slot)), exact = false;
        // 用 slot_index 索引查找，替代遍历整个 actors 表。
        // 旧实现每个移动包遍历最多 8192 个 actor，绝密模式每秒几十个移动包
        // => 每秒数百万次比较 => 单线程 CPU 吃满 => 数据积压 => 雷达位置滞后。
        if (f.slot_dup.contains(int(slot))) {
            conflict = true;   // 该 slot 出现过多个 actor，判为冲突
        } else {
            auto si = f.slot_index.find(int(slot));
            if (si != f.slot_index.end() && si->second) {
                auto ai = f.actors.find(si->second);
                if (ai != f.actors.end()) {
                    actor_id = ai->first;
                    ps_id = player_state_for(f, ai->second);
                }
            }
        }
        if (conflict) {
            actor_id = ps_id = 0;
        }
        if (!actor_id && f.slots.contains(key)) {
            auto previous = f.slots.at(key).actor;
            if (previous && f.lifecycle.contains(previous) && !f.lifecycle.at(previous).value("present",true)) return;
        }
        if (ps_id && f.actors.contains(ps_id)) {
            auto &ps = f.actors.at(ps_id);
            fields = ps.fields;
            exact = ps.exact;
            name = fields.value("PlayerNamePrivate", Json());
            team = fields.value("TeamID", Json());
        }
        if (emitted.size() >= 4000)
            throw ue::Error("player events per packet limit");
        auto &track = f.slots[key];
        if (!track.poses || track.generation != channel.generation || track.actor != actor_id ||
            track.ps != ps_id) {
            track.poses = 0;
            ++track.revision;
            track.generation = channel.generation;
            track.actor = actor_id;
            track.ps = ps_id;
            track.errors = channel.errors;
            track.packet_errors = f.packet_errors;
            track.motion = Json::object();
        }
        if (block_complete)
            extend_motion(track.motion, item, d.at("timestampMs").get<int64_t>());
        const auto track_id = "reset:" + std::to_string(reset_generation) +
            "/channel:" + std::to_string(b.channel) + "/generation:" + std::to_string(channel.generation) +
            "/slot:" + std::to_string(slot) + "/identity:" + std::to_string(track.revision);
        if (!has_position) {
            if (track.poses && item.contains("u160")) {
                ++motion_updates;
                emit(e, "player_motion", {{"flow", flow}, {"epoch", d.at("epoch")},
                    {"channel", b.channel}, {"slot", slot}, {"track", track_id},
                    {"timestampMs", d.at("timestampMs")}, {"sourcePacketOrdinal", d.value("packetOrdinal", uint64_t(0))},
                    {"motion", track.motion}});
            }
            return;
        }
        const auto &p = *pos;
        auto pose = ++track.poses;
        Json data = Json::object();
        data["flow"] = std::move(flow);
        data["epoch"] = d.at("epoch");
        data["track"] = "reset:" + std::to_string(reset_generation) +
                        "/channel:" + std::to_string(b.channel) +
                        "/generation:" + std::to_string(channel.generation) +
                        "/slot:" + std::to_string(slot) + "/identity:" + std::to_string(track.revision);
        data["slot"] = slot;
        data["channel"] = b.channel;
        data["poseSequence"] = pose;
        data["timestampMs"] = d.at("timestampMs");
        data["x"] = p[0];
        data["y"] = p[1];
        data["z"] = p[2];
        data["yaw"] = item.contains("rot") ? item["rot"][1] : Json(nullptr);
        data["name"] = std::move(name);
        data["teamId"] = std::move(team);
        data["camp"] = fields.value("Camp", Json());
        data["teamSource"] = ps_id && fields.contains("TeamID") ? "PlayerState.TeamID" : "unknown";
        data["kind"] = actor_id ? "player" : "unclassified_movement_slot";
        data["actorGuid"] = actor_id;
        data["playerStateGuid"] = ps_id;
        if (ps_id && f.actors.contains(ps_id) && !f.actors.at(ps_id).identity_key.empty())
            data["identityKey"] = f.actors.at(ps_id).identity_key;
        data["identitySource"] = conflict   ? "conflicting_explicit_claims"
                                 : actor_id ? "explicit_character_move_handle"
                                            : "unassociated_slot";
        data["identityFieldsExact"] = exact;
        data["playerStateSource"] = !actor_id || !ps_id        ? "unknown"
                                    : f.actors.at(actor_id).ps ? "Character.PlayerState"
                                                               : "shared_Controller_Owner";
        data["handshakeOk"] = f.welcome;
        data["decoderPacketErrors"] = f.packet_errors;
        data["decoderSemanticErrors"] = f.errors;
        data["decoderChannelErrors"] = channel.errors;
        data["packetErrorsSincePrevious"] = f.packet_errors - track.packet_errors;
        data["semanticErrorsSincePrevious"] = channel.errors - track.errors;
        data["movementFrameComplete"] = frame_complete;
        data["movementBlockComplete"] = block_complete;
        data["decodeCompleteness"] = block_complete ? "complete" : "known_prefix";
        data["decodeStopReason"] = stop;
        data["bankFingerprint"] = d.value("fingerprint", std::string());
        data["coordinateSource"] = "UE_MovementReplicationActor_repmove4";
        data["channelSource"] = exported ? "exported_archetype" : "structural_frame_h21";
        data["sourcePacketOrdinal"] = d.value("packetOrdinal", uint64_t(0));
        data["nativeUEPort"] = true;
        data["properties"] = std::move(fields);
        data["attributes"] = attributes(data["properties"]);
        data["vitals"] = vitals_for(f,actor_id);
        data["equipment"] = f.equipment.snapshot(actor_id);
        if (f.lifecycle.contains(actor_id)) data["lifecycle"] = f.lifecycle.at(actor_id);
        data["motion"] = block_complete ? track.motion : Json::object();
        track.errors = channel.errors;
        track.packet_errors = f.packet_errors;
        ++samples;
        synthetic |= e.value("synthetic", true);
        emit(e, "player_sample", std::move(data));
    }
    void server_self(Flow &f, Channel &channel, const Bunch &b, const Json &event, const Json &frame) {
        if (!frame.contains("embedded") || !frame["embedded"].contains("pos") ||
            !world(frame["embedded"]["pos"])) return;
        if (f.owning_actor && f.lifecycle.contains(f.owning_actor) && !f.lifecycle.at(f.owning_actor).value("present",true)) return;
        const auto &d = event.at("data"), &item = frame.at("embedded");
        uint32_t actor_id = f.owning_actor, ps_id = 0;
        Json fields = Json::object(), slot = nullptr;
        auto own = f.channels.find(3);
        if (own != f.channels.end() && own->second.profile == "character") {
            actor_id = own->second.actor;
        }
        {
            auto a = f.actors.find(actor_id);
            if (a != f.actors.end()) {
                if (a->second.slot >= 0) slot = a->second.slot;
                ps_id = player_state_for(f, a->second);
                if (ps_id) fields = f.actors.at(ps_id).fields;
            }
        }
        Json motion = Json::object();
        extend_motion(motion, item, d.at("timestampMs").get<int64_t>());
        ++server_self_samples;
        emit(event, "self_server_sample", {
            {"flow", d.at("local").get<std::string>() + ">" + d.at("remote").get<std::string>()},
            {"epoch", d.at("epoch")}, {"channel", b.channel}, {"slot", slot}, {"kind", "self_server"},
            {"track", "reset:" + std::to_string(reset_generation) + "/server-self/channel:" +
                std::to_string(b.channel) + "/generation:" + std::to_string(channel.generation) +
                "/actor:" + std::to_string(actor_id)},
            {"poseSequence", ++f.server_self_poses}, {"timestampMs", d.at("timestampMs")},
            {"x", item["pos"][0]}, {"y", item["pos"][1]}, {"z", item["pos"][2]},
            {"yaw", item.contains("rot") ? item["rot"][1] : Json()}, {"motion", motion},
            {"attributes", attributes(fields)}, {"vitals",vitals_for(f,actor_id)}, {"equipment",f.equipment.snapshot(actor_id)}, {"name", fields.value("PlayerNamePrivate", Json())},
            {"lifecycle",f.lifecycle.contains(actor_id) ? f.lifecycle.at(actor_id) : Json::object()},
            {"teamId", fields.value("TeamID", Json())}, {"camp", fields.value("Camp", Json())},
            {"actorGuid", actor_id}, {"playerStateGuid", ps_id}, {"handshakeOk", f.welcome},
            {"semanticErrorsSincePrevious", channel.errors - channel.server_self_errors},
            {"packetErrorsSincePrevious", f.packet_errors - f.server_packet_errors},
            {"identitySource", "embedded_self_candidate"}, {"decodeCompleteness", "complete"},
            {"coordinateSource", "UE_h21_embedded_downstream"},
            {"sourcePacketOrdinal", d.value("packetOrdinal", uint64_t(0))}});
        channel.server_self_errors = channel.errors;
        f.server_packet_errors = f.packet_errors;
    }
    void logical_body(Flow &f, Channel &channel, const Bunch &b, const Json &event) {
        auto &d = event.at("data");
        if (d.at("direction") == "up") {
            if (b.channel != 3)
                return;
            ue::Bits bits(b.payload, b.bits);
            auto pose = ue::self_movement(bits);
            if (pose.is_null())
                return;
            if (f.owning_actor && f.lifecycle.contains(f.owning_actor) && !f.lifecycle.at(f.owning_actor).value("present",true)) return;
            uint32_t actor_id = f.owning_actor, ps_id = 0;
            Json fields = Json::object(), slot = nullptr;
            auto own_channel = f.channels.find(3);
            if (own_channel != f.channels.end() && own_channel->second.profile == "character") {
                actor_id = own_channel->second.actor;
            }
            {
                auto actor = f.actors.find(actor_id);
                if (actor != f.actors.end()) {
                    if (actor->second.slot >= 0)
                        slot = actor->second.slot;
                    ps_id = player_state_for(f, actor->second);
                    if (ps_id)
                        fields = f.actors.at(ps_id).fields;
                }
            }
            Json sample = {
                {"flow", d.at("local").get<std::string>() + ">" + d.at("remote").get<std::string>()},
                {"epoch", d.at("epoch")},
                {"channel", 3},
                {"slot", slot},
                {"kind", "self"},
                {"track", "reset:" + std::to_string(reset_generation) + "/self/channel:3"},
                {"poseSequence", ++f.self_poses},
                {"timestampMs", d.at("timestampMs")},
                {"x", pose["loc"][0]},
                {"y", pose["loc"][1]},
                {"z", pose["loc"][2]},
                {"yaw", pose["yaw"]},
                {"pitch", pose["pitch"]},
                {"gameTime", pose["ts"]},
                {"velocity", pose["accel"]},
                {"acceleration", pose["vel"]},
                {"vectorSchema", "FGPMoveParams_v2"},
                {"motion", {{"velocity", pose["accel"]}, {"inputAcceleration", pose["vel"]},
                    {"timeSeconds", pose["ts"]}, {"timeAtMs", d.at("timestampMs")},
                    {"velocityAtMs", d.at("timestampMs")}, {"viewYaw", pose["yaw"]},
                    {"viewPitch", pose["pitch"]}, {"viewAtMs", d.at("timestampMs")},
                    {"updatedAtMs", d.at("timestampMs")}, {"source", "upstream_FGPMove"}}},
                {"attributes", attributes(fields)},
                {"vitals", vitals_for(f,actor_id)},{"equipment",f.equipment.snapshot(actor_id)},
                {"lifecycle",f.lifecycle.contains(actor_id) ? f.lifecycle.at(actor_id) : Json::object()},
                {"tail", pose["tail"]},
                {"name", fields.value("PlayerNamePrivate", Json())},
                {"teamId", fields.value("TeamID", Json())},
                {"camp", fields.value("Camp", Json())},
                {"actorGuid", actor_id},
                {"playerStateGuid", ps_id},
                {"identitySource", actor_id ? "owning_character_channel" : "upstream_channel_3"},
                {"teamSource", ps_id && fields.contains("TeamID") ? "PlayerState.TeamID" : "unknown"},
                {"handshakeOk", f.welcome},
                {"packetErrorsSincePrevious", f.up_packet_errors - f.self_packet_errors},
                {"semanticErrorsSincePrevious", channel.errors - f.self_errors},
                {"coordinateSource", "UE_Channel3_FGPMove_upstream"},
                {"decodeCompleteness", "complete"},
                {"sourcePacketOrdinal", d.value("packetOrdinal", uint64_t(0))},
                {"nativeUEPort", true}};
            f.self_errors = channel.errors;
            f.self_packet_errors = f.up_packet_errors;
            // A packet may carry old/new move variants together. Publish its final pose, as LiveView does.
            for (auto it = emitted.rbegin(); it != emitted.rend(); ++it)
                if (it->at("type") == "self_sample" &&
                    it->at("data")["sourcePacketOrdinal"] == sample["sourcePacketOrdinal"] &&
                    it->at("data")["timestampMs"] == sample["timestampMs"]) {
                    sample["packetErrorsSincePrevious"] =
                        sample["packetErrorsSincePrevious"].get<size_t>() +
                        it->at("data")["packetErrorsSincePrevious"].get<size_t>();
                    sample["semanticErrorsSincePrevious"] =
                        sample["semanticErrorsSincePrevious"].get<size_t>() +
                        it->at("data")["semanticErrorsSincePrevious"].get<size_t>();
                    (*it)["data"] = std::move(sample);
                    return;
                }
            ++self_samples;
            synthetic |= event.value("synthetic", true);
            emit(event, "self_sample", std::move(sample));
            return;
        }
        ue::Bits r(b.payload, b.bits);
        bool hx = false, hm = false;
        for (auto &frag : b.fragments) {
            hx |= frag.exports;
            hm |= frag.must;
        }
        // A channel opened and closed in one bunch can carry only its actor GUID (destroy notice).
        // It has no NewActor archetype or transforms. Accept only the exact GUID-only shape.
        if (b.channel != 0 && b.open && b.close && !hx && !hm) {
            auto probe = r;
            Json unused = Json::array();
            auto actor = ue::object(probe, false, unused);
            if (actor && probe.left() == 0) {
                if(f.loot.close(actor,b.close_reason,d.at("timestampMs"),f.logical_order))loot_event(f,event,actor);
                channel.actor = actor;
                channel.profile = "character";
                return;
            }
        }
        if (b.channel == 0) {
            auto map = welcome_map(b);
            if (!map.empty() && !f.welcome) {
                f.welcome = true;
                emit(event, "battle_welcome",
                     {{"flow", d.at("local").get<std::string>() + ">" + d.at("remote").get<std::string>()},
                      {"epoch", d.at("epoch")},
                      {"map", map}});
            }
            return;
        }
        if (b.open) {
            if (channel.actor) {
                if (channel.profile == "character") lifecycle(f,event,channel.actor,false,"channel_reused");
                f.actors.erase(channel.actor);
            }
            channel.actor = channel.arch = 0;
            channel.profile.clear();
            channel.movement_confirmed = false;
            ++channel.generation;
            Json parsed = ue::prefix(r, hx, hm, false, b.fragments, true);
            update_exports(f, parsed.at("exports"));
            auto arch = parsed.value("arch", uint32_t(0));
            auto path = path_for(f, arch);
            auto profile = profile_for(path);
            channel.actor = parsed.at("actor");
            channel.arch = arch;
            channel.profile = profile.empty() && !path.empty() ? "other" : profile;
            // Other actor classes have their own NewActor transform conventions.
            // An unresolved class can be identified by later PackageMap exports.
            if (profile.empty())
                return;
            r.pos = 0;
            parsed = ue::prefix(r, hx, hm, profile == "character" || profile == "pickup" || profile == "death_box" || profile == "container", b.fragments);
            update_exports(f, parsed.at("exports"));
            channel.actor = parsed.at("actor");
            channel.arch = arch;
            channel.profile = profile;
            if(profile=="pickup"||profile=="death_box"||profile=="container"){
                f.loot.open(channel.actor,parsed.value("loc",Json()),d.at("timestampMs"),f.logical_order,profile);
                loot_event(f,event,channel.actor);
            }
            // Character NewActor carries an extra byte before its first content block (original ss+8).
            if (profile == "character")
                r.skip(8);
            if (profile == "character") {
                if (b.channel == 3) f.owning_actor = channel.actor;
                lifecycle(f,event,channel.actor,true,"channel_open");
            }
        } else {
            Json exp = Json::array();
            if (hx)
                for (auto &fragment : b.fragments)
                    if (fragment.exports) {
                        r.pos = fragment.start;
                        ue::exports(r, exp);
                    }
            update_exports(f, exp);
            if (hm) {
                for (auto &fragment : b.fragments)
                    if (fragment.must) {
                        r.pos = fragment.start;
                        break;
                    }
                auto n = r.read(16);
                if (n > 256)
                    throw ue::Error("must-map count limit");
                for (unsigned i = 0; i < n; i++)
                    r.packed();
            }
            if (channel.profile.empty()) {
                auto path = path_for(f, channel.arch);
                channel.profile = profile_for(path);
                if(channel.profile=="pickup"||channel.profile=="death_box"||channel.profile=="container")
                    f.loot.open(channel.actor,Json(),d.at("timestampMs"),f.logical_order,channel.profile);
                if (channel.profile.empty() && !path.empty())
                    channel.profile = "other";
            }
        }
        if (!r.left() || channel.profile == "other" || channel.profile == "container")
            return;
        std::vector<ue::Block> all;
        std::string block_tail_error;
        try {
            all = ue::blocks(r, channel.profile == "character" ? &block_tail_error : nullptr);
        } catch (const ue::Error &) {
            if (channel.profile.empty())
                return;
            throw;
        }
        if (!block_tail_error.empty())
            diagnostic(f, event, "Character content tail: " + block_tail_error, b.channel, true);
        for (auto &block : all) {
            if (!block.rep || block.deleted)
                continue;
            ue::Bits data(b.payload, block.end, block.start);
            if(channel.profile=="pickup"||channel.profile=="death_box"){
                if(block.actor&&f.loot.apply(channel.actor,data,d.at("timestampMs"),f.logical_order))loot_event(f,event,channel.actor);
                continue;
            }
            if (!block.actor) {
                auto component=f.paths.find(block.sub);
                if(channel.profile=="character"&&component!=f.paths.end()&&component->second.value("outer",uint32_t(0))==channel.actor){
                    auto profile=EquipmentTracker::profile(path_for(f,block.cls));
                    if(profile.empty())profile=EquipmentTracker::profile(path_for(f,block.sub));
                    if(!profile.empty()){
                        try{if(f.equipment.apply(channel.actor,block.sub,profile,data,{d.at("timestampMs").get<int64_t>(),f.logical_order}))state_event(f,event,channel.actor);}
                        catch(const ue::Error &){/* Unknown/truncated equipment cannot alter movement or commit partial state. */}
                        continue;
                    }
                }
                if (f.health.component(block.sub,channel.actor,channel.profile == "health")) {
                    try { f.health.apply(block.sub,ue::health(data),{d.at("timestampMs").get<int64_t>(),f.logical_order}); }
                    catch (const ue::Error &x) { diagnostic(f,event,std::string("health: ") + x.what(),b.channel,true); }
                }
                continue;
            }
            if (channel.profile == "current" || channel.profile == "legacy" ||
                channel.profile == "character") {
                auto props = ue::properties(data, channel.profile);
                auto status = props.at("status").get<std::string>();
                // 即使状态是 error/unsupported，只要解析出了字段就降级为 prefix_only。
                // 对齐 Python netrader_live.decode_character_identity：Opaque 时保留前缀，
                // 否则 h60 MoveHandle 一旦被后续未知属性打断，整条身份链就断了。
                const auto &parsed = props["fields"];
                if (status == "error" && parsed.is_object() && !parsed.empty())
                    status = "prefix_only";
                if (status != "exact" && status != "prefix_only")
                    diagnostic(f, event,
                               "properties " + status + ": " +
                                   props.value("error", std::string("unconsumed bits")),
                               b.channel);
                if (status == "error")
                    continue;
                if (!channel.actor)
                    continue;
                if (!f.actors.contains(channel.actor) && f.actors.size() >= 8192)
                    throw ue::Error("actor cache capacity");
                auto &actor = f.actors[channel.actor];
                f.state_bindings_dirty = true;
                const auto &fields = props.at("fields");
                if (channel.profile == "character") {
                    if (fields.contains("Owner"))
                        actor.owner = guid(fields.at("Owner"));
                    if (fields.contains("PlayerState"))
                        actor.ps = guid(fields.at("PlayerState"));
                    if (fields.contains("MoveHandle")) {
                        auto slot = fields.at("MoveHandle").get<int>();
                        auto stamp = std::pair{d.at("timestampMs").get<int64_t>(),
                                               d.value("packetOrdinal", uint64_t(0))};
                        if (slot < 0)
                            actor.slot = -1;
                        else if (slot < 512 &&
                                 (!f.claim_time.contains(slot) || stamp > f.claim_time.at(slot))) {
                            // 用 slot_index 找占用该 slot 的旧 actor，替代遍历整个 actors 表。
                            // 旧实现每个 MoveHandle 更新都遍历最多 8192 个 actor，
                            // 玩家一直移动时每秒触发几十次 => CPU 吃满 => 雷达滞后。
                            auto si = f.slot_index.find(slot);
                            if (si != f.slot_index.end() && si->second && si->second != channel.actor) {
                                auto oi = f.actors.find(si->second);
                                if (oi != f.actors.end()) {
                                    lifecycle(f,event,oi->first,false,"slot_reused");
                                    oi->second.slot = -1;
                                }
                            }
                            actor.slot = slot;
                            f.slot_index[slot] = channel.actor;
                            f.slot_dup.erase(slot);
                            f.claim_time[slot] = stamp;
                            f.claim_conflicts.erase(slot);
                        } else if (slot < 512 && f.claim_time.contains(slot) &&
                                   stamp == f.claim_time.at(slot) && actor.slot != slot) {
                            auto si = f.slot_index.find(slot);
                            if (si != f.slot_index.end() && si->second) {
                                auto oi = f.actors.find(si->second);
                                if (oi != f.actors.end()) oi->second.slot = -1;
                            }
                            f.slot_index[slot] = 0;
                            f.slot_dup.insert(slot);
                            f.claim_conflicts.insert(slot);
                        }
                    }
                } else {
                    actor.player_state = true;
                    if (fields.contains("Owner"))
                        actor.owner = guid(fields.at("Owner"));
                    static const std::set<std::string> identity = {
                        "PlayerNamePrivate", "PlayerId",    "Uin", "TeamID", "Camp", "Owner",
                        "bIsABot",           "PlayerGender"};
                    for (auto &[k, v] : fields.items())
                        if (status == "exact" || identity.contains(k))
                            actor.fields[k] = v;
                    actor.exact = status == "exact";
                    if (fields.contains("Uin")) {
                        actor.identity_key.clear();
                        const auto &uin = fields.at("Uin");
                        const bool valid_uin = uin.is_number_unsigned() ? uin.get<uint64_t>() != 0 :
                            uin.is_number_integer() && uin.get<int64_t>() > 0;
                        if (status == "exact" && valid_uin) {
                            // Cache an opaque, connection-scoped key; do not expose a 64-bit
                            // account number to JavaScript (which would also lose precision).
                            const auto identity = Json::array({event.value("session", std::string()), d.at("local"),
                                d.at("remote"), d.at("epoch"), uin}).dump();
                            actor.identity_key = fingerprint(View(reinterpret_cast<const uint8_t *>(identity.data()), identity.size()));
                        }
                    }
                    if (status == "exact" && fields.contains("CurrentCharacterLiveStatus")) {
                        actor.live_status = fields.at("CurrentCharacterLiveStatus");
                        actor.life_stamp = {d.at("timestampMs").get<int64_t>(),f.logical_order};
                    }
                    ++identities;
                    emit(event, "player_identity",
                         {{"actorGuid", channel.actor},
                          {"channel", b.channel},
                          {"profile", channel.profile},
                          {"status", status},
                          {"fields", actor.fields}});
                }
                continue;
            }
            if (!channel.profile.empty() && channel.profile != "movement")
                continue;
            auto probe = data;
            if (probe.bit())
                continue;
            auto handle = probe.packed();
            if (handle != 21)
                continue;
            // Strict bounded frame parsing also permits a capture that starts after channel open.
            // It never claims a named player without an explicit Character -> PlayerState link.
            auto frame = ue::movement(probe);
            bool frame_complete = frame.at("complete");
            std::string stop;
            if (!frame_complete)
                stop = "unsupported movement native object mask 0x10";
            else if (probe.packed() != 0)
                stop = "unsupported property after movement h21";
            else if (probe.left())
                // 原实现 throw，会把整条通道打死 => Character 通道建不起来 =>
                // actor.slot 永远 -1 => identitySource=unassociated_slot => 队伍全空。
                // 与上面 0x10 分支保持一致：只记诊断，保留已解析坐标。
                stop = "movement property trailing bits";
            bool block_complete = stop.empty();
            // A partial h21 match cannot establish an unknown channel's schema.
            // 注意：0x10 对象（新版本新增属性）长度未知，解析器会停在原地。
            // 此时仍保留已解析出的坐标（position() 已按 frame_complete 处理），
            // 只记录诊断，不再抛异常中断整条通道 —— 否则 MoveHandle/身份全部丢失。
            if (!block_complete && channel.profile != "movement" && !channel.movement_confirmed)
                diagnostic(f, event, stop + " on unconfirmed movement channel", b.channel, true);
            if (block_complete)
                channel.movement_confirmed = true;
            // 0x10 is an intentional stopping boundary, as in the original reader.
            // Keep coverage on samples without emitting a warning for every such frame.
            else if (frame_complete)
                diagnostic(f, event, stop, b.channel, true);
            for (auto &[s, item] : frame["items"].items())
                position(f, channel, b, event, unsigned(std::stoul(s)), item, channel.profile == "movement",
                         frame_complete, block_complete, stop);
            if (block_complete)
                server_self(f, channel, b, event, frame);
        }
    }
    void logical(Flow &f, Channel &c, Bunch b, const Json &e) {
        ++f.logical_order;
        auto finish = [&] {
            flush_state(f,e);
            if (b.close && e.at("data").at("direction") == "down") {
                const bool removed = b.close_reason == 0 || b.close_reason == 2 || b.close_reason == 4;
                if(f.loot.close(c.actor,b.close_reason,e.at("data").at("timestampMs"),f.logical_order))loot_event(f,e,c.actor);
                if (c.actor && c.profile == "character")
                    lifecycle(f,e,c.actor,!removed,b.close_reason == 1 ? "dormancy" : b.close_reason == 3 ? "out_of_relevancy" : "channel_close_" + std::to_string(b.close_reason));
                if (removed) {
                    // 删 actor 时必须同步清掉 slot_index，否则索引会指向已删除的 actor。
                    auto ai = f.actors.find(c.actor);
                    if (ai != f.actors.end() && ai->second.slot >= 0) {
                        auto si = f.slot_index.find(ai->second.slot);
                        if (si != f.slot_index.end() && si->second == c.actor)
                            si->second = 0;
                    }
                    f.actors.erase(c.actor);
                }
                c.actor = c.arch = 0; c.profile.clear(); c.movement_confirmed = false; ++c.generation;
            }
        };
        try { logical_body(f,c,b,e); } catch (...) { finish(); throw; }
        finish();
    }
    // partial 缓冲的字节数也要计入 buffered_bytes，否则「队列 + 分片」总量会低估。
    void drop_partial(Flow &f, Channel &c) {
        if (c.partial)
            f.buffered_bytes -= std::min(f.buffered_bytes, c.partial->payload.size());
        c.partial.reset();
    }
    void drop_queue(Flow &f, Channel &c) {
        for (auto &[seq, q] : c.queue)
            f.buffered_bytes -= std::min(f.buffered_bytes, q.bunch.payload.size());
        c.queue.clear();
    }
    void fragment(Flow &f, Channel &c, Bunch b, const Json &e, int64_t sequence) {
        if (!b.partial) {
            if (c.partial) {
                diagnostic(f, e, "complete bunch interrupted partial chain", b.channel);
                drop_partial(f, c);
            }
            logical(f, c, std::move(b), e);
            return;
        }
        if (b.initial) {
            if (c.partial)
                diagnostic(f, e, "new initial fragment replaced partial chain", b.channel);
            drop_partial(f, c);
            c.partial = b;
            f.buffered_bytes += c.partial->payload.size();
            c.partial_sequence = sequence;
        } else {
            if (!c.partial) {
                diagnostic(f, e, "orphan partial bunch", b.channel);
                return;
            }
            if (c.partial->reliable != b.reliable || (b.reliable && sequence != c.partial_sequence + 1) ||
                (!b.reliable && sequence != c.partial_sequence && sequence != c.partial_sequence + 1)) {
                drop_partial(f, c);
                diagnostic(f, e, "partial sequence/reliability gap", b.channel);
                return;
            }
            auto &p = *c.partial;
            auto start = p.bits;
            const size_t before = p.payload.size();
            ue::append(p.payload, p.bits, b.payload, b.bits);
            f.buffered_bytes += p.payload.size() - before;
            p.fragments.push_back({start, b.exports, b.must});
            if (p.fragments.size() > 1024)
                throw ue::Error("partial fragment count limit");
            p.close = b.close;
            p.close_reason = b.close_reason;
            c.partial_sequence = sequence;
        }
        if (b.final) {
            auto whole = std::move(*c.partial);
            drop_partial(f, c);
            logical(f, c, std::move(whole), e);
        }
    }
    void consume(const Json &e) {
        const auto type = e.value("type", std::string());
        if (type == "session_started" || type == "capture_gap") {
            flows.clear();
            epochs.clear();
            retired.clear();
            ++reset_generation;
            return;
        }
        if (type == "udp_binding_reset") {
            auto flow = e.at("data").at("flow").get<std::string>();
            ++reset_generation;
            auto pair = std::pair{e.value("session", std::string()), flow};
            retired[pair] = std::max(epochs[pair], e.at("data").value("epoch", uint64_t(0)));
            std::erase_if(flows, [&](auto &entry) {
                return std::get<0>(entry.first) == pair.first && std::get<1>(entry.first) == flow;
            });
            return;
        }
        if (type != "udp_plaintext")
            return;
        auto &d = e.at("data");
        std::string flow = d.at("local").get<std::string>() + ">" + d.at("remote").get<std::string>();
        auto session = e.value("session", std::string());
        auto epoch = d.at("epoch").get<uint64_t>();
        auto pair = std::pair{session, flow};
        if (epoch < epochs[pair] || (retired.contains(pair) && epoch <= retired[pair])) {
            ++duplicates;
            return;
        }
        epochs[pair] = epoch;
        std::erase_if(flows, [&](auto &entry) {
            return std::get<0>(entry.first) == session && std::get<1>(entry.first) == flow &&
                   std::get<2>(entry.first) != epoch;
        });
        auto key = std::tuple{session, flow, epoch};
        if (!flows.contains(key) && flows.size() >= 64)
            throw ue::Error("player decoder flow limit");
        auto &f = flows[key];
        ++packets;
        try {
            if (!d.value("structuralValid", false))
                throw ue::Error("plaintext missing structural validation");
            auto direction = d.at("direction").get<std::string>();
            if (direction != "up" && direction != "down")
                throw ue::Error("invalid plaintext direction");
            auto encoded = d.at("payloadBase64").get<std::string>();
            if (encoded.size() > 90000)
                throw ue::Error("oversize plaintext");
            auto bytes = unbase64(encoded);
            if (bytes.size() != d.at("payloadLength").get<size_t>())
                throw ue::Error("plaintext length mismatch");
            auto p = ue::packet(bytes, direction == "up");
            if (p.control)
                return;
            auto now = d.at("timestampMs").get<int64_t>();
            if (now <= 0)
                throw ue::Error("invalid packet timestamp");
            auto high = f.packet_high.find(direction);
            auto packet_seq =
                high == f.packet_high.end() ? int64_t(p.sequence) : unwrap(p.sequence, high->second, 16384);
            auto packet_key = std::pair{direction, packet_seq};
            if (f.seen.contains(packet_key)) {
                ++duplicates;
                return;
            }
            f.seen.insert(packet_key);
            f.seen_order.push_back(packet_key);
            if (f.seen_order.size() > 1024) {
                f.seen.erase(f.seen_order.front());
                f.seen_order.pop_front();
            }
            if (high == f.packet_high.end() || packet_seq > high->second)
                f.packet_high[direction] = packet_seq;
            for (auto &b : p.bunches) {
                // Keep independent upstream channels; only downstream updates produce remote samples.
                uint32_t id = b.channel + (direction == "up" ? 32768 : 0);
                if (!f.channels.contains(id) && f.channels.size() >= 2048)
                    throw ue::Error("channel capacity");
                auto &c = f.channels[id];
                // 通道 2 秒没数据就清空队列 -> 判为过期。
                // 开局/换图时包量大、处理慢，2 秒太紧，会把还在排队的新数据一起清掉。
                // 放宽到 5 秒：正常对局每个通道每秒都有包，5 秒足够区分「真的断了」。
                if (c.last_time && now - c.last_time > 5000 && (c.partial || !c.queue.empty())) {
                    drop_partial(f, c);
                    drop_queue(f, c);
                    c.delivered.reset();
                    diagnostic(f, e, "incomplete channel data expired", b.channel);
                }
                c.last_time = std::max(c.last_time, now);
                if (!b.reliable) {
                    if (packet_seq <= c.last_unreliable) {
                        ++duplicates;
                        continue;
                    }
                    // Multiple bunches in the same packet are handled in wire order.
                    try {
                        fragment(f, c, std::move(b), e, packet_seq);
                    } catch (const std::exception &x) {
                        c.partial.reset();
                        diagnostic(f, e, x.what(), id % 32768);
                    }
                    continue;
                }
                auto seq = c.delivered ? unwrap(b.sequence, *c.delivered, 1024) : int64_t(b.sequence);
                if (c.delivered && seq <= *c.delivered) {
                    ++duplicates;
                    continue;
                }
                if (!c.delivered)
                    c.delivered = seq - 1;
                if (seq > *c.delivered + 1) {
                    // 回滚到原始行为：队列 64，满了抛异常（由外层 catch 处理）。
                    // 之前改成 512 导致延迟爆炸（队列越大等得越久），16 也未必对。
                    // 先恢复原状，再单独定位真正的延迟原因。
                    if (c.queue.size() >= 64 || f.buffered() + b.payload.size() > 8 * 1024 * 1024)
                        throw ue::Error("reliable reorder queue capacity@" + std::to_string(id));
                    f.buffered_bytes += b.payload.size();
                    c.queue.try_emplace(seq, Queued{std::move(b), e, seq});
                    continue;
                }
                try {
                    fragment(f, c, std::move(b), e, seq);
                } catch (const std::exception &x) {
                    c.partial.reset();
                    diagnostic(f, e, x.what(), id % 32768);
                }
                c.delivered = seq;
                while (c.queue.contains(*c.delivered + 1)) {
                    auto it = c.queue.find(*c.delivered + 1);
                    auto q = std::move(it->second);
                    c.queue.erase(it);
                    f.buffered_bytes -= std::min(f.buffered_bytes, q.bunch.payload.size());
                    try {
                        fragment(f, c, std::move(q.bunch), q.event, q.sequence);
                    } catch (const std::exception &x) {
                        c.partial.reset();
                        diagnostic(f, q.event, x.what(), id % 32768);
                    }
                    c.delivered = q.sequence;
                }
            }
            for (auto &b : p.bunches)
                if (!b.reliable) {
                    auto &c = f.channels[b.channel + (direction == "up" ? 32768 : 0)];
                    c.last_unreliable = std::max(c.last_unreliable, packet_seq);
                }
            if (f.buffered() > 8 * 1024 * 1024)
                throw ue::Error("flow buffer capacity");
        } catch (const std::exception &x) {
            diagnostic(f, e, x.what());
            // 只清出问题的通道，不能清全部。
            // 旧实现无条件清空所有通道的 queue/partial：绝密模式包量大时
            // 某个通道队列一满（64 个）就抛异常，导致全流所有玩家通道被连带
            // 清空 => 大量 movement context missing / truncated bitstream
            // => 玩家位置长时间不更新 => 雷达上「人还在原地」。
            //
            // 通道号从异常消息里解析（"reliable reorder queue capacity@<id>"），
            // 拿不到才退化为全清。用消息传号是为了不改所有 throw 点的签名。
            int bad = -1;
            {
                const std::string msg = x.what();
                auto at = msg.rfind('@');
                if (at != std::string::npos && at + 1 < msg.size()) {
                    try { bad = std::stoi(msg.substr(at + 1)); } catch (...) { bad = -1; }
                }
            }
            if (bad >= 0) {
                auto it = f.channels.find(uint32_t(bad));
                if (it != f.channels.end()) {
                    drop_partial(f, it->second);
                    drop_queue(f, it->second);
                }
            } else {
                for (auto &[id, c] : f.channels) {
                    drop_partial(f, c);
                    drop_queue(f, c);
                }
            }
        }
    }
    Json summary() const {
        uint64_t loot_exact=0,loot_partial=0,loot_rejected=0;
        for(const auto &[key,f]:flows){loot_exact+=f.loot.exact;loot_partial+=f.loot.partial;loot_rejected+=f.loot.rejected;}
        return {{"decoder", "native C++ UE player decoder"},
                {"lootExactBlocks",loot_exact},{"lootPartialBlocks",loot_partial},{"lootRejectedBlocks",loot_rejected},
                {"nativeUEPort", true},
                {"inputPackets", packets},
                {"samples", samples},
                {"selfSamples", self_samples},
                {"serverSelfSamples", server_self_samples},
                {"motionUpdates", motion_updates},
                {"identityUpdates", identities},
                {"errors", errors},
                {"warnings", warnings},
                {"duplicatePacketsOrBunches", duplicates},
                {"containsSyntheticSamples", synthetic},
                {"activeFlows", flows.size()},
                {"status", samples || self_samples ? "samples_emitted" : "no_player_samples"}};
    }
};
PlayerDecoder::PlayerDecoder(Sink sink) : impl_(std::make_unique<Impl>(std::move(sink))) {}
PlayerDecoder::~PlayerDecoder() = default;
void PlayerDecoder::consume(const Json &e) {
    impl_->emitted.clear();
    impl_->consume(e);
    auto emitted = std::move(impl_->emitted);
    for (auto &event : emitted)
        if (impl_->sink)
            impl_->sink(std::move(event));
}
Json PlayerDecoder::summary() const {
    return impl_->summary();
}
Json decode_player_file(const std::string &input, const std::string &output) {
    namespace fs = std::filesystem;
    std::ifstream in(input);
    if (!in)
        throw std::runtime_error("cannot open plaintext input");
    if (fs::exists(output))
        throw std::runtime_error("decoded output directory already exists");
    fs::create_directories(output);
    std::ofstream players(fs::path(output) / "players.jsonl"),
        errors(fs::path(output) / "decoder-errors.jsonl"), identities(fs::path(output) / "identities.jsonl"),
        warnings(fs::path(output) / "decoder-warnings.jsonl"),
        quality(fs::path(output) / "player-quality.jsonl"), battles(fs::path(output) / "battle-events.jsonl");
    std::ofstream self(fs::path(output) / "self.jsonl"),
        self_quality(fs::path(output) / "self-quality.jsonl"),
        server_self(fs::path(output) / "self-server.jsonl"),
        server_quality(fs::path(output) / "self-server-quality.jsonl"),
        motion(fs::path(output) / "motion-updates.jsonl"), states(fs::path(output) / "player-state.jsonl"), loot(fs::path(output) / "loot.jsonl");
    if(!loot)throw std::runtime_error("cannot create loot output");
    if (!server_self || !server_quality || !motion) throw std::runtime_error("cannot create extended player output");
    if (!players || !errors || !warnings || !identities || !quality || !battles || !self || !self_quality)
        throw std::runtime_error("cannot create player output");
    int64_t replay_clock = 0;
    BattleTracker battle(
        [&](Json e) {
            battles << e.dump() << '\n';
            if (!battles)
                throw std::runtime_error("battle output write failed");
        },
        [&] { return replay_clock; });
    PlayerValidator validator;
    PlayerDecoder decoder([&](Json e) {
        auto kind = e.at("type").get<std::string>();
        if (kind == "player_sample" || kind == "self_sample" || kind == "self_server_sample") {
            (kind == "self_server_sample" ? server_self : kind == "self_sample" ? self : players) << e.dump() << '\n';
            auto validation = validator.inspect(e);
            for (auto key : {"session", "device", "synthetic", "timestampMs"})
                if (e.contains(key))
                    validation[key] = e[key];
            (kind == "self_server_sample" ? server_quality : kind == "self_sample" ? self_quality : quality) << validation.dump() << '\n';
            battle.consume(validation);
        } else if (kind == "loot_state") {
            loot << e.dump() << '\n';if(!loot)throw std::runtime_error("loot output write failed");
            battle.consume(e);
        } else if (kind == "player_state") {
            states << e.dump() << '\n';
            if (!states) throw std::runtime_error("player state output write failed");
            battle.consume(e);
        } else if (kind == "player_motion") {
            motion << e.dump() << '\n';
            battle.consume(e);
        } else if (kind == "battle_welcome") {
            battles << e.dump() << '\n';
            battle.consume(e);
        } else if (kind == "player_identity")
            identities << e.dump() << '\n';
        else if (kind == "player_decode_warning")
            warnings << e.dump() << '\n';
        else
            errors << e.dump() << '\n';
        if (!players || !errors || !warnings || !identities || !quality || !battles || !self || !self_quality ||
            !server_self || !server_quality || !motion)
            throw std::runtime_error("player output write failed");
    });
    std::string line;
    size_t number = 0, exceptions = 0;
    while (std::getline(in, line)) {
        ++number;
        if (number == 1 && line.starts_with("\xef\xbb\xbf"))
            line.erase(0, 3);
        if (line.empty())
            continue;
        try {
            if (line.size() > 2 * 1024 * 1024)
                throw ue::Error("JSONL record too large");
            auto event = Json::parse(line);
            replay_clock = std::max(replay_clock, event.value("timestampMs", int64_t(0)));
            battle.tick(replay_clock);
            // Recompute derived events from plaintext; do not trust old decoder results.
            const auto type = event.value("type", std::string());
            if (type != "battle_welcome" && type != "player_validation" && type != "self_validation" &&
                type != "self_server_validation" && type != "player_motion" && type != "player_state")
                battle.consume(event);
            decoder.consume(event);
        } catch (const std::exception &x) {
            ++exceptions;
            errors << Json({{"line", number}, {"error", x.what()}}).dump() << '\n';
        }
    }
    if (in.bad())
        throw std::runtime_error("plaintext input read failed");
    players.flush();
    errors.flush();
    warnings.flush();
    identities.flush();
    quality.flush();
    battles.flush();
    self.flush();
    self_quality.flush();
    server_self.flush(); server_quality.flush(); motion.flush(); states.flush();
    if (!states) throw std::runtime_error("player state output write failed");
    if (!server_self || !server_quality || !motion) throw std::runtime_error("extended player output write failed");
    if (!players || !errors || !warnings || !identities || !quality || !battles || !self || !self_quality)
        throw std::runtime_error("player output write failed");
    auto summary = decoder.summary();
    summary["exceptions"] = exceptions;
    std::ofstream report(fs::path(output) / "summary.json");
    report << summary.dump(2) << '\n';
    report.flush();
    if (!report)
        throw std::runtime_error("player summary write failed");
    return summary;
}
} // namespace radaer
