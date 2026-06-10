#pragma once

/*
    srtla_rec - Anti-DoS: IP-based Rate Limiter

    Limits the number of REG1 (group registration) packets per source IP
    within a configurable time window. Prevents attackers from flooding
    the receiver with connection requests.

    Copyright (C) 2025 IRLServer.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <ctime>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "../receiver_config.h"

namespace srtla::security {

struct RateEntry {
    int count = 0;
    time_t window_start = 0;
};

class RateLimiter {
public:
    /// Returns true if the request from this IP should be allowed.
    /// Returns false if the rate limit has been exceeded.
    bool allow(const std::string &ip, time_t now) {
        auto &entry = entries_[ip];

        // Reset window if expired
        if ((now - entry.window_start) >= REG1_RATE_WINDOW) {
            entry.count = 0;
            entry.window_start = now;
        }

        entry.count++;

        if (entry.count > REG1_RATE_LIMIT) {
            spdlog::warn("[security] Rate limit exceeded for IP {}: {}/{} in {}s window",
                         ip, entry.count, REG1_RATE_LIMIT, REG1_RATE_WINDOW);
            return false;
        }

        return true;
    }

    /// Remove entries that haven't been seen in 2x the rate window.
    /// Call this periodically (e.g., from cleanup_inactive).
    void cleanup(time_t now) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if ((now - it->second.window_start) >= REG1_RATE_WINDOW * 2) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t size() const { return entries_.size(); }

private:
    std::unordered_map<std::string, RateEntry> entries_;
};

} // namespace srtla::security
