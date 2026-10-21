// Synthetic test fixture only. Never installs a driver or touches real packets.
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define WINDIVERTEXPORT extern __declspec(dllexport)
#include "windivert/windivert.h"
namespace {
struct Source {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false, sniff = false;
    WINDIVERT_LAYER layer;
    std::deque<std::vector<char>> packets;
};
Source *network_source = nullptr;
std::mutex log_mutex;
} // namespace
extern "C" {
HANDLE WinDivertOpen(const char *filter, WINDIVERT_LAYER layer, INT16, UINT64 flags) {
    bool sniff = layer == WINDIVERT_LAYER_NETWORK_FORWARD &&
                 flags == (WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    bool tcp = std::getenv("RADAER_MOCK_TCP") && flags == 0 &&
               (layer == WINDIVERT_LAYER_NETWORK_FORWARD || layer == WINDIVERT_LAYER_NETWORK);
    const char *path = std::getenv(sniff ? "RADAER_MOCK_PACKETS" : "RADAER_MOCK_TCP");
    if (!path || (!sniff && !tcp)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    auto source = std::make_unique<Source>();
    source->sniff = sniff;
    source->layer = layer;
    if (layer == WINDIVERT_LAYER_NETWORK_FORWARD) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        for (uint32_t n = 0; file.read(reinterpret_cast<char *>(&n), 4);) {
            if (n > 65575) {
                SetLastError(ERROR_INVALID_DATA);
                return INVALID_HANDLE_VALUE;
            }
            std::vector<char> packet(n);
            file.read(packet.data(), n);
            if (!file) {
                SetLastError(ERROR_INVALID_DATA);
                return INVALID_HANDLE_VALUE;
            }
            source->packets.push_back(std::move(packet));
        }
    } else {
        network_source = source.get();
    }
    if (const char *log = std::getenv("RADAER_MOCK_FILTERS")) {
        std::lock_guard lock(log_mutex);
        std::ofstream(log, std::ios::app) << int(layer) << ' ' << flags << ' ' << filter << '\n';
    }
    return reinterpret_cast<HANDLE>(source.release());
}
BOOL WinDivertRecv(HANDLE h, VOID *data, UINT capacity, UINT *size, WINDIVERT_ADDRESS *address) {
    auto *source = reinterpret_cast<Source *>(h);
    std::unique_lock lock(source->mutex);
    if (source->packets.empty() && source->sniff && std::getenv("RADAER_MOCK_RECEIVE_ERROR")) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    source->changed.wait(lock, [&] { return source->stopped || !source->packets.empty(); });
    if (source->stopped) {
        SetLastError(ERROR_NO_DATA);
        return FALSE;
    }
    auto packet = std::move(source->packets.front());
    source->packets.pop_front();
    if (packet.size() > capacity) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(data, packet.data(), packet.size());
    *size = UINT(packet.size());
    *address = {};
    LARGE_INTEGER stamp{};
    QueryPerformanceCounter(&stamp);
    address->Timestamp = stamp.QuadPart;
    address->Layer = source->layer;
    address->Sniffed = source->sniff;
    address->Outbound = source->layer == WINDIVERT_LAYER_NETWORK;
    return TRUE;
}
BOOL WinDivertSend(HANDLE h, const VOID *data, UINT size, UINT *written, const WINDIVERT_ADDRESS *address) {
    auto *source = reinterpret_cast<Source *>(h);
    // UDP-copy mode must never call Send; surface any regression as capture failure.
    const char *log = std::getenv("RADAER_MOCK_SENT");
    if (source->sniff || !log || !std::getenv("RADAER_MOCK_TCP")) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    {
        std::lock_guard lock(log_mutex);
        std::ofstream out(log, std::ios::binary | std::ios::app);
        out.write(reinterpret_cast<const char *>(&size), 4);
        out.write(reinterpret_cast<const char *>(address), sizeof(*address));
        out.write(reinterpret_cast<const char *>(data), size);
        if (!out) {
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
    }
    // Emulate the listener's SYN+ACK entirely in memory to exercise return rewriting.
    if (address->Layer == WINDIVERT_LAYER_NETWORK && !address->Outbound && size == 40 && network_source) {
        auto p = static_cast<const char *>(data);
        std::vector<char> reply(p, p + size);
        std::swap_ranges(reply.begin() + 12, reply.begin() + 16, reply.begin() + 16);
        std::swap_ranges(reply.begin() + 20, reply.begin() + 22, reply.begin() + 22);
        reply[33] = 0x12;
        std::lock_guard lock(network_source->mutex);
        network_source->packets.push_back(std::move(reply));
        network_source->changed.notify_one();
    }
    *written = size;
    return TRUE;
}
BOOL WinDivertShutdown(HANDLE h, WINDIVERT_SHUTDOWN) {
    auto *source = reinterpret_cast<Source *>(h);
    std::lock_guard lock(source->mutex);
    source->stopped = true;
    source->changed.notify_all();
    return TRUE;
}
BOOL WinDivertClose(HANDLE h) {
    auto *source = reinterpret_cast<Source *>(h);
    if (source == network_source)
        network_source = nullptr;
    delete source;
    return TRUE;
}
BOOL WinDivertSetParam(HANDLE, WINDIVERT_PARAM, UINT64) {
    return TRUE;
}
BOOL WinDivertGetParam(HANDLE, WINDIVERT_PARAM param, UINT64 *value) {
    *value = (param == WINDIVERT_PARAM_VERSION_MAJOR || param == WINDIVERT_PARAM_VERSION_MINOR) ? 2 : 0;
    return TRUE;
}
BOOL WinDivertHelperCalcChecksums(VOID *, UINT, WINDIVERT_ADDRESS *, UINT64) {
    return TRUE;
}
BOOL WinDivertHelperCompileFilter(const char *, WINDIVERT_LAYER, char *, UINT, const char **, UINT *) {
    return TRUE;
}
}
