#include "radaer/battle.hpp"
#include <iostream>
#include <stdexcept>
using namespace radaer;
int main() {
    try {
        int checks = 0;
        auto check = [&](bool ok, const char *message) {
            ++checks;
            if (!ok)
                throw std::runtime_error(message);
        };
        int64_t clock = 0;
        std::vector<Json> events;
        BattleTracker battle([&](Json e) { events.push_back(std::move(e)); }, [&] { return clock; });
        auto emit = [&](std::string type, Json data = Json::object()) {
            battle.consume({{"type", type},
                            {"session", "s"},
                            {"synthetic", true},
                            {"timestampMs", 1700000000000LL + clock},
                            {"data", data}});
        };
        auto plain = [&](std::string remote, uint64_t epoch = 1, std::string direction = "down") {
            emit("udp_plaintext", {{"local", "client:1"},
                                   {"remote", remote},
                                   {"epoch", epoch},
                                   {"timestampMs", 1700000000000LL + clock},
                                   {"direction", direction},
                                   {"structuralValid", true}});
        };
        auto welcome = [&](std::string remote, uint64_t epoch = 1, std::string map = "/Game/Map") {
            emit("battle_welcome", {{"flow", "client:1>" + remote}, {"epoch", epoch}, {"map", map}});
        };
        auto player = [&](std::string remote, uint64_t epoch = 1, unsigned slot = 1) {
            emit("player_validation", {{"flow", "client:1>" + remote},
                                       {"epoch", epoch},
                                       {"channel", 14},
                                       {"slot", slot},
                                       {"x", 123},
                                       {"timestampMs", 1700000000000LL + clock}});
        };
        auto self = [&](std::string remote, uint64_t epoch = 1) {
            emit("self_validation", {{"flow", "client:1>" + remote}, {"epoch", epoch}, {"x", 456}});
        };
        emit("session_started");
        plain("a:1");
        player("a:1");
        check(battle.snapshot()["status"] == "waiting" && battle.snapshot()["players"].empty(),
              "movement without Welcome must not activate the display");
        welcome("a:1", 1, "/NotGame/Map");
        check(battle.snapshot()["status"] == "waiting", "reject unrelated map");
        welcome("a:1");
        self("a:1");
        emit("loot_state",{{"flow","client:1>a:1"},{"epoch",1},{"actorGuid",100},{"itemId","15080050142"},{"present",true},{"timestampMs",100}});
        check(battle.snapshot()["loot"].size()==1,"ground item attached to confirmed flow");
        check(battle.snapshot()["self"]["x"] == 456, "self pose attaches to its confirmed flow");
        check(battle.snapshot()["status"] == "live" && battle.snapshot()["players"].size() == 1,
              "Welcome activates the correct cached flow");
        auto count = events.size();
        plain("a:1");
        battle.tick();
        check(events.size() == count, "no state event per packet or unchanged timer tick");
        ++clock;
        plain("b:2");
        player("b:2");
        check(battle.snapshot()["flow"] == "client:1>a:1", "unconfirmed traffic cannot steal selection");
        welcome("b:2");
        check(battle.snapshot()["loot"].empty(),"flow switch cannot inherit ground items");
        check(battle.snapshot()["self"].is_null(), "flow switch cannot inherit self pose");
        check(battle.snapshot()["flow"] == "client:1>b:2" && battle.snapshot()["players"].size() == 1,
              "newly confirmed recent flow selected without mixing players");
        check(events.back()["data"]["previousFlow"] == "client:1>a:1", "switch event retains old flow");
        plain("a:1", 1, "up");
        check(battle.snapshot()["loot"].size()==1,"return restores same flow ground items");
        check(battle.snapshot()["self"]["x"] == 456, "return to flow restores its own pose");
        check(battle.snapshot()["flow"] == "client:1>a:1", "upstream plaintext also selects latest flow");
        clock = 15001;
        battle.tick();
        check(battle.snapshot()["status"] == "live", "exactly 15 seconds matches original strict threshold");
        ++clock;
        emit("udp_raw", {{"flow", "client:1>a:1"}});
        emit("bank_candidate");
        player("a:1");
        battle.tick();
        check(battle.snapshot()["self"].is_null(), "idle hides self pose");
        check(battle.snapshot()["loot"].empty(),"idle hides ground items");
        check(battle.snapshot()["status"] == "idle" && battle.snapshot()["players"].empty(),
              "timer alone empties display; raw traffic and samples cannot refresh plaintext activity");
        count = events.size();
        clock += 50000;
        battle.tick();
        check(events.size() == count, "idle event is not repeatedly logged");
        plain("a:1", 1, "up");
        check(battle.snapshot()["status"] == "live" && events.back()["data"]["reason"] == "udp_resumed",
              "same confirmed flow resumes after silence");
        plain("a:1", 2);
        check(battle.snapshot()["flow"] == "client:1>b:2" && battle.snapshot()["status"] == "idle",
              "new epoch retires old confirmation and falls back to previous confirmed flow");
        welcome("a:1", 2);
        check(battle.snapshot()["loot"].empty(),"bank change clears old ground items");
        check(battle.snapshot()["self"].is_null(), "epoch change clears self pose");
        check(battle.snapshot()["epoch"] == 2 && battle.snapshot()["players"].empty(),
              "rebind requires a new Welcome and does not inherit old players");
        plain("a:1", 1);
        welcome("a:1", 1);
        player("a:1", 1);
        check(battle.snapshot()["epoch"] == 2 && battle.snapshot()["players"].empty(),
              "late old epoch cannot replace the new epoch");
        emit("udp_binding_reset", {{"flow", "client:1>a:1"}, {"epoch", 2}});
        plain("a:1", 2);
        welcome("a:1", 2);
        check(battle.snapshot()["flow"] == "client:1>b:2", "retired epoch cannot revive after reset");
        plain("a:1", 3);
        welcome("a:1", 3);
        for (unsigned i = 0; i < 140; ++i)
            player("a:1", 3, i);
        check(battle.snapshot()["players"].size() == 128, "per-flow display cache bounded");
        emit("capture_gap");
        check(battle.snapshot()["self"].is_null(), "capture gap clears self pose");
        check(battle.snapshot()["status"] == "recovery_required" && battle.snapshot()["players"].empty(),
              "capture gap discards all confirmations and samples");
        plain("a:1", 4);
        battle.tick();
        check(battle.snapshot()["status"] == "recovery_required",
              "plaintext and timer cannot hide missing session initialization after loss");
        welcome("a:1", 4);
        player("a:1", 4);
        check(battle.snapshot()["status"] == "live" && battle.snapshot()["players"].size() == 1,
              "new confirmed connection automatically recovers after capture loss");
        emit("session_stopped");
        plain("a:1", 4);
        battle.tick();
        check(battle.snapshot()["status"] == "stopped" && battle.snapshot()["players"].empty(),
              "capture stop is distinct from idle and late data cannot restart it");
        emit("session_started");
        plain("a:1");
        check(battle.snapshot()["status"] == "waiting", "new capture cannot inherit Welcome");
        welcome("a:1");
        Json row = {{"flow", "client:1>a:1"}, {"epoch", 1}, {"channel", 14}, {"slot", 1},
                    {"track", "current"}, {"timestampMs", 100}, {"poseSequence", 7}, {"x", 123},
                    {"motion", {{"viewYaw", 10}, {"updatedAtMs", 100}}}};
        emit("player_validation", row);
        Json delta = {{"flow", "client:1>a:1"}, {"epoch", 1}, {"track", "current"},
                      {"timestampMs", 110}, {"motion", {{"viewYaw", 90}, {"updatedAtMs", 110}}}};
        emit("player_motion", delta);
        auto merged = battle.snapshot()["players"][0];
        check(merged["motion"]["viewYaw"] == 90 && merged["timestampMs"] == 100 && merged["poseSequence"] == 7,
              "view delta must not invent a coordinate update");
        delta["track"] = "retired"; delta["motion"]["viewYaw"] = 180;
        emit("player_motion", delta);
        check(battle.snapshot()["players"][0]["motion"]["viewYaw"] == 90, "retired identity delta ignored");
        delta["track"] = "current"; delta["timestampMs"] = 105;
        emit("player_motion", delta);
        check(battle.snapshot()["players"][0]["motion"]["viewYaw"] == 90, "older view delta ignored");
        // Coordinates replace the displayed row; equipment must survive every replacement.
        row["actorGuid"]=4;
        row["equipment"]={{"timestampMs",100},{"sourceOrder",1},{"helmet",{{"level",2}}},{"armor",{{"level",3}}},{"weapon",{{"status","resolved"},{"name","M4A1"}}}};
        emit("player_validation",row);
        check(battle.snapshot()["players"][0].value("equipment",Json())==row["equipment"],"coordinate projection retains equipment snapshot");
        for(int i=0;i<5;++i){
            row["equipment"]["timestampMs"]=200+i;
            row["equipment"]["weapon"]["name"]=i%2?"AKM":"M4A1";
            row["vitals"]={{"life",{{"state",i%2?"downed":"alive"},{"timestampMs",200+i}}}};
            Json state=row;state.erase("x");
            emit("player_state",state);
            row["timestampMs"]=300+i;row["x"]=124+i;
            emit("player_validation",row);
            auto shown=battle.snapshot()["players"][0];
            check(shown.value("equipment",Json())==row["equipment"],"weapon/life event followed by movement cannot clear armor");
        }
        row["equipment"]={{"timestampMs",500},{"sourceOrder",10},{"weapon",{{"status","empty"}}}};
        emit("player_validation",row);
        check(!battle.snapshot()["players"][0]["equipment"].contains("armor"),"explicit equipment removal still reaches display");
        row["identityKey"] = "opaque-account-a";
        emit("player_validation", row);
        Json reincarnated = row;
        reincarnated["flow"] = "client:1>a:1"; reincarnated["epoch"] = 1; reincarnated["channel"] = 18;
        reincarnated["slot"] = 6; reincarnated["actorGuid"] = 986; reincarnated["playerStateGuid"] = 952;
        reincarnated["identityKey"] = "opaque-account-a"; reincarnated["timestampMs"] = 600;
        reincarnated["name"] = "相同名字"; reincarnated["x"] = 200;
        emit("player_validation", reincarnated);
        check(battle.snapshot()["players"].size() == 1 && battle.snapshot()["players"][0]["slot"] == 6,
              "a stable identity moving channel/slot retires its historical row");
        Json late = reincarnated;
        late["channel"] = 14; late["slot"] = 1; late["actorGuid"] = 4; late["timestampMs"] = 599; late["x"] = 100;
        emit("player_validation", late);
        check(battle.snapshot()["players"].size() == 1 && battle.snapshot()["players"][0]["x"] == 200,
              "a late old incarnation cannot revive over a newer identity row");
        Json namesake = reincarnated;
        namesake["channel"] = 19; namesake["slot"] = 8; namesake["actorGuid"] = 990; namesake["playerStateGuid"] = 956;
        namesake["identityKey"] = "opaque-account-b"; namesake["timestampMs"] = 601;
        emit("player_validation", namesake);
        check(battle.snapshot()["players"].size() == 2,
              "matching display names with distinct stable identities remain separate");
        emit("self_server_validation", {{"flow", "client:1>a:1"}, {"epoch", 1}, {"x", 789}});
        check(battle.snapshot()["serverSelf"]["x"] == 789 && battle.snapshot()["self"].is_null(),
              "downstream self must not replace upstream self");
        emit("capture_gap");
        check(battle.snapshot()["serverSelf"].is_null(), "capture gap clears downstream self");
        std::cout << Json({{"battleChecks", checks}, {"realCapture", false}}).dump() << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
