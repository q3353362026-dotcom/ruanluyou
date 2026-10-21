#pragma once
#include "ue.hpp"
namespace radaer {
class LootTracker {
    struct Record { Json fields=Json::object(), position; int64_t time=0; uint64_t order=0; bool removed=false; std::string status="spawn", reason="open", profile="pickup"; };
    std::map<uint32_t,Record> records_;
    std::string box_profile_;
public:
    uint64_t exact=0, partial=0, rejected=0;
    static std::string profile(const std::string &path);
    void exports(const Json &entries);
    void open(uint32_t actor,const Json &position,int64_t time,uint64_t order,const std::string &profile="pickup");
    bool apply(uint32_t actor,ue::Bits &bits,int64_t time,uint64_t order);
    bool close(uint32_t actor,unsigned reason,int64_t time,uint64_t order);
    Json snapshot(uint32_t actor) const;
};
// Apply a bounded event to a selected battle's item table; tombstones remove rows.
void apply_loot_state(Json &rows,const Json &event);
}
