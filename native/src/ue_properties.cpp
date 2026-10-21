#include "player_schema.hpp"
#include "loot_schema.hpp"
#include "container_schema.hpp"
#include "radaer/ue.hpp"
#include <bit>
#include <cmath>
namespace radaer::ue {
namespace {
struct Opaque : Error {
    using Error::Error;
};
// InventoryItemInfo native serialization, matching the recovered inventory reader.
// Variable arrays are bounded; an unknown extension stops before publishing its item ID.
unsigned item_count(Bits &r){auto n=r.read(32);if(n>512)throw Error("pickup array count limit");return unsigned(n);}
void item_array(Bits &r,unsigned bits){auto n=item_count(r);r.skip(size_t(n)*bits);}
void item_dynamic(Bits &r){
    item_array(r,64);auto n=item_count(r);
    for(unsigned i=0;i<n;++i){item_array(r,32);auto indices=item_count(r);unsigned names=0;
        for(unsigned j=0;j<indices;++j)if(!r.read(32))++names;
        for(unsigned j=0;j<names;++j)fname(r);
    }
    if(!r.read(32))r.skip(64);
    auto ammo=item_count(r);for(unsigned i=0;i<ammo;++i){r.skip(64+64+32+64);item_array(r,64);r.skip(32);}
    r.skip(3*32);
}
void item_skin(Bits &r){
    if(r.read(32))return;
    r.skip(10*64);r.string();r.skip(2*64+4*32);item_array(r,128);
}
Json inventory_item(Bits &r){
    auto high=r.read(32),low=r.read(32),gid=r.read(64);
    Json out={{"itemId",std::to_string(high*10000+low)},{"guid",gid}};
    r.skip(32+4*8+32+8);
    r.skip(32+8+64+32+64+8+64+32+32);
    item_array(r,64);item_array(r,64);
    if(r.bit()){out["count"]=std::bit_cast<int32_t>(uint32_t(r.read(32)));r.skip(32);item_array(r,64);item_array(r,32);}
    if(r.bit())r.skip(64); // durability / max
    if(r.bit())r.skip(64); // armor / max
    if(r.bit()){
        auto parts=r.read(8);for(unsigned i=0;i<parts;++i){r.skip(64+16+64+64+16+32+32+16+8+32+32+8+64+32);item_array(r,64);item_array(r,32);}
        item_dynamic(r);r.skip(9*64+32+2*64);r.string();
    }
    if(r.bit())throw Opaque("unsupported pickup inventory archive");
    if(r.bit()){auto n=item_count(r);for(unsigned i=0;i<n;++i){r.skip(24);item_array(r,8);item_array(r,8);}}
    if(r.bit()){r.string();r.skip(64);}
    r.skip(64);
    if(r.bit())r.skip(32+64+64);
    if(r.bit()){item_array(r,64);item_array(r,128);r.skip(32+64);}
    if(r.bit()){auto n=item_count(r);for(unsigned i=0;i<n;++i){r.skip(64);r.string();}}
    item_array(r,32);
    if(r.bit()){r.skip(64);r.string();r.string();r.skip(128);}
    r.skip(32+1);
    if(r.bit())r.skip(64);
    if(r.bit())r.skip(65);
    if(item_count(r))throw Opaque("unsupported pickup polymorphic extension");
    if(r.bit())r.skip(40);
    if(r.bit())r.string();
    if(r.bit())r.skip(32);
    if(r.bit())r.skip(32);
    return out;
}
Json value(Bits &r, int type, const Json &meta, bool character, bool pickup=false, bool box=false) {
    if (pickup && meta.value("name", std::string()) == "SourceType") {
        auto v = r.read(3);
        if (v >= 4) throw Error("invalid pickup source enum");
        return v;
    }
    if (meta.contains("departure_bits")) {
        auto v = r.read(meta.at("departure_bits").get<unsigned>());
        auto &allowed = meta.at("departure_values");
        if (std::find(allowed.begin(), allowed.end(), Json(v)) == allowed.end())
            throw Error("invalid departure enum");
        return v;
    }
    if (box && meta.value("prop_class", std::string()) == "EnumProperty" && meta.contains("enum_max")) {
        auto maximum=meta.at("enum_max").get<unsigned>();
        auto v=r.read(std::bit_width(maximum));
        if(v>=maximum)throw Error("invalid container enum");
        return v;
    }
    if (type == 6 && meta.contains("enum_max"))
        return r.integer(meta.at("enum_max"));
    auto pc = meta.value("prop_class", std::string()), st = meta.value("struct", std::string());
    if(box && pc=="TextProperty"){
        Json out={{"flags",r.read(32)}};auto history=r.read(8);out["history"]=history;
        if(history==11){out["table"]=fname(r);out["key"]=r.string();}
        else if(history==0){out["namespace"]=r.string();out["key"]=r.string();out["source"]=r.string();}
        else if(history==255){auto present=r.read(32);if(present>1)throw Error("invalid container text presence");out["source"]=present?r.string():"";}
        else throw Opaque("unsupported container text history");
        return out;
    }
    if(box && st=="LootBoxRepData"){
        // Native grid geometry, not the contents of the box. Bounds match the recovered serializer.
        auto spaces=r.read(32);if(spaces>128)throw Error("container space count limit");
        for(unsigned i=0;i<spaces;++i){
            r.skip(64);auto grids=r.read(32);if(grids>1024)throw Error("container grid count limit");
            r.skip(size_t(grids)*(5*32+2*8));
        }
        return Json::object();
    }
    if (pc == "EnumProperty" && meta.contains("enum_max"))
        return r.read(std::bit_width(meta.at("enum_max").get<unsigned>()));
    if (st == "GameplayTagContainer") {
        Json tags = Json::array();
        if (!r.bit()) {
            auto count = r.read(6);
            for (unsigned i = 0; i < count; i++)
                tags.push_back(fname(r));
        }
        return tags;
    }
    switch (type) {
    case 3:
    case 21:
        return r.bit();
    case 4: {
        auto v = r.floating();
        if (!std::isfinite(v))
            throw Error("nonfinite property");
        return v;
    }
    case 5:
        return std::bit_cast<int32_t>(uint32_t(r.read(32)));
    case 6:
        return r.read(8);
    case 7:
        return fname(r);
    case 8:
        return Json{{"guid", r.packed()}};
    case 9:
        return r.read(32);
    case 10:
    case 12: {
        Json a = Json::array();
        for (int i = 0; i < (type == 10 ? 3 : 4); i++) {
            auto v = r.floating();
            if (!std::isfinite(v))
                throw Error("nonfinite vector");
            a.push_back(v);
        }
        return a;
    }
    case 11:
        return rotation(r);
    case 13:
        return vector(r, 100, 30);
    case 14: {
        auto count = r.packed();
        if (count > 64)
            throw Error("NetId size limit");
        return hex(r.take(count * 8));
    }
    case 15: {
        auto flags = r.read(2);
        double scale = character ? 10 : 1;
        auto pos = vector(r, scale);
        auto rot = rotation(r);
        auto vel = vector(r, scale);
        Json v = {{"location", pos}, {"rotation", rot}, {"velocity", vel}};
        if (!character && (flags & 2))
            v["angularVelocity"] = vector(r);
        return v;
    }
    case 16: {
        Json a = Json::array();
        for (int i = 0; i < 3; i++)
            a.push_back(int(r.read(16)) - 32768);
        return a;
    }
    case 17:
        return vector(r, 10, 24);
    case 18:
        return vector(r, 1, 20);
    case 19:
        return r.string();
    case 20:
        return r.read(64);
    case 2:
        if(pickup&&st=="InventoryItemInfo")return inventory_item(r);
        if(pickup&&st=="ModularWeaponDynamicAttrbuites"){item_dynamic(r);return Json::object();}
        if(pickup&&st=="WeaponSkinInfo"){item_skin(r);return Json::object();}
        if (pc == "Int64Property")
            return r.read(64);
        if (pc == "UInt16Property")
            return r.read(16);
        if (pc == "Int16Property")
            return std::bit_cast<int16_t>(uint16_t(r.read(16)));
        if (pc == "Int8Property")
            return std::bit_cast<int8_t>(uint8_t(r.read(8)));
        if (st == "CharacterMoveHandle")
            return std::bit_cast<int32_t>(r.packed());
        // Unknown native structures cannot be skipped without their own wire format.
        throw Opaque("unsupported native property: " + pc + "/" + st);
    default:
        throw Opaque("unsupported replication type " + std::to_string(type));
    }
}
struct Walker {
    Bits &r;
    const Json &commands;
    bool checksum;
    bool pickup=false;
    bool box=false;
    uint32_t handle = 0;
    size_t budget = 0;
    Json fields = Json::object();
    uint32_t next() {
        auto h = r.packed();
        if (checksum)
            r.skip(32);
        return h;
    }
    void walk(size_t first, size_t last, unsigned &current, unsigned depth) {
        if (depth > 8)
            throw Error("replication array nesting limit");
        for (size_t i = first; i < last; i++) {
            if (++budget > 32768)
                throw Error("replication work limit");
            // current 是 handle 计数：数组内部命令不占 handle，所以数组分支跳过时
            // current 只加 1（对应数组本身），i 跳到 end-1。二者刻意不同步。
            // 已验证：TeamID(index 93) 在此逻辑下 current=62，与 Python catalog 的 handle 62 一致。
            ++current;
            auto &c = commands.at(i);
            auto type = c.at("type").get<int>();
            auto end = c.value("end", size_t(0));
            if (!end)
                end = i + 2;
            if (type == 0 && (end <= i + 1 || end > commands.size()))
                throw Error("invalid array catalog");
            if (handle != current) {
                if (type == 0)
                    i = end - 1;
                continue;
            }
            if (type != 0) {
                auto v = value(r, type, c.at("meta"), false, pickup, box);
                if (!depth && c.at("single").get<bool>())
                    fields[c.at("name").get<std::string>()] = v;
                if (!depth && c.at("meta").contains("departure_name"))
                    fields[c["meta"]["departure_name"].get<std::string>()] = v;
                if (!depth && box && c.at("meta").contains("field_name"))
                    fields[c["meta"]["field_name"].get<std::string>()] = v;
                handle = next();
            } else {
                auto count = r.read(16);
                if (count > 4096)
                    throw Error("replication array size limit");
                handle = next();
                unsigned inside = 0;
                if (handle)
                    for (unsigned e = 0; e < count; e++)
                        walk(i + 1, end - 1, inside, depth + 1);
                if (handle)
                    throw Error("unterminated replication array");
                i = end - 1;
                handle = next();
            }
        }
    }
};
} // namespace
Json properties(Bits &r, const std::string &profile) {
    static const Json schemas = Json::parse(player_schema);
    static const Json pickup = Json::parse(loot_schema);
    static const Json boxes = Json::parse(container_schema);
    if (profile != "pickup" && !schemas.contains(profile) && !boxes.contains(profile))
        throw Error("unknown actor property profile");
    const auto &commands = profile == "pickup" ? pickup : boxes.contains(profile) ? boxes.at(profile) : schemas.at(profile);
    Json result = {{"fields", Json::object()}, {"status", "exact"}, {"position", r.pos}};
    try {
        if (profile == "character") {
            if (r.bit())
                throw Opaque("Character prefix with checksums");
            unsigned last = 0;
            while (true) {
                auto h = r.packed();
                if (!h)
                    break;
                if (h <= last)
                    throw Error("nonascending Character handle");
                if (h > 60) {
                    result["status"] = "prefix_only";
                    break;
                }
                auto &c = commands.at(h - 1);
                Json v;
                if (h == 60) {
                    if (c.at("name") != "MoveHandle")
                        throw Error("Character MoveHandle schema mismatch");
                    v = std::bit_cast<int32_t>(r.packed());
                } else if (h == 6 || h == 15)
                    v = r.integer(4);
                else
                    v = value(r, c.at("type"), Json::object(), true);
                if (h == 14 || h == 18 || h == 60)
                    result["fields"][c["name"].get<std::string>()] = v;
                last = h;
            }
        } else {
            Walker w{r, commands, r.bit()};
            w.box=boxes.contains(profile);
            w.pickup=profile=="pickup"||w.box;
            w.handle = w.next();
            unsigned current = 0;
            try {
                w.walk(0, commands.size(), current, 0);
            } catch (...) {
                result["fields"] = w.fields;
                throw;
            }
            result["fields"] = w.fields;
            if (w.handle)
                throw Error("replication handle outside catalog");
        }
        if (result["status"] == "exact" && r.left())
            result["status"] = "short";
    } catch (const Opaque &e) {
        // 未知 native struct：保留已解析的前缀字段（Owner / PlayerState / MoveHandle）。
        // 对齐 Python repdec4.decode_block 的 "opaque -> return ev" 语义。
        // 原实现这里只设 status/error，导致 h60 MoveHandle 解析到了也被丢弃，
        // 进而 actor.slot 永远为 -1，身份链断裂（identitySource=unassociated_slot）。
        result["status"] = "unsupported";
        result["error"] = e.what();
    } catch (const std::exception &e) {
        // 截断/乱序：Python 侧返回 {}（宁可不要，不给半截身份），这里保持一致。
        result["status"] = "error";
        result["error"] = e.what();
        result["fields"] = Json::object();
    }
    result["position"] = r.pos;
    return result;
}
} // namespace radaer::ue
