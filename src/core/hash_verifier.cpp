#include "core/hash_verifier.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <sstream>

namespace litewin {

namespace {

struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const noexcept {
        if (ctx) {
            EVP_MD_CTX_free(ctx);
        }
    }
};

using ScopedEvpMdCtx = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

std::string to_lower_hex(const unsigned char* hash, std::size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

} // namespace

std::string compute_file_sha256(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    ScopedEvpMdCtx ctx(EVP_MD_CTX_new());
    if (!ctx) {
        return "";
    }

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return "";
    }

    std::array<char, 65536> buffer{}; // 64 KiB streaming chunk
    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(file.gcount())) != 1) {
            return "";
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &hash_len) != 1) {
        return "";
    }

    return to_lower_hex(hash.data(), hash_len);
}

bool verify_file_sha256(
    const std::string& filepath,
    const std::string& expected_hex
) {
    if (expected_hex.empty()) {
        return false;
    }

    std::string actual = compute_file_sha256(filepath);
    if (actual.empty() || actual.size() != expected_hex.size()) {
        return false;
    }

    // Case-insensitive comparison
    return std::equal(
        actual.begin(), actual.end(),
        expected_hex.begin(),
        [](char a, char b) noexcept {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        }
    );
}

} // namespace litewin
