#include "radaer/monitor.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace radaer;
int checks = 0;
void check(bool v, const char *message) {
    ++checks;
    if (!v)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f, const char *message) {
    bool thrown = false;
    try {
        f();
    } catch (const std::exception &) {
        thrown = true;
    }
    check(thrown, message);
}
Bytes data(size_t n, int seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; i++)
        b[i] = uint8_t(i * 37 + seed * 19);
    return b;
}
Bytes envelope(unsigned seq) {
    Bytes b(10);
    b[1] = uint8_t(seq);
    b[2] = uint8_t(seq >> 8);
    b[9] = 12;
    return b;
}
Bytes wire(bool up, unsigned cmd, View body, View pub = {}) {
    size_t h = pub.empty() ? 21 : 88;
    Bytes b(h + body.size());
    auto magic = unhex("3366000b000c");
    std::copy(magic.begin(), magic.end(), b.begin());
    b[6] = uint8_t(cmd >> 8);
    b[7] = uint8_t(cmd);
    b[8] = up ? 0 : 1;
    put_be32(b, 9, 1);
    put_be32(b, 13, uint32_t(h));
    put_be32(b, 17, uint32_t(body.size()));
    if (!pub.empty()) {
        b[21] = 3;
        b[23] = 64;
        std::copy(pub.begin(), pub.end(), b.begin() + 24);
    }
    std::copy(body.begin(), body.end(), b.begin() + h);
    return b;
}
int main() {
    try {
        Key key{};
        auto raw = data(16, 7);
        std::copy(raw.begin(), raw.end(), key.begin());
        Bank bank{};
        raw = data(128, 19);
        std::copy(raw.begin(), raw.end(), bank.begin());
        for (size_t n = 0; n < 160; n++) {
            auto p = data(n, 3);
            check(unpad(aes(encrypt(p, key), key, false)) == p, "AES padding roundtrip");
            check(unbase64(base64(p)) == p, "base64 roundtrip");
            check(udp_crypt(udp_crypt(p, bank, true), bank) == p, "XTEA roundtrip including tail");
        }
        rejects([&] { unpad(Bytes(16)); }, "invalid padding");
        rejects([&] { unbase64("AAAA===="); }, "invalid base64");
        check(extract_banks(Bytes{}).empty(), "empty bank extraction");
        auto token = base64(bank);
        std::string message = "bank=" + token + ";end";
        Bytes plain(message.begin(), message.end());
        check(extract_banks(plain).size() == 1, "canonical bank extraction");
        auto noncanonical = token;
        noncanonical[170] = 'B';
        check(extract_banks(Bytes(noncanonical.begin(), noncanonical.end())).empty(),
              "noncanonical bank rejected");
        Dh a(unhex("12d687")), b(unhex("74cbb1"));
        check(a.derive(b.public_key()) == b.derive(a.public_key()), "DH agreement");
        rejects([&] { a.derive(unhex("01")); }, "bad public rejected");
        Packet packet{false, data(16, 4), 1234, "client:1", "server:2", 7, 2, 3};
        auto metadata = udp_raw_event(packet, false);
        check(metadata["type"] == "udp_raw" && metadata["data"]["payloadLength"] == 16,
              "UDP metadata records payload length");
        check(!metadata["data"].contains("payloadBase64"), "UDP payload is omitted by default");
        auto raw = udp_raw_event(packet, true);
        check(raw["data"].contains("payloadBase64") &&
                  unbase64(raw["data"]["payloadBase64"].get<std::string>()) == packet.payload,
              "explicit UDP payload logging preserves bytes");
        std::vector<Json> events;
        int found = 0;
        TgcpSession session([&](Json e) { events.push_back(e); },
                            [&](const Bank &k) {
                                check(k == bank, "observed bank");
                                found++;
                            });
        auto client_hello = wire(true, 0x1001, {}, a.public_key());
        check(session.feed(true, View(client_hello).first(15)).to_server.empty(),
              "fragmented hello buffered");
        auto s = session.feed(true, View(client_hello).subspan(15)).to_server;
        auto server_key = b.derive(View(s).subspan(24, 64));
        auto response =
            session.feed(false, wire(false, 0x1002, encrypt(plain, server_key), b.public_key())).to_client;
        auto client_key = a.derive(View(response).subspan(24, 64));
        check(unpad(aes(View(response).subspan(88), client_key, false)) == plain,
              "encrypted server hello translated");
        check(found == 1 && session.established(), "DH established and bank extracted");
        auto up = wire(true, 0x2001, encrypt(data(43, 7), client_key));
        auto request = session.feed(true, up).to_server;
        check(unpad(aes(View(request).subspan(21), server_key, false)) == data(43, 7),
              "client-to-server translation");
        auto down = wire(false, 0x2002, encrypt(data(29, 9), server_key));
        auto answer = session.feed(false, down).to_client;
        check(unpad(aes(View(answer).subspan(21), client_key, false)) == data(29, 9),
              "server-to-client translation");
        rejects([&] { session.feed(true, Bytes{1}); }, "no silent fallback after substitution");
        TgcpSession pass;
        Bytes http{'G', 'E', 'T'};
        check(pass.feed(true, http).to_server == http && pass.bypass(), "non TGCP passthrough");
        for (auto payload :
             {unhex("16030300050102030405"), wire(true, 0x2001, {}), wire(true, 0x1001, {}, Bytes(64))}) {
            TgcpSession other;
            auto result = other.feed(true, payload);
            check(other.bypass() && result.to_server == payload, "unsupported first frame preserved");
        }
        for (int variant = 0; variant < 8; ++variant) {
            auto hello = client_hello;
            if (variant == 0)
                hello[21] = 2;
            if (variant == 1)
                hello[23] = 63;
            if (variant == 2)
                hello[8] = 1;
            if (variant == 3)
                put_be32(hello, 13, 20);
            if (variant == 4)
                put_be32(hello, 13, 65537);
            if (variant == 5)
                put_be32(hello, 17, 16777217);
            if (variant == 6)
                std::fill(hello.begin() + 24, hello.end(), 255);
            if (variant == 7) {
                std::fill(hello.begin() + 24, hello.end(), 0);
                hello.back() = 1;
            }
            TgcpSession other;
            Bytes received;
            for (auto byte : hello) {
                auto result = other.feed(true, View(&byte, 1));
                received.insert(received.end(), result.to_server.begin(), result.to_server.end());
            }
            check(other.bypass() && received == hello && !other.buffered(),
                  "fragmented non-mode3 passthrough");
        }
        for (const char *reason : {"prefix_timeout", "client_eof"}) {
            TgcpSession incomplete;
            auto prefix = View(client_hello).first(12);
            check(incomplete.feed(true, prefix).to_server.empty(), "prefix awaits classification");
            check(incomplete.pass_through(reason).to_server == Bytes(prefix.begin(), prefix.end()),
                  "timeout/EOF flushes undecided prefix");
            check(incomplete.pass_through(reason).to_server.empty(), "prefix flush is idempotent");
        }
        TgcpSession server_first;
        server_first.feed(true, View(client_hello).first(3));
        auto banner = unhex("3366000b000c");
        auto first = server_first.feed(false, banner);
        check(first.to_client == banner &&
                  first.to_server == Bytes(client_hello.begin(), client_hello.begin() + 3) &&
                  server_first.bypass(),
              "server first flushes both directions even with TGCP magic");
        TgcpSession committed;
        committed.feed(true, client_hello);
        check(committed.committed() && !committed.established(), "commit starts at client replacement");
        rejects([&] { committed.pass_through("prefix_timeout"); }, "no fallback while awaiting server DH");
        rejects([&] { committed.feed(false, http); }, "invalid server after replacement fails closed");
        check(valid_envelope(envelope(1)), "valid envelope fixture");
        auto invalid = envelope(1);
        invalid[9] = 0;
        check(!valid_envelope(invalid), "truncated trailer");
        Bytes initial(33);
        initial[8] = 1;
        initial[32] = 8;
        check(initial_handshake(initial), "initial handshake");
        initial[9] = 1;
        check(!initial_handshake(initial), "noninitial handshake");
        events.clear();
        BankBroker broker([&](Json e) { events.push_back(e); });
        broker.candidate(bank, "test", 1000);
        for (unsigned i = 1; i <= 3; i++)
            broker.packet({false, udp_crypt(envelope(i), bank, true), 1000 + int64_t(i), "a:1", "b:2", i});
        auto count = [&](const std::string &t) {
            return std::count_if(events.begin(), events.end(), [&](const Json &e) { return e["type"] == t; });
        };
        check(count("bank_bound") == 1 && count("udp_plaintext") == 3,
              "bind after three distinct valid packets");
        Bytes handshake(25);
        handshake[0] = 1;
        handshake.back() = 8;
        auto before_handshake = count("bank_use");
        broker.packet({false, udp_crypt(handshake, bank, true), 1500, "a:1", "b:2", 100});
        check(count("bank_use") == before_handshake, "handshake shape cannot prove bank use");
        Bank newer = bank;
        newer[0] ^= 0x61;
        broker.candidate(newer, "test", 2000);
        for (unsigned i = 4; i <= 7; i++)
            broker.packet({false, udp_crypt(envelope(i), newer, true), 2000 + int64_t(i), "a:1", "b:2", i});
        check(count("bank_bound") == 2, "reset and rebind new key");
        for (int i = 0; i < 1200; ++i)
            broker.packet({false, Bytes{0, 1, 2}, 2100 + i, "a:1", "noise:" + std::to_string(i)});
        auto uses_before_noise = count("bank_use");
        broker.packet({false, udp_crypt(envelope(8), newer, true), 3400, "a:1", "b:2"});
        check(count("bank_use") == uses_before_noise + 1 && count("bank_bound") == 2,
              "UDP churn beyond capacity preserves the active bank and capture");
        for (unsigned i = 1; i <= 3; ++i)
            broker.packet({false, udp_crypt(envelope(i), bank, true), 3500 + int64_t(i), "a:3", "b:4"});
        check(count("bank_bound") == 3, "new session can bind after unrelated UDP fills capacity");
        // All slots validated: overload must preserve active flows; stale slots
        // can later retire and the same endpoint must get a fresh epoch.
        std::vector<Json> capacity_events;
        BankBroker capacity([&](Json e) { capacity_events.push_back(std::move(e)); });
        capacity.candidate(bank, "test", 1000);
        auto feed = [&](std::string remote, int64_t ts) {
            for (unsigned i = 1; i <= 3; ++i)
                capacity.packet({false, udp_crypt(envelope(i), bank, true), ts + i, "a", remote});
        };
        for (int i = 0; i < 256; ++i) feed(std::to_string(i), 1000);
        feed("overflow", 2000);
        check(std::count_if(capacity_events.begin(), capacity_events.end(), [](const Json &e) {
            return e["type"] == "bank_bound";
        }) == 256, "full active capacity does not terminate capture or evict live bindings");
        feed("overflow", 62000);
        feed("0", 63000);
        check(capacity_events.back()["data"]["epoch"].get<uint64_t>() > 256,
              "retired endpoint rebind uses a fresh epoch");
        broker.gap(3000, "test_gap");
        check(count("capture_gap") == 1, "gap breaks binding");
        std::vector<Json> dup;
        BankBroker duplicate([&](Json e) { dup.push_back(e); });
        duplicate.candidate(bank, "test", 1000);
        for (int i = 0; i < 8; i++)
            duplicate.packet({false, udp_crypt(envelope(1), bank, true), 1000 + i, "a", "b", uint64_t(i)});
        check(std::none_of(dup.begin(), dup.end(), [](const Json &e) { return e["type"] == "bank_bound"; }),
              "duplicate datagrams cannot prove bank");
        std::vector<Json> ambiguous_events;
        BankBroker ambiguous([&](Json e) { ambiguous_events.push_back(e); });
        Bank partial_bank = bank;
        partial_bank[127] ^= 1;
        ambiguous.candidate(bank, "test", 1000);
        ambiguous.candidate(partial_bank, "test", 1000);
        for (unsigned i = 1; i <= 4; i++)
            ambiguous.packet({false, udp_crypt(envelope(i), bank, true), 1000 + int64_t(i), "a", "b", i});
        check(std::none_of(ambiguous_events.begin(), ambiguous_events.end(),
                           [](const Json &e) { return e["type"] == "bank_bound"; }),
              "ambiguous bank evidence must not bind");
        PlayerLimits limits;
        limits.max_speed = 100;
        PlayerValidator player(limits);
        auto sample = [](int seq, int64_t ts, double x) {
            return Json{{"type", "player_sample"},
                        {"session", "s"},
                        {"timestampMs", ts},
                        {"data",
                         {{"flow", "f"},
                          {"epoch", 1},
                          {"track", "slot:1"},
                          {"poseSequence", seq},
                          {"timestampMs", ts},
                          {"x", x},
                          {"y", 0},
                          {"z", 0},
                          {"identitySource", "heuristic"}}}};
        };
        check(player.inspect(sample(1, 1000, 0))["data"]["status"] == "warming_up", "player warmup");
        player.inspect(sample(2, 1100, 1));
        auto validation = player.inspect(sample(3, 1200, 2));
        check(validation["data"]["status"] == "plausible" &&
                  !validation["data"]["identityVerified"].get<bool>(),
              "position does not prove identity");
        check(player.inspect(sample(4, 1300, 1000))["data"]["status"] == "rejected",
              "unrealistic speed rejected");
        check(player.inspect(sample(3, 1200, 2))["data"]["status"] == "rejected", "duplicate pose rejected");
        auto prefix_sample = sample(4, 1300, 1000);
        prefix_sample["data"]["decodeCompleteness"] = "known_prefix";
        auto prefix_result = player.inspect(prefix_sample);
        check(prefix_result["data"]["status"] == "rejected",
              "partial coordinates still enforce speed bounds");
        check(prefix_result["data"]["flags"].size() == 2,
              "partial coverage and rejection reported separately");
        prefix_sample["data"]["x"] = 3;
        prefix_sample["data"]["handshakeOk"] = false;
        check(player.inspect(prefix_sample)["data"]["status"] == "rejected",
              "partial coordinates still require Welcome");
        // A prior channel error breaks continuity, not the next independently decoded block.
        PlayerValidator recovery(limits);
        for (int i = 1; i <= 3; ++i) recovery.inspect(sample(i, 900 + 100 * i, i));
        auto recovered = sample(4, 1300, 4);
        recovered["data"]["semanticErrorsSincePrevious"] = 2;
        recovered["data"]["decodeCompleteness"] = "complete";
        recovered["data"]["movementFrameComplete"] = true;
        recovered["data"]["movementBlockComplete"] = true;
        auto reset = recovery.inspect(recovered)["data"];
        check(reset["status"] == "warming_up" && reset["consecutivePlausibleSamples"] == 1 &&
                  reset["flags"] == Json::array({"decoder_continuity_reset"}),
              "complete position after historical error starts a usable continuity baseline");
        check(recovery.inspect(sample(5, 1400, 5))["data"]["status"] == "warming_up" &&
                  recovery.inspect(sample(6, 1500, 6))["data"]["status"] == "plausible",
              "recovery builds a new plausible streak");
        auto packet_recovery = recovered;
        packet_recovery["data"].erase("semanticErrorsSincePrevious");
        packet_recovery["data"]["packetErrorsSincePrevious"] = 1;
        check(PlayerValidator(limits).inspect(packet_recovery)["data"]["status"] == "warming_up",
              "new complete packet recovers from prior packet framing errors");
        for (int variant = 0; variant < 4; ++variant) {
            auto partial = recovered;
            if (variant == 0) partial["data"].erase("decodeCompleteness");
            if (variant == 1) partial["data"]["decodeCompleteness"] = "known_prefix";
            if (variant == 2) partial["data"]["movementFrameComplete"] = false;
            if (variant == 3) partial["data"]["movementBlockComplete"] = false;
            check(PlayerValidator(limits).inspect(partial)["data"]["status"] == "rejected",
                  "incomplete or legacy sample cannot recover historical decoder errors");
        }
        for (int variant = 0; variant < 7; ++variant) {
            PlayerLimits bounded = limits;
            bounded.max_abs_coordinate = 100;
            PlayerValidator guarded(bounded);
            guarded.inspect(sample(3, 1200, 3));
            auto invalid = recovered;
            if (variant == 0) invalid["data"]["handshakeOk"] = false;
            if (variant == 1) invalid["data"]["x"] = 99; // Within world bounds, excessive speed.
            if (variant == 2) invalid["data"]["x"] = 101;
            if (variant == 3) invalid["data"]["poseSequence"] = 3;
            if (variant == 4) invalid["data"]["timestampMs"] = 1100;
            if (variant == 5) invalid["timestampMs"] = 1299;
            if (variant == 6) invalid["timestampMs"] = 5000;
            check(guarded.inspect(invalid)["data"]["status"] == "rejected",
                  "historical-error recovery preserves handshake, speed, world and time checks");
        }
        auto first_packet = sample(1, 1000, 0);
        first_packet["data"]["sourcePacketOrdinal"] = 10;
        auto next_packet = sample(2, 1000, .05);
        next_packet["data"]["sourcePacketOrdinal"] = 11;
        PlayerValidator clock_precision(limits);
        clock_precision.inspect(first_packet);
        auto coincident = clock_precision.inspect(next_packet)["data"];
        check(coincident["status"] == "warming_up" && coincident["timestampMs"] == 1000 &&
                  coincident["intervalSource"] == "capture_packet_order_same_millisecond" &&
                  coincident["speedLowerBoundUnitsPerSecond"] == 50 && !coincident.contains("speedUnitsPerSecond"),
              "ordered packets sharing a millisecond update coordinates without inventing time");
        check(clock_precision.inspect(next_packet)["data"]["status"] == "rejected",
              "same-millisecond recovery never accepts a repeated pose");
        for (int variant = 0; variant < 5; ++variant) {
            PlayerValidator ordered(limits);
            ordered.inspect(first_packet);
            auto invalid = next_packet;
            if (variant == 0) invalid["data"]["sourcePacketOrdinal"] = 10;
            if (variant == 1) invalid["data"]["sourcePacketOrdinal"] = 9;
            if (variant == 2) invalid["data"].erase("sourcePacketOrdinal");
            if (variant == 3) invalid["data"]["timestampMs"] = 999;
            if (variant == 4) invalid["data"]["x"] = 1;
            check(ordered.inspect(invalid)["data"]["status"] == "rejected",
                  "coincident-time acceptance requires ordered packets and preserves speed limits");
        }
        auto path = (std::filesystem::temp_directory_path() /
                     ("radaer-native-test-" + std::to_string(now_ms()) + ".jsonl"))
                        .string();
        {
            Journal j(path, "synthetic-device", true);
            j.emit({{"type", "bank_use"}, {"data", {{"flow", "x"}, {"fingerprint", "y"}}}});
        }
        auto ordered_path = path + ".ordered";
        {
            Journal queued(ordered_path, "writer-test", true);
            for (int i = 0; i < 5000; ++i)
                queued.emit({{"type", "writer_test"}, {"data", {{"index", i}, {"text", std::string(120, 'x')}}}});
            queued.finish();
            std::ifstream input(ordered_path);
            std::string line;
            int index = 0;
            while (std::getline(input, line)) {
                const auto event = Json::parse(line);
                check(event["sequence"] == index + 1 && event["data"]["index"] == index,
                      "async journal drains all events in sequence order");
                ++index;
            }
            check(index == 5000, "finish flushes queued events before returning");
            rejects([&] { queued.emit({{"type", "too_late"}}); }, "finished journal rejects late writes");
        }
        auto limited_path = path + ".limited";
        {
            Journal limited(limited_path, "limited-writer", true, {}, true, 1024);
            limited.emit({{"type", "small"}});
            std::string error;
            try {
                limited.emit({{"type", "oversized"}, {"data", {{"text", std::string(1024, 'x')}}}});
            } catch (const std::exception &e) {
                error = e.what();
            }
            check(error.find("size limit") != std::string::npos,
                  "journal rejects events that exceed the configured size limit");
            check(error.find(limited_path) != std::string::npos && error.find("--max-log-bytes") != std::string::npos,
                  "journal size-limit error identifies the file and recovery option");
            limited.finish();
            check(std::filesystem::file_size(limited_path) <= 1024,
                  "journal size limit keeps the file within the configured bound");
        }
        std::filesystem::remove(limited_path);
        auto invalid_path = path + ".invalid";
        rejects([&] { Journal invalid(invalid_path, "invalid-writer", true, {}, true, min_journal_max_bytes - 1); },
                "journal rejects a size limit below the minimum");
        check(!std::filesystem::exists(invalid_path), "invalid journal limit creates no output");
        auto blocked_parent = path + ".blocked";
        {
            std::ofstream blocker(blocked_parent);
            blocker << "not a directory";
        }
        std::string create_error;
        try {
            Journal blocked(blocked_parent + "/session.jsonl", "blocked-writer", true);
        } catch (const std::exception &e) {
            create_error = e.what();
        }
        check(create_error.find("cannot create journal directory") != std::string::npos &&
                  create_error.find("free") != std::string::npos && create_error.find("disable logging") != std::string::npos,
              "journal creation failure reports path capacity context and recovery advice");
        std::filesystem::remove(blocked_parent);
        std::filesystem::remove(ordered_path);
        {
            Json observed = Json::array();
            Journal disabled(path, "memory-device", true, [&](Json e) { observed.push_back(e); }, false);
            disabled.emit({{"type", "player_sample"}});
            disabled.emit({{"type", "session_stopped"}});
            check(observed.size() == 2 && observed[1]["sequence"] == 2 &&
                  observed[0]["session"] == disabled.session() && observed[0].contains("timestampMs"),
                  "disabled logging still delivers ordered live events and metadata");
            Journal absent(path + ".disabled", "memory-device", true, {}, false);
            absent.emit({{"type", "session_started"}});
            check(!std::filesystem::exists(path + ".disabled"), "disabled logging creates no journal");
        }
        auto report = bank_report({path});
        check(report["flows"].empty() && report["excludedSyntheticOrUnclassifiedEvents"] == 1,
              "synthetic excluded from empirical report");
        std::filesystem::remove(path);
        std::cout << "passed " << checks << " assertions (synthetic only)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED after " << checks << ": " << e.what() << '\n';
        return 1;
    }
}
