#include "radaer/core.hpp"
#include <algorithm>
#include <chrono>
#include <climits>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdexcept>
namespace radaer {
namespace {
void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
using BN = std::unique_ptr<BIGNUM, decltype(&BN_clear_free)>;
BN number(View b) {
    return BN(BN_bin2bn(b.data(), int(b.size()), nullptr), BN_clear_free);
}
BN fresh() {
    return BN(BN_new(), BN_clear_free);
}
const char *prime_hex = "97981e0aade0de72e29cd2789193562547bad2591fb7e59c523923dddecebccae49bdcd4b8ec39022b0b"
                        "d95d4af8fd3cd600919393255c1084c2fd5abb2fede3";
uint32_t le(View b, size_t p) {
    return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) | (uint32_t(b[p + 2]) << 16) |
           (uint32_t(b[p + 3]) << 24);
}
void put_le(Bytes &b, size_t p, uint32_t n) {
    for (int j = 0; j < 4; j++)
        b[p + j] = uint8_t(n >> (8 * j));
}
bool alphabet(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
}
} // namespace
Bytes unhex(const std::string &s) {
    require(s.size() % 2 == 0, "odd hex length");
    Bytes b;
    auto digit = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        throw std::runtime_error("invalid hex");
    };
    for (size_t i = 0; i < s.size(); i += 2)
        b.push_back(uint8_t(digit(s[i]) * 16 + digit(s[i + 1])));
    return b;
}
std::string hex(View b) {
    std::string s;
    for (auto x : b) {
        s += "0123456789abcdef"[x >> 4];
        s += "0123456789abcdef"[x & 15];
    }
    return s;
}
std::string base64(View b) {
    require(b.size() < INT_MAX / 2, "base64 input too large");
    std::string s(4 * ((b.size() + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(s.data()), b.data(), int(b.size()));
    s.resize(n);
    return s;
}
Bytes unbase64(const std::string &s) {
    if (s.empty())
        return {};
    require(s.size() % 4 == 0 && s.size() < INT_MAX, "invalid base64 length");
    Bytes b(s.size() / 4 * 3);
    int n = EVP_DecodeBlock(b.data(), reinterpret_cast<const unsigned char *>(s.data()), int(s.size()));
    require(n >= 0, "invalid base64");
    if (s.back() == '=')
        --n;
    if (s.size() > 1 && s[s.size() - 2] == '=')
        --n;
    require(n >= 0, "invalid base64 padding");
    b.resize(n);
    require(base64(b) == s, "noncanonical base64");
    return b;
}
std::string fingerprint(View b) {
    std::array<uint8_t, 32> out{};
    unsigned n = 0;
    require(EVP_Digest(b.data(), b.size(), out.data(), &n, EVP_sha256(), nullptr) == 1 && n == 32,
            "SHA256 failure");
    return hex(out);
}
struct Dh::Impl {
    BN secret = fresh(), prime = number(unhex(prime_hex)), pub = fresh();
};
Dh::Dh() : p_(std::make_unique<Impl>()) {
    std::array<uint8_t, 64> s{};
    require(RAND_bytes(s.data(), int(s.size())) == 1, "random failure");
    s[0] = (s[0] & 0x7f) | 0x40;
    p_->secret = number(s);
    OPENSSL_cleanse(s.data(), s.size());
    BN_set_flags(p_->secret.get(), BN_FLG_CONSTTIME);
    auto g = fresh();
    BN_set_word(g.get(), 2);
    auto ctx = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>(BN_CTX_new(), BN_CTX_free);
    require(BN_mod_exp(p_->pub.get(), g.get(), p_->secret.get(), p_->prime.get(), ctx.get()) == 1,
            "DH failure");
}
Dh::Dh(View s) : p_(std::make_unique<Impl>()) {
    require(!s.empty() && s.size() <= 64, "DH secret length");
    p_->secret = number(s);
    require(!BN_is_zero(p_->secret.get()), "DH zero secret");
    BN_set_flags(p_->secret.get(), BN_FLG_CONSTTIME);
    auto g = fresh();
    BN_set_word(g.get(), 2);
    auto ctx = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>(BN_CTX_new(), BN_CTX_free);
    require(BN_mod_exp(p_->pub.get(), g.get(), p_->secret.get(), p_->prime.get(), ctx.get()) == 1,
            "DH failure");
}
Dh::~Dh() = default;
Bytes Dh::public_key() const {
    Bytes b(64);
    require(BN_bn2binpad(p_->pub.get(), b.data(), 64) == 64, "DH public encode");
    return b;
}
bool Dh::valid_public(View peer) const {
    if (peer.empty() || peer.size() > 64)
        return false;
    auto x = number(peer), limit = fresh();
    BN_copy(limit.get(), p_->prime.get());
    BN_sub_word(limit.get(), 1);
    return BN_cmp(x.get(), BN_value_one()) > 0 && BN_cmp(x.get(), limit.get()) < 0;
}
Key Dh::derive(View peer) const {
    require(valid_public(peer), "DH peer outside bounds");
    auto x = number(peer), shared = fresh();
    auto ctx = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>(BN_CTX_new(), BN_CTX_free);
    require(BN_mod_exp(shared.get(), x.get(), p_->secret.get(), p_->prime.get(), ctx.get()) == 1,
            "DH derive");
    Bytes bytes(BN_num_bytes(shared.get()));
    BN_bn2bin(shared.get(), bytes.data());
    Key k{};
    unsigned n = 0;
    int ok = EVP_Digest(bytes.data(), bytes.size(), k.data(), &n, EVP_md5(), nullptr);
    OPENSSL_cleanse(bytes.data(), bytes.size());
    require(ok == 1 && n == 16, "protocol MD5 unavailable");
    return k;
}
Bytes pad(View b) {
    size_t n = (b.size() % 16 < 11 ? 16 : 32) - b.size() % 16;
    Bytes r(b.begin(), b.end());
    r.resize(r.size() + n);
    std::copy_n("tsf4g", 5, r.end() - 6);
    r.back() = uint8_t(n);
    return r;
}
Bytes unpad(View b) {
    require(!b.empty() && b.size() % 16 == 0, "AES alignment");
    size_t n = b.back();
    require(n >= 6 && n <= 21 && n <= b.size(), "AES pad length");
    require(std::equal(b.end() - 6, b.end() - 1, "tsf4g"), "AES pad marker");
    size_t len = b.size() - n;
    require(n == (len % 16 < 11 ? 16 : 32) - len % 16, "AES pad residue");
    return Bytes(b.begin(), b.begin() + len);
}
Bytes aes(View b, const Key &k, bool enc) {
    require(!b.empty() && b.size() % 16 == 0 && b.size() < INT_MAX, "AES input alignment");
    std::array<uint8_t, 16> iv{};
    for (size_t i = 0; i < 16; i++)
        iv[i] = uint8_t(i);
    auto c = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(EVP_CIPHER_CTX_new(),
                                                                             EVP_CIPHER_CTX_free);
    require(c != nullptr, "AES allocation");
    require(EVP_CipherInit_ex(c.get(), EVP_aes_128_cbc(), nullptr, k.data(), iv.data(), enc ? 1 : 0) == 1,
            "AES init");
    EVP_CIPHER_CTX_set_padding(c.get(), 0);
    Bytes out(b.size() + 16);
    int n = 0, m = 0;
    require(EVP_CipherUpdate(c.get(), out.data(), &n, b.data(), int(b.size())) == 1 &&
                EVP_CipherFinal_ex(c.get(), out.data() + n, &m) == 1,
            "AES operation");
    out.resize(n + m);
    return out;
}
Bytes encrypt(View b, const Key &k) {
    auto p = pad(b);
    auto out = aes(p, k, true);
    OPENSSL_cleanse(p.data(), p.size());
    return out;
}
Bytes udp_crypt(View b, const Bank &bank, bool enc) {
    Bytes r(b.begin(), b.end());
    for (size_t i = 0; i + 8 <= b.size(); i += 8) {
        uint32_t a = le(b, i), c = le(b, i + 4), sum = enc ? 0 : 0xc6ef3720u;
        size_t k = (i / 8 % 8) * 16;
        for (int j = 0; j < 32; j++) {
            if (enc) {
                a += (((c << 4) ^ (c >> 5)) + c) ^ (sum + le(bank, k + (sum & 3) * 4));
                sum += 0x9e3779b9u;
                c += (((a << 4) ^ (a >> 5)) + a) ^ (sum + le(bank, k + ((sum >> 11) & 3) * 4));
            } else {
                c -= (((a << 4) ^ (a >> 5)) + a) ^ (sum + le(bank, k + ((sum >> 11) & 3) * 4));
                sum -= 0x9e3779b9u;
                a -= (((c << 4) ^ (c >> 5)) + c) ^ (sum + le(bank, k + (sum & 3) * 4));
            }
        }
        put_le(r, i, a);
        put_le(r, i + 4, c);
    }
    return r;
}
std::vector<Bank> extract_banks(View b) {
    std::vector<Bank> out;
    std::string s(b.begin(), b.end());
    for (size_t i = 0; i + 172 <= s.size(); i++) {
        if (i && alphabet(s[i - 1]))
            continue;
        if (s[i + 171] != '=' || (i + 172 < s.size() && (alphabet(s[i + 172]) || s[i + 172] == '=')))
            continue;
        if (!std::all_of(s.begin() + i, s.begin() + i + 171, alphabet))
            continue;
        try {
            auto decoded = unbase64(s.substr(i, 172));
            if (decoded.size() == 128) {
                Bank bank{};
                std::copy(decoded.begin(), decoded.end(), bank.begin());
                out.push_back(bank);
            }
        } catch (const std::exception &) {
        }
    }
    return out;
}
uint32_t be32(View b, size_t p) {
    if (p + 4 > b.size())
        throw std::runtime_error("short u32");
    return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 | uint32_t(b[p + 2]) << 8 | b[p + 3];
}
void put_be32(Bytes &b, size_t p, uint32_t n) {
    for (int i = 0; i < 4; i++)
        b.at(p + i) = uint8_t(n >> (24 - i * 8));
}
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace radaer
