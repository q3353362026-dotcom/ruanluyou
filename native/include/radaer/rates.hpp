#pragma once
#include "core.hpp"
#include <tuple>

namespace radaer {
// One-second windows use receipt time, independent of browser polling and packet wall clocks.
class LiveRates {
    struct Window {
        std::deque<std::pair<int64_t, uint64_t>> buckets;
        uint64_t total = 0;
        uint64_t count(int64_t now);
        void add(int64_t now);
    };
    struct Slot {
        Window updates;
        std::string track;
        int64_t received = 0, source_time = 0;
        std::optional<int64_t> source_interval;
    };
    using FlowKey = std::tuple<std::string, std::string, uint64_t>;
    std::function<int64_t()> clock_;
    std::string session_;
    bool running_ = false;
    std::optional<FlowKey> current_;
    Window decrypted_, coordinate_packets_, coordinate_samples_, self_samples_;
    std::optional<int64_t> last_decrypted_, last_coordinate_;
    std::map<std::pair<uint32_t, unsigned>, Slot> slots_;
    std::set<uint64_t> packets_seen_;
    std::deque<std::pair<int64_t, uint64_t>> packet_times_;
    void clear_coordinates();
    void prune_packets(int64_t now);

  public:
    static constexpr int64_t window_ms = 1000;
    explicit LiveRates(std::function<int64_t()> clock = {});
    void reset();
    void consume(const Json &event);
    Json snapshot();
    Json slot(uint32_t channel, unsigned index);
};
} // namespace radaer
