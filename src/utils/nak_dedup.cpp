#include "nak_dedup.h"

namespace srtla::utils {

uint64_t NakDeduplicator::hash_nak_payload(const uint8_t *buffer, int length, int prefix_bytes) {
    if (length <= 16) {
        return 0;
    }

    const uint8_t *payload = buffer + 16;
    size_t payload_length = static_cast<size_t>(length - 16);
    if (prefix_bytes >= 0 && static_cast<size_t>(prefix_bytes) < payload_length) {
        payload_length = static_cast<size_t>(prefix_bytes);
    }

    uint64_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < payload_length; ++i) {
        hash ^= static_cast<uint64_t>(payload[i]);
        hash *= FNV_PRIME;
    }

    return hash;
}

bool NakDeduplicator::should_accept_nak(std::unordered_map<uint64_t, NakHashEntry> &cache,
                                        uint64_t hash,
                                        uint64_t current_time_ms) {
    auto it = cache.find(hash);
    if (it == cache.end()) {
        cache.emplace(hash, NakHashEntry{current_time_ms, 0});
        return true;
    }

if (current_time_ms < it->second.timestamp_ms) {
        // Clock moved backwards, treat as within suppression window
        return false;
    }
    
    if (current_time_ms - it->second.timestamp_ms < SUPPRESS_MS) {
        return false;
    }

    if (it->second.repeat_count >= MAX_REPEATS) {
        return false;
    }

    it->second.timestamp_ms = current_time_ms;
    ++it->second.repeat_count;
    return true;
}

} // namespace srtla::utils
