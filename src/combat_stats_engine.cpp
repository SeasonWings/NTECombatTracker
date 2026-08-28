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

bool IsResolvedParticipant(const std::string& value) {
    return !value.empty() && !value.starts_with("Unknown");
}

std::string ParticipantLabel(const std::string& path, const Handle handle) {
    if (IsResolvedParticipant(path)) {
        const std::size_t slash = path.find_last_of("/\\");
        const std::size_t dot = path.find_last_of('.');
        const std::size_t position = slash == std::string::npos
            ? dot
            : (dot == std::string::npos ? slash : (std::max)(slash, dot));
        if (position != std::string::npos && position + 1 < path.size()) {
            return path.substr(position + 1);
        }
        return path;
    }
    return "Actor " + std::to_string(handle.id) + ':' + std::to_string(handle.generation);
}

std::string HandleIdentity(const Handle handle) {
    return "handle:" + std::to_string(handle.id) + ':' + std::to_string(handle.generation);
}

std::string PathIdentity(const std::string& path) {
    return "path:" + path;
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

CombatStatsEngine::PlayerAggregate& CombatStatsEngine::ConfirmControlledActor(
    const Handle handle,
    const std::string& object_path) {
    const bool resolved = IsResolvedParticipant(object_path);
    const std::string identity = resolved ? PathIdentity(object_path) : HandleIdentity(handle);
    if (resolved) {
        const auto previous_key = confirmed_actor_keys_.find(handle);
        if (previous_key != confirmed_actor_keys_.end() && previous_key->second != identity) {
            auto previous = confirmed_controlled_actors_.extract(previous_key->second);
            if (!previous.empty()) {
                if (auto found = confirmed_controlled_actors_.find(identity);
                    found == confirmed_controlled_actors_.end()) {
                    previous.key() = identity;
                    previous.mapped().object_path = object_path;
                    previous.mapped().label = ParticipantLabel(object_path, handle);
                    confirmed_controlled_actors_.insert(std::move(previous));
                } else {
                    PlayerAggregate& destination = found->second;
                    PlayerAggregate& source = previous.mapped();
                    quality_.overflow = SaturatingAdd(destination.damage, source.damage) ||
                        quality_.overflow;
                    const auto add_count = [this](std::uint64_t& destination_count,
                                               const std::uint64_t source_count) {
                        if (source_count > std::numeric_limits<std::uint64_t>::max() -
                                destination_count) {
                            destination_count = std::numeric_limits<std::uint64_t>::max();
                            quality_.overflow = true;
                        } else {
                            destination_count += source_count;
                        }
                    };
                    add_count(destination.positive_hits, source.positive_hits);
                    add_count(destination.critical_hits, source.critical_hits);
                    add_count(destination.head_hits, source.head_hits);
                    add_count(destination.weak_hits, source.weak_hits);
                    quality_.overflow = SaturatingAdd(
                        destination.incoming_damage, source.incoming_damage) ||
                        quality_.overflow;
                    add_count(destination.incoming_positive_hits,
                        source.incoming_positive_hits);
                    destination.maximum_incoming_hit = (std::max)(
                        destination.maximum_incoming_hit, source.maximum_incoming_hit);
                    if (source.maximum_hit > destination.maximum_hit) {
                        destination.maximum_hit = source.maximum_hit;
                        destination.maximum_source_id = source.maximum_source_id;
                        destination.maximum_source_name = std::move(source.maximum_source_name);
                    }
                    if (destination.first_positive_damage == TimePoint{} ||
                        (source.first_positive_damage != TimePoint{} &&
                         source.first_positive_damage < destination.first_positive_damage)) {
                        destination.first_positive_damage = source.first_positive_damage;
                    }
                    destination.last_positive_damage = (std::max)(
                        destination.last_positive_damage, source.last_positive_damage);
                    destination.rolling_damage.insert(destination.rolling_damage.end(),
                        source.rolling_damage.begin(), source.rolling_damage.end());
                }
            }
        }
    }
    confirmed_actor_keys_.insert_or_assign(handle, identity);
    PlayerAggregate& aggregate = confirmed_controlled_actors_[identity];
    aggregate.handle = handle;
    if (resolved) aggregate.object_path = object_path;
    if (aggregate.label.empty() || resolved) {
        aggregate.label = ParticipantLabel(aggregate.object_path, handle);
    }
    return aggregate;
}

bool CombatStatsEngine::AcceptDamage(const DamageEvent& event, const TimePoint now) {
    if (event.world != combatant_.world) return false;
    const bool relevant = IsRelevant(event.world, event.attacker, event.victim);
    if (state_ != EncounterState::Active && !relevant) return false;
    const bool outgoing = controlled_characters_.contains(event.attacker);
    const bool incoming = controlled_characters_.contains(event.victim);
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
    PlayerAggregate* outgoing_player{};
    if (outgoing) {
        outgoing_player = &ConfirmControlledActor(event.attacker, event.attacker_name);
    }
    PlayerAggregate* incoming_player{};
    if (incoming) {
        incoming_player = &ConfirmControlledActor(event.victim, event.victim_name);
    }
    if (relevant) last_activity_ = now;
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
            if (outgoing_player != nullptr) {
                quality_.overflow = SaturatingAdd(
                    outgoing_player->damage, event.final_damage) || quality_.overflow;
                quality_.overflow = SaturatingIncrement(outgoing_player->positive_hits) ||
                    quality_.overflow;
                if ((event.flags & (1u << 1u)) != 0) {
                    quality_.overflow = SaturatingIncrement(outgoing_player->critical_hits) ||
                        quality_.overflow;
                }
                if ((event.flags & (1u << 2u)) != 0) {
                    quality_.overflow = SaturatingIncrement(outgoing_player->head_hits) ||
                        quality_.overflow;
                }
                if ((event.flags & (1u << 3u)) != 0) {
                    quality_.overflow = SaturatingIncrement(outgoing_player->weak_hits) ||
                        quality_.overflow;
                }
                if (event.final_damage > outgoing_player->maximum_hit) {
                    outgoing_player->maximum_hit = event.final_damage;
                    outgoing_player->maximum_source_id = event.source_id;
                    outgoing_player->maximum_source_name = event.source_name;
                }
                if (outgoing_player->first_positive_damage == TimePoint{}) {
                    outgoing_player->first_positive_damage = now;
                }
                outgoing_player->last_positive_damage = now;
                outgoing_player->rolling_damage.push_back({now, event.final_damage});
            }
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
            if (incoming_player != nullptr) {
                quality_.overflow = SaturatingAdd(
                    incoming_player->incoming_damage, event.final_damage) ||
                    quality_.overflow;
                quality_.overflow = SaturatingIncrement(
                    incoming_player->incoming_positive_hits) || quality_.overflow;
                incoming_player->maximum_incoming_hit = (std::max)(
                    incoming_player->maximum_incoming_hit, event.final_damage);
            }
        }
    } else if (outgoing || incoming) {
        quality_.overflow = SaturatingIncrement(non_positive_events_) || quality_.overflow;
    }

    RecentEvent recent;
    recent.sequence = event.sequence;
    recent.tick_sequence = event.tick_sequence;
    recent.flags = event.flags;
    recent.direction = outgoing && incoming
        ? EventDirection::Both
        : (outgoing ? EventDirection::Outgoing
                    : (incoming ? EventDirection::Incoming : EventDirection::Visible));
    recent.source_id = event.source_id;
    recent.attacker_handle = event.attacker;
    recent.victim_handle = event.victim;
    recent.final_damage = event.final_damage;
    recent.display_damage = event.display_damage;
    recent.basic_damage = event.basic_damage;
    recent.hit_location = event.hit_location;
    recent.damage_type = event.damage_type;
    recent.display_type = event.display_type;
    recent.reaction_type = event.reaction_type;
    recent.reaction_display_type = event.reaction_display_type;
    recent.received_utc_ms = event.received_utc_ms;
    recent.received_offset_us = event.received_offset_us;
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
    confirmed_controlled_actors_.clear();
    confirmed_actor_keys_.clear();
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
    snapshot.controlled_character_count = confirmed_controlled_actors_.size();
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
            last_activity_ - encounter_started_).count());
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
    const double encounter_seconds = (std::max)(1.0,
        std::chrono::duration<double>(last_activity_ - encounter_started_).count());
    snapshot.players.reserve(confirmed_controlled_actors_.size());
    for (const auto& [identity, aggregate] : confirmed_controlled_actors_) {
        PlayerRow row;
        row.identity_key = identity;
        row.handle = aggregate.handle;
        row.object_path = aggregate.object_path;
        row.label = aggregate.label;
        row.damage = aggregate.damage;
        row.positive_hits = aggregate.positive_hits;
        row.critical_hits = aggregate.critical_hits;
        row.head_hits = aggregate.head_hits;
        row.weak_hits = aggregate.weak_hits;
        row.maximum_hit = aggregate.maximum_hit;
        row.maximum_source_id = aggregate.maximum_source_id;
        row.maximum_source_name = aggregate.maximum_source_name;
        row.damage_share = outgoing_damage_ > 0
            ? static_cast<double>(aggregate.damage) * 100.0 /
                static_cast<double>(outgoing_damage_)
            : 0.0;
        row.encounter_dps = static_cast<double>(aggregate.damage) / encounter_seconds;
        if (aggregate.positive_hits != 0) {
            const double active_seconds = (std::max)(1.0, std::chrono::duration<double>(
                aggregate.last_positive_damage - aggregate.first_positive_damage).count());
            row.active_dps = static_cast<double>(aggregate.damage) / active_seconds;
        }
        long double player_rolling_damage{};
        TimePoint player_rolling_start = now;
        for (const TimedDamage& item : aggregate.rolling_damage) {
            if (now - item.time < std::chrono::seconds(10)) {
                player_rolling_damage += item.damage;
                player_rolling_start = (std::min)(player_rolling_start, item.time);
            }
        }
        if (player_rolling_damage > 0) {
            const double rolling_seconds = (std::max)(1.0, (std::min)(10.0,
                std::chrono::duration<double>(now - player_rolling_start).count()));
        row.rolling_dps = static_cast<double>(player_rolling_damage) / rolling_seconds;
        }
        row.incoming_damage = aggregate.incoming_damage;
        row.incoming_positive_hits = aggregate.incoming_positive_hits;
        row.maximum_incoming_hit = aggregate.maximum_incoming_hit;
        row.incoming_share = incoming_damage_ > 0
            ? static_cast<double>(aggregate.incoming_damage) * 100.0 /
                static_cast<double>(incoming_damage_)
            : 0.0;
        snapshot.players.push_back(std::move(row));
    }
    std::ranges::sort(snapshot.players, [](const PlayerRow& left, const PlayerRow& right) {
        if (left.damage != right.damage) return left.damage > right.damage;
        return left.label < right.label;
    });
    snapshot.hp = combatant_.hp;
    snapshot.max_hp = combatant_.max_hp;
    snapshot.shield = combatant_.shield;
    return snapshot;
}

}  // namespace nte::combat_stats
