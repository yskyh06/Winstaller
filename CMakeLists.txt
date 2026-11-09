cmake_minimum_required(VERSION 3.20)
project(litewin LANGUAGES CXX)

add_library(litewin_inventory STATIC src/core/disk_inventory.cpp)
target_compile_features(litewin_inventory PUBLIC cxx_std_20)
set_target_properties(litewin_inventory PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(litewin_inventory PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(litewin_inventory SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(litewin_inventory PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(CURL REQUIRED)
    find_package(OpenSSL REQUIRED)

    add_library(litewin_network STATIC
        src/core/hash_verifier.cpp
        src/core/esd_catalog.cpp
        src/core/linux_catalog.cpp
        src/core/unattend_generator.cpp
        src/platform/network_inspector.cpp
        src/platform/network_probe.cpp
        src/platform/network_configurator.cpp
        src/platform/process_runner.cpp
        src/platform/disk_partitioner.cpp
        src/platform/disk_inspector.cpp
        src/platform/http_downloader.cpp
        src/platform/mido_resolver.cpp
        src/platform/wim_deployer.cpp
        src/platform/winpe_bootstrapper.cpp
        src/platform/install_pipeline.cpp
        src/platform/linux_installer.cpp
        src/ui/ansi_terminal.cpp
        src/ui/winstaller_tui.cpp
    )
    target_link_libraries(litewin_network PUBLIC CURL::libcurl OpenSSL::Crypto)
    target_include_directories(litewin_network SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    target_compile_features(litewin_network PUBLIC cxx_std_20)
    set_target_properties(litewin_network PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(litewin_network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(litewin_network PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_executable(inspect_network examples/inspect_network.cpp)
    target_link_libraries(inspect_network PRIVATE litewin_network)
    add_executable(probe_internet examples/probe_internet.cpp)
    target_link_libraries(probe_internet PRIVATE litewin_network)
    add_executable(list_disks examples/list_disks.cpp)
    target_link_libraries(list_disks PRIVATE litewin_network litewin_inventory)
    add_executable(litewin src/main.cpp)
    target_link_libraries(litewin PRIVATE litewin_network litewin_inventory)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(disk_inventory_tests tests/disk_inventory_tests.cpp)
    target_link_libraries(disk_inventory_tests PRIVATE litewin_inventory)
    target_compile_definitions(disk_inventory_tests PRIVATE
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(disk_inventory_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    foreach(scenario IN ITEMS
        empty_inventory mixed_tree optional_metadata mountpoint_states large_size
        max_size unknown_fields malformed_json invalid_root missing_inventory
        invalid_inventory missing_required_fields invalid_required_strings
        invalid_sizes invalid_optional_metadata invalid_mountpoints invalid_children
        invalid_node nested_error no_partial_success)
        add_test(NAME "inventory.${scenario}" COMMAND disk_inventory_tests "${scenario}")
    endforeach()

    add_executable(disk_target_tests tests/disk_target_tests.cpp)
    target_link_libraries(disk_target_tests PRIVATE litewin_inventory)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(disk_target_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    foreach(scenario IN ITEMS
        vmware_scsi_eligible vmware_nvme_32gb too_small cdrom_device
        mounted_boot_device has_existing_partitions)
        add_test(NAME "target.${scenario}" COMMAND disk_target_tests "${scenario}")
    endforeach()

    if(TARGET litewin_network)
        add_executable(malfunction_regression_tests tests/malfunction_regression_tests.cpp)
        target_link_libraries(malfunction_regression_tests PRIVATE litewin_network litewin_inventory)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(malfunction_regression_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS signatures unknown_signatures unknown_mounts
                layout_bounds invalid_layout closed_stdin large_stdio numeric_only)
            add_test(NAME "regression.${scenario}" COMMAND malfunction_regression_tests "${scenario}")
            set_tests_properties("regression.${scenario}" PROPERTIES LABELS regression TIMEOUT 15)
        endforeach()

        add_executable(network_inspector_tests tests/network_inspector_tests.cpp)
        target_link_libraries(network_inspector_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(network_inspector_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS
            empty_inventory loopback ethernet_no_ip wifi_markers unknown_kind
            other_link_type addresses sorted_deduplicated query_error unnamed_interface
            null_address native_smoke)
            add_test(NAME "network.${scenario}" COMMAND network_inspector_tests "${scenario}")
        endforeach()
        set_tests_properties(network.native_smoke PROPERTIES LABELS integration TIMEOUT 10)

        add_executable(network_probe_tests tests/network_probe_tests.cpp)
        target_link_libraries(network_probe_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(network_probe_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS
            has_active_ethernet invalid_address unreachable_timeout native_smoke)
            add_test(NAME "probe.${scenario}" COMMAND network_probe_tests "${scenario}")
        endforeach()
        set_tests_properties(probe.native_smoke PROPERTIES LABELS integration TIMEOUT 10)

        add_executable(network_action_tests tests/network_action_tests.cpp)
        target_link_libraries(network_action_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(network_action_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS
            case1_wired_happy
            case2_wifi_disconnected
            case3_wired_disconnected
            case4_no_adapters
            case5_endpoint_unreachable
            case6_wifi_happy
            case7_native_smoke_single_ping)
            add_test(NAME "action.${scenario}" COMMAND network_action_tests "${scenario}")
        endforeach()
        set_tests_properties(action.case7_native_smoke_single_ping PROPERTIES LABELS integration TIMEOUT 10)

        add_executable(partition_layout_tests tests/partition_layout_tests.cpp)
        target_link_libraries(partition_layout_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(partition_layout_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS
            calculate_32gb too_small_disk generate_sfdisk_script
            process_runner_echo process_runner_stdin)
            add_test(NAME "layout.${scenario}" COMMAND partition_layout_tests "${scenario}")
        endforeach()

        add_executable(disk_partitioner_tests tests/disk_partitioner_tests.cpp)
        target_link_libraries(disk_partitioner_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(disk_partitioner_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS
            partition_path_naming dry_run_apply_layout
            dry_run_format_partition dry_run_mount_unmount)
            add_test(NAME "partitioner.${scenario}" COMMAND disk_partitioner_tests "${scenario}")
        endforeach()

        add_executable(disk_inspector_tests tests/disk_inspector_tests.cpp)
        target_link_libraries(disk_inspector_tests PRIVATE litewin_network litewin_inventory)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(disk_inspector_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS missing_tool query_failure malformed_inventory empty_inventory independent_disks native_smoke)
            add_test(NAME "inspector.${scenario}" COMMAND disk_inspector_tests "${scenario}")
        endforeach()
        set_tests_properties(inspector.native_smoke PROPERTIES LABELS integration TIMEOUT 10)

        add_executable(http_downloader_tests tests/http_downloader_tests.cpp)
        target_link_libraries(http_downloader_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(http_downloader_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS
            sha256_streaming_known sha256_corrupted_byte
            session_cache_validity download_precheck_disk_space
            download_local_file_with_hash)
            add_test(NAME "downloader.${scenario}" COMMAND http_downloader_tests "${scenario}")
        endforeach()

        add_executable(mido_resolver_tests tests/mido_resolver_tests.cpp)
        target_link_libraries(mido_resolver_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(mido_resolver_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        foreach(scenario IN ITEMS
            cache_save_and_load cache_hit_prevents_script_execution
            cache_expired_triggers_script mock_script_resolution)
            add_test(NAME "mido.${scenario}" COMMAND mido_resolver_tests "${scenario}")
        endforeach()

        add_executable(esd_catalog_tests tests/esd_catalog_tests.cpp)
        target_link_libraries(esd_catalog_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(esd_catalog_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME "esd.catalog_unit_tests" COMMAND esd_catalog_tests)

        add_executable(wim_deployer_tests tests/wim_deployer_tests.cpp)
        target_link_libraries(wim_deployer_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(wim_deployer_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME "wim.deployer_unit_tests" COMMAND wim_deployer_tests)

        add_executable(unattend_generator_tests tests/unattend_generator_tests.cpp)
        target_link_libraries(unattend_generator_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(unattend_generator_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME "unattend.generator_unit_tests" COMMAND unattend_generator_tests)

        add_executable(winpe_bootstrapper_tests tests/winpe_bootstrapper_tests.cpp)
        target_link_libraries(winpe_bootstrapper_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(winpe_bootstrapper_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME "bootstrapper.unit_tests" COMMAND winpe_bootstrapper_tests)

        add_executable(install_pipeline_tests tests/install_pipeline_tests.cpp)
        target_link_libraries(install_pipeline_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(install_pipeline_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME "pipeline.integration_tests" COMMAND install_pipeline_tests)

        add_executable(tui_tests tests/tui_tests.cpp)
        target_link_libraries(tui_tests PRIVATE litewin_network litewin_inventory)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(tui_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME "ui.tui_unit_tests" COMMAND tui_tests)

        add_executable(linux_catalog_tests tests/linux_catalog_tests.cpp)
        target_link_libraries(linux_catalog_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(linux_catalog_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME "linux.catalog_unit_tests" COMMAND linux_catalog_tests)

        add_executable(linux_installer_tests tests/linux_installer_tests.cpp)
        target_link_libraries(linux_installer_tests PRIVATE litewin_network)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(linux_installer_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME "linux.installer_unit_tests" COMMAND linux_installer_tests)
    endif()
endif()
