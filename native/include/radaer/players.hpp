#pragma once
#include "monitor.hpp"

namespace radaer {
// Stateful UE packet/bunch/actor decoder. Every sample retains its source flow and bank epoch.
// Recovered Python modules are used only by reference tests, never by this runtime.
class PlayerDecoder {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit PlayerDecoder(Sink sink);
    ~PlayerDecoder();
    PlayerDecoder(const PlayerDecoder &) = delete;
    PlayerDecoder &operator=(const PlayerDecoder &) = delete;
    void consume(const Json &event);
    Json summary() const;
};
Json decode_player_file(const std::string &input, const std::string &output_directory);
} // namespace radaer
