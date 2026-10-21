#include "radaer/webui.hpp"
#include <stdexcept>
namespace radaer {
DiscoveryRange discovery_range(uint32_t host, unsigned prefix) {
    if (prefix > 32 || (host >> 24) == 0 || (host >> 24) == 127 || (host >> 24) >= 224)
        throw std::runtime_error("invalid discovery IPv4/prefix");
    bool limited = prefix < 20;
    unsigned bits = limited ? 24 : prefix;
    uint32_t mask = bits == 0 ? 0 : 0xffffffffU << (32 - bits);
    uint32_t first = host & mask, last = first | ~mask;
    if (bits < 31) {
        ++first;
        --last;
    }
    return {first, last, bits, limited};
}
} // namespace radaer
