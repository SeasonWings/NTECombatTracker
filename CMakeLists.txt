cmake_minimum_required(VERSION 3.22)

project(NteCombatStatsPlugin VERSION 0.4.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NTE Combat Stats targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
set(JSON_MultipleHeaders OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(nte_combat_stats_engine STATIC
    src/combat_stats_engine.cpp
    src/name_catalog.cpp
    src/damage_cursor.cpp
    src/damage_log.cpp
    src/log_worker.cpp)
target_include_directories(nte_combat_stats_engine PUBLIC include)
target_compile_definitions(nte_combat_stats_engine PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(nte_combat_stats_engine PUBLIC
    bcrypt shell32 ole32 nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(nte_combat_stats_engine PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_library(nte_overlay_protocol STATIC src/overlay_protocol.cpp)
target_include_directories(nte_overlay_protocol PUBLIC include)
target_link_libraries(nte_overlay_protocol PUBLIC
    nte_combat_stats_engine nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(nte_overlay_protocol PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_library(nte_overlay_model STATIC
    src/overlay_model.cpp
    src/overlay_publish_policy.cpp
    src/history_reader.cpp)
target_include_directories(nte_overlay_model PUBLIC include)
target_link_libraries(nte_overlay_model PUBLIC nte_overlay_protocol)
if(MSVC)
    target_compile_options(nte_overlay_model PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_executable(NteCombatOverlay WIN32 src/overlay_app.cpp)
target_link_libraries(NteCombatOverlay PRIVATE
    nte_overlay_model
    d2d1 dwrite windowscodecs winhttp shell32 ole32 user32 gdi32)
target_compile_definitions(NteCombatOverlay PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(NteCombatOverlay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/overlay")
if(MSVC)
    target_compile_options(NteCombatOverlay PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(NteCombatOverlay PRIVATE /INCREMENTAL:NO)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(nte_combat_stats_tests tests/combat_stats_engine_tests.cpp)
    target_link_libraries(nte_combat_stats_tests PRIVATE nte_combat_stats_engine)
    if(MSVC)
        target_compile_options(nte_combat_stats_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME nte_combat_stats_engine COMMAND nte_combat_stats_tests)

    add_executable(nte_damage_log_tests tests/damage_log_tests.cpp)
    target_link_libraries(nte_damage_log_tests PRIVATE nte_combat_stats_engine)
    if(MSVC)
        target_compile_options(nte_damage_log_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME nte_damage_log COMMAND nte_damage_log_tests)

    add_executable(nte_overlay_protocol_tests tests/overlay_protocol_tests.cpp)
    target_link_libraries(nte_overlay_protocol_tests PRIVATE nte_overlay_protocol)
    if(MSVC)
        target_compile_options(nte_overlay_protocol_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME nte_overlay_protocol COMMAND nte_overlay_protocol_tests)

    add_executable(nte_overlay_model_tests tests/overlay_model_tests.cpp)
    target_link_libraries(nte_overlay_model_tests PRIVATE nte_overlay_model)
    if(MSVC)
        target_compile_options(nte_overlay_model_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME nte_overlay_model COMMAND nte_overlay_model_tests)

    add_executable(nte_overlay_publish_policy_tests tests/overlay_publish_policy_tests.cpp)
    target_link_libraries(nte_overlay_publish_policy_tests PRIVATE nte_overlay_model)
    if(MSVC)
        target_compile_options(nte_overlay_publish_policy_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME nte_overlay_publish_policy COMMAND nte_overlay_publish_policy_tests)

    add_executable(nte_history_reader_tests tests/history_reader_tests.cpp)
    target_link_libraries(nte_history_reader_tests PRIVATE nte_overlay_model)
    if(MSVC)
        target_compile_options(nte_history_reader_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME nte_history_reader COMMAND nte_history_reader_tests)

    add_executable(nte_name_catalog_tests tests/name_catalog_tests.cpp)
    target_link_libraries(nte_name_catalog_tests PRIVATE nte_combat_stats_engine)
    if(MSVC)
        target_compile_options(nte_name_catalog_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME nte_name_catalog COMMAND nte_name_catalog_tests)
endif()

option(NTE_COMBAT_STATS_BUILD_PLUGIN "Build the Anomaly plugin package" ON)
if(NTE_COMBAT_STATS_BUILD_PLUGIN)
    find_package(AnomalySDK 1.7.0 EXACT CONFIG REQUIRED)
    set(NTE_COMBAT_STATS_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package/NteCombatStats")
    set(NTE_COMBAT_STATS_BUILD_DIR "${CMAKE_BINARY_DIR}/artifacts/NteCombatStats")
    anomaly_add_plugin(nte_combat_stats
        SOURCES src/plugin.cpp
        MANIFEST manifest.json
        PACKAGE_NAME NteCombatStats
        OUTPUT_DIRECTORY "${NTE_COMBAT_STATS_BUILD_DIR}")
    target_link_libraries(nte_combat_stats PRIVATE nte_overlay_model)
    set_target_properties(nte_combat_stats PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
        COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols")
    target_compile_definitions(nte_combat_stats PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(nte_combat_stats PRIVATE /W4 /permissive- /EHsc /utf-8)
        target_link_options(nte_combat_stats PRIVATE /INCREMENTAL:NO)
    endif()
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/script/package-files.txt"
        NTE_COMBAT_STATS_PACKAGE_FILES)
    set(NTE_COMBAT_STATS_PACKAGE_COMMANDS)
    foreach(package_file IN LISTS NTE_COMBAT_STATS_PACKAGE_FILES)
        if(IS_ABSOLUTE "${package_file}" OR package_file MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "Invalid package path: ${package_file}")
        endif()
        if(package_file STREQUAL "plugin.dll")
            set(package_source "$<TARGET_FILE:nte_combat_stats>")
        elseif(package_file STREQUAL "NteCombatOverlay.exe")
            set(package_source "$<TARGET_FILE:NteCombatOverlay>")
        else()
            set(package_source "${CMAKE_CURRENT_SOURCE_DIR}/${package_file}")
        endif()
        get_filename_component(package_parent "${package_file}" DIRECTORY)
        if(package_parent)
            set(package_destination_directory
                "${NTE_COMBAT_STATS_PACKAGE_DIR}/${package_parent}")
        else()
            set(package_destination_directory "${NTE_COMBAT_STATS_PACKAGE_DIR}")
        endif()
        list(APPEND NTE_COMBAT_STATS_PACKAGE_COMMANDS
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${package_destination_directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${package_source}"
                    "${NTE_COMBAT_STATS_PACKAGE_DIR}/${package_file}")
    endforeach()
    add_custom_target(nte_combat_stats_package
        ${NTE_COMBAT_STATS_PACKAGE_COMMANDS}
        DEPENDS nte_combat_stats NteCombatOverlay
        COMMENT "Assembling NTE Combat Stats plugin package"
        VERBATIM)
endif()
