#include "radaer/core.hpp"
#include <algorithm>
#include <openssl/crypto.h>
#include <stdexcept>
namespace radaer {
namespace {
constexpr std::array<uint8_t, 6> magic{0x33, 0x66, 0, 0x0b, 0, 0x0c};
void append(Bytes &a, View b) {
    a.insert(a.end(), b.begin(), b.end());
}
void need(bool x, const char *why) {
    if (!x)
        throw std::runtime_error(why);
}
struct Bits {
    View b;
    size_t end, pos = 1;
    uint32_t read(unsigned n) {
        need(n <= 32 && pos <= end && n <= end - pos, "truncated bits");
        uint32_t x = 0;
        for (unsigned i = 0; i < n; i++)
            x |= uint32_t((b[(pos + i) / 8] >> ((pos + i) % 8)) & 1) << i;
        pos += n;
        return x;
    }
    void skip(size_t n) {
        need(pos <= end && n <= end - pos, "truncated skip");
        pos += n;
    }
    uint32_t packed() {
        uint32_t x = 0;
        for (unsigned i = 0; i < 5; i++) {
            auto c = read(8);
            need(i != 4 || !(c & 225), "packed overflow");
            x |= (c >> 1) << (i * 7);
            if (!(c & 1))
                return x;
        }
        throw std::runtime_error("packed overflow");
    }
};
} // namespace
bool valid_envelope(View b) {
    if (b.size() < 10 || b.size() > 2048 || (b[0] & 1) || !b.back())
        return false;
    unsigned n = 0;
    for (unsigned x = b.back(); x; x >>= 1)
        ++n;
    size_t end = (b.size() - 1) * 8 + n - 2;
    if (!((b[end / 8] >> (end % 8)) & 1))
        return false;
    try {
        Bits r{b, end};
        auto count = r.read(4);
        r.pos = 33 + (count + 1) * 32;
        if (r.read(1))
            r.read(8);
        r.read(8);
        unsigned bunches = 0;
        while (r.pos < end) {
            need(end - r.pos >= 20 && bunches++ < 128, "bunch bound");
            bool open = false;
            if (r.read(1)) {
                open = r.read(1) != 0;
                if (r.read(1))
                    r.read(4);
            }
            r.read(1);
            bool reliable = r.read(1) != 0;
            r.read(1);
            need(r.packed() <= 32767, "channel bound");
            r.read(1);
            r.read(1);
            bool partial = r.read(1) != 0;
            if (reliable)
                r.read(10);
            if (partial) {
                r.read(1);
                r.read(1);
            }
            if (reliable || open) {
                if (r.read(1))
                    r.packed();
                else {
                    auto len = r.read(32);
                    need(len >= 1 && len <= 256, "name bound");
                    r.skip(size_t(len) * 8);
                    r.read(32);
                }
            }
            auto payload = r.read(13);
            r.skip(payload);
        }
        return r.pos == end;
    } catch (const std::exception &) {
        return false;
    }
}
bool valid_plaintext(View b) {
    return (b.size() == 25 && (b[0] & 1)) || valid_envelope(b);
}
// Initial-handshake detection is deliberately isolated from bank acceptance.
bool initial_handshake(View b) {
    return b.size() == 33 && b[8] == 1 && b.back() == 8 &&
           std::all_of(b.begin() + 9, b.begin() + 32, [](uint8_t x) { return x == 0; });
}
TgcpSession::TgcpSession(Sink s, std::function<void(const Bank &)> b)
    : sink_(std::move(s)), bank_sink_(std::move(b)) {}
TgcpSession::~TgcpSession() {
    OPENSSL_cleanse(client_key_.data(), client_key_.size());
    OPENSSL_cleanse(server_key_.data(), server_key_.size());
}
bool TgcpSession::buffered() const {
    return !buffers_[0].empty() || !buffers_[1].empty() || !pending_.empty();
}
void TgcpSession::translate(bool up, Bytes &f) {
    auto h = be32(f, 13), len = be32(f, 17);
    if (!len)
        return;
    auto padded = aes(View(f).subspan(h, len), up ? client_key_ : server_key_, false);
    auto plain = unpad(padded);
    try {
        for (const auto &bank : extract_banks(plain)) {
            auto fp = fingerprint(bank);
            if (seen_.size() < 1024 && seen_.insert(fp).second && bank_sink_)
                bank_sink_(bank);
        }
        if (sink_)
            sink_({{"type", "tcp_plaintext"},
                   {"data",
                    {{"direction", up ? "up" : "down"},
                     {"payloadLength", plain.size()},
                     {"payloadBase64", base64(plain)},
                     {"wireSequence", be32(f, 9)}}}});
        auto cipher = aes(padded, up ? server_key_ : client_key_, true);
        std::copy(cipher.begin(), cipher.end(), f.begin() + h);
    } catch (...) {
        OPENSSL_cleanse(plain.data(), plain.size());
        OPENSSL_cleanse(padded.data(), padded.size());
        throw;
    }
    OPENSSL_cleanse(plain.data(), plain.size());
    OPENSSL_cleanse(padded.data(), padded.size());
}
void TgcpSession::frame(bool up, Bytes f, Translation &out) {
    unsigned cmd = (unsigned(f[6]) << 8) | f[7];
    auto h = be32(f, 13);
    // f[8] 方向标记（0=上行 1=下行）只用于断言，不参与解密：translate() 用 up 参数选 key。
    // 实测服务器会主动下发 f[8]=0 的帧；旧实现 throw 后 capture.cpp 把连接标记 dead，
    // 游戏 TCP 65010 被掐断 => 雷达全瞎。改为只记诊断，继续按 socket 方向解密。
    if (f[8] != (up ? 0 : 1) && sink_)
        sink_({{"type", "tcp_direction_mismatch"},
               {"data", {{"wireDirection", int(f[8])}, {"socketUp", up ? 1 : 0}, {"cmd", cmd}}}});
    auto hello = [&]() {
        need(h >= 88 && f[21] == 3 && f[22] == 0 && f[23] == 64, "unsupported DH hello");
        return View(f).subspan(24, 64);
    };
    if (state_ == 0) {
        need(up && cmd == 0x1001 && be32(f, 17) == 0, "expected empty client hello");
        client_key_ = client_.derive(hello());
        auto pub = server_.public_key();
        std::copy(pub.begin(), pub.end(), f.begin() + 24);
        state_ = 1;
        append(out.to_server, f);
        if (sink_)
            sink_({{"type", "dh_client_replaced"}});
        return;
    }
    if (state_ == 1) {
        if (up) {
            need(cmd != 0x1001, "duplicate client hello");
            need(pending_bytes_ + f.size() <= 2 * 1024 * 1024, "pending hello limit");
            pending_bytes_ += f.size();
            pending_.push_back(std::move(f));
            return;
        }
        need(cmd == 0x1002, "expected server hello");
        server_key_ = server_.derive(hello());
        auto pub = client_.public_key();
        std::copy(pub.begin(), pub.end(), f.begin() + 24);
        state_ = 2;
        translate(false, f);
        append(out.to_client, f);
        if (sink_)
            sink_({{"type", "dh_established"}});
        while (!pending_.empty()) {
            auto p = std::move(pending_.front());
            pending_.pop_front();
            translate(true, p);
            append(out.to_server, p);
        }
        pending_bytes_ = 0;
        return;
    }
    need(cmd != 0x1001 && cmd != 0x1002, "unexpected repeated DH hello");
    translate(up, f);
    append(up ? out.to_server : out.to_client, f);
}
Translation TgcpSession::pass_through(const std::string &reason) {
    need(state_ == 0, "cannot bypass after DH substitution");
    Translation out;
    out.to_server.swap(buffers_[0]);
    out.to_client.swap(buffers_[1]);
    if (!bypass_ && sink_)
        sink_({{"type", "tcp_passthrough"}, {"data", {{"reason", reason}}}});
    bypass_ = true;
    return out;
}
Translation TgcpSession::feed(bool up, View data) {
    Translation out;
    if (bypass_) {
        append(up ? out.to_server : out.to_client, data);
        return out;
    }
    auto &buf = buffers_[up ? 0 : 1];
    need(buf.size() + data.size() <= 17 * 1024 * 1024, "TGCP stream buffer limit");
    append(buf, data);
    if (state_ == 0 && !up && !data.empty())
        return pass_through("server_first");
    if (state_ == 0 && buf.size() > 1024 * 1024)
        return pass_through("prefix_limit");
    while (!buf.empty()) {
        auto n = std::min(buf.size(), magic.size());
        if (!std::equal(buf.begin(), buf.begin() + n, magic.begin())) {
            need(state_ == 0, "TGCP framing lost after DH substitution");
            return pass_through("non_tgcp");
        }
        if (buf.size() < 21)
            break;
        auto h = be32(buf, 13), b = be32(buf, 17);
        if (state_ == 0) {
            // Sniff before committing: unsupported protocols keep their original bytes.
            if (h < 88 || h > 65536 || b > 16777216 || buf[6] != 0x10 || buf[7] != 1 || buf[8] != 0 || b != 0)
                return pass_through("non_tgcp");
            if (buf.size() >= 24 && (buf[21] != 3 || buf[22] != 0 || buf[23] != 64))
                return pass_through("non_tgcp");
            if (buf.size() >= size_t(h) + b && !client_.valid_public(View(buf).subspan(24, 64)))
                return pass_through("non_tgcp");
            if (buf.size() == 1024 * 1024 && buf.size() < size_t(h) + b)
                return pass_through("prefix_limit");
        }
        need(h >= 21 && h <= 65536 && b <= 16777216, "TGCP frame length");
        if (buf.size() < size_t(h) + b)
            break;
        Bytes f(buf.begin(), buf.begin() + h + b);
        buf.erase(buf.begin(), buf.begin() + h + b);
        frame(up, std::move(f), out);
    }
    return out;
}
} // namespace radaer
