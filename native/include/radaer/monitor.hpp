#pragma once
#include "core.hpp"
#include <fstream>
#include <mutex>
#include <cstddef>
#include <utility>
namespace radaer {
inline constexpr size_t min_journal_max_bytes = 1024;
inline constexpr size_t default_journal_max_bytes = 64 * 1024 * 1024;
inline constexpr size_t max_journal_max_bytes = size_t(2) * 1024 * 1024 * 1024;
struct Packet {
    bool up = false;
    Bytes payload;
    int64_t timestamp = 0;
    std::string local, remote;
    uint64_t ordinal = 0;
    unsigned ip_fragments = 1;
    int64_t reassembly_delay_ms = 0;
};
Json udp_raw_event(const Packet &packet, bool save_payload, const char *timestamp_source = "capture");
class BankBroker {
    struct Candidate {
        Bank bank;
        std::string fp;
    };
    struct Flow {
        std::optional<Candidate> bound;
        std::deque<Packet> history;
        uint64_t epoch = 0;
        unsigned failures = 0;
        int64_t last_seen = 0;
    };
    std::deque<Candidate> candidates_;
    std::map<std::string, Flow> flows_;
    uint64_t next_epoch_ = 0;
    Sink sink_;
    void attempt(const std::string &key, Flow &f);
    void emit_plain(const Packet &p, Flow &f);
    void reset(const std::string &key, Flow &f, const std::string &why, int64_t ts);

  public:
    explicit BankBroker(Sink sink) : sink_(std::move(sink)) {}
    ~BankBroker();
    void candidate(const Bank &bank, const std::string &source, int64_t ts);
    void packet(Packet p);
    void gap(int64_t ts, const std::string &reason, int64_t since = 0);
};
class Journal {
    struct Writer;
    std::ofstream out_;
    std::unique_ptr<Writer> writer_;
    std::string session_, device_;
    bool synthetic_;
    uint64_t seq_ = 0;
    std::mutex mutex_;
    Sink observer_;

  public:
    Journal(const std::string &path, std::string device, bool synthetic, Sink observer = {}, bool save_logs = true,
            size_t max_bytes = default_journal_max_bytes);
    ~Journal();
    void finish();
    void emit(Json event);
    const std::string &session() const {
        return session_;
    }
};
struct PlayerLimits {
    double max_abs_coordinate = 0, max_speed = 0;
    int64_t max_age_ms = 2000, max_gap_ms = 2000;
    unsigned minimum_samples = 3;
};
class PlayerValidator {
    struct Last {
        double x, y, z;
        int64_t ts;
        uint64_t seq;
        unsigned good;
        double game_time = 0;
        uint64_t source_packet = 0;
    };
    std::map<std::string, Last> last_;
    PlayerLimits limits_;

  public:
    explicit PlayerValidator(PlayerLimits l = {}) : limits_(l) {}
    Json inspect(const Json &event);
};
Json bank_report(const std::vector<std::string> &files, int64_t max_gap_ms = 5000,
                 bool synthetic_only = false);
} // namespace radaer
