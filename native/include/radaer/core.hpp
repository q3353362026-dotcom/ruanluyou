#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
namespace radaer {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
using Key = std::array<uint8_t, 16>;
using Bank = std::array<uint8_t, 128>;
// 说明：曾尝试把 Json 换成 basic_json<unordered_map,...> 提升插入速度，
// 但 nlohmann json.hpp:3591 的 actual_object_comparator 用 std::conditional
// 无条件实例化 object_t::key_compare，unordered_map 没有该类型 => 编译失败。
// nlohmann 官方不支持 unordered_map 作为 ObjectType，此路不通。
// 性能问题改在调用侧解决：减少每包 JSON 字段数、消除深拷贝、避免 O(n) 扫描。
using Json = nlohmann::json;
using Sink = std::function<void(Json)>;
Bytes unhex(const std::string &s);
std::string hex(View b);
std::string base64(View b);
Bytes unbase64(const std::string &s);
std::string fingerprint(View b);
Bytes pad(View b);
Bytes unpad(View b);
Bytes aes(View b, const Key &key, bool encrypt);
Bytes encrypt(View b, const Key &key);
Bytes udp_crypt(View b, const Bank &bank, bool encrypt = false);
std::vector<Bank> extract_banks(View b);
bool valid_envelope(View b);
bool valid_plaintext(View b);
bool initial_handshake(View b);
class Dh {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    Dh();
    explicit Dh(View secret); // deterministic test/reference input
    ~Dh();
    Dh(const Dh &) = delete;
    Dh &operator=(const Dh &) = delete;
    Bytes public_key() const;
    bool valid_public(View peer) const;
    Key derive(View peer) const;
};
struct Translation {
    Bytes to_server, to_client;
};
class TgcpSession {
    Dh client_, server_;
    Key client_key_{}, server_key_{};
    Bytes buffers_[2];
    std::deque<Bytes> pending_;
    size_t pending_bytes_ = 0;
    int state_ = 0;
    bool bypass_ = false;
    Sink sink_;
    std::function<void(const Bank &)> bank_sink_;
    std::set<std::string> seen_;
    void frame(bool up, Bytes f, Translation &out);
    void translate(bool up, Bytes &f);

  public:
    TgcpSession(Sink sink = {}, std::function<void(const Bank &)> bank_sink = {});
    ~TgcpSession();
    Translation feed(bool up, View data);
    Translation pass_through(const std::string &reason);
    bool committed() const {
        return state_ != 0;
    }
    bool established() const {
        return state_ == 2;
    }
    bool bypass() const {
        return bypass_;
    }
    bool buffered() const;
};
uint32_t be32(View b, size_t p);
void put_be32(Bytes &b, size_t p, uint32_t v);
int64_t now_ms();
} // namespace radaer
