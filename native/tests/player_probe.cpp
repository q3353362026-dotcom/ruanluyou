#include "radaer/ue.hpp"
#include "radaer/equipment.hpp"
#include <iostream>
using namespace radaer;
int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            auto j = Json::parse(line);
            auto bytes = unhex(j.at("hex"));
            auto mode = j.at("mode").get<std::string>();
            ue::Bits r(bytes, j.value("bits", bytes.size() * 8), j.value("start", size_t(0)));
            Json out;
            if(mode=="equipment"){
                EquipmentTracker tracker;
                Json history=Json::array();
                for(const auto &block:j.at("blocks")){
                    auto payload=unhex(block.at("hex"));ue::Bits bits(payload,block.at("bits"));
                    Json error;
                    try{tracker.apply(4,block.value("component",uint32_t(15)),block.at("profile"),bits,{block.value("time",int64_t(1000)),0});}
                    catch(const ue::Error &e){if(!j.value("continueOnError",false))throw;error=e.what();}
                    if(j.value("history",false))history.push_back({{"equipment",tracker.snapshot(4)},{"error",error}});
                }
                out={{"equipment",tracker.snapshot(4)},{"maxHealth",tracker.max_health(4)}};
                if(j.value("history",false))out["history"]=history;
            }
            else if (mode == "health")
                out = ue::health(r);
            else if (mode == "movement")
                out = ue::movement(r);
            else if (mode == "self")
                out = ue::self_movement(r);
            else if (mode == "packet")
                out = ue::debug_packet(bytes, j.value("up", false));
            else if (mode == "properties")
                out = ue::properties(r, j.at("profile"));
            else if (mode == "prefix")
                out = ue::prefix(r, j.value("exports", false), j.value("must", false),
                                 j.value("quantizedBit", false));
            else if (mode == "string")
                out = r.string();
            else
                throw ue::Error("unknown probe mode");
            std::cout << Json({{"ok", true}, {"result", out}, {"position", r.pos}}).dump() << '\n';
        } catch (const std::exception &e) {
            std::cout << Json({{"ok", false}, {"error", e.what()}}).dump() << '\n';
        }
    }
}
