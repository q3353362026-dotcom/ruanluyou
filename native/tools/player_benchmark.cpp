#include "radaer/players.hpp"
#include "radaer/ue.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

using namespace radaer;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
int main(int argc, char **argv) {
    try {
        if (argc < 3 || argc > 4)
            throw std::runtime_error(
                "usage: radaer-player-benchmark INPUT MODE[packet|decode|validate|serialize] [ROUNDS]");
        const std::string mode = argv[2];
        if (mode != "packet" && mode != "decode" && mode != "validate" && mode != "serialize")
            throw std::runtime_error("invalid benchmark mode");
        unsigned rounds = argc == 4 ? unsigned(std::stoul(argv[3])) : 7;
        if (!rounds || rounds > 100)
            throw std::runtime_error("rounds must be 1..100");
        std::ifstream input(argv[1]);
        if (!input)
            throw std::runtime_error("cannot open input");
        std::vector<Json> events;
        std::vector<std::pair<Bytes, bool>> packets;
        std::string line;
        while (std::getline(input, line)) {
            auto e = Json::parse(line);
            auto type = e.value("type", std::string());
            if (type == "udp_plaintext") {
                auto &d = e.at("data");
                packets.emplace_back(unbase64(d.at("payloadBase64")), d.at("direction") == "up");
            }
            if (type == "udp_plaintext" || type == "session_started" || type == "capture_gap" ||
                type == "udp_binding_reset")
                events.push_back(std::move(e));
        }
        if (packets.empty())
            throw std::runtime_error("no plaintext packets");
        Json times = Json::array(), summary, counts;
        // One unreported warmup round initializes schemas and warms allocator/code paths.
        for (unsigned round = 0; round <= rounds; ++round) {
            uint64_t emitted = 0, bytes = 0, bunches = 0, packet_errors = 0;
            PlayerValidator validator;
            PlayerDecoder decoder([&](Json e) {
                ++emitted;
                if (mode == "validate" || mode == "serialize") {
                    if (e.at("type") == "player_sample") {
                        auto q = validator.inspect(e);
                        if (mode == "serialize")
                            bytes += q.dump().size();
                    }
                    if (mode == "serialize")
                        bytes += e.dump().size();
                }
            });
            auto start = Clock::now();
            if (mode == "packet") {
                for (auto &[data, up] : packets)
                    try {
                        bunches += ue::packet(data, up).bunches.size();
                    } catch (const ue::Error &) {
                        ++packet_errors;
                    }
            } else {
                for (auto &e : events)
                    decoder.consume(e);
            }
            auto elapsed = ms(start);
            if (round)
                times.push_back(elapsed);
            auto current = Json{{"emittedEvents", emitted},
                                {"serializedBytes", bytes},
                                {"bunches", bunches},
                                {"packetErrors", packet_errors}};
            auto current_summary = decoder.summary();
            if (round && (current != counts || current_summary != summary))
                throw std::runtime_error("benchmark output changed between rounds");
            counts = std::move(current);
            summary = std::move(current_summary);
        }
        auto sorted = times.get<std::vector<double>>();
        std::sort(sorted.begin(), sorted.end());
        double median = sorted[sorted.size() / 2];
        if (sorted.size() % 2 == 0)
            median = (median + sorted[sorted.size() / 2 - 1]) / 2;
        std::cout
            << Json({{"mode", mode},
                     {"rounds", rounds},
                     {"packets", packets.size()},
                     {"elapsedMs", times},
                     {"medianMs", median},
                     {"packetsPerSecond", packets.size() * 1000.0 / median},
                     {"meanMicrosecondsPerPacket", median * 1000.0 / packets.size()},
                     {"counts", counts},
                     {"decoderSummary", summary},
                     {"scope",
                      "preloaded plaintext; excludes input JSON parsing, decryption, disk I/O and WebUI"}})
                   .dump(2)
            << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
