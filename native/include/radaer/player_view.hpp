#pragma once
#include "core.hpp"
namespace radaer {
inline bool same_player_identity(const Json &a, const Json &b) {
    if (!a.is_object() || !b.is_object() || a.value("flow", std::string()) != b.value("flow", std::string()) ||
        a.value("epoch", uint64_t(0)) != b.value("epoch", uint64_t(0)))
        return false;
    const auto ak = a.value("identityKey", std::string()), bk = b.value("identityKey", std::string());
    if (!ak.empty() && !bk.empty())
        return ak == bk;
    const auto aa = a.value("actorGuid", uint32_t(0)), ba = b.value("actorGuid", uint32_t(0));
    if (aa && ba && aa == ba)
        return true;
    const auto ap = a.value("playerStateGuid", uint32_t(0)), bp = b.value("playerStateGuid", uint32_t(0));
    return ap && bp && ap == bp;
}

// Replace a coordinate row while retiring a prior slot or a prior incarnation of
// the same explicitly identified player. A late packet from an older incarnation
// must not resurrect it over a newer row occupying that slot.
inline bool upsert_player_row(Json &rows, Json row) {
    const auto incoming_time = row.value("timestampMs", int64_t(0));
    bool rejected = false;
    for (const auto &old : rows) {
        if (old.value("flow", std::string()) != row.value("flow", std::string()) ||
            old.value("epoch", uint64_t(0)) != row.value("epoch", uint64_t(0)))
            continue;
        const bool same_slot = old.value("channel", 0U) == row.value("channel", 0U) &&
                               old.value("slot", 0U) == row.value("slot", 0U);
        const bool same_identity = same_player_identity(old, row);
        if ((same_slot || same_identity) && incoming_time < old.value("timestampMs", int64_t(0))) {
            rejected = true;
            break;
        }
    }
    if (rejected)
        return false;
    for (size_t i = rows.size(); i > 0; --i) {
        const auto &old = rows[i - 1];
        if (old.value("flow", std::string()) != row.value("flow", std::string()) ||
            old.value("epoch", uint64_t(0)) != row.value("epoch", uint64_t(0)))
            continue;
        const bool same_slot = old.value("channel", 0U) == row.value("channel", 0U) &&
                               old.value("slot", 0U) == row.value("slot", 0U);
        const bool same_identity = same_player_identity(old, row);
        if (same_slot || same_identity)
            rows.erase(i - 1);
    }
    rows.push_back(std::move(row));
    if (rows.size() > 128)
        rows.erase(0);
    return true;
}

inline void merge_player_state(Json &row, const Json &delta) {
    if (!row.is_object() || !delta.value("actorGuid",uint32_t(0)) ||
        row.value("actorGuid",uint32_t(0)) != delta.at("actorGuid") ||
        row.value("flow",std::string()) != delta.value("flow",std::string()) || row.value("epoch",uint64_t(0)) != delta.value("epoch",uint64_t(0))) return;
    auto newer = [](const Json &a, const Json &b) {
        return std::pair{a.value("timestampMs",int64_t(0)),a.value("sourceOrder",uint64_t(0))} >=
               std::pair{b.value("timestampMs",int64_t(0)),b.value("sourceOrder",uint64_t(0))};
    };
    if (delta.contains("vitals")) {
        const auto &v = delta.at("vitals");
        if (!row.contains("vitals")) row["vitals"] = Json::object();
        auto &dest = row["vitals"];
        if (v.contains("life") && !v["life"].empty() && newer(v["life"],dest.value("life",Json::object()))) dest["life"] = v["life"];
        if (v.contains("health")) for (const auto &[k,value] : v["health"].items()) {
            if (!dest.contains("health")) dest["health"] = Json::object();
            if (newer(value,dest["health"].value(k,Json::object()))) dest["health"][k] = value;
        }
    }
    if (delta.contains("lifecycle") && newer(delta["lifecycle"],row.value("lifecycle",Json::object()))) row["lifecycle"] = delta["lifecycle"];
    if (delta.contains("equipment") && newer(delta["equipment"],row.value("equipment",Json::object()))) row["equipment"] = delta["equipment"];
}
inline void apply_player_state(Json &rows, Json &self, Json &server_self, const Json &delta) {
    for (auto &row : rows) merge_player_state(row,delta);
    merge_player_state(self,delta); merge_player_state(server_self,delta);
}
// A view-only delta must not refresh coordinate timestamps, pose counters or rate metrics.
inline void apply_player_motion(Json &rows, const Json &delta) {
    for (auto &row : rows)
        if (row.value("flow", std::string()) == delta.at("flow").get<std::string>() && row.at("epoch") == delta.at("epoch") &&
            row.value("track", std::string()) == delta.at("track").get<std::string>() &&
            delta.at("timestampMs").get<int64_t>() >= row.value("timestampMs", int64_t(0)) &&
            delta.at("timestampMs").get<int64_t>() >= row.value("motion", Json::object()).value("updatedAtMs", int64_t(0))) {
            row["motion"] = delta.at("motion");
            return;
        }
}
} // namespace radaer
