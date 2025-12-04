#pragma once

#include <cstdint>
#include <unordered_map>

namespace srtla::utils {

struct NakHashEntry {
    uint64_t timestamp_ms = 0;
    int repeat_count = 0;
};

class NakDeduplicator {
public:
    static uint64_t hash_nak_payload(const uint8_t *buffer, int length, int prefix_bytes = -1);
    static bool should_accept_nak(std::unordered_map<uint64_t, NakHashEntry> &cache,
                                  uint64_t hash,
                                  uint64_t current_time_ms);

private:
    static constexpr uint64_t FNV_OFFSET_BASIS = 1469598103934665603ull;
    static constexpr uint64_t FNV_PRIME = 1099511628211ull;
    static constexpr uint64_t SUPPRESS_MS = 100;
    static constexpr int MAX_REPEATS = 1;
};

} // namespace srtla::utils
