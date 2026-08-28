#pragma once

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nte::combat_stats {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct Handle {
    std::uint64_t id{};
    std::uint64_t generation{};

    friend bool operator==(const Handle&, const Handle&) = default;
};

struct HandleHash {
    std::size_t operator()(const Handle& value) const noexcept;
};

enum class EncounterState { Idle, Active, Completed };
enum class CriticalDataStatus : std::uint8_t { Unavailable, Available, Partial };

[[nodiscard]] inline std::string_view CriticalDataStatusName(
    const CriticalDataStatus status) noexcept {
    switch (status) {
    case CriticalDataStatus::Available: return "available";
    case CriticalDataStatus::Partial: return "partial";
    case CriticalDataStatus::Unavailable: return "unavailable";
    }
    return "unavailable";
}

struct Combatant {
    Handle world;
    Handle character;
    double hp{};
    double max_hp{};
    double shield{};
};

struct DamageEvent {
    std::uint64_t sequence{};
    std::uint64_t tick_sequence{};
    std::uint32_t flags{};
    Handle world;
    Handle attacker;
    Handle victim;
    std::uint64_t source_id{};
    std::int64_t display_damage{};
    std::int64_t basic_damage{};
    std::int64_t final_damage{};
    std::array<double, 3> hit_location{};
    std::uint32_t damage_type{};
    std::uint32_t display_type{};
    std::uint32_t reaction_type{};
    std::uint32_t reaction_display_type{};
    std::int64_t received_utc_ms{};
    std::uint64_t received_offset_us{};
    std::string source_name;
    std::string source_display_name;
    std::string source_name_origin;
    std::string name_catalog_version;
    std::string attacker_name;
    std::string attacker_display_name;
    std::string attacker_name_origin;
    std::string victim_name;
    std::string victim_display_name;
    std::string victim_name_origin;
};

inline constexpr std::uint32_t kDamageFlagClientPresented = 1U << 0U;
inline constexpr std::uint32_t kDamageFlagCritical = 1U << 1U;
inline constexpr std::uint32_t kDamageFlagHeadHit = 1U << 2U;
inline constexpr std::uint32_t kDamageFlagWeakUnbalance = 1U << 3U;

[[nodiscard]] inline bool IsPositiveDamage(const std::int64_t final_damage) noexcept {
    return final_damage > 0;
}

[[nodiscard]] inline bool IsPositiveDamage(const DamageEvent& event) noexcept {
    return IsPositiveDamage(event.final_damage);
}

[[nodiscard]] inline bool IsCriticalPositiveDamage(
    const std::int64_t final_damage,
    const std::uint32_t flags) noexcept {
    return IsPositiveDamage(final_damage) && (flags & kDamageFlagCritical) != 0;
}

[[nodiscard]] inline bool IsCriticalPositiveDamage(const DamageEvent& event) noexcept {
    return IsCriticalPositiveDamage(event.final_damage, event.flags);
}

struct AggregateRow {
    std::string label;
    std::int64_t damage{};
    std::uint64_t positive_hits{};
    std::int64_t maximum_hit{};
};

struct PlayerRow {
    std::string identity_key;
    Handle handle;
    std::string object_path;
    std::string label;
    std::string display_name;
    std::string name_origin;
    std::string name_catalog_version;
    std::int64_t damage{};
    std::uint64_t positive_hits{};
    std::uint64_t critical_hits{};
    std::uint64_t head_hits{};
    std::uint64_t weak_hits{};
    std::int64_t maximum_hit{};
    std::uint64_t maximum_source_id{};
    std::string maximum_source_name;
    std::string maximum_source_display_name;
    std::string maximum_source_name_origin;
    std::string maximum_source_catalog_version;
    double damage_share{};
    double encounter_dps{};
    double active_dps{};
    double rolling_dps{};
    std::int64_t incoming_damage{};
    std::uint64_t incoming_positive_hits{};
    std::int64_t maximum_incoming_hit{};
    double incoming_share{};
};

[[nodiscard]] inline double PlayerRate(
    const std::uint64_t count, const std::uint64_t positive_hits) noexcept {
    return positive_hits == 0 ? 0.0 : static_cast<double>(count) * 100.0 /
        static_cast<double>(positive_hits);
}

[[nodiscard]] inline double PlayerCriticalRate(const PlayerRow& row) noexcept {
    return PlayerRate(row.critical_hits, row.positive_hits);
}

[[nodiscard]] inline double PlayerHeadRate(const PlayerRow& row) noexcept {
    return PlayerRate(row.head_hits, row.positive_hits);
}

[[nodiscard]] inline double PlayerWeakRate(const PlayerRow& row) noexcept {
    return PlayerRate(row.weak_hits, row.positive_hits);
}

enum class EventDirection { Visible, Outgoing, Incoming, Both };

struct RecentEvent {
    std::uint64_t sequence{};
    std::uint64_t tick_sequence{};
    std::uint32_t flags{};
    EventDirection direction{EventDirection::Outgoing};
    std::uint64_t source_id{};
    Handle attacker_handle;
    Handle victim_handle;
    std::int64_t final_damage{};
    std::int64_t display_damage{};
    std::int64_t basic_damage{};
    std::array<double, 3> hit_location{};
    std::uint32_t damage_type{};
    std::uint32_t display_type{};
    std::uint32_t reaction_type{};
    std::uint32_t reaction_display_type{};
    std::int64_t received_utc_ms{};
    std::uint64_t received_offset_us{};
    std::string source;
    std::string source_display_name;
    std::string attacker;
    std::string attacker_display_name;
    std::string victim;
    std::string victim_display_name;
};

struct Quality {
    bool partial{};
    bool overflow{};
    bool cursor_gap{};
    bool aggregate_truncated{};
};

struct DamageFlagCount {
    std::uint32_t flags{};
    std::uint64_t count{};
};

struct TimedDamage {
    TimePoint time;
    std::int64_t damage{};
};

struct SourceKey {
    std::uint64_t source_id{};
    Handle participant;
    friend bool operator==(const SourceKey&, const SourceKey&) = default;
};

struct SourceKeyHash {
    std::size_t operator()(const SourceKey& value) const noexcept;
};

struct Snapshot {
    EncounterState state{EncounterState::Idle};
    double duration_seconds{};
    std::size_t controlled_character_count{};
    std::int64_t outgoing_damage{};
    std::int64_t incoming_damage{};
    std::uint64_t outgoing_positive_hits{};
    std::uint64_t incoming_positive_hits{};
    std::uint64_t non_positive_events{};
    std::int64_t maximum_outgoing_hit{};
    double encounter_dps{};
    double rolling_dps{};
    double hp{};
    double max_hp{};
    double shield{};
    Quality quality;
    CriticalDataStatus critical_data_status{CriticalDataStatus::Unavailable};
    std::vector<DamageFlagCount> damage_flag_histogram;
    std::vector<AggregateRow> outgoing_sources;
    std::vector<AggregateRow> outgoing_targets;
    std::vector<AggregateRow> incoming_sources;
    std::vector<PlayerRow> players;
    std::vector<RecentEvent> recent_events;
};

class CombatStatsEngine {
public:
    void ObserveCombatant(const Combatant& combatant, TimePoint now);
    [[nodiscard]] bool IsRelevant(Handle world, Handle attacker, Handle victim) const;
    bool AcceptDamage(const DamageEvent& event, TimePoint now);
    void Advance(TimePoint now);
    void Reset();
    void MarkPartial(bool cursor_gap);
    void MarkOverflow();
    [[nodiscard]] EncounterState state() const noexcept { return state_; }
    [[nodiscard]] Snapshot BuildSnapshot(TimePoint now) const;

private:
    struct PlayerAggregate {
        Handle handle;
        std::string object_path;
        std::string label;
        std::string display_name;
        std::string name_origin;
        std::string name_catalog_version;
        std::int64_t damage{};
        std::uint64_t positive_hits{};
        std::uint64_t critical_hits{};
        std::uint64_t head_hits{};
        std::uint64_t weak_hits{};
        std::int64_t maximum_hit{};
        std::uint64_t maximum_source_id{};
        std::string maximum_source_name;
        std::string maximum_source_display_name;
        std::string maximum_source_name_origin;
        std::string maximum_source_catalog_version;
        TimePoint first_positive_damage{};
        TimePoint last_positive_damage{};
        std::vector<TimedDamage> rolling_damage;
        std::int64_t incoming_damage{};
        std::uint64_t incoming_positive_hits{};
        std::int64_t maximum_incoming_hit{};
    };

    PlayerAggregate& ConfirmControlledActor(
        Handle handle,
        const std::string& object_path,
        const std::string& display_name,
        const std::string& name_origin,
        const std::string& catalog_version);
    void ClearEncounter();

    EncounterState state_{EncounterState::Idle};
    Combatant combatant_;
    std::unordered_set<Handle, HandleHash> controlled_characters_;
    TimePoint encounter_started_{};
    TimePoint last_activity_{};
    TimePoint first_positive_outgoing_{};
    TimePoint last_positive_outgoing_{};
    std::int64_t outgoing_damage_{};
    std::int64_t incoming_damage_{};
    std::uint64_t outgoing_positive_hits_{};
    std::uint64_t incoming_positive_hits_{};
    std::uint64_t non_positive_events_{};
    std::uint64_t critical_metadata_positive_hits_{};
    std::int64_t maximum_outgoing_hit_{};
    std::vector<RecentEvent> recent_events_;
    std::unordered_map<std::uint32_t, std::uint64_t> damage_flag_histogram_;
    std::vector<TimedDamage> rolling_outgoing_;
    std::unordered_map<std::uint64_t, AggregateRow> outgoing_sources_;
    std::unordered_map<Handle, AggregateRow, HandleHash> outgoing_targets_;
    std::unordered_map<SourceKey, AggregateRow, SourceKeyHash> incoming_sources_;
    std::unordered_map<std::string, PlayerAggregate> confirmed_controlled_actors_;
    std::unordered_map<Handle, std::string, HandleHash> confirmed_actor_keys_;
    AggregateRow other_outgoing_source_;
    AggregateRow other_outgoing_target_;
    AggregateRow other_incoming_source_;
    Quality quality_;
    Quality pending_quality_;
};

}  // namespace nte::combat_stats
