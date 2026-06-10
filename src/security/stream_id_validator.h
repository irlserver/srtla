#pragma once

/*
    srtla_rec - Anti-DoS: StreamID Extraction & Validation

    Extracts the SRT StreamID from SRT Conclusion handshake packets and
    validates it against an authorized list loaded from a configuration file.
    Supports hot-reload via SIGHUP.

    The StreamID is carried in SRT handshake extension type 5 (SRT_CMD_SID),
    which is only present in the Conclusion phase of the SRT handshake
    (handshake_type = 0xFFFFFFFD).

    Copyright (C) 2025 IRLServer.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace srtla::security {

// SRT handshake constants
inline constexpr uint32_t SRT_HS_TYPE_CONCLUSION = 0xFFFFFFFD;
inline constexpr uint16_t SRT_CMD_SID = 5;
inline constexpr int SRT_HANDSHAKE_MIN_SIZE = 64;

class StreamIdValidator {
public:
    /// Load authorized StreamIDs from a file (one per line).
    /// Lines starting with '#' are treated as comments.
    /// Returns true if the file was loaded successfully.
    bool load(const std::string &filepath) {
        filepath_ = filepath;
        return reload();
    }

    /// Reload the StreamID list from the previously configured file.
    /// Can be called from a SIGHUP handler.
    bool reload() {
        if (filepath_.empty()) {
            return false;
        }

        std::ifstream file(filepath_);
        if (!file.is_open()) {
            spdlog::warn("[security] Cannot open StreamID file: {}", filepath_);
            return false;
        }

        std::vector<std::string> new_ids;
        std::string line;
        while (std::getline(file, line)) {
            // Trim whitespace
            auto start = line.find_first_not_of(" \t\r\n");
            auto end = line.find_last_not_of(" \t\r\n");
            if (start == std::string::npos) {
                continue;
            }
            std::string trimmed = line.substr(start, end - start + 1);
            // Skip empty lines and comments
            if (!trimmed.empty() && trimmed[0] != '#') {
                new_ids.push_back(trimmed);
            }
        }

        authorized_ids_ = std::move(new_ids);
        enabled_ = true;
        spdlog::info("[security] Loaded {} authorized StreamID(s) from {}",
                     authorized_ids_.size(), filepath_);
        return true;
    }

    /// Returns true if StreamID validation is enabled.
    bool is_enabled() const { return enabled_; }

    /// Validate a StreamID against the authorized list.
    /// Returns true if validation is disabled or the StreamID is authorized.
    bool validate(const std::string &stream_id) const {
        if (!enabled_) {
            return true; // Validation disabled — allow all
        }
        if (stream_id.empty()) {
            return false; // Empty StreamID is never valid when validation is on
        }
        for (const auto &id : authorized_ids_) {
            if (id == stream_id) {
                return true;
            }
        }
        return false;
    }

    /// Extract the StreamID from an SRT packet buffer.
    /// Returns the StreamID string, or empty string if:
    /// - The packet is not an SRT control packet
    /// - The packet is not a Conclusion handshake
    /// - No StreamID extension (SRT_CMD_SID) is present
    static std::string extract_stream_id(const char *buf, int len) {
        if (len < SRT_HANDSHAKE_MIN_SIZE) {
            return "";
        }

        const auto *p = reinterpret_cast<const uint8_t *>(buf);

        // Check: SRT control packet (bit 0 of first byte set)
        if ((p[0] & 0x80) == 0) {
            return "";
        }

        // Check: Handshake type (control type = 0x0000, meaning first 2 bytes = 0x80 0x00)
        if (p[0] != 0x80 || p[1] != 0x00) {
            return "";
        }

        // Read handshake_type at offset 36 (big-endian uint32)
        uint32_t hs_type = (uint32_t(p[36]) << 24) | (uint32_t(p[37]) << 16) |
                           (uint32_t(p[38]) << 8) | uint32_t(p[39]);

        // Only Conclusion handshakes carry the StreamID extension
        if (hs_type != SRT_HS_TYPE_CONCLUSION) {
            return "";
        }

        // Check ext_field at offset 22 — if 0, no extensions present
        uint16_t ext_field = (uint16_t(p[22]) << 8) | uint16_t(p[23]);
        if (ext_field == 0) {
            return "";
        }

        // Walk the extension blocks starting at offset 64
        int offset = SRT_HANDSHAKE_MIN_SIZE;
        while (offset + 4 <= len) {
            uint16_t ext_type = (uint16_t(p[offset]) << 8) | uint16_t(p[offset + 1]);
            uint16_t ext_len = (uint16_t(p[offset + 2]) << 8) | uint16_t(p[offset + 3]);
            int ext_bytes = ext_len * 4; // Length is in 32-bit words

            if (ext_bytes <= 0 || offset + 4 + ext_bytes > len) {
                break;
            }

            if (ext_type == SRT_CMD_SID) {
                // The StreamID string is stored with per-word big-endian encoding.
                // We need to swap each 4-byte group to recover the original string.
                std::string result(ext_bytes, '\0');
                for (int i = 0; i < ext_bytes; i += 4) {
                    int remaining = std::min(4, ext_bytes - i);
                    // Read the 4-byte word and reverse byte order
                    for (int j = 0; j < remaining && j < 4; j++) {
                        result[i + j] = static_cast<char>(p[offset + 4 + i + (3 - j)]);
                    }
                }

                // Trim trailing null bytes
                auto end = result.find_last_not_of('\0');
                if (end != std::string::npos) {
                    result.resize(end + 1);
                } else {
                    result.clear();
                }

                return result;
            }

            offset += 4 + ext_bytes;
        }

        return "";
    }

private:
    std::vector<std::string> authorized_ids_;
    std::string filepath_;
    bool enabled_ = false;
};

} // namespace srtla::security
