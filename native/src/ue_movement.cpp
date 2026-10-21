#include "radaer/ue.hpp"
#include <cmath>
namespace radaer::ue {
namespace {
struct Context {
    int bits[4] = {-1, -1, -1, -1};
    uint32_t value[4] = {};
};
void header(Bits &r, Context &ctx) {
    auto flags = r.read(2);
    for (unsigned k = 0; k < 4; k++)
        if (flags & (k == 3 ? 2 : 1)) {
            auto n = unsigned(r.read(5));
            ctx.bits[k] = int(n);
            ctx.value[k] = n ? uint32_t(r.read(n)) << (32 - n) : 0;
        }
}
double component(Bits &r, const Context &ctx, unsigned k, double scale) {
    if (ctx.bits[k] < 0 || ctx.bits[k] > 32)
        throw Error("movement context missing");
    auto v = uint32_t(r.read(unsigned(32 - ctx.bits[k]))) | ctx.value[k];
    auto signed_value = int64_t(v >> 1) ^ -int64_t(v & 1);
    return double(signed_value) / scale;
}
Json item(Bits &r, const Context &ctx) {
    auto sel = r.read(2);
    unsigned mask = sel == 0 ? 65 : sel == 1 ? 32 : sel == 2 ? 97 : unsigned(r.read(12));
    Json out = {{"sel", sel}, {"mask", mask}};
    if (mask & 64)
        out["scalar"] = component(r, ctx, 3, 10000);
    if (mask & 1) {
        Json p = Json::array();
        for (unsigned k = 0; k < 3; k++)
            p.push_back(component(r, ctx, k, 10));
        out["pos"] = p;
        out["pv"] = vector(r);
    }
    if (mask & 128)
        out["b168"] = r.read(8);
    for (auto [flag, name] : {std::pair{256, "b169"}, {512, "b170"}, {2048, "b172"}, {1024, "b171"}})
        if (mask & unsigned(flag))
            out[name] = r.read(1);
    if (mask & 1) {
        out["f52"] = r.read(2);
        out["rot"] = rotation(r);
    }
    if (mask & 2) {
        out["guid"] = r.packed();
        out["fname64"] = fname(r);
        out["b97"] = r.read(1);
        out["u32_84"] = Json::array();
        for (unsigned i = 0; i < 6; i++)
            out["u32_84"].push_back(r.read(32));
        out["b96"] = r.read(1);
        out["b98"] = r.read(1);
    }
    if (mask & 4) {
        bool a = r.bit(), b = r.bit();
        out["b132"] = int(a);
        out["b134"] = int(b);
        if (a) {
            out["b133"] = r.read(8);
            out["vec116"] = vector(r);
        }
        if (b) {
            out["vec104"] = vector(r);
            out["b135"] = r.read(1);
            out["b136"] = r.read(8);
        }
    }
    if (mask & 8) {
        out["u144"] = r.read(32);
        out["b142"] = r.read(1);
        out["b140"] = r.read(1);
        out["b141"] = r.read(1);
    }
    if (mask & 32)
        out["u160"] = r.read(32);
    // All known fields precede the 0x10 object. The caller must stop here, not guess its length.
    return out;
}
} // namespace
Json movement(Bits &r) {
    auto count = r.read(32);
    if (count > 512)
        throw Error("movement slot count exceeds 512");
    std::vector<uint32_t> masks;
    for (unsigned i = 0; i < (count + 31) / 32; i++)
        masks.push_back(uint32_t(r.read(32)));
    Json out = {{"count", count}, {"items", Json::object()}, {"complete", true}};
    auto stopped = [&](const std::string &where) {
        out["complete"] = false;
        out["stopped"] = Json::array({where, "m&0x10"});
        out["pos"] = r.pos;
        out["left"] = r.left();
        return out;
    };
    Context ctx;
    int group = -1;
    for (unsigned slot = 0; slot < count; slot++)
        if ((masks[slot / 32] >> (slot % 32)) & 1) {
            if (int(slot >> 4) != group) {
                group = int(slot >> 4);
                header(r, ctx);
            }
            auto decoded = item(r, ctx);
            bool stop = (decoded.at("mask").get<unsigned>() & 16) != 0;
            out["items"][std::to_string(slot)] = std::move(decoded);
            if (stop)
                return stopped("item" + std::to_string(slot));
        }
    out["bit_a"] = r.read(1);
    bool has_bits = r.bit();
    out["has_bits"] = int(has_bits);
    if (has_bits)
        r.skip(size_t(count));
    bool embedded = r.bit();
    out["has_embedded"] = int(embedded);
    if (embedded) {
        Context other;
        header(r, other);
        out["embedded"] = item(r, other);
        if (out["embedded"]["mask"].get<unsigned>() & 16)
            return stopped("embedded");
    }
    out["tail32"] = r.read(32);
    out["pos"] = r.pos;
    out["left"] = r.left();
    return out;
}
uint32_t object(Bits &r, bool exporting, Json &out, unsigned depth) {
    if (depth > 16)
        throw Error("NetGUID recursion limit");
    auto guid = r.packed();
    if (!guid)
        return 0;
    unsigned flags = (guid == 1 || exporting) ? unsigned(r.read(8)) : 0;
    if (flags & 248)
        throw Error("invalid NetGUID export flags");
    if (flags & 1) {
        auto outer = object(r, exporting, out, depth + 1);
        auto path = r.string();
        if (flags & 4)
            r.skip(32);
        out.push_back({{"guid", guid}, {"outer", outer}, {"path", path}, {"flags", flags}});
    } else if (exporting)
        out.push_back({{"guid", guid}, {"outer", nullptr}, {"path", nullptr}, {"flags", flags}});
    if (out.size() > 8192)
        throw Error("NetGUID export limit");
    return guid;
}
void exports(Bits &r, Json &out) {
    if (r.bit())
        throw Error("unsupported NetField compatibility exports");
    auto count = r.read(32);
    if (count > 2048)
        throw Error("NetGUID count exceeds 2048");
    for (unsigned i = 0; i < count; i++)
        object(r, true, out);
}
namespace {
Json conditional_vector(Bits &r, bool quantized_bit) {
    if (!r.bit())
        return nullptr;
    if (!quantized_bit || r.bit())
        return vector(r, 10, 24);
    Json v = Json::array();
    for (int i = 0; i < 3; i++)
        v.push_back(r.floating());
    return v;
}
} // namespace
Json prefix(Bits &r, bool hx, bool hm, bool quantized_bit, const std::vector<Bunch::Fragment> &fragments,
            bool identity_only) {
    Json out = {{"exports", Json::array()}, {"must", Json::array()}};
    if (hx) {
        if (fragments.empty())
            exports(r, out["exports"]);
        else
            for (auto &f : fragments)
                if (f.exports) {
                    r.pos = f.start;
                    exports(r, out["exports"]);
                }
    }
    if (hm) {
        for (auto &f : fragments)
            if (f.must) {
                r.pos = f.start;
                break;
            }
        auto count = r.read(16);
        if (count > 256)
            throw Error("must-map count exceeds 256");
        for (unsigned i = 0; i < count; i++)
            out["must"].push_back(r.packed());
    }
    auto actor = object(r, false, out["exports"]);
    if (!actor)
        throw Error("zero actor NetGUID");
    out["actor"] = actor;
    if (!(actor & 1)) {
        out["arch"] = object(r, false, out["exports"]);
        out["level"] = object(r, false, out["exports"]);
        if (identity_only) {
            out["stream_start"] = r.pos;
            return out;
        }
        out["loc"] = conditional_vector(r, quantized_bit);
        out["rot"] = r.bit() ? rotation(r, 16) : Json(nullptr);
        out["scale"] = conditional_vector(r, quantized_bit);
        out["vel"] = conditional_vector(r, quantized_bit);
    } else
        out["static_actor"] = true;
    out["stream_start"] = r.pos;
    return out;
}
std::vector<Block> blocks(Bits &r, std::string *tail_error) {
    std::vector<Block> result;
    while (r.left()) {
        try {
            if (result.size() >= 256)
                throw Error("content block limit");
            Block b;
            b.rep = r.bit();
            b.actor = r.bit();
            if (!b.actor) {
                b.sub = r.packed();
                if (!r.bit()) {
                    b.cls = r.packed();
                    b.deleted = b.cls == 0;
                }
            }
            size_t count = b.deleted ? 0 : r.packed();
            b.start = r.pos;
            r.skip(count);
            b.end = r.pos;
            result.push_back(b);
        } catch (const Error &e) {
            if (!tail_error || result.empty())
                throw;
            *tail_error = e.what();
            break;
        }
    }
    return result;
}
Json self_movement(Bits &r) {
    if (r.end < 190 || r.end > 800)
        return nullptr;
    try {
        auto p = r;
        p.pos = 53;
        if (!p.bit())
            return nullptr;
        auto ts = p.floating();
        if (!std::isfinite(ts) || ts < 1 || ts > 1000000)
            return nullptr;
        auto velocity = vector(p, 10, 24);
        p.skip(32);
        auto packed_angle = p.read(32);
        auto location = vector(p, 100, 30);
        for (auto &v : location)
            if (std::abs(v.get<double>()) >= 2000000)
                return nullptr;
        if (std::max(std::abs(location[0].get<double>()), std::abs(location[1].get<double>())) < 5000)
            return nullptr;
        auto angle = [](uint16_t value) {
            double degrees = value * 360.0 / 65536;
            return degrees > 180 ? degrees - 360 : degrees;
        };
        const auto tail = p.pos;
        for (bool full : {true, false}) {
            try {
                p.pos = tail;
                p.skip(48); // Movement mode byte, auxiliary word and byte precede either tail.
                Json acceleration = nullptr, flags = nullptr;
                if (full) {
                    acceleration = vector(p, 100, 30);
                    auto bits = p.read(8);
                    if (bits & 251)
                        continue;
                    flags = bits;
                    if (bits & 4) {
                        if (p.left() != 696)
                            continue;
                        p.skip(696);
                    }
                } else
                    p.skip(8);
                if (p.left())
                    continue;
                r.pos = p.pos;
                return {{"ts", ts},
                        {"loc", location},
                        {"vel", velocity},
                        {"accel", acceleration},
                        {"yaw", angle(uint16_t(packed_angle >> 16))},
                        {"pitch", angle(uint16_t(packed_angle))},
                        {"flags", flags},
                        {"tail", full ? "full" : "lite"}};
            } catch (const Error &) {
            }
        }
    } catch (const Error &) {
    }
    return nullptr;
}
} // namespace radaer::ue
