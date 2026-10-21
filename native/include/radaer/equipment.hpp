#pragma once
#include "health.hpp"
namespace radaer {
class EquipmentTracker {
public:
    using Stamp=HealthTracker::Stamp;
    static std::string profile(const std::string &path);
    bool apply(uint32_t actor, uint32_t component, const std::string &profile, ue::Bits &bits, Stamp stamp);
    Json snapshot(uint32_t actor) const;
    Json max_health(uint32_t actor) const;
    // 每次 apply() 成功都会让对应 actor 的版本号 +1。position() 用它判断
    // 装备表是否真的变了，避免每个 sample 都深拷贝一份完整装备表。
    uint64_t version(uint32_t actor) const {
        auto it = versions_.find(actor);
        return it == versions_.end() ? 0 : it->second;
    }
private:
    struct State { uint32_t actor=0; std::string profile; Json fields=Json::object(), extra=Json::object(); Stamp stamp{}; };
    std::map<uint32_t,State> components_;
    mutable std::map<uint32_t,uint64_t> versions_;
};
}
