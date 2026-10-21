#include "radaer/loot.hpp"
#include <algorithm>
#include <cmath>
namespace radaer {
namespace {
bool position_ok(const Json &p){
    if(!p.is_array()||p.size()!=3)return false;
    for(const auto &n:p)if(!n.is_number()||!std::isfinite(n.get<double>())||std::abs(n.get<double>())>2097152)return false;
    return true;
}
}
std::string LootTracker::profile(const std::string &path){
    auto tail=path.substr(path.find_last_of("/.")+1);
    if(tail.starts_with("Default__"))tail.erase(0,9);
    if(tail=="BP_Inventory_DeadBody_C")return "death_box";
    if(tail.starts_with("BP_InteractorContainer_")||tail.starts_with("BP_LootContainer_"))return "container";
    if(tail.starts_with("BP_InventoryPickup"))return "pickup";
    return {};
}
void LootTracker::exports(const Json &entries){
    for(const auto &entry:entries){
        auto path=entry.value("path",Json());if(!path.is_string())continue;
        auto s=path.get<std::string>();auto tail=s.substr(s.find_last_of("/.")+1);
        if(tail=="Default__BP_SOLPlayerState_C"||tail=="BP_SOLPlayerState_C")box_profile_="death_box";
        else if(tail=="Default__BP_DFMPlayerState_C"||tail=="BP_DFMPlayerState_C")box_profile_="death_box_legacy";
    }
}
void LootTracker::open(uint32_t actor,const Json &position,int64_t time,uint64_t order,const std::string &profile){
    if(!actor||(!records_.contains(actor)&&records_.size()>=32768))return;
    auto &r=records_[actor];if(std::pair{time,order}<std::pair{r.time,r.order})return;
    if(r.removed||r.profile!=profile)r=Record{};
    r.profile=profile;
    r.time=time;r.order=order;r.removed=false;r.reason="open";
    if(position_ok(position))r.position=position;
}
bool LootTracker::apply(uint32_t actor,ue::Bits &bits,int64_t time,uint64_t order){
    auto it=records_.find(actor);if(it==records_.end()||std::pair{time,order}<std::pair{it->second.time,it->second.order})return false;
    const auto &profile=it->second.profile;
    if(profile=="container"||(profile=="death_box"&&box_profile_.empty()))return false;
    auto p=ue::properties(bits,profile=="death_box"?box_profile_:"pickup");std::string status=p.at("status");
    if(status!="exact"&&status!="unsupported"){++rejected;return false;}
    if(status=="exact")++exact;else ++partial;
    const auto &fields=p.at("fields");if(fields.empty())return false;
    auto &r=it->second;
    if(fields.contains("InventoryGID")&&r.fields.contains("InventoryGID")&&fields["InventoryGID"]!=r.fields["InventoryGID"]){r.fields=Json::object();r.removed=false;}
    r.fields.update(fields);r.time=time;r.order=order;r.status=status;
    if(status=="exact"&&fields.contains("PickupItemInfo")){
        r.fields["InventoryGID"]=fields["PickupItemInfo"]["guid"];
        r.fields["InventoryIdName"]={{"text",fields["PickupItemInfo"]["itemId"]}};
        if(fields["PickupItemInfo"].contains("count"))r.fields["StackCount"]=fields["PickupItemInfo"]["count"];
    }
    if(fields.contains("ReplicatedMovement")){
        auto pos=fields["ReplicatedMovement"].value("location",Json());if(position_ok(pos))r.position=pos;
    }
    if(status=="exact"){
        if(r.fields.value("bIsHiddenItem",false)||r.fields.value("bHidden",false)||(profile=="pickup"&&(r.fields.value("ItemState",0)==5||r.fields.value("StackCount",-1)==0))){r.removed=true;r.reason="item_unavailable";}
        else if(fields.value("ItemState",0)==1||(fields.contains("bIsHiddenItem")&&fields["bIsHiddenItem"]==false)){r.removed=false;r.reason="visible";}
        else if(profile=="death_box"&&fields.contains("bHidden")&&fields["bHidden"]==false){r.removed=false;r.reason="visible";}
    }
    return true;
}
bool LootTracker::close(uint32_t actor,unsigned reason,int64_t time,uint64_t order){
    auto it=records_.find(actor);if(it==records_.end()||std::pair{time,order}<std::pair{it->second.time,it->second.order})return false;
    auto &r=it->second;r.time=time;r.order=order;
    r.reason=reason==1?"dormant":reason==3?"out_of_relevancy":"channel_close_"+std::to_string(reason);
    if(reason==0||reason==2||reason==4)r.removed=true;
    return true;
}
Json LootTracker::snapshot(uint32_t actor)const{
    auto it=records_.find(actor);if(it==records_.end())return nullptr;
    const auto &r=it->second;
    Json out={{"actorGuid",actor},{"timestampMs",r.time},{"sourceOrder",r.order},{"present",!r.removed},{"reason",r.reason},{"decodeStatus",r.status}};
    if(position_ok(r.position)){out["x"]=r.position[0];out["y"]=r.position[1];out["z"]=r.position[2];}
    if(r.profile!="pickup"){
        std::string type="ordinary";
        if(r.profile=="death_box"){
            type="death_unknown";
            auto mesh=r.fields.value("DeadBodyBoxInfo.DeadBodySkeletalMeshType",0);
            // KillInfo.Taker is the deceased; Maker describes the killer and must never classify this box.
            if(mesh==6||mesh==7||r.fields.value("KillInfo.TakerIsAIBoss",false))type="boss_death";
            else if(mesh==1||mesh==9||mesh==10||r.fields.value("KillInfo.TakerIsPlayer",false))type="player_death";
            else if((mesh>=2&&mesh<=5)||r.fields.value("KillInfo.TakerIsRobotAI",false)||r.fields.value("KillInfo.TakerIsAILAB",false))type="ai_death";
            else if(r.fields.contains("bIsAI"))type=r.fields["bIsAI"]==true?"ai_death":"player_death";
            if(r.fields.contains("bIsEmpty"))out["empty"]=r.fields["bIsEmpty"];
            // 死者归属：KillInfo.Taker 就是死者（Maker 是凶手，绝不能拿来归属）。
            // 原实现只用 TakerIsPlayer 这类布尔做分类，把 Taker 本体丢掉了，
            // 导致桥接无法把「玩家盒子」对到具体玩家名，只能靠 6m 距离瞎猜
            // （实测最近盒子离玩家 79m，全失败）。这里把原始字段带出来。
            if(r.fields.contains("KillInfo.Taker"))out["deceased"]=r.fields["KillInfo.Taker"];
            if(r.fields.contains("KillInfo.Maker"))out["killer"]=r.fields["KillInfo.Maker"];
            if(r.fields.contains("DeadBodyBoxInfo.DeadBodySkeletalMeshType"))
                out["meshType"]=r.fields["DeadBodyBoxInfo.DeadBodySkeletalMeshType"];
            // 诊断：把死亡盒全部字段名 + 值（限长）带出来，定位 deceased 到底叫什么。
            // 字段名表通常 <40 项，每项值截断到 80 字节，开销可忽略。
            if(r.fields.is_object()){
                Json names=Json::array();
                for(const auto &kv : r.fields.items()){
                    std::string v=kv.value().dump();
                    if(v.size()>80)v=v.substr(0,80)+"...";
                    names.push_back(kv.key()+"="+v);
                }
                out["fieldDump"]=names;
            }
        }
        out["kind"]="container";out["containerType"]=type;out["contentsKnown"]=false;
        return out;
    }
    auto name=r.fields.value("InventoryIdName",Json());
    if(name.is_object()&&name.value("text",Json()).is_string()){
        auto id=name["text"].get<std::string>();
        if(!id.empty()&&id.size()<=16&&std::all_of(id.begin(),id.end(),[](char c){return c>='0'&&c<='9';}))out["itemId"]=id;
    }
    // 诊断：把 InventoryIdName 的原始结构带出来（hardcoded / hardcoded_index / text / number），
    // 用于定位「itemId 为空」到底是无物资变体还是解析错位。字段很小，不影响性能。
    if(name.is_object())out["idNameRaw"]=name;
    if(r.fields.contains("StackCount"))out["count"]=r.fields["StackCount"];
    return out;
}
void apply_loot_state(Json &rows,const Json &d){
    if(!rows.is_object())rows=Json::object();
    auto key=std::to_string(d.at("actorGuid").get<uint32_t>());
    if(rows.contains(key)&&std::pair{d.value("timestampMs",int64_t(0)),d.value("sourceOrder",uint64_t(0))}<std::pair{rows[key].value("timestampMs",int64_t(0)),rows[key].value("sourceOrder",uint64_t(0))})return;
    if(!d.value("present",false)){rows.erase(key);return;}
    if(rows.contains(key)||rows.size()<32768)rows[key]=d;
}
}
