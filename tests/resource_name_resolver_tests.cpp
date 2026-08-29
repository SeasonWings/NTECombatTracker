#include "resource_name_resolver.hpp"
#include "event_damage_source_resolver.hpp"

#include <array>
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

void RepositoryResourcesResolveSdkTechnicalPaths() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "res", error);
    if (!error.empty()) std::cerr << "resource load error: " << error << '\n';
    Check(error.empty() &&
              resolver.version() == "5806a02dfdaacb96b8910f2b5389de3acb980fb1",
        "the checked-in resource set should load with its asset revision");

    const auto shinku = resolver.ResolveParticipant(
        "/Game/NTE/Characters/player_076_shinku_C");
    Check(shinku.display_name == "真红" && shinku.character_id == 1076u &&
              shinku.attribute == "光" &&
              shinku.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "player number and codename should resolve 真红 with its attribute");
    Check(resolver.ResolveParticipant("Player_075_Oneiroi_C").display_name == "伊洛伊",
        "participant matching should be case-insensitive");
    Check(resolver.ResolveParticipant("Player_051_Female_C").display_name == "「零」",
        "Female051 should resolve through its stable character id and codename");
    Check(resolver.ResolveParticipant("player_025_hathor_C").display_name == "哈索尔",
        "Hathor should resolve through its stable character id and codename");
    Check(resolver.ResolveParticipant("Boss_07_BP_WorldBoss_C").display_name == "塞润尼缇",
        "enemy config ids should resolve after stripping the Unreal class suffix");

    const auto damage = resolver.ResolveDamageSource(
        "/Game/NTE/GE_Player_Shinku_WatchEx_Damage."
        "Default__GE_Player_Shinku_WatchEx_Damage_C");
    Check(damage.gameplay_effect_name == "GE_Player_Shinku_WatchEx_Damage" &&
              damage.ability_name == "GA_Shinku_Melee" &&
              damage.display_name ==
                  "普通攻击：特种格斗术 · 即瞬猛袭追加伤害" &&
              damage.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "GE should resolve through skill damage, ability tips, and component semantics");
    const auto mixed_case = resolver.ResolveDamageSource(
        "default__ge_player_shinku_watchex_damage_c");
    Check(mixed_case.display_name == damage.display_name &&
              mixed_case.gameplay_effect_name == damage.gameplay_effect_name,
        "GE resource matching should be case-insensitive and return the canonical name");
    const auto hidden_parent = resolver.ResolveDamageSource(
        "Default__GE_Player_Mint_Skill4_Damage_New_C");
    Check(hidden_parent.display_name == "极限反击：焦糖脆片",
        "show_parent_ability=false should display only the Chinese component name");
    const auto semantic_parent = resolver.ResolveDamageSource(
        "GE_ActorReaction_1_1019_Damage");
    Check(semantic_parent.ability_name == "GA_Oneiroi_Passive_1" &&
              semantic_parent.display_name == "镜象 · 复制创生花",
        "an explicit semantic parent should override the skill-damage GA mapping");
    const auto unmapped = resolver.ResolveDamageSource("GE_Boss_Unmapped_Damage");
    Check(unmapped.display_name == "GE_Boss_Unmapped_Damage" &&
              unmapped.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "unmapped GE names should fall back to the technical identifier");
}

void EventSourceResolutionDoesNotCacheReusableSdkIds() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "res", error);
    Check(error.empty(), "event source fixture resources should load");

    std::string current =
        "Default__GE_Player_Shinku_Melee1_1_Damage_C";
    std::uint32_t lookups{};
    const auto lookup = [&](const std::uint64_t source_id) {
        Check(source_id == 0x8000000000000005ULL,
            "event source resolver should forward the opaque SDK id");
        ++lookups;
        return current;
    };
    const auto before_reset = nte::combat_stats::ResolveEventDamageSource(
        0x8000000000000005ULL, lookup, resolver);
    current = "Default__GE_Player_Lacrimosa_Melee1_Damage_C";
    const auto after_reset = nte::combat_stats::ResolveEventDamageSource(
        0x8000000000000005ULL, lookup, resolver);

    Check(lookups == 2,
        "the same source id must be looked up again for every accepted event");
    Check(before_reset.display_name == "普通攻击：特种格斗术" &&
              after_reset.display_name == "酸甜口味的制裁" &&
              after_reset.technical_name == current,
        "a reused source id must resolve from its current technical path");
}

void MissingResourcesDegradeSafely() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "missing-res", error);
    Check(!error.empty() && resolver.version().empty(),
        "missing required resources should report a clear load error");
    const auto fallback = resolver.ResolveParticipant("Player_076_shinku_C");
    Check(fallback.display_name == "Player_076_shinku_C" &&
              fallback.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "a failed resource load must still allow safe technical-name fallback");
    const auto source = resolver.ResolveDamageSource(
        "Default__GE_Player_Shinku_WatchEx_Damage_C");
    Check(source.display_name == "GE_Player_Shinku_WatchEx_Damage" &&
              source.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "damage-source fallback should preserve the compact GE identifier");
}

void CorruptAndAmbiguousResourcesDegradeSafely() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-resource-name-test-" + std::to_string(nonce));
    const auto source = std::filesystem::path(NTE_TEST_SOURCE_DIR) / "res";
    constexpr std::array required{
        "data/asset_manifest.json",
        "data/characters/characters.json",
        "data/enemies/enemies.json",
        "data/skills/skill_damage.json",
        "data/skills/ability_tips.json",
        "data/skills/gameplay_effect_semantics.json"};
    for (const auto relative : required) {
        const auto destination = root / relative;
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(source / relative, destination);
    }
    {
        std::ofstream characters(root / "data/characters/characters.json",
            std::ios::binary | std::ios::trunc);
        characters << R"({"characters":{"1001":{"attribute":"灵","codename":"Shared","name_zh":"甲"},"1002":{"attribute":"相","codename":"shared","name_zh":"乙"}}})";
    }
    std::string error;
    auto resolver = nte::combat_stats::ResourceNameResolver::Load(root, error);
    Check(error.empty(), "runtime loading should tolerate ambiguous codenames safely");
    const auto ambiguous = resolver.ResolveParticipant("Player_999_Shared_C");
    Check(ambiguous.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "ambiguous codename fallback must not guess a Chinese character name");
    Check(resolver.ResolveDamageSource("Default__GE-Bad-Path_C").origin ==
              nte::combat_stats::DisplayNameOrigin::Technical,
        "malformed GE paths should use technical-name fallback");

    {
        std::ofstream characters(root / "data/characters/characters.json",
            std::ios::binary | std::ios::trunc);
        characters << R"({"characters":{"1001":{"codename":"NoAttribute","name_zh":"无属性"}}})";
    }
    resolver = nte::combat_stats::ResourceNameResolver::Load(root, error);
    const auto without_attribute = resolver.ResolveParticipant(
        "Player_001_NoAttribute_C");
    Check(error.empty() && without_attribute.display_name == "无属性" &&
              without_attribute.attribute.empty() &&
              without_attribute.origin == nte::combat_stats::DisplayNameOrigin::Resource,
        "a missing character attribute should preserve name resolution for color fallback");

    {
        std::ofstream abilities(root / "data/skills/ability_tips.json",
            std::ios::binary | std::ios::trunc);
        abilities << "{invalid";
    }
    resolver = nte::combat_stats::ResourceNameResolver::Load(root, error);
    Check(!error.empty() && resolver.version().empty(),
        "corrupt resources should reject the set and expose a quality error");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void SanitizedSdkSourceRegressionKeepsPlayerCoverage() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "res", error);
    std::ifstream sources(std::filesystem::path(NTE_TEST_SOURCE_DIR) /
        "tests/fixtures/sdk-source-names.txt", std::ios::binary);
    std::size_t total{};
    std::size_t player_related{};
    std::size_t localized_player_related{};
    std::string name;
    while (std::getline(sources, name)) {
        if (name.empty()) continue;
        ++total;
        const bool player = !name.starts_with("GE_Boss_");
        if (player) ++player_related;
        const auto resolved = resolver.ResolveDamageSource(name);
        if (player && resolved.origin ==
                nte::combat_stats::DisplayNameOrigin::Resource) {
            ++localized_player_related;
        }
    }
    Check(total == 52 && player_related == 48 && localized_player_related == 48,
        "the sanitized SDK sample should localize all 48 player-related sources");
}

void UnknownNamesFallBackWithoutChangingIdentity() {
    std::string error;
    const auto resolver = nte::combat_stats::ResourceNameResolver::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "res", error);
    const auto participant = resolver.ResolveParticipant(
        "/Game/NTE/Characters/Player_999_Missing_C");
    Check(participant.technical_name ==
              "/Game/NTE/Characters/Player_999_Missing_C" &&
              participant.display_name == "Player_999_Missing_C" &&
              participant.origin == nte::combat_stats::DisplayNameOrigin::Technical,
        "unmapped participants should keep full identity and compact display text");
    const auto unknown = resolver.ResolveDamageSource("Unknown source");
    Check(unknown.display_name == "Unknown" &&
              unknown.origin == nte::combat_stats::DisplayNameOrigin::Unknown,
        "unresolved SDK values should retain an explicit unknown state");
}

}  // namespace

int main() {
    RepositoryResourcesResolveSdkTechnicalPaths();
    EventSourceResolutionDoesNotCacheReusableSdkIds();
    UnknownNamesFallBackWithoutChangingIdentity();
    MissingResourcesDegradeSafely();
    CorruptAndAmbiguousResourcesDegradeSafely();
    SanitizedSdkSourceRegressionKeepsPlayerCoverage();
    if (failures != 0) return 1;
    std::cout << "All resource name resolver tests passed\n";
    return 0;
}
