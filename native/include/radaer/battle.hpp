#pragma once
#include "core.hpp"
#include <tuple>

namespace radaer {
// Presentation/session tracking only. Does not control TCP relay or bank acquisition.
class BattleTracker {
    using Key = std::tuple<std::string, std::string, uint64_t>;
    struct Flow {
        std::string map;
        int64_t received = 0, timestamp = 0;
        uint64_t order = 0;
        Json players = Json::array();
        Json loot = Json::object();
        Json self = nullptr;
        Json server_self = nullptr;
    };
    std::map<Key, Flow> flows_;
    std::map<std::pair<std::string, std::string>, uint64_t> epochs_, retired_;
    std::optional<Key> selected_;
    std::string status_ = "waiting";
    Json identity_ = Json::object(), last_transition_;
    uint64_t order_ = 0;
    bool stopped_ = false, recovery_required_ = false;
    Sink sink_;
    std::function<int64_t()> clock_;
    void update(const std::string &reason, int64_t timestamp);

  public:
    static constexpr int64_t idle_ms = 15000;
    explicit BattleTracker(Sink sink, std::function<int64_t()> clock = {});
    void consume(const Json &event);
    void tick(int64_t timestamp = now_ms());
    Json snapshot() const;
};
} // namespace radaer
