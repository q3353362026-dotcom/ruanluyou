#include "radaer/health.hpp"
#include "radaer/player_view.hpp"
#include "radaer/battle.hpp"
#include <iostream>
using namespace radaer;
int main() {
    try {
        auto check=[](bool v,const char *msg){if(!v)throw std::runtime_error(msg);};
        HealthTracker h;
        auto apply=[&](int object, double hp, double life, int64_t t) {
            h.apply(20,Json::array({{{"handle",3},{"count",92},{"items",Json::array({Json::array({91,{{"Object",object},{"Health",hp},{"IsAlive",life}}})})}}}),{t,uint64_t(t)});
        };
        apply(8,75,1,1); check(h.drain().empty(),"unresolved object cannot guess movement slot 91");
        h.exports(Json::array({{{"guid",8},{"path","AttributeCenter"},{"outer",4}}}));
        check(h.drain()==std::set<uint32_t>{4},"late GUID export resolves actor 4");
        check(h.snapshot(4)["health"]["Health"]["value"]==75,"HP association");
        apply(8,0,3,2); check(h.snapshot(4)["life"]["state"]=="downed","zero HP and enum 3 is downed");
        apply(8,0,0,3); check(h.snapshot(4)["life"]["state"]=="downed","unknown enum cannot prove death");
        apply(8,0,2,4); check(h.snapshot(4)["life"]["alive"]==false,"explicit enum 2 death");
        h.life(4,1,{3,3},"playerstate"); check(h.snapshot(4)["life"]["state"]=="dead","late life event rejected");
        h.life(4,1,{4,4},"playerstate"); check(h.snapshot(4)["life"]["state"]=="dead","same stamp source priority");
        apply(8,20,1,5); check(h.snapshot(4)["life"]["state"]=="alive","explicit revival");
        apply(8,0,2,6); apply(8,10,1,6); check(h.snapshot(4)["life"]["state"]=="alive","same logical stamp final field wins");
        h.exports(Json::array({{{"guid",10},{"path","AttributeCenter"},{"outer",6}}}));
        h.apply(20,Json::array({{{"handle",3},{"count",92},{"items",Json::array({Json::array({91,{{"Object",10},{"IsAlive",1}}})})}}}),{7,7});
        check(h.snapshot(6)["health"].empty(),"reused container index cannot inherit HP");
        check(h.snapshot(4)["health"]["Health"]["value"]==10,"old object stays separate");
        h.exports(Json::array({{{"guid",20},{"path","PropertyReplication"},{"outer",30}},{{"guid",30},{"path","BP_PropertyReplicationCharacterHealth_C_X"},{"outer",0}}}));
        check(h.component(20,30)&&!h.component(20,4),"health component must belong to verified actor");
        Json row={{"actorGuid",4},{"flow","a>b"},{"epoch",1},{"x",20},{"timestampMs",100},{"poseSequence",9}};
        Json delta={{"actorGuid",4},{"flow","a>b"},{"epoch",1},{"timestampMs",200},{"vitals",h.snapshot(4)}};
        Json rows=Json::array({row}),self=row,server=row;
        apply_player_state(rows,self,server,delta);
        check(rows[0]["timestampMs"]==100&&rows[0]["poseSequence"]==9&&rows[0]["x"]==20,"state never changes pose clock or counter");
        check(self["vitals"]==server["vitals"]&&self["vitals"]==rows[0]["vitals"],"self and remote state projection");
        delta["equipment"]={{"timestampMs",200},{"sourceOrder",1},{"weapon",{{"status","resolved"},{"name","M4A1"}}},{"helmet",{{"level",2}}}};
        apply_player_state(rows,self,server,delta);
        check(self["equipment"]==rows[0]["equipment"]&&rows[0]["timestampMs"]==100,"equipment updates self and remote without changing coordinate clock");
        delta["equipment"]={{"timestampMs",300},{"sourceOrder",2},{"weapon",{{"status","empty"}}}};
        apply_player_state(rows,self,server,delta);
        check(!self["equipment"].contains("helmet")&&self["equipment"]["weapon"]["status"]=="empty","complete snapshot removes old equipment");
        delta["equipment"]={{"timestampMs",100},{"helmet",{{"level",6}}}};
        apply_player_state(rows,self,server,delta);
        check(!self["equipment"].contains("helmet"),"old equipment delta rejected");
        delta["epoch"]=2; delta["vitals"]=Json::object(); apply_player_state(rows,self,server,delta);
        check(rows[0]["vitals"]==h.snapshot(4),"epoch isolation");
        std::cout << "Health ownership, enums, ordering, late exports, reuse and independent projection passed\n";
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
