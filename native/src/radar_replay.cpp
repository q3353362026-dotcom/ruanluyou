#include "radaer/radar_replay.hpp"
#include "radaer/battle.hpp"
#include <fstream>
#include <regex>
#include <set>
namespace radaer {
namespace {
bool recording_name(const std::string &name) {
    static const std::regex pattern("web-[0-9]+-[a-f0-9]+\\.jsonl");
    return std::regex_match(name, pattern);
}
Json indexed(const Json &rows) {
    Json result = Json::object();
    for (const auto &row : rows)
        result[std::to_string(row.at("channel").get<unsigned>()) + ":" + std::to_string(row.at("slot").get<unsigned>())] = row;
    return result;
}
Json changes(const Json &before, const Json &after) {
    Json result = Json::object();
    for (const auto &[key, value] : after.items())
        if (!before.contains(key) || before.at(key) != value) result[key] = value;
    for (const auto &[key, value] : before.items())
        if (!after.contains(key)) result[key] = nullptr;
    return result;
}
Json player_changes(const Json &before, const Json &after) {
    Json result = Json::object();
    for (const auto &[key, row] : after.items()) {
        if (!before.contains(key)) { result[key] = {{"replace", true}, {"set", row}}; continue; }
        const auto &old = before.at(key);
        Json set = Json::object(), removed = Json::array();
        for (const auto &[field, value] : row.items())
            if (!old.contains(field) || old.at(field) != value) set[field] = value;
        for (const auto &[field, value] : old.items())
            if (!row.contains(field)) removed.push_back(field);
        if (!set.empty() || !removed.empty()) result[key] = {{"set", std::move(set)}, {"remove", std::move(removed)}};
    }
    for (const auto &[key, row] : before.items()) if (!after.contains(key)) result[key] = nullptr;
    return result;
}
}
Json radar_recordings(const std::filesystem::path &directory, const std::string &active) {
    std::map<std::string, Json, std::greater<>> sorted;
    if (std::filesystem::exists(directory)) for (const auto &file : std::filesystem::directory_iterator(directory)) {
        const auto name = file.path().filename().string();
        if (!recording_name(name) || file.is_symlink() || !file.is_regular_file()) continue;
        sorted[name] = {{"id", name}, {"bytes", file.file_size()},
                        {"active", !active.empty() && std::filesystem::path(active).filename() == file.path().filename()}};
    }
    Json result = Json::array();
    for (const auto &[key, value] : sorted) { result.push_back(value); if (result.size() == 200) break; }
    return result;
}
Json radar_clear_recordings(const std::filesystem::path &directory, const std::string &active) {
    uint64_t removed = 0, bytes = 0, preserved_active = 0;
    Json failed = Json::array();
    if (!std::filesystem::is_directory(directory))
        return {{"removed", removed}, {"bytes", bytes}, {"preservedActive", preserved_active}, {"failed", failed}};
    const auto active_name = active.empty() ? std::string() : std::filesystem::path(active).filename().string();
    for (const auto &file : std::filesystem::directory_iterator(directory)) {
        const auto name = file.path().filename().string();
        if (!recording_name(name) || file.is_symlink() || !file.is_regular_file())
            continue;
        if (name == active_name) {
            ++preserved_active;
            continue;
        }
        std::error_code error;
        const auto size = file.file_size(error);
        if (error) { failed.push_back(name); continue; }
        if (std::filesystem::remove(file.path(), error)) {
            ++removed;
            bytes += size;
        }
        if (error) failed.push_back(name);
    }
    return {{"removed", removed}, {"bytes", bytes}, {"preservedActive", preserved_active}, {"failed", failed}};
}
Json radar_replay(const std::filesystem::path &directory, const std::string &name, const std::string &active) {
    if (!recording_name(name)) throw std::runtime_error("无效的采集记录名称");
    const auto path = directory / name;
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
        std::filesystem::canonical(path).parent_path() != std::filesystem::canonical(directory))
        throw std::runtime_error("采集记录不存在");
    if (!active.empty() && std::filesystem::path(active).filename() == path.filename())
        throw std::runtime_error("该记录正在采集，请结束这次采集后再回放；可以选择其他历史记录");
    if (std::filesystem::file_size(path) > 1024ULL * 1024 * 1024)
        throw std::runtime_error("单条回放记录最大支持 1 GiB");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("无法读取采集记录");
    int64_t clock = 0, start = -1, end = 0, next = 0;
    BattleTracker tracker({}, [&] { return clock; });
    Json frames = Json::array(), previous;
    size_t bytes = 0, malformed = 0;
    bool has_positions = false;
    auto emit = [&](int64_t time) {
        clock = time; tracker.tick(time);
        auto battle = tracker.snapshot();
        Json state = {{"players", indexed(battle.at("players"))}, {"self", battle.at("self")},
                      {"serverSelf", battle.at("serverSelf")}, {"loot", battle.at("loot")}};
        if (state["self"].is_object()) state["self"]["kind"] = "self";
        has_positions = has_positions || !state["players"].empty() || !state["self"].is_null();
        for (auto key : {"players", "self", "serverSelf", "loot", "lastPlaintextAtMs"}) battle.erase(key);
        state["battle"] = std::move(battle);
        Json delta = {{"t", time}};
        for (auto key : {"battle", "self", "serverSelf"})
            if (previous.is_null() || state[key] != previous[key]) delta[key] = state[key];
        for (auto key : {"players", "loot"}) {
            const auto before = previous.is_null() ? Json::object() : previous[key];
            auto patch = std::string_view(key) == "players" ? player_changes(before, state[key]) : changes(before, state[key]);
            if (!patch.empty()) delta[key] = std::move(patch);
        }
        if (delta.size() > 1 || frames.empty()) {
            bytes += delta.dump().size();
            if (bytes > 128 * 1024 * 1024) throw std::runtime_error("回放数据超过 128 MiB，请使用较短的采集记录");
            frames.push_back(std::move(delta));
        }
        previous = std::move(state);
    };
    static const std::set<std::string> types = {"session_started", "session_stopped", "capture_gap", "udp_binding_reset",
        "udp_plaintext", "battle_welcome", "player_validation", "self_validation", "self_server_validation",
        "player_motion", "player_state", "loot_state"};
    std::string line;
    while (std::getline(input, line)) {
        const auto event = Json::parse(line, nullptr, false);
        if (event.is_discarded()) { ++malformed; continue; }
        if (!event.is_object() || !event.contains("type") || !event["type"].is_string() ||
            !types.contains(event["type"].get<std::string>()) || !event.contains("timestampMs") ||
            !event["timestampMs"].is_number_integer()) continue;
        const auto time = std::max(end, event["timestampMs"].get<int64_t>());
        if (start < 0) { start = time; next = time + 50; }
        if (time - start > 8 * 60 * 60 * 1000LL) throw std::runtime_error("回放时长最大支持 8 小时");
        // Flush before consuming the next interval: future events must never leak into earlier frames.
        while (next < time) { emit(next); next += 50; }
        clock = time; end = time;
        try { tracker.consume(event); } catch (const Json::exception &) { ++malformed; }
    }
    if (input.bad()) throw std::runtime_error("读取采集记录失败");
    if (start < 0) throw std::runtime_error("该记录没有可回放的解析事件");
    emit(end);
    if (!has_positions) throw std::runtime_error("该记录没有已确认战斗流的人物数据");
    return {{"format", "radaer-replay-v1"}, {"rowPatches", true}, {"name", name}, {"startMs", start}, {"endMs", end},
            {"sampleIntervalMs", 50}, {"skippedLines", malformed}, {"frames", std::move(frames)}};
}
}
