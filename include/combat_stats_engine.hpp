#pragma once

#include <chrono>
#include <array>
#include <cstdint>
#include <string>
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
    std::string attacker_name;
    std::string victim_name;
};

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
    std::int64_t damage{};
    std::uint64_t positive_hits{};
    std::uint64_t critical_hits{};
    std::uint64_t head_hits{};
    std::uint64_t weak_hits{};
    std::int64_t maximum_hit{};
    std::uint64_t maximum_source_id{};
    std::string maximum_source_name;
    double damage_share{};
    double encounter_dps{};
    double active_dps{};
    double rolling_dps{};
};

enum class EventDirection { Visible, Outgoing, Incoming, Both };

struct RecentEvent {
    std::uint64_t sequence{};
    std::uint64_t tick_sequence{};
    std::uint32_t flags{};
    EventDirection direction{EventDirection::Outgoing};
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
    std::string attacker;
    std::string victim;
};

struct Quality {
    bool partial{};
    bool overflow{};
    bool cursor_gap{};
    bool aggregate_truncated{};
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
        std::int64_t damage{};
        std::uint64_t positive_hits{};
        std::uint64_t critical_hits{};
        std::uint64_t head_hits{};
        std::uint64_t weak_hits{};
        std::int64_t maximum_hit{};
        std::uint64_t maximum_source_id{};
        std::string maximum_source_name;
        TimePoint first_positive_damage{};
        TimePoint last_positive_damage{};
        std::vector<TimedDamage> rolling_damage;
    };

    PlayerAggregate& ConfirmControlledActor(Handle handle, const std::string& object_path);
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
    std::int64_t maximum_outgoing_hit_{};
    std::vector<RecentEvent> recent_events_;
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
