#include "core/esd_catalog.hpp"

#include <algorithm>
#include <cctype>

namespace litewin {

namespace {
bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}
} // namespace

std::string_view to_string(WindowsVersion ver) {
    switch (ver) {
    case WindowsVersion::Win11_26H2:
        return "Windows 11 26H2 (Recommended)";
    case WindowsVersion::Win11_25H2:
        return "Windows 11 25H2";
    case WindowsVersion::Win11_24H2:
        return "Windows 11 24H2";
    case WindowsVersion::Win10_22H2:
        return "Windows 10 22H2";
    }
    return "Unknown";
}

std::optional<WindowsVersion> parse_windows_version(std::string_view str) {
    if (str == "win11_26h2" || str == "26h2" || str == "Windows 11 26H2") {
        return WindowsVersion::Win11_26H2;
    }
    if (str == "win11_25h2" || str == "25h2" || str == "Windows 11 25H2") {
        return WindowsVersion::Win11_25H2;
    }
    if (str == "win11_24h2" || str == "24h2" || str == "Windows 11 24H2") {
        return WindowsVersion::Win11_24H2;
    }
    if (str == "win10_22h2" || str == "22h2" || str == "Windows 10 22H2") {
        return WindowsVersion::Win10_22H2;
    }
    return std::nullopt;
}

static const std::vector<EsdTarget> g_known_targets = {
    // Windows 11 26H2 (Default / Recommended)
    {
        .version = WindowsVersion::Win11_26H2,
        .version_name = "Windows 11 26H2 (Recommended)",
        .language_code = "ko-kr",
        .language_name = "Korean (한국어)",
        .architecture = "x64",
        .file_name = "26300.9457.260913-1737.26h2_ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_ko-kr.esd",
        .download_url = "http://dl.delivery.mp.microsoft.com/filestreamingservice/files/a1e7a893-29cf-4dfb-98c8-bbb126b4a9ec/26300.9457.260913-1737.26h2_ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_ko-kr.esd",
        .size_bytes = 6042869216ULL,
        .sha1_hash = "",
        .sha256_hash = "8ad8b47100725744d541c10f76cdc78a054d0f5eb2ecf60ad99d015c17da60d4",
        .encrypted = false,
    },
    {
        .version = WindowsVersion::Win11_26H2,
        .version_name = "Windows 11 26H2 (Recommended)",
        .language_code = "en-us",
        .language_name = "English (United States)",
        .architecture = "x64",
        .file_name = "26300.9457.260913-1737.26h2_ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_en-us.esd",
        .download_url = "http://dl.delivery.mp.microsoft.com/filestreamingservice/files/3d9e62bb-e92f-443e-94da-fc4e03021048/26300.9457.260913-1737.26h2_ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_en-us.esd",
        .size_bytes = 6205178813ULL,
        .sha1_hash = "",
        .sha256_hash = "f70f55d199fcfde2b19303254bd6853d04c089e5c2aa2907de3e3a44001c1300",
        .encrypted = false,
    },

    // Windows 11 24H2
    {
        .version = WindowsVersion::Win11_24H2,
        .version_name = "Windows 11 24H2",
        .language_code = "ko-kr",
        .language_name = "Korean (한국어)",
        .architecture = "x64",
        .file_name = "26100.4349.250607-1500.ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_ko-kr.esd",
        .download_url = "http://dl.delivery.mp.microsoft.com/filestreamingservice/files/68319007-ebd8-401b-bece-299f3cb76357/26100.4349.250607-1500.ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_ko-kr.esd",
        .size_bytes = 4516705975ULL,
        .sha1_hash = "2bb9bc4cb05b166d8efb183ba19d462ec5b5e73d",
        .sha256_hash = "",
        .encrypted = false,
    },
    {
        .version = WindowsVersion::Win11_24H2,
        .version_name = "Windows 11 24H2",
        .language_code = "en-us",
        .language_name = "English (United States)",
        .architecture = "x64",
        .file_name = "26100.4349.250607-1500.ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_en-us.esd",
        .download_url = "http://dl.delivery.mp.microsoft.com/filestreamingservice/files/009d9a0d-8e1a-45ce-9540-21377534803e/26100.4349.250607-1500.ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_en-us.esd",
        .size_bytes = 4680320173ULL,
        .sha1_hash = "8ceab2838f8e90180ac7490e8752157fb05588cb",
        .sha256_hash = "",
        .encrypted = false,
    },

    // Windows 10 22H2
    {
        .version = WindowsVersion::Win10_22H2,
        .version_name = "Windows 10 22H2",
        .language_code = "ko-kr",
        .language_name = "Korean (한국어)",
        .architecture = "x64",
        .file_name = "19045.2965.230505-1139.22h2_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_ko-kr.esd",
        .download_url = "http://dl.delivery.mp.microsoft.com/filestreamingservice/files/10db8ee5-6e16-49ac-bc94-902aeffee618/19045.2965.230505-1139.22h2_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_ko-kr.esd",
        .size_bytes = 3856872178ULL,
        .sha1_hash = "",
        .sha256_hash = "",
        .encrypted = false,
    },
};

EsdTarget get_default_esd_target() {
    return g_known_targets[0]; // Win11 26H2 ko-kr x64
}

std::optional<EsdTarget> find_esd_target(
    WindowsVersion version,
    std::string_view language_code,
    std::string_view architecture
) {
    for (const auto& target : g_known_targets) {
        if (target.version == version &&
            iequals(target.language_code, language_code) &&
            iequals(target.architecture, architecture)) {
            return target;
        }
    }
    return std::nullopt;
}

std::vector<WindowsVersion> get_supported_versions() {
    return {
        WindowsVersion::Win11_26H2, // Default
        WindowsVersion::Win11_25H2,
        WindowsVersion::Win11_24H2,
        WindowsVersion::Win10_22H2,
    };
}

std::vector<std::string> get_supported_architectures(WindowsVersion version) {
    if (version == WindowsVersion::Win10_22H2) {
        return {"x64", "arm64", "x86"};
    }
    // Windows 11 is 64-bit only
    return {"x64", "arm64"};
}

std::vector<std::string> get_supported_languages(
    WindowsVersion version,
    std::string_view architecture
) {
    std::vector<std::string> langs;
    for (const auto& target : g_known_targets) {
        if (target.version == version && target.architecture == architecture) {
            if (std::find(langs.begin(), langs.end(), target.language_code) == langs.end()) {
                langs.push_back(target.language_code);
            }
        }
    }
    return langs;
}

} // namespace litewin
