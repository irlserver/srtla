#pragma once

#include <ctime>
#include <string>
#include <unordered_map>

#include <sys/socket.h>

namespace srtla::utils {

// Tracks SRT authentication failures per source IP and refuses new group
// registrations from an address that keeps failing within a window.
//
// srtla_rec is a dumb SRT relay: the actual auth (streamid/passphrase) happens
// at the SRT server, whose rejection flows back through the relay. Counting
// those rejections lets us throttle a source that is brute forcing or otherwise
// repeatedly failing auth, without affecting legitimate broadcasters. Keys are
// IP-only (port stripped) so an attacker cannot evade by rotating source ports.
class AuthRateLimiter {
public:
    void record_failure(const struct sockaddr_storage &addr, time_t now);
    bool is_blocked(const struct sockaddr_storage &addr, time_t now) const;

    // Drops stale entries; call periodically from the cleanup loop.
    void cleanup(time_t now);

private:
    struct Entry {
        int failures = 0;
        time_t window_start = 0;
        time_t blocked_until = 0;
    };

    static std::string key_for(const struct sockaddr_storage &addr);

    std::unordered_map<std::string, Entry> entries_;
};

} // namespace srtla::utils
