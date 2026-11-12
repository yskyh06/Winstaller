#if __has_include("platform/windows_compatibility.hpp")
#include "platform/windows_compatibility.hpp"
#else
#include "../src/platform/windows_compatibility.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define TEST_REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            throw std::runtime_error(std::string("Requirement failed: ") + #expr + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

class TempFixture {
public:
    TempFixture() {
        auto base = std::filesystem::temp_directory_path();
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = base / ("litewin_compat_test_" + std::to_string(stamp));
        std::filesystem::create_directories(root_);
    }

    ~TempFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    const std::filesystem::path& root() const {
        return root_;
    }

    void write_text(const std::filesystem::path& rel, const std::string& text) {
        auto full = root_ / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream ofs(full);
        if (!ofs) {
            throw std::runtime_error("Failed creating test file: " + full.string());
        }
        ofs << text;
    }

    void write_binary(const std::filesystem::path& rel, const std::vector<uint8_t>& bytes) {
        auto full = root_ / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream ofs(full, std::ios::binary);
        if (!ofs) {
            throw std::runtime_error("Failed creating test binary: " + full.string());
        }
        ofs.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

private:
    std::filesystem::path root_;
};

void test_tpm2() {
    TempFixture fix;
    fix.write_text("sys/class/tpm/tpm0/tpm_version_major", "2\n");
    auto res = litewin::inspect_windows_compatibility(fix.root());
    TEST_REQUIRE(res.tpm == litewin::HardwareSupport::Supported);
    TEST_REQUIRE(!res.bypass_tpm());
}

void test_tpm1() {
    TempFixture fix;
    fix.write_text("sys/class/tpm/tpm0/tpm_version_major", "1\n");
    auto res = litewin::inspect_windows_compatibility(fix.root());
    TEST_REQUIRE(res.tpm == litewin::HardwareSupport::Unsupported);
    TEST_REQUIRE(res.bypass_tpm());
}

void test_tpm_none() {
    TempFixture fix;
    auto res = litewin::inspect_windows_compatibility(fix.root());
    TEST_REQUIRE(res.tpm == litewin::HardwareSupport::Unknown);
    TEST_REQUIRE(!res.bypass_tpm());
}

void test_tpm_malformed() {
    TempFixture fix;
    fix.write_text("sys/class/tpm/tpm0/tpm_version_major", "invalid\n");
    auto res = litewin::inspect_windows_compatibility(fix.root());
    TEST_REQUIRE(res.tpm == litewin::HardwareSupport::Unknown);
    TEST_REQUIRE(!res.bypass_tpm());
}

void test_tpm_conflicting() {
    {
        TempFixture fix;
        fix.write_text("sys/class/tpm/tpm0/tpm_version_major", "1\n");
        fix.write_text("sys/class/tpm/tpm1/tpm_version_major", "2\n");
        auto res = litewin::inspect_windows_compatibility(fix.root());
        TEST_REQUIRE(res.tpm == litewin::HardwareSupport::Supported);
    }
    {
        TempFixture fix;
        fix.write_text("sys/class/tpm/tpm0/tpm_version_major", "1\n");
        fix.write_text("sys/class/tpm/tpm1/tpm_version_major", "bad\n");
        auto res = litewin::inspect_windows_compatibility(fix.root());
        TEST_REQUIRE(res.tpm == litewin::HardwareSupport::Unknown);
    }
}

void test_efi_enabled() {
    TempFixture fix;
    fix.write_binary("sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c", {0, 0, 0, 0, 1});
    auto res = litewin::inspect_windows_compatibility(fix.root());
    TEST_REQUIRE(res.uefi == true);
    TEST_REQUIRE(res.secure_boot == litewin::HardwareSupport::Supported);
    TEST_REQUIRE(res.secure_boot_enabled.has_value());
    TEST_REQUIRE(*res.secure_boot_enabled == true);
    TEST_REQUIRE(!res.bypass_secure_boot());
}

void test_efi_disabled_still_capable() {
    TempFixture fix;
    fix.write_binary("sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c", {0, 0, 0, 0, 0});
    auto res = litewin::inspect_windows_compatibility(fix.root());
    TEST_REQUIRE(res.uefi == true);
    TEST_REQUIRE(res.secure_boot == litewin::HardwareSupport::Supported);
    TEST_REQUIRE(res.secure_boot_enabled.has_value());
    TEST_REQUIRE(*res.secure_boot_enabled == false);
    TEST_REQUIRE(!res.bypass_secure_boot());
}

void test_missing_efi_vars_unknown() {
    TempFixture fix;
    std::filesystem::create_directories(fix.root() / "sys/firmware/efi");
    auto res = litewin::inspect_windows_compatibility(fix.root());
    TEST_REQUIRE(res.uefi == true);
    TEST_REQUIRE(res.secure_boot == litewin::HardwareSupport::Unknown);
    TEST_REQUIRE(!res.secure_boot_enabled.has_value());
    TEST_REQUIRE(!res.bypass_secure_boot());
}

void test_legacy_unknown() {
    TempFixture fix;
    auto res = litewin::inspect_windows_compatibility(fix.root());
    TEST_REQUIRE(res.uefi == false);
    TEST_REQUIRE(res.secure_boot == litewin::HardwareSupport::Unknown);
    TEST_REQUIRE(!res.secure_boot_enabled.has_value());
    TEST_REQUIRE(!res.bypass_secure_boot());
}

void test_summary_and_bypasses() {
    litewin::WindowsCompatibility compat;
    compat.uefi = true;
    compat.tpm = litewin::HardwareSupport::Unsupported;
    compat.secure_boot = litewin::HardwareSupport::Unsupported;
    compat.secure_boot_enabled = false;

    TEST_REQUIRE(compat.bypass_tpm());
    TEST_REQUIRE(compat.bypass_secure_boot());

    auto summary = compat.summary();
    TEST_REQUIRE(!summary.empty());
    TEST_REQUIRE(summary.find("Notice") != std::string::npos);

    compat.uefi = false;
    TEST_REQUIRE(!compat.bypass_secure_boot());
}

int main() {
    try {
        test_tpm2();
        test_tpm1();
        test_tpm_none();
        test_tpm_malformed();
        test_tpm_conflicting();
        test_efi_enabled();
        test_efi_disabled_still_capable();
        test_missing_efi_vars_unknown();
        test_legacy_unknown();
        test_summary_and_bypasses();
        std::cout << "All Windows compatibility tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed: " << ex.what() << "\n";
        return 1;
    }
}
