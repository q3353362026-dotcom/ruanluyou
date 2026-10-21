#include "radaer/health.hpp"
#include <cmath>
namespace radaer::ue {
Json health(Bits &r) {
    static const char *names[] = {"Health", "ShieldHealth", "ArmorShieldHealth", "ArmorHealth", "HelmetArmorHealth", "IsAlive", "Object"};
    if (r.bit()) throw Error("checked health block unsupported");
    Json out = Json::array();
    unsigned previous = 0;
    for (;;) {
        auto h = r.packed();
        if (!h) { if (r.left()) throw Error("health block trailing bits"); return out; }
        if (h <= previous || h > 4) throw Error("health component handle order/range");
        previous = h;
        if (h <= 2) { r.bit(); continue; }
        bool dense = r.bit(); auto count = r.packed();
        if (count > 65535) throw Error("health container count limit");
        std::vector<unsigned> indices;
        if (dense) {
            if (count > r.left()) throw Error("truncated health presence set");
            for (unsigned i = 0; i < count; ++i) if (r.bit()) indices.push_back(i);
        } else for (;;) {
            auto index = unsigned(r.read(16));
            if (!index) break;
            if (index > count || (!indices.empty() && index - 1 <= indices.back()))
                throw Error("health presence index order/range");
            indices.push_back(index - 1);
        }
        Json items = Json::array();
        for (auto index : indices) {
            auto size = r.packed();
            if (size > r.left()) throw Error("truncated health item");
            Bits inner(r.data, r.pos + size, r.pos);
            Json values = Json::object(); unsigned last = 0;
            for (;;) {
                auto field = inner.packed();
                if (!field) break;
                if (field <= last || field > 7) throw Error("health item handle order/range");
                last = field;
                if (field == 7) values[names[field-1]] = inner.packed();
                else {
                    double v = inner.floating();
                    if (!std::isfinite(v)) throw Error("nonfinite health field");
                    values[names[field-1]] = v;
                }
            }
            if (inner.left()) throw Error("health item trailing bits");
            r.pos = inner.pos;
            items.push_back(Json::array({index, values}));
        }
        out.push_back({{"handle",h},{"count",count},{"items",items}});
    }
}
} // namespace radaer::ue
namespace radaer {
namespace {
std::string leaf(const Json &entry) {
    auto s = entry.value("path", std::string());
    auto p = s.find_last_of('.'); return p == std::string::npos ? s : s.substr(p+1);
}
}
void HealthTracker::exports(const Json &entries) {
    for (const auto &e : entries) if (e.value("path", Json()).is_string()) {
        auto id = e.at("guid").get<uint32_t>();
        if (!exports_.contains(id) && exports_.size() >= 32768) throw ue::Error("health export capacity");
        exports_[id] = e;
        if (objects_.contains(id)) resolve(id);
    }
}
bool HealthTracker::component(uint32_t sub, uint32_t actor, bool verified_class) const {
    auto it = exports_.find(sub), owner = exports_.find(actor);
    return it != exports_.end() &&
        (leaf(it->second) == "PropertyReplication" || leaf(it->second) == "PropertyReplicationComponent") &&
        it->second.value("outer", uint32_t(0)) == actor &&
        (verified_class || (owner != exports_.end() && owner->second.value("path", std::string()).find("BP_PropertyReplicationCharacterHealth_C") != std::string::npos));
}
void HealthTracker::apply(uint32_t component_id, const Json &deltas, Stamp stamp) {
    // Decode the entire block before calling apply; a truncated block commits nothing.
    for (const auto &delta : deltas) {
        auto key = std::pair{component_id,delta.at("handle").get<unsigned>()};
        if (!containers_.contains(key) && containers_.size() >= 1024) throw ue::Error("health container capacity");
        auto &c = containers_[key]; if (stamp < c.stamp) continue; c.stamp = stamp;
        auto count = delta.at("count").get<unsigned>();
        std::erase_if(c.items,[&](const auto &v){return v.first >= count;});
        for (const auto &entry : delta.at("items")) {
            auto index = entry[0].get<unsigned>(); const auto &values = entry[1];
            auto &item = c.items[index];
            if (!item.is_object() || (values.contains("Object") && values["Object"] != item.value("Object",Json()))) item = Json::object();
            item.update(values);
            auto obj = item.value("Object",uint32_t(0)); if (!obj) continue;
            if (!objects_.contains(obj) && objects_.size() >= 8192) throw ue::Error("health object capacity");
            for (const auto &[name,value] : values.items()) if (name != "Object") {
                auto &field = objects_[obj][name];
                if (stamp >= field.stamp) field = {value,stamp};
            }
            resolve(obj);
        }
    }
}
void HealthTracker::resolve(uint32_t object) {
    auto e = exports_.find(object); if (e == exports_.end()) return;
    auto name = leaf(e->second);
    if (name != "AttributeCenter" && name != "GPAttributeSetHealth") return;
    auto actor = e->second.value("outer",uint32_t(0)); if (!actor) return;
    if (!actors_.contains(actor) && actors_.size() >= 8192) throw ue::Error("health actor capacity");
    auto &state = actors_[actor];
    for (const auto &[key,f] : objects_.at(object)) {
        if (key == "IsAlive") { life(actor,f.value,f.stamp,"property_replication"); continue; }
        if (state.fields.contains(key) && (f.stamp < state.fields[key] ||
            (f.stamp == state.fields[key] && state.health[key]["value"] == f.value))) continue;
        state.fields[key] = f.stamp;
        state.health[key] = {{"value",f.value},{"timestampMs",f.stamp.first},{"sourceOrder",f.stamp.second},{"source","property_replication"}};
        dirty_.insert(actor);
    }
}
void HealthTracker::life(uint32_t actor, const Json &value, Stamp stamp, const std::string &source) {
    if (!actor || !value.is_number()) return;
    double n = value.get<double>();
    if (n != 1 && n != 2 && n != 3) return; // Unknown enums never prove death or revival.
    if (!actors_.contains(actor) && actors_.size() >= 8192) throw ue::Error("health actor capacity");
    auto &s = actors_[actor]; int priority = source == "property_replication" ? 3 : 1;
    if (stamp < s.life_stamp || (stamp == s.life_stamp && priority < s.priority) ||
        (stamp == s.life_stamp && priority == s.priority && s.life.value("raw",Json()) == value)) return;
    s.life_stamp = stamp; s.priority = priority;
    s.life = {{"state",n == 1 ? "alive" : n == 2 ? "dead" : "downed"},{"raw",value},
        {"alive",n != 2},{"downed",n == 3},{"timestampMs",stamp.first},{"sourceOrder",stamp.second},{"source",source}};
    dirty_.insert(actor);
}
Json HealthTracker::snapshot(uint32_t actor) const {
    auto it = actors_.find(actor);
    return it == actors_.end() ? Json::object() : Json{{"health",it->second.health},{"life",it->second.life}};
}
std::set<uint32_t> HealthTracker::drain() { auto out = std::move(dirty_); dirty_.clear(); return out; }
} // namespace radaer
