#include "auth_rate_limiter.h"

#include <netinet/in.h>

#include <spdlog/spdlog.h>

#include "../receiver_config.h"

namespace srtla::utils {

std::string AuthRateLimiter::key_for(const struct sockaddr_storage &addr) {
    if (addr.ss_family == AF_INET6) {
        auto *a = reinterpret_cast<const struct sockaddr_in6 *>(&addr);
        return std::string("6:") +
               std::string(reinterpret_cast<const char *>(&a->sin6_addr), sizeof(a->sin6_addr));
    }

    auto *a = reinterpret_cast<const struct sockaddr_in *>(&addr);
    return std::string("4:") +
           std::string(reinterpret_cast<const char *>(&a->sin_addr), sizeof(a->sin_addr));
}

void AuthRateLimiter::record_failure(const struct sockaddr_storage &addr, time_t now) {
    auto &entry = entries_[key_for(addr)];

    if (entry.window_start == 0 || (now - entry.window_start) > AUTH_FAIL_WINDOW) {
        entry.window_start = now;
        entry.failures = 0;
    }

    entry.failures++;
    if (entry.failures >= AUTH_FAIL_THRESHOLD) {
        entry.blocked_until = now + AUTH_FAIL_COOLDOWN;
        entry.failures = 0;
        entry.window_start = 0;
        spdlog::warn("Source IP throttled after {} SRT auth failures (cooldown {}s)",
                     AUTH_FAIL_THRESHOLD, AUTH_FAIL_COOLDOWN);
    }
}

bool AuthRateLimiter::is_blocked(const struct sockaddr_storage &addr, time_t now) const {
    auto it = entries_.find(key_for(addr));
    if (it == entries_.end()) {
        return false;
    }
    return it->second.blocked_until > now;
}

void AuthRateLimiter::cleanup(time_t now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        const Entry &e = it->second;
        bool blocked = e.blocked_until > now;
        bool window_active = e.window_start != 0 && (now - e.window_start) <= AUTH_FAIL_WINDOW;
        if (!blocked && !window_active) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace srtla::utils
