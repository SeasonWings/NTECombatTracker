#include "name_catalog.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void VersionedCatalogResolvesStablePathsAndFallsBackSafely() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-name-catalog-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const auto path = root / "zh-CN.json";
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"({"schemaVersion":1,"locale":"zh-CN","catalogVersion":"2026.08.28","builds":{"default":{"characters":{"/Game/Player/player_001_C":"默认角色"},"damageSources":{"/Game/Ability/GE_A":"默认技能"}},"nte-test":{"characters":{"/Game/Player/player_001_C":"构建角色"},"damageSources":{}}}})";
    }
    std::string error;
    const auto catalog = nte::combat_stats::NameCatalog::Load(path, "nte-test", error);
    Check(error.empty() && catalog.version() == "2026.08.28",
        "a valid UTF-8 versioned name catalog should load");
    const auto character = catalog.Character("/Game/Player/player_001_C");
    Check(character.display_name == "构建角色" &&
              character.origin == nte::combat_stats::NameOrigin::Catalog,
        "exact build entries override default character names");
    const auto source = catalog.DamageSource("/Game/Ability/GE_A");
    Check(source.display_name == "默认技能",
        "missing build entries fall back to the default catalog");
    const auto unknown = catalog.Character("/Game/Player/player_999_C");
    Check(unknown.display_name == "player_999_C" &&
              unknown.technical_name == "/Game/Player/player_999_C" &&
              unknown.origin == nte::combat_stats::NameOrigin::Technical,
        "unmapped paths fall back to a compact technical name without changing identity");
    const auto missing = catalog.DamageSource("Unknown source");
    Check(missing.display_name == "Unknown" &&
              missing.origin == nte::combat_stats::NameOrigin::Unknown,
        "unresolved SDK names retain an explicit unknown state");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    VersionedCatalogResolvesStablePathsAndFallsBackSafely();
    if (failures != 0) return 1;
    std::cout << "All name catalog tests passed\n";
    return 0;
}
