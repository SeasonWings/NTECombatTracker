#pragma once

#include "combat_stats_engine.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nte::combat_stats::overlay {

enum class DetailDirection : std::uint8_t { Outgoing, Incoming };

struct HistoricalEncounter {
    std::string report_id;
    std::string encounter_id;
    std::string game_build;
    std::int64_t started_utc_ms{};
    std::int64_t closed_utc_ms{};
    double duration_seconds{};
    std::int64_t outgoing_damage{};
    std::int64_t incoming_damage{};
    double encounter_dps{};
    std::string end_reason;
    bool complete{true};
    CriticalDataStatus critical_data_status{CriticalDataStatus::Unavailable};
    std::vector<DamageFlagCount> damage_flag_histogram;
};

struct CombatEventDetail {
    std::string encounter_id;
    std::uint64_t sequence{};
    std::uint64_t tick_sequence{};
    std::uint32_t flags{};
    std::uint64_t source_id{};
    Handle attacker;
    Handle victim;
    std::int64_t final_damage{};
    std::int64_t basic_damage{};
    std::int64_t display_damage{};
    std::int64_t received_utc_ms{};
    std::uint64_t received_offset_us{};
    std::string source;
    std::string source_display_name;
    std::string source_name_origin;
    std::string source_resource_version;
    std::string attacker_name;
    std::string attacker_display_name;
    std::string attacker_name_origin;
    std::string attacker_resource_version;
    std::string victim_name;
    std::string victim_display_name;
    std::string victim_name_origin;
    std::string victim_resource_version;
};

struct DamageSourceSummary {
    std::uint64_t source_id{};
    Handle participant;
    std::string source_name;
    std::string label;
    std::int64_t damage{};
    std::uint64_t hits{};
    std::int64_t maximum_hit{};
};

struct EncounterDetail {
    HistoricalEncounter summary;
    std::vector<PlayerRow> players;
    std::vector<CombatEventDetail> events;
};

class CombatLogReader {
public:
    explicit CombatLogReader(std::filesystem::path root);

    [[nodiscard]] std::vector<HistoricalEncounter> History(std::string& error) const;
    [[nodiscard]] std::optional<EncounterDetail> Encounter(
        const std::string& report_id,
        const std::string& encounter_id,
        std::string& error) const;
    [[nodiscard]] std::vector<CombatEventDetail> Events(
        const std::string& report_id,
        const std::string& encounter_id,
        std::string& error) const;

private:
    std::filesystem::path root_;
};

[[nodiscard]] std::vector<DamageSourceSummary> GroupDamageSources(
    const std::vector<CombatEventDetail>& events,
    Handle player,
    DetailDirection direction);
[[nodiscard]] std::vector<CombatEventDetail> FilterDamageEvents(
    const std::vector<CombatEventDetail>& events,
    Handle player,
    DetailDirection direction,
    const DamageSourceSummary& source);
void CanonicalizePlayerEvents(
    std::vector<CombatEventDetail>& events,
    const std::vector<PlayerRow>& players);
void RecomputePlayerHitCounts(
    const std::vector<CombatEventDetail>& events,
    std::vector<PlayerRow>& players,
    bool complete);
[[nodiscard]] CriticalDataStatus CriticalDataStatusForEvents(
    const std::vector<CombatEventDetail>& events,
    const std::vector<PlayerRow>& players,
    bool complete) noexcept;

}  // namespace nte::combat_stats::overlay
