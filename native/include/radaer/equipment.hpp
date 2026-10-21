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
private:
    struct State { uint32_t actor=0; std::string profile; Json fields=Json::object(), extra=Json::object(); Stamp stamp{}; };
    std::map<uint32_t,State> components_;
};
}
