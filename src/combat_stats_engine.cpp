#include "combat_stats_engine.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace nte::combat_stats {

std::size_t HandleHash::operator()(const Handle& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.id) ^
        (std::hash<std::uint64_t>{}(value.generation) << 1U);
}

std::size_t SourceKeyHash::operator()(const SourceKey& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.source_id) ^
        (HandleHash{}(value.participant) << 1U);
}

namespace {

bool SaturatingAdd(std::int64_t& total, const std::int64_t damage) {
    if (damage > std::numeric_limits<std::int64_t>::max() - total) {
        total = std::numeric_limits<std::int64_t>::max();
        return true;
    }
    total += damage;
    return false;
}

bool SaturatingIncrement(std::uint64_t& value) {
    if (value == std::numeric_limits<std::uint64_t>::max()) return true;
    ++value;
    return false;
}

bool AddAggregate(
    AggregateRow& row,
    const std::string& label,
    const std::int64_t damage) {
    if (row.label.empty()) row.label = label.empty() ? "Unknown" : label;
    const bool overflow = SaturatingAdd(row.damage, damage);
    const bool count_overflow = SaturatingIncrement(row.positive_hits);
    row.maximum_hit = (std::max)(row.maximum_hit, damage);
    return overflow || count_overflow;
}

template <typename Map, typename Key>
void AddBoundedAggregate(
    Map& rows,
    const Key& key,
    const std::string& label,
    const std::int64_t damage,
    AggregateRow& other,
    bool& truncated,
    bool& overflow) {
    if (const auto found = rows.find(key); found != rows.end()) {
        overflow = AddAggregate(found->second, label, damage) || overflow;
        return;
    }
    // Reserve the 256th visible key for the catch-all Other bucket.
    if (rows.size() < 255) {
        overflow = AddAggregate(rows[key], label, damage) || overflow;
        return;
    }
    truncated = true;
    overflow = AddAggregate(other, "Other", damage) || overflow;
}

template <typename Map>
std::vector<AggregateRow> CopyRows(const Map& rows) {
    std::vector<AggregateRow> result;
    result.reserve(rows.size());
    for (const auto& [key, row] : rows) {
        static_cast<void>(key);
        result.push_back(row);
    }
    return result;
}

void SortRows(std::vector<AggregateRow>& rows) {
    std::ranges::sort(rows, [](const AggregateRow& left, const AggregateRow& right) {
        if (left.damage != right.damage) return left.damage > right.damage;
        return left.label < right.label;
    });
}

}  // namespace

void CombatStatsEngine::ObserveCombatant(const Combatant& combatant, TimePoint) {
    if (combatant_.world.id != 0 && combatant.world != combatant_.world) {
        Reset();
    }
    combatant_ = combatant;
    if (combatant.character.id != 0) controlled_characters_.insert(combatant.character);
}

bool CombatStatsEngine::IsRelevant(
    const Handle world,
    const Handle attacker,
    const Handle victim) const {
    return world == combatant_.world &&
        (controlled_characters_.contains(attacker) || controlled_characters_.contains(victim));
}

bool CombatStatsEngine::AcceptDamage(const DamageEvent& event, const TimePoint now) {
    if (!IsRelevant(event.world, event.attacker, event.victim)) return false;
    const bool outgoing = controlled_characters_.contains(event.attacker);
    const bool incoming = controlled_characters_.contains(event.victim);
    if (!outgoing && !incoming) {
        return false;
    }
    if (state_ == EncounterState::Completed) ClearEncounter();
    if (state_ == EncounterState::Idle) {
        state_ = EncounterState::Active;
        encounter_started_ = now;
        quality_.partial = pending_quality_.partial;
        quality_.overflow = pending_quality_.overflow;
        quality_.cursor_gap = pending_quality_.cursor_gap;
        quality_.aggregate_truncated = pending_quality_.aggregate_truncated;
        pending_quality_ = {};
    }
    last_activity_ = now;
    if (event.final_damage > 0) {
        if (outgoing) {
            quality_.overflow = SaturatingAdd(outgoing_damage_, event.final_damage) ||
                quality_.overflow;
            quality_.overflow = SaturatingIncrement(outgoing_positive_hits_) ||
                quality_.overflow;
            maximum_outgoing_hit_ = (std::max)(maximum_outgoing_hit_, event.final_damage);
            if (first_positive_outgoing_ == TimePoint{}) first_positive_outgoing_ = now;
            last_positive_outgoing_ = now;
            rolling_outgoing_.push_back({now, event.final_damage});
            AddBoundedAggregate(
                outgoing_sources_, event.source_id, event.source_name,
                event.final_damage, other_outgoing_source_, quality_.aggregate_truncated,
                quality_.overflow);
            AddBoundedAggregate(
                outgoing_targets_, event.victim, event.victim_name,
                event.final_damage, other_outgoing_target_, quality_.aggregate_truncated,
                quality_.overflow);
        }
        if (incoming) {
            quality_.overflow = SaturatingAdd(incoming_damage_, event.final_damage) ||
                quality_.overflow;
            quality_.overflow = SaturatingIncrement(incoming_positive_hits_) ||
                quality_.overflow;
            AddBoundedAggregate(
                incoming_sources_, SourceKey{event.source_id, event.attacker},
                event.attacker_name + " / " + event.source_name,
                event.final_damage, other_incoming_source_, quality_.aggregate_truncated,
                quality_.overflow);
        }
    } else {
        quality_.overflow = SaturatingIncrement(non_positive_events_) || quality_.overflow;
    }

    RecentEvent recent;
    recent.sequence = event.sequence;
    recent.direction = outgoing && incoming
        ? EventDirection::Both
        : (outgoing ? EventDirection::Outgoing : EventDirection::Incoming);
    recent.final_damage = event.final_damage;
    recent.source = event.source_name;
    recent.attacker = event.attacker_name;
    recent.victim = event.victim_name;
    if (recent_events_.size() == 256) recent_events_.erase(recent_events_.begin());
    recent_events_.push_back(std::move(recent));
    return true;
}

void CombatStatsEngine::Advance(const TimePoint now) {
    if (state_ == EncounterState::Active &&
        now - last_activity_ >= std::chrono::seconds(10)) {
        state_ = EncounterState::Completed;
    }
}

void CombatStatsEngine::ClearEncounter() {
    state_ = EncounterState::Idle;
    encounter_started_ = {};
    last_activity_ = {};
    first_positive_outgoing_ = {};
    last_positive_outgoing_ = {};
    outgoing_damage_ = 0;
    incoming_damage_ = 0;
    outgoing_positive_hits_ = 0;
    incoming_positive_hits_ = 0;
    non_positive_events_ = 0;
    maximum_outgoing_hit_ = 0;
    recent_events_.clear();
    rolling_outgoing_.clear();
    outgoing_sources_.clear();
    outgoing_targets_.clear();
    incoming_sources_.clear();
    other_outgoing_source_ = {};
    other_outgoing_target_ = {};
    other_incoming_source_ = {};
    quality_ = {};
}

void CombatStatsEngine::Reset() {
    ClearEncounter();
    combatant_ = {};
    controlled_characters_.clear();
    pending_quality_ = {};
}
void CombatStatsEngine::MarkPartial(const bool cursor_gap) {
    quality_.partial = true;
    quality_.cursor_gap = quality_.cursor_gap || cursor_gap;
    if (state_ != EncounterState::Active) {
        pending_quality_.partial = true;
        pending_quality_.cursor_gap = pending_quality_.cursor_gap || cursor_gap;
    }
}
void CombatStatsEngine::MarkOverflow() {
    quality_.overflow = true;
    if (state_ != EncounterState::Active) pending_quality_.overflow = true;
}

Snapshot CombatStatsEngine::BuildSnapshot(const TimePoint now) const {
    Snapshot snapshot;
    snapshot.state = state_;
    snapshot.controlled_character_count = controlled_characters_.size();
    snapshot.outgoing_damage = outgoing_damage_;
    snapshot.incoming_damage = incoming_damage_;
    snapshot.outgoing_positive_hits = outgoing_positive_hits_;
    snapshot.incoming_positive_hits = incoming_positive_hits_;
    snapshot.non_positive_events = non_positive_events_;
    snapshot.maximum_outgoing_hit = maximum_outgoing_hit_;
    snapshot.recent_events = recent_events_;
    snapshot.quality = quality_;
    snapshot.outgoing_sources = CopyRows(outgoing_sources_);
    snapshot.outgoing_targets = CopyRows(outgoing_targets_);
    snapshot.incoming_sources = CopyRows(incoming_sources_);
    if (other_outgoing_source_.positive_hits != 0) {
        snapshot.outgoing_sources.push_back(other_outgoing_source_);
    }
    if (other_outgoing_target_.positive_hits != 0) {
        snapshot.outgoing_targets.push_back(other_outgoing_target_);
    }
    if (other_incoming_source_.positive_hits != 0) {
        snapshot.incoming_sources.push_back(other_incoming_source_);
    }
    SortRows(snapshot.outgoing_sources);
    SortRows(snapshot.outgoing_targets);
    SortRows(snapshot.incoming_sources);
    if (state_ != EncounterState::Idle) {
        const auto duration_end = state_ == EncounterState::Completed ? last_activity_ : now;
        snapshot.duration_seconds = std::chrono::duration<double>(
            duration_end - encounter_started_).count();
    }
    if (outgoing_positive_hits_ != 0) {
        const double seconds = (std::max)(1.0, std::chrono::duration<double>(
            last_positive_outgoing_ - first_positive_outgoing_).count());
        snapshot.encounter_dps = static_cast<double>(outgoing_damage_) / seconds;
    }
    long double rolling_damage{};
    TimePoint rolling_start = now;
    for (const TimedDamage& item : rolling_outgoing_) {
        if (now - item.time < std::chrono::seconds(10)) {
            rolling_damage += item.damage;
            rolling_start = (std::min)(rolling_start, item.time);
        }
    }
    if (rolling_damage > 0) {
        const double seconds = (std::max)(1.0, (std::min)(10.0,
            std::chrono::duration<double>(now - rolling_start).count()));
        snapshot.rolling_dps = static_cast<double>(rolling_damage) / seconds;
    }
    snapshot.hp = combatant_.hp;
    snapshot.max_hp = combatant_.max_hp;
    snapshot.shield = combatant_.shield;
    return snapshot;
}

}  // namespace nte::combat_stats
