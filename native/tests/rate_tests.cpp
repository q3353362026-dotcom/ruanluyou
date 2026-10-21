#include "radaer/rates.hpp"
#include <iostream>
#include <stdexcept>
using namespace radaer;
int main() {
    try {
        int64_t now = 0;
        int checks = 0;
        auto check = [&](bool ok, const char *why) {
            ++checks;
            if (!ok)
                throw std::runtime_error(why);
        };
        LiveRates rates([&] { return now; });
        auto emit = [&](std::string type, Json data = Json::object(), std::string session = "capture") {
            rates.consume({{"type", type}, {"session", session}, {"data", data}});
        };
        auto select = [&](std::string flow, uint64_t epoch = 1, std::string status = "live") {
            emit("battle_state",
                 {{"status", status}, {"flowSession", "capture"}, {"flow", flow}, {"epoch", epoch}});
        };
        auto sample = [&](std::string flow, unsigned slot, uint64_t packet, int64_t timestamp,
                          uint64_t epoch = 1, std::string track = "actor") {
            emit("player_sample", {{"flow", flow},
                                   {"epoch", epoch},
                                   {"channel", 14},
                                   {"slot", slot},
                                   {"sourcePacketOrdinal", packet},
                                   {"timestampMs", timestamp},
                                   {"track", track}});
        };
        check(rates.snapshot()["udpDecryptPacketsPerSecond"] == 0, "initial rates zero");
        emit("session_started");
        emit("udp_plaintext", {{"direction", "down"}, {"structuralValid", true}});
        emit("udp_plaintext", {{"direction", "down"}, {"structuralValid", true}});
        emit("udp_plaintext", {{"direction", "up"}, {"structuralValid", true}});
        emit("udp_plaintext", {{"direction", "down"}, {"structuralValid", false}});
        emit("udp_raw");
        emit("udp_invalid");
        check(rates.snapshot()["udpDecryptPacketsPerSecond"] == 2,
              "only successful downstream decrypts count");
        sample("a", 1, 1, 1000);
        check(rates.snapshot()["coordinateSamplesPerSecond"] == 0, "unconfirmed flow excluded");
        select("a");
        emit("self_sample", {{"flow", "a"}, {"epoch", 1}});
        emit("self_sample", {{"flow", "b"}, {"epoch", 1}});
        check(rates.snapshot()["selfSamplesPerSecond"] == 1 &&
                  rates.snapshot()["coordinateSamplesPerSecond"] == 0,
              "self rate is current-flow only and separate from downstream");
        sample("a", 0, 1, 1000);
        sample("a", 17, 1, 1000);
        emit("player_validation");
        auto s = rates.snapshot();
        check(s["coordinatePacketsPerSecond"] == 1 && s["coordinateSamplesPerSecond"] == 2,
              "two players in one packet count as one update packet and two samples");
        check(rates.slot(14, 17)["updatesPerSecond"] == 1 && rates.slot(14, 17)["sourceIntervalMs"].is_null(),
              "per-slot rate and unknown first interval");
        now = 250;
        sample("a", 17, 2, 1050);
        sample("b", 17, 3, 1050);
        sample("a", 17, 3, 1050, 2);
        check(rates.snapshot()["coordinatePacketsPerSecond"] == 2 &&
                  rates.snapshot()["coordinateSamplesPerSecond"] == 3,
              "non-current flow and epoch excluded");
        check(rates.slot(14, 17)["updatesPerSecond"] == 2 && rates.slot(14, 17)["sourceIntervalMs"] == 50,
              "source interval independent of callback receipt interval");
        now = 999;
        auto first_poll = rates.snapshot();
        for (unsigned i = 0; i < 10; ++i)
            check(first_poll == rates.snapshot(), "browser polls do not reset or change counts");
        now = 1000;
        s = rates.snapshot();
        check(s["udpDecryptPacketsPerSecond"] == 0 && s["coordinatePacketsPerSecond"] == 1 &&
                  s["coordinateSamplesPerSecond"] == 1,
              "window expiration needs no new events");
        check(s["selfSamplesPerSecond"] == 0, "self rate expires independently of browser frames");
        check(rates.slot(14, 0)["updatesPerSecond"] == 0 && rates.slot(14, 17)["lastUpdateAgeMs"] == 750,
              "slot activity ages while capture is silent");
        now = 1250;
        check(rates.snapshot()["coordinatePacketsPerSecond"] == 0 &&
                  rates.snapshot()["coordinateSamplesPerSecond"] == 0,
              "window fully expires at boundary");
        sample("a", 17, 4, 2000, 1, "new-actor");
        check(rates.slot(14, 17)["updatesPerSecond"] == 1 && rates.slot(14, 17)["sourceIntervalMs"].is_null(),
              "slot reuse resets actor interval and rate");
        emit("udp_plaintext", {{"direction", "down"}, {"structuralValid", true}});
        select("b");
        check(rates.snapshot()["selfSamplesPerSecond"] == 0, "flow change clears self rate");
        check(rates.snapshot()["coordinateSamplesPerSecond"] == 0 &&
                  rates.snapshot()["udpDecryptPacketsPerSecond"] == 1,
              "switch resets coordinates, keeps total decrypt rate");
        sample("b", 17, 4, 2050);
        check(rates.snapshot()["coordinatePacketsPerSecond"] == 1, "packet numbering isolated across flows");
        select("b", 2);
        check(rates.snapshot()["coordinateSamplesPerSecond"] == 0, "new epoch cannot inherit rate");
        sample("b", 17, 5, 2100, 2);
        select("b", 2, "idle");
        check(rates.snapshot()["coordinateSamplesPerSecond"] == 0 &&
                  rates.slot(14, 17)["lastUpdateAgeMs"].is_null(),
              "idle clears current coordinate metrics");
        select("b", 2);
        sample("b", 17, 6, 2200, 2);
        emit("udp_binding_reset", {{"flow", "b"}});
        check(rates.snapshot()["coordinateSamplesPerSecond"] == 0, "binding reset clears current metrics");
        emit("capture_gap");
        check(rates.snapshot()["udpDecryptPacketsPerSecond"] == 0, "capture gaps invalidate rate window");
        emit("session_stopped");
        emit("udp_plaintext", {{"direction", "down"}, {"structuralValid", true}});
        check(rates.snapshot()["udpDecryptPacketsPerSecond"] == 0, "stopped capture ignores late events");
        emit("session_started", Json::object(), "next-capture");
        emit("udp_plaintext", {{"direction", "down"}, {"structuralValid", true}});
        check(rates.snapshot()["udpDecryptPacketsPerSecond"] == 0,
              "prior capture cannot contaminate new rate");
        std::cout << Json({{"rateChecks", checks}, {"realCapture", false}}).dump() << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
