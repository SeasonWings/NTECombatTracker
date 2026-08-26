#pragma once

#include <chrono>
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
    Handle world;
    Handle attacker;
    Handle victim;
    std::uint64_t source_id{};
    std::int64_t final_damage{};
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

enum class EventDirection { Outgoing, Incoming, Both };

struct RecentEvent {
    std::uint64_t sequence{};
    EventDirection direction{EventDirection::Outgoing};
    std::int64_t final_damage{};
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
    [[nodiscard]] Snapshot BuildSnapshot(TimePoint now) const;

private:
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
    AggregateRow other_outgoing_source_;
    AggregateRow other_outgoing_target_;
    AggregateRow other_incoming_source_;
    Quality quality_;
    Quality pending_quality_;
};

}  // namespace nte::combat_stats
