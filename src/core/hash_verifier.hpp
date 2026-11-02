#pragma once

#include <string>

namespace litewin {

// Computes SHA-256 hash of a file using 64 KiB chunks (streaming, minimal memory footprint)
// Returns lowercase hex string on success, empty string on failure.
[[nodiscard]] std::string compute_file_sha256(const std::string& filepath);

// Compares file SHA-256 against expected hash (case-insensitive)
[[nodiscard]] bool verify_file_sha256(
    const std::string& filepath,
    const std::string& expected_hex
);

} // namespace litewin
