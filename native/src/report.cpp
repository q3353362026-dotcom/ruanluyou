#include "radaer/monitor.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
namespace radaer {
Json bank_report(const std::vector<std::string> &files, int64_t max_gap_ms, bool synthetic_only) {
    struct Observation {
        int64_t ts;
        std::string fp;
        uint64_t ordinal;
    };
    struct Group {
        std::string session, device, flow;
        std::vector<Observation> uses;
        std::vector<int64_t> breaks;
        unsigned binds = 0;
    };
    std::map<std::string, Group> groups;
    std::map<std::string, std::vector<std::pair<int64_t, int64_t>>> gaps;
    std::map<std::string, std::pair<uint64_t, int64_t>> last_event;
    std::map<std::string, std::map<std::string, std::set<std::string>>> devices;
    std::set<std::string> dedup, replays;
    size_t synthetic = 0, read = 0, duplicates = 0, replay_count = 0;
    Json errors = Json::array();
    for (const auto &file : files) {
        std::ifstream in(file);
        if (!in)
            throw std::runtime_error("cannot read " + file);
        std::string line;
        size_t line_no = 0;
        while (std::getline(in, line)) {
            ++line_no;
            if (line.empty())
                continue;
            Json e;
            try {
                e = Json::parse(line);
            } catch (const std::exception &) {
                errors.push_back({{"file", file}, {"line", line_no}, {"error", "invalid_json"}});
                continue;
            }
            ++read;
            if (!e.contains("synthetic") || e.at("synthetic").get<bool>() != synthetic_only) {
                ++synthetic;
                continue;
            }
            if (!e.contains("session") || !e.contains("device") || !e.contains("sequence")) {
                errors.push_back({{"file", file}, {"line", line_no}, {"error", "missing_provenance"}});
                continue;
            }
            auto session = e.at("session").get<std::string>(), device = e.at("device").get<std::string>();
            auto sid = device + "|" + session;
            auto identity = sid + "|" + std::to_string(e.at("sequence").get<uint64_t>());
            if (!dedup.insert(identity).second) {
                ++duplicates;
                continue;
            }
            auto kind = e.value("type", std::string());
            if (kind == "session_started" && e.at("data").value("mode", std::string()) == "offline_replay")
                replays.insert(sid);
            if (replays.contains(sid) && !synthetic_only) {
                ++replay_count;
                continue;
            }
            auto ts = e.at("timestampMs").get<int64_t>();
            auto sequence = e.at("sequence").get<uint64_t>();
            if (auto previous = last_event.find(sid); previous != last_event.end()) {
                if (sequence != previous->second.first + 1) {
                    auto old = previous->second.second;
                    gaps[sid].push_back({std::min(old, ts), std::max(old, ts)});
                }
            }
            last_event[sid] = {sequence, ts};
            if (kind == "capture_gap") {
                auto since = e.at("data").value("sinceMs", ts);
                gaps[sid].push_back({std::min(since, ts), std::max(since, ts)});
                continue;
            }
            if (kind != "bank_use" && kind != "bank_bound" && kind != "udp_binding_reset")
                continue;
            const auto &d = e.at("data");
            auto flow = d.at("flow").get<std::string>();
            auto &g = groups[sid + "|" + flow];
            g.session = session;
            g.device = device;
            g.flow = flow;
            if (kind == "bank_bound") {
                ++g.binds;
                continue;
            }
            if (kind == "udp_binding_reset") {
                if (d.value("reason", std::string()) != "three_invalid_downstream_packets")
                    g.breaks.push_back(ts);
                continue;
            }
            auto fp = d.at("fingerprint").get<std::string>();
            g.uses.push_back({ts, fp, d.value("packetOrdinal", uint64_t(0))});
            devices[device][session].insert(fp);
        }
    }
    Json reports = Json::array();
    size_t total_changes = 0;
    for (auto &[key, g] : groups) {
        auto &uses = g.uses;
        std::sort(uses.begin(), uses.end(), [](const auto &a, const auto &b) {
            return a.ts != b.ts ? a.ts < b.ts : a.ordinal < b.ordinal;
        });
        auto &global = gaps[g.device + "|" + g.session];
        std::sort(g.breaks.begin(), g.breaks.end());
        Json windows = Json::array(), intervals = Json::array(), gapped = Json::array();
        std::set<std::string> distinct;
        int64_t prev_lo = 0, prev_hi = 0;
        bool previous_window = false;
        unsigned generation = 0, prev_gen = 0;
        std::vector<double> mids;
        for (size_t i = 0; i < uses.size(); i++) {
            auto &b = uses[i];
            distinct.insert(b.fp);
            if (!i)
                continue;
            auto &a = uses[i - 1];
            bool broken = b.ts - a.ts > max_gap_ms ||
                          std::any_of(g.breaks.begin(), g.breaks.end(),
                                      [&](auto t) { return t > a.ts && t <= b.ts; }) ||
                          std::any_of(global.begin(), global.end(), [&](auto window) {
                              return window.first <= b.ts && window.second >= a.ts;
                          });
            if (broken) {
                ++generation;
                previous_window = false;
            }
            if (a.fp == b.fp)
                continue;
            ++total_changes;
            Json w = {{"old", a.fp},
                      {"new", b.fp},
                      {"afterMs", a.ts},
                      {"byMs", b.ts},
                      {"uncertaintyMs", b.ts - a.ts}};
            if (broken) {
                w["classification"] = "different_bank_across_observation_gap";
                gapped.push_back(w);
                continue;
            }
            w["classification"] = "observed_transition_window";
            windows.push_back(w);
            if (previous_window && generation == prev_gen) {
                auto low = std::max<int64_t>(0, a.ts - prev_hi), high = b.ts - prev_lo;
                intervals.push_back({{"minMs", low}, {"maxMs", high}});
                mids.push_back((double(low) + double(high)) / 2);
            }
            prev_lo = a.ts;
            prev_hi = b.ts;
            previous_window = true;
            prev_gen = generation;
        }
        Json cycle = {{"status", "insufficient_intervals"},
                      {"intervalCount", intervals.size()},
                      {"estimateMs", nullptr}};
        if (mids.size() >= 3) {
            double mean = 0, var = 0;
            for (double x : mids)
                mean += x;
            mean /= double(mids.size());
            for (double x : mids)
                var += (x - mean) * (x - mean);
            double cv = mean > 0 ? std::sqrt(var / double(mids.size())) / mean : 1;
            std::sort(mids.begin(), mids.end());
            double width = 0;
            for (const auto &interval : intervals)
                width = std::max(width, interval["maxMs"].get<double>() - interval["minMs"].get<double>());
            bool resolution = mean > 0 && width / mean <= 0.25;
            cycle["temporalResolutionAdequate"] = resolution;
            cycle["coefficientOfVariation"] = cv;
            cycle["status"] = !resolution ? "insufficient_temporal_resolution"
                              : cv <= 0.2 ? "candidate_period_requires_more_sessions"
                                          : "not_consistent_with_fixed_period";
            if (cv <= 0.2 && resolution)
                cycle["estimateMs"] = mids[mids.size() / 2];
        }
        reports.push_back({{"device", g.device},
                           {"session", g.session},
                           {"flow", g.flow},
                           {"confirmedUses", uses.size()},
                           {"distinctBanks", distinct.size()},
                           {"bindEvents", g.binds},
                           {"observationSpanMs", uses.empty() ? 0 : uses.back().ts - uses.front().ts},
                           {"transitions", windows},
                           {"changesAcrossGaps", gapped},
                           {"intervalBounds", intervals},
                           {"cycle", cycle}});
    }
    Json comparisons = Json::array();
    for (auto &[device, sessions] : devices) {
        std::set<std::string> all;
        Json samples = Json::array();
        for (auto &[s, banks] : sessions) {
            all.insert(banks.begin(), banks.end());
            samples.push_back({{"session", s}, {"bankFingerprints", banks}});
        }
        comparisons.push_back(
            {{"device", device},
             {"sessionCount", sessions.size()},
             {"distinctObservedBanks", all.size()},
             {"samples", samples},
             {"conclusion", sessions.size() < 2 ? "need_more_capture_sessions"
                            : all.size() > 1    ? "multiple_banks_observed_not_proof_of_periodic_rotation"
                                                : "same_bank_in_observed_sessions"}});
    }
    return {{"schema", "radaer-bank-report/v1"},
            {"evidenceScope", synthetic_only ? "synthetic_test_only" : "real_capture_only"},
            {"inputEvents", read},
            {"excludedSyntheticOrUnclassifiedEvents", synthetic},
            {"excludedReplayEvents", replay_count},
            {"duplicateEventsIgnored", duplicates},
            {"errors", errors},
            {"maxContinuousObservationGapMs", max_gap_ms},
            {"fingerprintTransitions", total_changes},
            {"flows", reports},
            {"devices", comparisons},
            {"limitations",
             Json::array({"structural acceptance is not cryptographic authentication",
                          "capture start is not bank creation time",
                          "different flows/devices and capture gaps do not define a rotation period",
                          "no observed change does not prove a fixed bank"})}};
}
} // namespace radaer
