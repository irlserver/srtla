#pragma once

/*
    srtla_rec - JSON Metrics Writer

    Periodically writes per-group and per-connection metrics to a JSON file.
    The Go Manager (StreamStudio) or any monitoring tool can read this file
    to display real-time connection stats in the web UI.

    Copyright (C) 2025 IRLServer.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

#include <spdlog/spdlog.h>

#include "../connection/connection_registry.h"
#include "../security/rate_limiter.h"

extern "C" {
#include "../common.h"
}

namespace srtla::metrics {

class MetricsWriter {
public:
    explicit MetricsWriter(const std::string &filepath)
        : filepath_(filepath), tmp_filepath_(filepath + ".tmp") {}

    /// Write current metrics to the JSON file.
    /// Uses write-to-temp + rename for atomic updates (no partial reads).
    void write(const connection::ConnectionRegistry &registry,
               const security::RateLimiter &rate_limiter,
               std::size_t pending_groups,
               time_t now) {
        // Throttle writes to every METRICS_WRITE_PERIOD seconds
        if ((now - last_write_) < METRICS_WRITE_PERIOD) {
            return;
        }
        last_write_ = now;

        std::ofstream out(tmp_filepath_);
        if (!out.is_open()) {
            spdlog::warn("[metrics] Cannot open metrics file: {}", tmp_filepath_);
            return;
        }

        out << "{\n";
        out << "  \"timestamp\": " << now << ",\n";
        out << "  \"total_groups\": " << registry.groups().size() << ",\n";
        out << "  \"pending_groups\": " << pending_groups << ",\n";
        out << "  \"rate_limiter_entries\": " << rate_limiter.size() << ",\n";
        out << "  \"groups\": [\n";

        bool first_group = true;
        for (const auto &group : registry.groups()) {
            if (!first_group) out << ",\n";
            first_group = false;

            // Group ID as hex string (first 16 bytes for readability)
            char id_hex[33];
            for (int i = 0; i < 16; i++) {
                snprintf(id_hex + i * 2, 3, "%02x",
                         static_cast<unsigned char>(group->id()[i]));
            }
            id_hex[32] = '\0';

            out << "    {\n";
            out << "      \"id\": \"" << id_hex << "\",\n";
            out << "      \"authenticated\": " << (group->is_authenticated() ? "true" : "false") << ",\n";
            out << "      \"stream_id\": \"" << escape_json(group->stream_id()) << "\",\n";
            out << "      \"created_at\": " << group->created_at() << ",\n";
            out << "      \"age_seconds\": " << (now - group->created_at()) << ",\n";
            out << "      \"srt_socket\": " << group->srt_socket() << ",\n";
            out << "      \"connections\": [\n";

            bool first_conn = true;
            for (const auto &conn : group->connections()) {
                if (!first_conn) out << ",\n";
                first_conn = false;

                auto *addr_ptr = const_cast<struct sockaddr *>(
                    reinterpret_cast<const struct sockaddr *>(&conn->address()));
                const char *ip = print_addr(addr_ptr);
                int port = port_no(addr_ptr);

                const auto &stats = conn->stats();
                time_t last_ago = now - conn->last_received();

                out << "        {\n";
                out << "          \"ip\": \"" << (ip ? ip : "unknown") << "\",\n";
                out << "          \"port\": " << port << ",\n";
                out << "          \"packets_received\": " << stats.packets_received << ",\n";
                out << "          \"bytes_received\": " << stats.bytes_received << ",\n";
                out << "          \"packets_lost\": " << stats.packets_lost << ",\n";
                out << "          \"rtt_ms\": " << stats.rtt_ms << ",\n";
                out << "          \"nack_count\": " << stats.nack_count << ",\n";
                out << "          \"weight_percent\": " << static_cast<int>(stats.weight_percent) << ",\n";
                out << "          \"error_points\": " << stats.error_points << ",\n";
                out << "          \"sender_bitrate_bps\": " << stats.sender_bitrate_bps << ",\n";
                out << "          \"window\": " << stats.window << ",\n";
                out << "          \"in_flight\": " << stats.in_flight << ",\n";
                out << "          \"last_received_ago_s\": " << last_ago << ",\n";
                out << "          \"recovery_active\": " << (conn->recovery_start() > 0 ? "true" : "false") << "\n";
                out << "        }";
            }

            out << "\n      ]\n";
            out << "    }";
        }

        out << "\n  ]\n";
        out << "}\n";
        out.close();

        // Atomic rename: guarantees readers never see partial JSON
        std::rename(tmp_filepath_.c_str(), filepath_.c_str());
    }

private:
    static std::string escape_json(const std::string &s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:   result += c; break;
            }
        }
        return result;
    }

    std::string filepath_;
    std::string tmp_filepath_;
    time_t last_write_ = 0;

    static constexpr int METRICS_WRITE_PERIOD = 2; // Write every 2 seconds
};

} // namespace srtla::metrics
