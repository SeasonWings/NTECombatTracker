cmake_minimum_required(VERSION 3.22)

project(NteCombatStatsPlugin VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NTE Combat Stats targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(nte_combat_stats_engine STATIC
    src/combat_stats_engine.cpp)
target_include_directories(nte_combat_stats_engine PUBLIC include)
target_compile_definitions(nte_combat_stats_engine PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(nte_combat_stats_engine PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(nte_combat_stats_tests tests/combat_stats_engine_tests.cpp)
    target_link_libraries(nte_combat_stats_tests PRIVATE nte_combat_stats_engine)
    if(MSVC)
        target_compile_options(nte_combat_stats_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME nte_combat_stats_engine COMMAND nte_combat_stats_tests)
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
    target_link_libraries(nte_combat_stats PRIVATE nte_combat_stats_engine)
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
    add_custom_command(TARGET nte_combat_stats POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NTE_COMBAT_STATS_PACKAGE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:nte_combat_stats>"
                "${NTE_COMBAT_STATS_PACKAGE_DIR}/plugin.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
                "${NTE_COMBAT_STATS_PACKAGE_DIR}/manifest.json"
        VERBATIM)
endif()
