#pragma once
#include "core.hpp"
#include <filesystem>
namespace radaer {
Json radar_recordings(const std::filesystem::path &directory, const std::string &active);
Json radar_replay(const std::filesystem::path &directory, const std::string &name, const std::string &active);
Json radar_clear_recordings(const std::filesystem::path &directory, const std::string &active);
}
