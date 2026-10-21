#include "radaer/equipment.hpp"
#include "equipment_schema.hpp"
#include <bit>
#include <cmath>
namespace radaer {
namespace {
const Json &catalog(){static const Json j=Json::parse(equipment_schema);return j;}
unsigned count(ue::Bits &r){auto n=r.read(32);if(n>512)throw ue::Error("equipment count limit");return unsigned(n);}
void skin(ue::Bits &r){
    if(r.read(32))return;
    r.skip(10*64);r.string();r.skip(2*64+4*32);
    auto n=count(r);r.skip(n*128);
}
Json extra(ue::Bits &r){
    auto identity=r.read(32),id=r.read(64);
    if(!r.bit())r.skip(64);
    r.packed();r.packed();r.skip(8);
    auto n=count(r);
    for(unsigned i=0;i<n;++i){
        r.skip(64+64+8+32);auto name_omitted=r.read(32),has_guid=r.read(32);
        if(!name_omitted)ue::fname(r);
        if(has_guid)r.skip(128);
        r.skip(32+3);
    }
    if(!r.bit())r.skip(64);
    skin(r);r.skip(8);
    return {{"identity",identity},{"weaponId",id}};
}
Json read_value(ue::Bits &r,const Json &c,const std::string &profile){
    int type=c.at("type"),idx=c.at("index");
    if(profile=="WeaponReplicateComponent"&&(idx==26||idx==34))return r.read(4);
    switch(type){
    case 3:case 21:return r.bit();
    case 4:{auto v=r.floating();if(!std::isfinite(v))throw ue::Error("nonfinite equipment value");return v;}
    case 5:return std::bit_cast<int32_t>(uint32_t(r.read(32)));
    case 6:return r.read(8);
    case 7:return ue::fname(r);
    case 8:return Json{{"guid",r.packed()}};
    case 9:return r.read(32);
    case 19:return r.string();
    case 20:return r.read(64);
    case 2:{
        auto st=c.at("meta").value("struct",std::string());
        if(st=="GPFastWeaponBPClassPath"){auto type=r.read(8);if(!type)throw ue::Error("unsupported weapon class path");return {{"type",type},{"fastId",r.read(64)}};}
        if(st=="WeaponReplicateExtraData")return extra(r);
        if(st=="ModularWeaponDynamicAttributesBase"){
            auto guids=count(r);r.skip(guids*64);auto nodes=count(r);
            for(unsigned i=0;i<nodes;++i){
                auto values=count(r);r.skip(values*32);auto indices=count(r);unsigned names=0;
                for(unsigned j=0;j<indices;++j)if(!r.read(32))++names;
                for(unsigned j=0;j<names;++j)ue::fname(r);
            }
            return Json::object();
        }
        if(st=="Vector2D"){auto x=r.floating(),y=r.floating();if(!std::isfinite(x)||!std::isfinite(y))throw ue::Error("nonfinite equipment vector");return Json::array({x,y});}
        throw ue::Error("unsupported equipment native struct: "+st);
    }
    default:throw ue::Error("unsupported equipment replication type");
    }
}
struct Layout {
    ue::Bits &r;const Json &cmds;const std::string &profile;bool checksum=false;uint32_t handle=0;size_t budget=0;
    uint32_t next(){auto h=r.packed();if(checksum)r.skip(32);return h;}
    void walk(size_t first,size_t end,unsigned &current,Json &state,unsigned depth=0){
        if(depth>8)throw ue::Error("equipment nesting limit");
        for(size_t i=first;i<end;++i){
            if(++budget>32768)throw ue::Error("equipment decode budget");
            auto &c=cmds.at(i);int type=c.at("type");if(type==1)continue;
            ++current;size_t tail=c.value("end",size_t(0));
            if(type==0&&(tail<=i+1||tail>cmds.size()))throw ue::Error("equipment array schema");
            if(handle!=current){if(type==0)i=tail-1;continue;}
            auto key=std::to_string(i);
            if(type!=0){state[key]=read_value(r,c,profile);handle=next();}
            else{
                auto n=r.read(16);if(n>512)throw ue::Error("equipment layout count limit");
                Json a=state.value(key,Json::array());if(!a.is_array())throw ue::Error("equipment array state");
                while(a.size()>n)a.erase(a.size()-1);
                while(a.size()<n)a.push_back(Json::object());
                handle=next();unsigned inside=0;
                for(unsigned e=0;e<n&&handle;++e)walk(i+1,tail-1,inside,a[e],depth+1);
                if(handle)throw ue::Error("equipment unterminated array");
                state[key]=std::move(a);i=tail-1;handle=next();
            }
        }
    }
};
// Only explicit weapon identity -> extra-data mappings establish the held weapon.
void fast_extra(ue::Bits &r,Json &target){
    r.bit();r.read(32);auto base=std::bit_cast<int32_t>(uint32_t(r.read(32)));
    auto nd=r.read(32),nc=r.read(32);if(nd>4096||nc>4096)throw ue::Error("weapon FastArray count limit");
    if(base==0)target=Json::object();
    for(uint64_t i=0;i<nd;++i)target.erase(std::to_string(std::bit_cast<int32_t>(uint32_t(r.read(32)))));
    for(uint64_t i=0;i<nc;++i){auto id=std::to_string(std::bit_cast<int32_t>(uint32_t(r.read(32))));target[id]=extra(r);}
    if(target.size()>4096||r.left())throw ue::Error("weapon FastArray boundary/capacity");
}
Json stamp(Json value,EquipmentTracker::Stamp s){value["timestampMs"]=s.first;value["sourceOrder"]=s.second;return value;}
}
std::string EquipmentTracker::profile(const std::string &path){
    auto pos=path.find_last_of("/.");auto leaf=pos==std::string::npos?path:path.substr(pos+1);
    if(leaf=="CharacterEquip"||leaf=="CharacterEquipComponent"||leaf=="SOLCharacterEquipComponent")return "SOLCharacterEquipComponent";
    if(leaf=="WeaponReplicate"||leaf=="WeaponReplicateComponent")return "WeaponReplicateComponent";
    if(leaf=="AttributeCenter"||leaf=="GPAttributeSetHealth")return "GPAttributeSetHealth";
    return {};
}
bool EquipmentTracker::apply(uint32_t actor,uint32_t component,const std::string &profile,ue::Bits &r,Stamp s){
    if(!actor||!component||!catalog()["layouts"].contains(profile))return false;
    auto it=components_.find(component);if(it!=components_.end()&&s<it->second.stamp)return false;
    if(it==components_.end()&&components_.size()>=8192)throw ue::Error("equipment component capacity");
    State next=it==components_.end()?State{}:it->second;
    if(next.actor!=actor||next.profile!=profile)next=State{};
    next.actor=actor;next.profile=profile;
    Layout w{r,catalog()["layouts"][profile],profile};w.checksum=r.bit();w.handle=w.next();unsigned current=0;
    w.walk(0,w.cmds.size(),current,next.fields);
    if(w.handle)throw ue::Error("equipment handle outside layout");
    if(profile=="WeaponReplicateComponent")while(r.left()){
        auto field=r.integer(24);auto bits=r.packed();if(bits>r.left())throw ue::Error("weapon field size");
        ue::Bits inner(r.data,r.pos+bits,r.pos);
        if(field==17)fast_extra(inner,next.extra);
        r.skip(bits);
    }
    if(r.left())throw ue::Error("equipment trailing bits");
    bool changed=it==components_.end()||next.fields!=it->second.fields||next.extra!=it->second.extra;
    next.stamp=s;components_[component]=std::move(next);return changed;
}
Json EquipmentTracker::snapshot(uint32_t actor)const{
    Json out=Json::object();
    for(const auto &[id,s]:components_)if(s.actor==actor){
        if(s.stamp>=Stamp{out.value("timestampMs",int64_t(0)),out.value("sourceOrder",uint64_t(0))}){out["timestampMs"]=s.stamp.first;out["sourceOrder"]=s.stamp.second;}
        if(s.profile=="SOLCharacterEquipComponent"){
            // Array entries are equipped items; durability is never used as an armor tier.
            for(auto key:{"2","13"})if(s.fields.contains(key)){
                std::map<std::string,std::set<std::string>> candidates;
                for(const auto &row:s.fields.at(key))for(auto field:key==std::string("2")?std::vector<std::string>{"3","4"}:std::vector<std::string>{"15","16"}){
                    if(!row.contains(field)||!row[field].is_number_unsigned())continue;
                    auto item=std::to_string(row[field].get<uint64_t>());const auto &gear=catalog()["gear"];
                    if(gear.contains(item))candidates[gear[item]["type"]=="头盔"?"helmet":"armor"].insert(item);
                }
                for(const auto &[slot,items]:candidates)if(items.size()==1){auto item=*items.begin();const auto &g=catalog()["gear"][item];out[slot]=stamp({{"itemId",item},{"name",g["name"]},{"level",g["tier"]},{"source","equipped_item_catalog"}},s.stamp);}
            }
        }else if(s.profile=="WeaponReplicateComponent"&&s.fields.contains("24")){
            const Json *active=nullptr;for(const auto &row:s.fields["24"])if(row.value("30",0)==0)active=&row;
            auto identity=active?active->value("27",uint64_t(0)):0;std::set<uint64_t> ids;
            for(const auto &[rid,x]:s.extra.items())if(identity&&x.value("identity",uint64_t(0))==identity&&x.value("weaponId",uint64_t(0)))ids.insert(x["weaponId"].get<uint64_t>());
            Json weapon={{"status",!identity?"empty":ids.size()==1?"resolved":"unresolved"},{"identity",identity},{"source","current_weapon_identity"}};
            if(ids.size()==1){auto item=std::to_string(*ids.begin());weapon["itemId"]=item;weapon["name"]=catalog()["weapons"].value(item,std::string("武器 ")+item);}
            out["weapon"]=stamp(weapon,s.stamp);
        }
    }
    return out;
}
Json EquipmentTracker::max_health(uint32_t actor)const{
    Json out;Stamp latest{};
    for(const auto &[id,s]:components_)if(s.actor==actor&&s.profile=="GPAttributeSetHealth"&&s.fields.contains("3")&&s.stamp>=latest){
        double v=s.fields["3"].get<double>();if(v>0&&v<=100000){out=stamp({{"value",v},{"source","attribute_max_health"}},s.stamp);latest=s.stamp;}
    }
    return out;
}
}
