#include "platform/network_inspector.hpp"
#include "platform/detail/network_inspection.hpp"

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ifaddrs* query_head = nullptr;
int query_errno = 0;
int release_count = 0;
bool released_correct_list = false;

int fake_getifaddrs(ifaddrs** output) {
    if (query_errno != 0) {
        errno = query_errno;
        return -1;
    }
    *output = query_head;
    return 0;
}

void fake_freeifaddrs(ifaddrs* input) {
    ++release_count;
    released_correct_list = input == query_head;
}

struct Node {
    std::string name;
    ifaddrs entry{};
    sockaddr_ll link{};
    sockaddr_in ipv4{};
    sockaddr_in6 ipv6{};
};

struct Fixture {
    std::filesystem::path sys_class_net;
    std::vector<std::unique_ptr<Node>> nodes;

    Fixture() {
        char path[] = "/tmp/litewin-network-XXXXXX";
        const auto* created = mkdtemp(path);
        require(created != nullptr, "cannot create temporary sysfs fixture");
        sys_class_net = created;
        query_head = nullptr;
        query_errno = 0;
        release_count = 0;
        released_correct_list = false;
    }

    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(sys_class_net, ignored);
        query_head = nullptr;
    }

    Node& add(std::string name, unsigned int flags) {
        auto node = std::make_unique<Node>();
        node->name = std::move(name);
        node->entry.ifa_name = node->name.data();
        node->entry.ifa_flags = flags;
        if (!nodes.empty()) {
            nodes.back()->entry.ifa_next = &node->entry;
        }
        nodes.push_back(std::move(node));
        query_head = &nodes.front()->entry;
        return *nodes.back();
    }

    Node& link(std::string name, unsigned short type, unsigned int flags = 0) {
        auto& node = add(std::move(name), flags);
        node.link.sll_family = AF_PACKET;
        node.link.sll_hatype = type;
        node.entry.ifa_addr = reinterpret_cast<sockaddr*>(&node.link);
        return node;
    }

    void ipv4(std::string name, const char* address, unsigned int flags = IFF_UP) {
        auto& node = add(std::move(name), flags);
        node.ipv4.sin_family = AF_INET;
        require(inet_pton(AF_INET, address, &node.ipv4.sin_addr) == 1, "invalid test IPv4");
        node.entry.ifa_addr = reinterpret_cast<sockaddr*>(&node.ipv4);
    }

    void ipv6(std::string name, const char* address, std::uint32_t scope = 0,
              unsigned int flags = IFF_UP) {
        auto& node = add(std::move(name), flags);
        node.ipv6.sin6_family = AF_INET6;
        node.ipv6.sin6_scope_id = scope;
        require(inet_pton(AF_INET6, address, &node.ipv6.sin6_addr) == 1, "invalid test IPv6");
        node.entry.ifa_addr = reinterpret_cast<sockaddr*>(&node.ipv6);
    }

    litewin::NetworkInventoryResult inspect() const {
        return litewin::detail::inspect_network_interfaces_with(
            {fake_getifaddrs, fake_freeifaddrs, sys_class_net});
    }
};

litewin::NetworkInventory success(const Fixture& fixture) {
    auto result = fixture.inspect();
    if (const auto* error = std::get_if<litewin::NetworkInspectionError>(&result)) {
        throw std::runtime_error(error->operation + ": " + error->code.message());
    }
    return std::get<litewin::NetworkInventory>(std::move(result));
}

void empty_inventory() {
    const Fixture fixture;
    require(success(fixture).empty(), "empty successful query must not be an error");
    require(release_count == 0, "null list does not require releasing");
}

void loopback() {
    Fixture fixture;
    fixture.link("lo", ARPHRD_LOOPBACK, IFF_UP | IFF_RUNNING | IFF_LOOPBACK);
    fixture.ipv4("lo", "127.0.0.1", IFF_UP | IFF_RUNNING | IFF_LOOPBACK);
    fixture.ipv6("lo", "::1", 0, IFF_UP | IFF_RUNNING | IFF_LOOPBACK);
    const auto interfaces = success(fixture);
    require(interfaces.size() == 1, "one interface may have multiple records");
    const auto& interface = interfaces[0];
    require(interface.is_up && interface.is_running && interface.is_loopback,
            "loopback flags must be preserved");
    require(interface.kind == litewin::NetworkInterfaceKind::loopback,
            "loopback must not require sysfs classification");
    require(interface.addresses.size() == 2, "loopback IPv4 and IPv6 must survive");
    require(release_count == 1 && released_correct_list, "queried list must be released once");
}

void ethernet_no_ip() {
    Fixture fixture;
    fixture.link("en-test", ARPHRD_ETHER);
    std::filesystem::create_directory(fixture.sys_class_net / "en-test");
    const auto interfaces = success(fixture);
    require(interfaces.size() == 1, "addressless interface must not disappear");
    require(interfaces[0].kind == litewin::NetworkInterfaceKind::ethernet,
            "Ethernet link type must be identified without name guessing");
    require(!interfaces[0].is_up && !interfaces[0].is_running
                && interfaces[0].addresses.empty(), "down/addressless state must be preserved");
}

void wifi_markers() {
    Fixture fixture;
    // Deliberately use names that do not begin with wlan or wl.
    fixture.link("radio-a", ARPHRD_ETHER, IFF_UP);
    fixture.link("radio-b", ARPHRD_ETHER);
    std::filesystem::create_directories(fixture.sys_class_net / "radio-a" / "wireless");
    std::filesystem::create_directories(fixture.sys_class_net / "radio-b");
    std::filesystem::create_directory(fixture.sys_class_net / "phy0");
    std::filesystem::create_directory_symlink(fixture.sys_class_net / "phy0",
                                             fixture.sys_class_net / "radio-b" / "phy80211");
    const auto interfaces = success(fixture);
    require(interfaces.size() == 2, "two wireless interfaces expected");
    for (const auto& interface : interfaces) {
        require(interface.kind == litewin::NetworkInterfaceKind::wifi,
                "Wi-Fi markers must take precedence over Ethernet link type");
    }
}

void unknown_kind() {
    Fixture fixture;
    fixture.link("wlan-missing", ARPHRD_ETHER, IFF_UP);
    fixture.ipv4("address-only", "192.0.2.1");
    std::filesystem::create_directory(fixture.sys_class_net / "address-only");
    const auto interfaces = success(fixture);
    for (const auto& interface : interfaces) {
        require(interface.kind == litewin::NetworkInterfaceKind::unknown,
                "missing sysfs or link information must not imply wired or Wi-Fi");
    }
    require(interfaces[0].addresses.size() == 1, "unknown type must not discard addresses");
}

void other_link_type() {
    Fixture fixture;
    fixture.link("tun-test", ARPHRD_NONE, IFF_UP);
    std::filesystem::create_directory(fixture.sys_class_net / "tun-test");
    require(success(fixture)[0].kind == litewin::NetworkInterfaceKind::other,
            "non-Ethernet interface must not be treated as Wi-Fi or Ethernet");
}

void addresses() {
    Fixture fixture;
    fixture.link("net0", ARPHRD_ETHER, IFF_UP);
    fixture.ipv4("net0", "192.0.2.10");
    fixture.ipv4("net0", "198.51.100.20");
    fixture.ipv6("net0", "2001:db8::10");
    fixture.ipv6("net0", "fe80::1", 7);
    const auto interfaces = success(fixture);
    const auto& ips = interfaces[0].addresses;
    require(ips.size() == 4, "all addresses must be retained");
    require(ips[0] == litewin::InterfaceAddress{litewin::IpAddressFamily::ipv4, "192.0.2.10", 0},
            "IPv4 address must be formatted correctly");
    require(ips[2] == litewin::InterfaceAddress{litewin::IpAddressFamily::ipv6, "2001:db8::10", 0},
            "IPv6 address must be formatted correctly");
    require(ips[3] == litewin::InterfaceAddress{litewin::IpAddressFamily::ipv6, "fe80::1", 7},
            "link-local scope must be preserved separately");
}

void sorted_deduplicated() {
    Fixture fixture;
    fixture.link("z-net", ARPHRD_ETHER);
    fixture.ipv6("a-net", "fe80::1", 8);
    fixture.ipv4("a-net", "192.0.2.5");
    fixture.ipv6("a-net", "fe80::1", 7);
    fixture.ipv4("a-net", "192.0.2.5");
    const auto interfaces = success(fixture);
    require(interfaces.size() == 2 && interfaces[0].name == "a-net"
                && interfaces[1].name == "z-net", "interface ordering must be deterministic");
    const auto& ips = interfaces[0].addresses;
    require(ips.size() == 3, "duplicate addresses must be removed, different scopes retained");
    require(ips[0].family == litewin::IpAddressFamily::ipv4
                && ips[1].scope_id == 7 && ips[2].scope_id == 8,
            "address ordering must be deterministic");
}

void query_error() {
    const Fixture fixture;
    query_errno = EACCES;
    const auto result = fixture.inspect();
    const auto* error = std::get_if<litewin::NetworkInspectionError>(&result);
    require(error != nullptr, "failed query must not look like an empty inventory");
    require(error->operation == "getifaddrs" && error->code.value() == EACCES,
            "OS error and operation must be preserved");
    require(release_count == 0, "failed query did not allocate a valid list");
}

void unnamed_interface() {
    Fixture fixture;
    fixture.link("net0", ARPHRD_ETHER);
    auto& invalid = fixture.add("invalid", 0);
    invalid.entry.ifa_name = nullptr;
    auto result = fixture.inspect();
    require(std::holds_alternative<litewin::NetworkInspectionError>(result),
            "invalid native data must not return partial success");
    require(release_count == 1 && released_correct_list, "early errors must release the list");
    invalid.entry.ifa_name = invalid.name.data();
    invalid.name[0] = '\0';
    result = fixture.inspect();
    require(std::holds_alternative<litewin::NetworkInspectionError>(result),
            "empty native name must fail");
    require(release_count == 2, "each inspection must release its own list");
}

void null_address() {
    Fixture fixture;
    fixture.add("net0", IFF_UP);
    const auto interfaces = success(fixture);
    require(interfaces.size() == 1 && interfaces[0].name == "net0"
                && interfaces[0].addresses.empty(), "null address must preserve its interface");
}

void native_smoke() {
    const auto result = litewin::inspect_network_interfaces();
    if (const auto* error = std::get_if<litewin::NetworkInspectionError>(&result)) {
        throw std::runtime_error(error->operation + ": " + error->code.message());
    }
    const auto& interfaces = std::get<litewin::NetworkInventory>(result);
    for (const auto& interface : interfaces) {
        require(!interface.name.empty(), "native interface must have a name");
        for (const auto& address : interface.addresses) {
            in6_addr buffer{};
            const int family = address.family == litewin::IpAddressFamily::ipv4 ? AF_INET : AF_INET6;
            require(inet_pton(family, address.address.c_str(), &buffer) == 1,
                    "native address must be parseable");
        }
    }
    // No external request and no host IP addresses in test output.
    std::cout << "Native inspection succeeded: " << interfaces.size() << " interfaces\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string_view, void (*)()>> scenarios = {
        {"empty_inventory", empty_inventory}, {"loopback", loopback},
        {"ethernet_no_ip", ethernet_no_ip}, {"wifi_markers", wifi_markers},
        {"unknown_kind", unknown_kind}, {"other_link_type", other_link_type},
        {"addresses", addresses}, {"sorted_deduplicated", sorted_deduplicated},
        {"query_error", query_error}, {"unnamed_interface", unnamed_interface},
        {"null_address", null_address}, {"native_smoke", native_smoke},
    };
    if (argc != 2) {
        std::cerr << "usage: network_inspector_tests <scenario>\n";
        return 2;
    }
    for (const auto& [name, run] : scenarios) {
        if (name != argv[1]) {
            continue;
        }
        try {
            run();
            std::cout << "PASS " << name << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cerr << "unknown scenario: " << argv[1] << '\n';
    return 2;
}
