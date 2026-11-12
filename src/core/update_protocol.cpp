#include "core/update_protocol.hpp"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <algorithm>
#include <fstream>
#include <memory>
#include <map>
#include <cctype>
#include <set>
#include <stdexcept>

namespace litewin {
namespace {
using Json = nlohmann::json;
void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
Json parse(const std::string& data, size_t maximum) {
    require(!data.empty() && data.size() <= maximum, "Metadata size limit exceeded");
    std::vector<std::set<std::string>> keys;
    return Json::parse(data, [&](int depth, Json::parse_event_t event, Json& value) {
        require(depth <= 16, "Metadata nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key)
            require(keys.back().insert(value.get<std::string>()).second, "Duplicate metadata key");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
std::string text(const Json& object, const char* key, size_t maximum = 160) {
    const auto value = object.at(key).get<std::string>();
    require(!value.empty() && value.size() <= maximum, "Invalid metadata string length");
    for (unsigned char c : value) require(c >= 32 && c <= 126, "Invalid metadata text");
    return value;
}
std::uint64_t integer(const Json& object, const char* key, std::uint64_t maximum = 2147483647) {
    const auto& v = object.at(key);
    require(v.is_number_unsigned() || (v.is_number_integer() && v.get<std::int64_t>() >= 0), "Invalid metadata integer");
    const auto n = v.get<std::uint64_t>(); require(n <= maximum, "Metadata integer exceeds limit"); return n;
}
bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 80 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    }) && value != "." && value != "..";
}
std::vector<unsigned char> unhex(const std::string& value, size_t size) {
    require(value.size() == size * 2, "Invalid hex length");
    std::vector<unsigned char> result;
    auto digit = [](char c) { if (c >= '0' && c <= '9') return c-'0'; if (c >= 'a' && c <= 'f') return c-'a'+10; if (c >= 'A' && c <= 'F') return c-'A'+10; return -1; };
    for (size_t i = 0; i < value.size(); i += 2) {
        const int a = digit(value[i]), b = digit(value[i+1]); require(a >= 0 && b >= 0, "Invalid hex value");
        result.push_back(static_cast<unsigned char>(a*16+b));
    }
    return result;
}
std::string base64(const std::string& input) {
    require(!input.empty() && input.size() % 4 == 0 && input.size() <= 1500000, "Invalid base64 size");
    std::string result(input.size(), '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()), reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
    require(n >= 0, "Invalid base64 data");
    if (input.back() == '=') --n;
    if (input.size() >= 2 && input[input.size()-2] == '=') --n;
    require(n >= 0, "Invalid base64 padding"); result.resize(static_cast<size_t>(n));
    std::string canonical(4*((result.size()+2)/3)+1,'\0');
    const int count = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(canonical.data()), reinterpret_cast<const unsigned char*>(result.data()), static_cast<int>(result.size()));
    canonical.resize(static_cast<size_t>(count)); require(canonical == input, "Noncanonical base64");
    return result;
}
Json payload(const std::string& envelope, const UpdateConfig& config, const std::string& role, std::int64_t now) {
    const auto outer = parse(envelope, 2*1024*1024);
    require(outer.is_object() && outer.size() == 3, "Invalid signed envelope");
    const auto id = text(outer, "key_id", 80);
    const auto key = std::find_if(config.keys.begin(), config.keys.end(), [&](const UpdateKey& k) {
        return k.id == id && std::find(k.roles.begin(), k.roles.end(), role) != k.roles.end();
    });
    require(key != config.keys.end(), "Untrusted signing key or role");
    const auto raw = unhex(key->public_hex, 32);
    const auto body = base64(outer.at("payload_base64").get<std::string>());
    const auto signature = base64(outer.at("signature_base64").get<std::string>());
    require(signature.size() == 64, "Invalid signature length");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw.data(), raw.size()), EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(pkey && context && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, pkey.get()) == 1 &&
        EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(), reinterpret_cast<const unsigned char*>(body.data()), body.size()) == 1, "Metadata signature rejected");
    auto value = parse(body, 1024*1024);
    require(text(value,"document_type") == role && integer(value,"schema_version") == 1, "Unsupported metadata schema");
    const auto published = integer(value,"published_at", 4102444800ULL);
    const auto expires = integer(value,"expires_at", 4102444800ULL);
    require(now > 0 && published <= static_cast<std::uint64_t>(now) && expires > static_cast<std::uint64_t>(now) && expires > published, "Metadata expired or clock is invalid");
    require(integer(value,"revision") > 0, "Invalid metadata revision");
    return value;
}
void relative_path(const std::string& path) {
    require(!path.empty() && path.front() != '/' && path.size() < 240 && path.find('\\') == std::string::npos, "Invalid image path");
    size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find('/',start); auto part = path.substr(start,end-start);
        require(!part.empty() && part != "." && part != "..", "Unsafe image path");
        if (end == std::string::npos) break;
        start = end+1;
    }
}
}

bool valid_update_url(const std::string& url, bool image) {
    if (url.size() > 4096 || url.find_first_of("\r\n\t ") != std::string::npos) return false;
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) return false;
    auto part = [&](CURLUPart name) {
        char* value = nullptr; curl_url_get(parsed.get(), name, &value, 0);
        std::string result = value ? value : ""; curl_free(value); return result;
    };
    if (part(CURLUPART_SCHEME) != "https" || !part(CURLUPART_FRAGMENT).empty()) return false;
    const auto port = part(CURLUPART_PORT); if (!port.empty() && port != "443") return false;
    auto host = part(CURLUPART_HOST);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c){return static_cast<char>(std::tolower(c));});
    static const std::set<std::string> github = {"raw.githubusercontent.com","github.com","release-assets.githubusercontent.com","objects.githubusercontent.com"};
    if (!image) return github.contains(host);
    if (host == "releases.ubuntu.com" || host == "cdimage.ubuntu.com" || host == "dl.delivery.mp.microsoft.com" || host == "download.windowsupdate.com" || host == "software.download.prss.microsoft.com") return true;
    return host.size() > std::string(".download.windowsupdate.com").size() && host.ends_with(".download.windowsupdate.com");
}

bool UpdateConfig::configured() const { return !keys.empty() && valid_update_url(catalog_url,false) && valid_update_url(release_url,false); }
UpdateConfig UpdateConfig::load(const std::string& path) {
    UpdateConfig c;
    std::ifstream in(path);
    if (!in) return c;
    std::string data(65537,'\0'); in.read(data.data(),static_cast<std::streamsize>(data.size())); data.resize(static_cast<size_t>(in.gcount()));
    const auto value = parse(data,65536);
    c.catalog_url = text(value,"catalog_url",4096); c.release_url = text(value,"release_url",4096);
    c.release_sequence = integer(value,"release_sequence");
    require(value.at("keys").is_array() && value.at("keys").size() <= 8,"Invalid trust keys");
    std::set<std::string> ids;
    for (const auto& k : value.at("keys")) {
        UpdateKey key; key.id = text(k,"id",80); key.public_hex = text(k,"public_hex",64); unhex(key.public_hex,32);
        require(ids.insert(key.id).second,"Duplicate trust key");
        key.roles = k.at("roles").get<std::vector<std::string>>();
        require(!key.roles.empty() && key.roles.size() <= 2,"Invalid key roles");
        for (const auto& role : key.roles) require(role == "catalog" || role == "release","Unknown key role");
        c.keys.push_back(std::move(key));
    }
    require(valid_update_url(c.catalog_url,false) && valid_update_url(c.release_url,false),"Invalid update endpoint");
    return c;
}
RemoteCatalog verify_catalog(const std::string& envelope, const UpdateConfig& config, std::int64_t now) {
    const auto value = payload(envelope,config,"catalog",now);
    require(integer(value,"min_catalog_api") <= 1,"Catalog requires a newer engine");
    RemoteCatalog result; result.revision = integer(value,"revision");
    result.published_at=integer(value,"published_at",4102444800ULL);result.expires_at=integer(value,"expires_at",4102444800ULL);
    const auto& targets = value.at("targets"); require(targets.is_array() && targets.size() <= 1000,"Invalid catalog targets");
    std::set<std::string> ids, variants;
    std::map<std::string,std::pair<std::string,std::string>> products;
    for (const auto& t : targets) {
        RemoteTarget target;
        target.id = text(t,"id",80); target.product = text(t,"product",80);
        require(identifier(target.id) && identifier(target.product) && ids.insert(target.id).second,"Invalid or duplicate target ID");
        target.name = text(t,"display_name",100); target.language = text(t,"language",20); target.language_name = text(t,"language_name",60);
        require(std::all_of(target.language.begin(),target.language.end(),[](unsigned char c){return std::isalnum(c) || c=='-';}) && variants.insert(target.product+"/"+target.language).second,"Duplicate product language");
        const auto family = text(t,"family",20);
        require(family == "windows" || family == "ubuntu","Unsupported OS family");
        target.windows = family == "windows"; target.tab = target.windows ? "Windows" : "Ubuntu";
        target.strategy = text(t,"install_strategy",50);
        require(target.strategy == (target.windows ? "windows-esd-uefi-v1" : "ubuntu-iso-uefi-v1"),"Unsupported installation strategy");
        auto [product_it, inserted] = products.emplace(target.product,std::make_pair(target.name,target.strategy));
        require(inserted || product_it->second == std::make_pair(target.name,target.strategy),"Inconsistent product variants");
        require(text(t,"architecture",10) == "x64","Unsupported image architecture");
        const auto& artifact = t.at("artifact");
        const auto url = text(artifact,"url",4096); require(valid_update_url(url,true),"Untrusted image URL");
        const auto hash = text(artifact,"sha256",64); unhex(hash,32);
        const auto size = integer(artifact,"size_bytes",32ULL*1024*1024*1024); require(size>0,"Invalid image size");
        const auto capabilities = t.at("requires_capabilities").get<std::vector<std::string>>();
        require(capabilities.size()<=16,"Too many capabilities");
        const std::set<std::string> supported = {"uefi-x64","esd-apply","winpe-handoff","ubuntu-squashfs","ubuntu-layered","ubuntu-erofs"};
        for(const auto& capability : capabilities) if (!supported.contains(capability)) target.supported=false;
        if (integer(t,"minimum_engine_api") > 1) target.supported=false;
        if (!target.supported) target.unsupported_reason="A newer Winstaller version is required";
        if (target.windows) {
            require(text(artifact,"format",10)=="esd" || text(artifact,"format",10)=="wim","Invalid Windows image format");
            target.image_name=text(t.at("windows"),"expected_image_name",100);
            target.esd.version_name = text(t,"version",40); target.esd.language_code=target.language;
            target.esd.language_name=target.language_name; target.esd.architecture="x64";
            target.esd.file_name=target.id+".esd"; target.esd.download_url=url; target.esd.size_bytes=size; target.esd.sha256_hash=hash;
        } else {
            require(text(artifact,"format",10)=="iso","Invalid Linux image format");
            auto& l=target.linux_target; l.id=target.id; l.display_name=target.name; l.download_url=url; l.sha256_hash=hash; l.size_bytes=size;
            l.version=text(t,"version",40); l.family=LinuxDistroFamily::Debian; l.tab_category=OsTabCategory::Ubuntu;
            const auto& linux_spec=t.at("linux"); const auto mode=text(linux_spec,"extractor",30);
            require(mode=="squashfs" || mode=="layered-squashfs" || mode=="erofs","Unsupported extraction mode");
            l.extractor_mode = mode=="erofs" ? LinuxExtractorMode::Erofs : mode=="layered-squashfs" ? LinuxExtractorMode::LayeredSquashFS : LinuxExtractorMode::SingleSquashFS;
            l.image_layers=linux_spec.at("image_layers").get<std::vector<std::string>>();
            require(!l.image_layers.empty() && l.image_layers.size()<=16,"Invalid image layers");
            for(const auto& layer:l.image_layers) relative_path(layer);
            l.kernel_path=linux_spec.value("kernel_path",std::string{}); if(!l.kernel_path.empty()) relative_path(l.kernel_path);
        }
        const bool enabled=t.at("enabled").get<bool>();
        const auto status=text(t,"support_status",20); require(status=="verified" || status=="unverified","Invalid support status");
        if(enabled) { require(status=="verified","Unverified image cannot be enabled"); result.targets.push_back(std::move(target)); }
    }
    return result;
}
AppRelease verify_release(const std::string& envelope, const UpdateConfig& config, std::int64_t now) {
    const auto v=payload(envelope,config,"release",now);
    AppRelease r;
    auto identity=v;identity.erase("revision");identity.erase("published_at");identity.erase("expires_at");
    const auto canonical=identity.dump();unsigned char digest[32];unsigned int digest_size=0;
    require(EVP_Digest(canonical.data(),canonical.size(),digest,&digest_size,EVP_sha256(),nullptr)==1 && digest_size==32,"Could not fingerprint release metadata");
    const char* hex="0123456789abcdef";for(unsigned char c:digest){r.document_hash+=hex[c>>4];r.document_hash+=hex[c&15];}
    r.published_at=integer(v,"published_at",4102444800ULL);r.expires_at=integer(v,"expires_at",4102444800ULL);
    r.id=text(v,"release_id",80); require(identifier(r.id),"Invalid release ID");
    r.version=text(v,"version",40); r.revision=integer(v,"revision"); r.sequence=integer(v,"release_sequence"); require(r.sequence>0,"Invalid release sequence");
    require(text(v,"channel",10)=="stable" && text(v,"architecture",10)=="x64","Unsupported release channel or architecture");
    r.boot_abi=text(v,"boot_abi",50);
    const auto& layouts=v.at("usb_layout_versions");
    require(layouts.is_array()&&!layouts.empty()&&layouts.size()<=8,"Invalid USB layout versions");
    bool layout_two=false;
    for(const auto& layout:layouts) {
        require(layout.is_number_integer()&&layout.get<std::int64_t>()>0&&layout.get<std::uint64_t>()<=16,"Invalid USB layout version");
        if(layout.get<int>()==2)layout_two=true;
    }
    r.compatible=integer(v,"minimum_updater_api")<=1 && r.boot_abi=="winstaller-slots-v1" && layout_two;
    if (!r.compatible) r.incompatible_reason="This release requires a newly created USB";
    r.notes=text(v,"release_notes",2000);
    const auto& components=v.at("components"); require(components.is_array() && components.size()==3,"A release must contain kernel, initramfs and rootfs");
    std::set<std::string> roles;
    for(const auto& c:components) {
        UpdateComponent component; component.role=text(c,"role",20); update_component_path(component.role);
        require(roles.insert(component.role).second,"Duplicate release component");
        component.url=text(c,"url",4096); require(valid_update_url(component.url,false),"Untrusted release URL");
        const auto prefix="https://github.com/yskyh06/Winstaller/releases/download/"+r.id+"/";
        require(component.url.starts_with(prefix),"Mixed release asset identity");
        const auto filename=component.url.substr(prefix.size());
        require(!filename.empty() && filename.size()<=200 && std::all_of(filename.begin(),filename.end(),[](unsigned char ch){return std::isalnum(ch)||ch=='-'||ch=='_'||ch=='.';}),"Invalid release asset name");
        component.sha256=text(c,"sha256",64); unhex(component.sha256,32);
        component.size=integer(c,"size_bytes",0xffffffffULL); require(component.size>0,"Invalid component size");
        r.components.push_back(std::move(component));
    }
    return r;
}
std::string update_component_path(const std::string& role) {
    if(role=="kernel") return "arch/boot/x86_64/vmlinuz-linux";
    if(role=="initramfs") return "arch/boot/x86_64/initramfs-linux.img";
    if(role=="rootfs") return "arch/x86_64/airootfs.sfs";
    throw std::runtime_error("Unknown update component role");
}
std::uint64_t AppRelease::total_bytes() const { std::uint64_t n=0;for(const auto& c:components)n+=c.size;return n; }
std::vector<std::string> RemoteCatalog::tabs() const {
    std::vector<std::string> result;
    for(const auto& t:targets) if(std::find(result.begin(),result.end(),t.tab)==result.end()) result.push_back(t.tab);
    if(result.empty()) result.push_back("Windows");
    return result;
}
std::vector<std::pair<std::string,std::string>> RemoteCatalog::products(const std::string& tab) const {
    std::vector<std::pair<std::string,std::string>> result; std::set<std::string> seen;
    for(const auto& t:targets) if(t.tab==tab && seen.insert(t.product).second)
        result.emplace_back(t.product,t.name+(t.supported ? "" : " [Update required]"));
    return result;
}
std::vector<std::pair<std::string,std::string>> RemoteCatalog::languages(const std::string& product) const {
    std::vector<std::pair<std::string,std::string>> result;
    for(const auto& t:targets) if(t.product==product) result.emplace_back(t.language,t.language_name);
    return result;
}
std::optional<RemoteTarget> RemoteCatalog::find(const std::string& product,const std::string& language) const {
    for(const auto& t:targets) if(t.product==product && t.language==language) return t;
    return {};
}
} // namespace litewin
