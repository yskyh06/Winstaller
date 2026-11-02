#include "platform/disk_inspector.hpp"
#include "platform/process_runner.hpp"
#include "platform/detail/disk_inspection.hpp"

#include <nlohmann/json.hpp>

namespace litewin {

DiskInspectResult detail::inspect_disks_with(DiskCommandRunner run) {
    auto proc = run(
        "lsblk",
        {"--json", "--bytes", "-o", "NAME,PATH,TYPE,SIZE,MODEL,SERIAL,TRAN,MOUNTPOINTS"},
        ""
    );

    if (!proc.executed) {
        return DiskInspectionError{
            .operation = "lsblk execution",
            .message = "Failed to run lsblk: " + proc.error_message,
        };
    }

    if (proc.exit_code != 0) {
        return DiskInspectionError{
            .operation = "lsblk execution",
            .message = "lsblk failed with exit code " + std::to_string(proc.exit_code) +
                       ": " + proc.stderr_output,
        };
    }

    auto parse_res = parse_disk_inventory(proc.stdout_output);
    if (const auto* err = std::get_if<ParseError>(&parse_res)) {
        return DiskInspectionError{
            .operation = "parse_disk_inventory",
            .message = "Failed to parse lsblk JSON (" + err->location + "): " + err->message,
        };
    }

    auto inventory = std::get<DiskInventory>(std::move(parse_res));
    for (auto& disk : inventory) {
        if (disk.type != "disk") continue;
        // Listing only: --no-act also explicitly forbids writes. Failure leaves
        // the state unknown so target evaluation refuses installation.
        const auto signatures = run("wipefs", {"--json", "--output", "TYPE", "--no-act", "--", disk.path}, "");
        if (!signatures.success()) continue;
        try {
            const auto document = nlohmann::json::parse(signatures.stdout_output);
            if (!document.is_object() || !document.contains("signatures") ||
                !document["signatures"].is_array()) continue;
            const auto& entries = document["signatures"];
            // Any nonempty list is unsafe, even if an entry is unfamiliar.
            disk.has_signatures = !entries.empty();
        } catch (const nlohmann::json::exception&) {
            // Malformed output is unknown, never proof that the disk is empty.
        }
    }
    return inventory;
}

DiskInspectResult inspect_disks() {
    return detail::inspect_disks_with(ProcessRunner::run);
}

} // namespace litewin
