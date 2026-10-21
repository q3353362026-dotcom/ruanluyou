#pragma once
#include "ue.hpp"
namespace radaer {
// Independent replicated state. Container indices are never movement handles.
class HealthTracker {
public:
    using Stamp = std::pair<int64_t, uint64_t>;
    void exports(const Json &entries);
    bool component(uint32_t sub, uint32_t actor, bool verified_class = false) const;
    void apply(uint32_t component, const Json &deltas, Stamp stamp);
    void life(uint32_t actor, const Json &value, Stamp stamp, const std::string &source);
    Json snapshot(uint32_t actor) const;
    std::set<uint32_t> drain();
private:
    struct Field { Json value; Stamp stamp{}; };
    struct Container { Stamp stamp{}; std::map<unsigned, Json> items; };
    struct State { Json health = Json::object(), life = Json::object(); Stamp life_stamp{}; int priority = 0; std::map<std::string, Stamp> fields; };
    std::map<uint32_t, Json> exports_;
    std::map<std::pair<uint32_t,unsigned>, Container> containers_;
    std::map<uint32_t, std::map<std::string, Field>> objects_;
    std::map<uint32_t, State> actors_;
    std::set<uint32_t> dirty_;
    void resolve(uint32_t object);
};
} // namespace radaer
