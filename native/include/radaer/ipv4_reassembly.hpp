#pragma once
#include "core.hpp"
#include <mutex>
#include <tuple>

namespace radaer {
// Reassembles captured copies only. This class never forwards or injects packets.
class Ipv4UdpReassembler {
  public:
    struct Limits {
        size_t datagrams = 128, bytes = 4 * 1024 * 1024, fragments_per_datagram = 128;
        int64_t timeout_ms = 5000;
    };
    enum class Status { pending, complete, rejected };
    struct Result {
        Status status = Status::pending;
        Bytes packet;
        std::string reason;
        int64_t timestamp = 0;
        unsigned fragments = 0;
    };
    Ipv4UdpReassembler();
    explicit Ipv4UdpReassembler(Limits limits);
    // now is monotonic receipt time; timestamp is the capture's wall-clock time.
    Result push(View packet, int64_t now, int64_t timestamp);
    // Includes timeouts detected by push() since the previous expire() call.
    size_t expire(int64_t now);
    Json stats() const;

  private:
    using Key = std::tuple<uint32_t, uint32_t, uint16_t>;
    struct Part {
        Bytes bytes;
        bool more = false;
    };
    struct Datagram {
        int64_t created = 0, timestamp = 0;
        Bytes header;
        std::map<size_t, Part> parts;
        std::optional<size_t> end;
        size_t bytes = 0;
    };
    struct Recent {
        Bytes payload;
        int64_t completed = 0;
    };
    Limits limits_;
    mutable std::mutex mutex_;
    std::map<Key, Datagram> pending_;
    std::map<Key, int64_t> rejected_;
    std::map<Key, Recent> recent_;
    size_t bytes_ = 0;
    size_t recent_bytes_ = 0, unreported_timeouts_ = 0;
    uint64_t fragments_ = 0, completed_ = 0, duplicates_ = 0, dropped_ = 0, timed_out_ = 0;
    void erase(const Key &key);
    void forget_recent(const Key &key);
    Result reject(const Key &key, int64_t now, const char *reason);
};
} // namespace radaer
