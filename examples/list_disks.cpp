#include "core/disk_target.hpp"
#include "platform/disk_inspector.hpp"

#include <iostream>

void print_disk_tree(const litewin::DiskInfo& disk, int indent = 0) {
    std::string prefix(indent * 2, ' ');
    std::cout << prefix << "- " << disk.name << " (" << disk.path << ", "
              << disk.type << ", "
              << (disk.size_bytes / (1024 * 1024 * 1024)) << " GB";
    if (disk.model.has_value()) {
        std::cout << ", model: " << *disk.model;
    }
    if (disk.transport.has_value()) {
        std::cout << ", tran: " << *disk.transport;
    }
    std::cout << ")\n";

    if (disk.mountpoints.has_value() && !disk.mountpoints->empty()) {
        for (const auto& mp : *disk.mountpoints) {
            std::cout << prefix << "    mount: " << mp << "\n";
        }
    }

    for (const auto& child : disk.children) {
        print_disk_tree(child, indent + 1);
    }
}

int main() {
    std::cout << "[Litewin] Inspecting real system block devices via lsblk...\n";
    auto result = litewin::inspect_disks();

    if (const auto* err = std::get_if<litewin::DiskInspectionError>(&result)) {
        std::cerr << "Disk inspection failed: " << err->operation << " - " << err->message << "\n";
        return 1;
    }

    const auto& inventory = std::get<litewin::DiskInventory>(result);
    std::cout << "Found " << inventory.size() << " top-level block device(s).\n\n";

    for (const auto& disk : inventory) {
        print_disk_tree(disk);

        if (disk.type == "disk") {
            auto eval = litewin::evaluate_disk_target(disk);
            std::cout << "  ==> Installation Target: "
                      << (eval.can_install() ? "[ELIGIBLE]" : "[REJECTED]")
                      << " (" << eval.message << ")\n\n";
        }
    }

    return 0;
}
