#include "combat_stats_engine.hpp"
#include "damage_cursor.hpp"

#include <chrono>
#include <iostream>
#include <limits>

namespace {

using namespace std::chrono_literals;
using nte::combat_stats::CombatStatsEngine;
using nte::combat_stats::Combatant;
using nte::combat_stats::DamageEvent;
using nte::combat_stats::EncounterState;
using nte::combat_stats::Handle;
using nte::combat_stats::TimePoint;

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

DamageEvent Outgoing(
    std::uint64_t sequence,
    Handle world,
    Handle attacker,
    std::int64_t damage);

void RelevantDamageStartsEncounter() {
    CombatStatsEngine engine;
    const TimePoint start{100s};
    const Handle world{1, 1};
    const Handle player{2, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    DamageEvent event;
    event.sequence = 1;
    event.world = world;
    event.attacker = player;
    event.victim = {3, 1};
    event.source_id = 10;
    event.final_damage = 125;
    event.source_name = "GE_Test";
    event.victim_name = "Enemy";

    Check(engine.AcceptDamage(event, start), "player event should be accepted");
    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.state == EncounterState::Active, "first player event should start encounter");
    Check(snapshot.outgoing_damage == 125, "outgoing damage should be accumulated");
    Check(snapshot.outgoing_positive_hits == 1, "positive outgoing hit should be counted");
}

void ActiveEncounterCapturesEveryVisibleDamageField() {
    CombatStatsEngine engine;
    const TimePoint start{150s};
    const Handle world{5, 1};
    const Handle player{6, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    Check(engine.AcceptDamage(Outgoing(1, world, player, 10), start),
          "player damage should trigger the encounter");

    DamageEvent visible;
    visible.sequence = 2;
    visible.tick_sequence = 77;
    visible.flags = 0x0f;
    visible.world = world;
    visible.attacker = {70, 2};
    visible.victim = {80, 3};
    visible.source_id = 999;
    visible.display_damage = 31;
    visible.basic_damage = 29;
    visible.final_damage = 25;
    visible.hit_location = {1.5, 2.5, 3.5};
    visible.damage_type = 4;
    visible.display_type = 5;
    visible.reaction_type = 6;
    visible.reaction_display_type = 7;
    visible.source_name = "NpcSkill";
    visible.attacker_name = "NpcA";
    visible.victim_name = "NpcB";

    Check(engine.AcceptDamage(visible, start + 1s),
          "an active encounter should capture unrelated visible damage");
    const auto snapshot = engine.BuildSnapshot(start + 1s);
    Check(snapshot.outgoing_damage == 10,
          "unrelated visible damage should not enter controlled-character output");
    Check(snapshot.recent_events.size() == 2,
          "visible damage should be retained in the recent event stream");
    if (snapshot.recent_events.size() == 2) {
        const auto& recent = snapshot.recent_events.back();
        Check(recent.tick_sequence == 77 && recent.flags == 0x0f,
              "sequence metadata should be retained");
        Check(recent.display_damage == 31 && recent.basic_damage == 29 &&
                  recent.final_damage == 25,
              "all three damage values should be retained");
        Check(recent.hit_location[0] == 1.5 && recent.damage_type == 4 &&
                  recent.reaction_display_type == 7,
              "damage classification and hit location should be retained");
    }

    engine.Advance(start + 10s);
    Check(engine.BuildSnapshot(start + 10s).state == EncounterState::Completed,
          "unrelated visible damage must not extend player activity timeout");
}

DamageEvent Outgoing(
    const std::uint64_t sequence,
    const Handle world,
    const Handle attacker,
    const std::int64_t damage) {
    DamageEvent event;
    event.sequence = sequence;
    event.world = world;
    event.attacker = attacker;
    event.victim = {900 + sequence, 1};
    event.source_id = 10;
    event.final_damage = damage;
    event.source_name = "Source";
    event.victim_name = "Enemy";
    return event;
}

void EncounterLifecycleAndCharacterRotation() {
    CombatStatsEngine engine;
    const TimePoint start{200s};
    const Handle world{10, 1};
    const Handle first{11, 1};
    const Handle second{12, 1};
    engine.ObserveCombatant(Combatant{world, first, 80.0, 100.0, 5.0}, start);

    Check(!engine.AcceptDamage(Outgoing(1, world, {99, 1}, 50), start),
          "unrelated event should be ignored");
    Check(!engine.AcceptDamage(Outgoing(1, {999, 1}, first, 50), start),
          "event from another world should be ignored");
    Check(engine.AcceptDamage(Outgoing(2, world, first, 100), start),
          "first controlled character should be accepted");
    engine.ObserveCombatant(Combatant{world, second, 75.0, 100.0, 3.0}, start + 1s);
    Check(engine.AcceptDamage(Outgoing(3, world, second, 200), start + 1s),
          "rotated controlled character should join encounter");
    auto snapshot = engine.BuildSnapshot(start + 1s);
    Check(snapshot.controlled_character_count == 2, "character rotation should be merged");
    Check(snapshot.outgoing_damage == 300, "rotated character damage should share total");

    engine.Advance(start + 11s);
    snapshot = engine.BuildSnapshot(start + 11s);
    Check(snapshot.state == EncounterState::Completed, "ten idle seconds should complete encounter");

    Check(engine.AcceptDamage(Outgoing(4, world, first, 25), start + 12s),
          "new event after completion should start next encounter");
    snapshot = engine.BuildSnapshot(start + 12s);
    Check(snapshot.state == EncounterState::Active, "next encounter should be active");
    Check(snapshot.outgoing_damage == 25, "next encounter should start with clean totals");

    const Handle next_world{20, 1};
    engine.ObserveCombatant(Combatant{next_world, first, 100.0, 100.0, 0.0}, start + 13s);
    snapshot = engine.BuildSnapshot(start + 13s);
    Check(snapshot.state == EncounterState::Idle, "world change should reset encounter");
    Check(snapshot.outgoing_damage == 0, "world change should clear totals");
    Check(snapshot.controlled_character_count == 0,
          "world change should wait for actual encounter participation");
}

void ObservedCharactersRequireEncounterParticipation() {
    CombatStatsEngine engine;
    const TimePoint start{250s};
    const Handle world{21, 1};
    const Handle participating{22, 1};
    const Handle observed_only{23, 1};
    engine.ObserveCombatant(Combatant{world, participating, 100.0, 100.0, 0.0}, start);
    engine.ObserveCombatant(Combatant{world, observed_only, 100.0, 100.0, 0.0}, start);

    auto hit = Outgoing(1, world, participating, 120);
    hit.attacker_name = "/Game/Player/player_001_C";
    Check(engine.AcceptDamage(hit, start), "participating controlled character should start encounter");

    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.controlled_character_count == 1,
          "observed characters without encounter events must not be counted");
    Check(snapshot.players.size() == 1,
          "only confirmed controlled characters should produce player rows");
}

void ConfirmedCharactersMergeGenerationByResolvedPath() {
    CombatStatsEngine engine;
    const TimePoint start{275s};
    const Handle world{24, 1};
    const Handle first_generation{25, 1};
    const Handle second_generation{25, 2};
    engine.ObserveCombatant(
        Combatant{world, first_generation, 100.0, 100.0, 0.0}, start);

    auto first = Outgoing(1, world, first_generation, 100);
    first.attacker_name = "/Game/Player/player_076_shinku_C";
    first.source_id = 501;
    first.source_name = "SkillA";
    first.flags = (1u << 1u) | (1u << 2u);
    engine.AcceptDamage(first, start);

    engine.ObserveCombatant(
        Combatant{world, second_generation, 100.0, 100.0, 0.0}, start + 2s);
    auto second = Outgoing(2, world, second_generation, 300);
    second.attacker_name = first.attacker_name;
    second.source_id = 502;
    second.source_name = "SkillB";
    second.flags = (1u << 1u) | (1u << 3u);
    engine.AcceptDamage(second, start + 2s);

    const auto snapshot = engine.BuildSnapshot(start + 2s);
    Check(snapshot.controlled_character_count == 1,
          "same resolved character path should merge generation handles");
    Check(snapshot.players.size() == 1, "merged character should have one player row");
    if (snapshot.players.size() == 1) {
        const auto& row = snapshot.players.front();
        Check(row.damage == 400 && row.positive_hits == 2,
              "player row should aggregate positive damage and hits");
        Check(row.critical_hits == 2 && row.head_hits == 1 && row.weak_hits == 1,
              "player row should aggregate NTE hit flags");
        Check(row.maximum_hit == 300 && row.maximum_source_id == 502 &&
                  row.maximum_source_name == "SkillB",
              "player row should retain maximum hit ability");
        Check(row.encounter_dps == 200.0 && row.active_dps == 200.0,
              "player row should expose encounter and active DPS");
        Check(row.damage_share == 100.0,
              "single player should own one hundred percent of outgoing damage");
    }
}

void DirectionAndNonPositiveRules() {
    CombatStatsEngine engine;
    const TimePoint start{300s};
    const Handle world{30, 1};
    const Handle first{31, 1};
    const Handle second{32, 1};
    engine.ObserveCombatant(Combatant{world, first, 90.0, 100.0, 7.0}, start);
    engine.ObserveCombatant(Combatant{world, second, 70.0, 100.0, 2.0}, start);

    auto incoming = Outgoing(1, world, {40, 1}, 80);
    incoming.victim = first;
    incoming.attacker_name = "EnemyA";
    engine.AcceptDamage(incoming, start);

    auto both = Outgoing(2, world, first, 30);
    both.victim = second;
    engine.AcceptDamage(both, start + 1s);

    engine.AcceptDamage(Outgoing(3, world, first, 0), start + 2s);
    engine.AcceptDamage(Outgoing(4, world, first, -15), start + 3s);

    const auto snapshot = engine.BuildSnapshot(start + 3s);
    Check(snapshot.outgoing_damage == 30, "only positive outgoing damage should accumulate");
    Check(snapshot.incoming_damage == 110, "incoming should include enemy and controlled-to-controlled damage");
    Check(snapshot.outgoing_positive_hits == 1, "only positive outgoing hit should count");
    Check(snapshot.incoming_positive_hits == 2, "positive incoming hits should count");
    Check(snapshot.non_positive_events == 2, "non-positive events should be counted separately");
    Check(snapshot.recent_events.size() == 4, "each accepted event should produce one log row");
    if (snapshot.recent_events.size() > 1) {
        Check(snapshot.recent_events[1].direction == nte::combat_stats::EventDirection::Both,
              "controlled-to-controlled event should be marked both");
    }
}

void EncounterAndRollingDps() {
    CombatStatsEngine engine;
    const TimePoint start{400s};
    const Handle world{50, 1};
    const Handle player{51, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, world, player, 100), start);
    auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.encounter_dps == 100.0, "single hit encounter DPS should use one-second floor");
    Check(snapshot.rolling_dps == 100.0, "single hit rolling DPS should use one-second floor");

    engine.AcceptDamage(Outgoing(2, world, player, 300), start + 1s);
    snapshot = engine.BuildSnapshot(start + 1s);
    Check(snapshot.encounter_dps == 400.0, "encounter DPS should use first-to-last positive hit");
    Check(snapshot.rolling_dps == 400.0, "rolling DPS should use elapsed window before ten seconds");

    engine.Advance(start + 11s);
    snapshot = engine.BuildSnapshot(start + 11s);
    Check(snapshot.rolling_dps == 0.0, "rolling DPS should expire old hits");
    Check(snapshot.duration_seconds == 1.0, "completed encounter duration should stop at last activity");
}

void EncounterDpsUsesPlayerActivityWindow() {
    CombatStatsEngine engine;
    const TimePoint start{450s};
    const Handle world{55, 1};
    const Handle player{56, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, world, player, 100), start);
    auto activity = Outgoing(2, world, player, 0);
    engine.AcceptDamage(activity, start + 5s);
    const auto snapshot = engine.BuildSnapshot(start + 5s);
    Check(snapshot.encounter_dps == 20.0,
          "encounter DPS should use trigger-to-last-player-activity duration");
}

void AggregatesAndQualityFlags() {
    CombatStatsEngine engine;
    const TimePoint start{500s};
    const Handle world{60, 1};
    const Handle player{61, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);

    auto first = Outgoing(1, world, player, 100);
    first.source_id = 101;
    first.source_name = "SkillA";
    first.victim_name = "EnemyA";
    first.victim = {70, 1};
    engine.AcceptDamage(first, start);

    auto second = first;
    second.sequence = 2;
    second.final_damage = 50;
    engine.AcceptDamage(second, start + 1s);

    auto third = Outgoing(3, world, {80, 1}, 30);
    third.victim = player;
    third.source_id = 202;
    third.source_name = "EnemySkill";
    third.attacker_name = "EnemyB";
    engine.AcceptDamage(third, start + 2s);

    engine.MarkPartial(true);
    engine.MarkOverflow();
    const auto snapshot = engine.BuildSnapshot(start + 2s);
    Check(snapshot.outgoing_sources.size() == 1, "same source should aggregate into one row");
    if (!snapshot.outgoing_sources.empty()) {
        Check(snapshot.outgoing_sources[0].damage == 150, "source damage should aggregate");
        Check(snapshot.outgoing_sources[0].positive_hits == 2, "source hits should aggregate");
        Check(snapshot.outgoing_sources[0].maximum_hit == 100, "source maximum should aggregate");
    }
    Check(snapshot.outgoing_targets.size() == 1, "same target should aggregate");
    Check(snapshot.incoming_sources.size() == 1, "incoming attacker/source should aggregate");
    Check(snapshot.quality.partial && snapshot.quality.cursor_gap,
          "partial cursor gap flags should propagate");
    Check(snapshot.quality.overflow, "overflow flag should propagate");
}

void AggregateLimitUsesOtherBucket() {
    CombatStatsEngine engine;
    const TimePoint start{600s};
    const Handle world{90, 1};
    const Handle player{91, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    for (std::uint64_t index{}; index < 257; ++index) {
        auto event = Outgoing(index + 1, world, player, 1);
        event.source_id = 1000 + index;
        event.source_name = "Source" + std::to_string(index);
        event.victim = {2000 + index, 1};
        event.victim_name = "Target" + std::to_string(index);
        engine.AcceptDamage(event, start);
    }
    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.quality.aggregate_truncated, "aggregate limit should set quality flag");
    Check(snapshot.outgoing_sources.size() == 256, "source rows should stay bounded with Other bucket");
    Check(snapshot.outgoing_targets.size() == 256, "target rows should stay bounded with Other bucket");
    if (!snapshot.outgoing_sources.empty()) {
        Check(snapshot.outgoing_sources.front().label == "Other",
              "overflow source row should be named Other");
        Check(snapshot.outgoing_sources.front().damage == 2,
              "overflow source damage should be retained");
    }
}

void DamageTotalsSaturateOnOverflow() {
    CombatStatsEngine engine;
    const TimePoint start{700s};
    const Handle world{100, 1};
    const Handle player{101, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(
        1, world, player, std::numeric_limits<std::int64_t>::max()), start);
    engine.AcceptDamage(Outgoing(2, world, player, 1), start);
    const auto snapshot = engine.BuildSnapshot(start);
    Check(snapshot.outgoing_damage == std::numeric_limits<std::int64_t>::max(),
          "damage total should saturate instead of wrapping");
    Check(snapshot.quality.overflow, "local total overflow should set quality flag");
}

void CursorDefersResetAndRecoversRetainedWindow() {
    nte::combat_stats::DamageCursor cursor;
    static_cast<void>(cursor.Accept(40));
    cursor.RequestReset();
    const auto unavailable = cursor.BeginFrame(std::nullopt);
    Check(!unavailable.reset_applied && cursor.reset_pending(),
          "reset should remain pending while service is unavailable");
    const auto reset = cursor.BeginFrame(55);
    Check(reset.reset_applied && cursor.value() == 55,
          "available service should reset cursor to latest sequence");
    Check(!cursor.reset_pending(), "applied reset should clear pending state");

    static_cast<void>(cursor.Accept(70));
    const auto rewind = cursor.BeginFrame(10);
    Check(rewind.cursor_gap && cursor.value() == 0,
          "latest sequence rollback should rewind and report a gap");
    static_cast<void>(cursor.Accept(5));
    static_cast<void>(cursor.BeginFrame(20));
    Check(cursor.RecoverFromNotFound(20) && cursor.value() == 0,
          "stale retained-window cursor should recover once per frame");
    Check(!cursor.RecoverFromNotFound(20), "cursor recovery should be bounded to one attempt");
}

void CursorGapQualityAppliesToReplacementEncounter() {
    CombatStatsEngine engine;
    nte::combat_stats::DamageCursor cursor;
    const TimePoint start{800s};
    const Handle world{110, 1};
    const Handle player{111, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, world, player, 10), start);
    engine.Advance(start + 10s);
    static_cast<void>(cursor.Accept(50));
    const auto decision = cursor.BeginFrame(5);
    engine.AcceptDamage(Outgoing(2, world, player, 20), start + 11s);
    if (decision.cursor_gap) engine.MarkPartial(true);
    const auto snapshot = engine.BuildSnapshot(start + 11s);
    Check(snapshot.outgoing_damage == 20, "replacement encounter should start clean");
    Check(snapshot.quality.partial && snapshot.quality.cursor_gap,
          "cursor gap should apply after replacement encounter is created");
}

void CursorGapWaitsForLaterReplacementEncounter() {
    CombatStatsEngine engine;
    const TimePoint start{900s};
    const Handle world{120, 1};
    const Handle player{121, 1};
    engine.ObserveCombatant(Combatant{world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, world, player, 10), start);
    engine.Advance(start + 10s);
    engine.MarkPartial(true);
    auto snapshot = engine.BuildSnapshot(start + 10s);
    Check(snapshot.quality.cursor_gap, "completed encounter should show detected cursor gap");

    engine.Advance(start + 11s);
    engine.AcceptDamage(Outgoing(2, world, player, 20), start + 12s);
    snapshot = engine.BuildSnapshot(start + 12s);
    Check(snapshot.outgoing_damage == 20, "later replacement encounter should start clean");
    Check(snapshot.quality.partial && snapshot.quality.cursor_gap,
          "pending cursor gap should survive until a later relevant event");
}

void WorldChangeDropsPendingQuality() {
    CombatStatsEngine engine;
    const TimePoint start{1000s};
    const Handle first_world{130, 1};
    const Handle next_world{140, 1};
    const Handle player{131, 1};
    engine.ObserveCombatant(Combatant{first_world, player, 100.0, 100.0, 0.0}, start);
    engine.AcceptDamage(Outgoing(1, first_world, player, 10), start);
    engine.Advance(start + 10s);
    engine.MarkPartial(true);

    engine.ObserveCombatant(Combatant{next_world, player, 100.0, 100.0, 0.0}, start + 11s);
    engine.AcceptDamage(Outgoing(2, next_world, player, 20), start + 12s);
    const auto snapshot = engine.BuildSnapshot(start + 12s);
    Check(!snapshot.quality.partial && !snapshot.quality.cursor_gap,
          "world change should discard pending quality from the previous world");
}

}  // namespace


int main() {
    RelevantDamageStartsEncounter();
    ActiveEncounterCapturesEveryVisibleDamageField();
    EncounterLifecycleAndCharacterRotation();
    ObservedCharactersRequireEncounterParticipation();
    ConfirmedCharactersMergeGenerationByResolvedPath();
    DirectionAndNonPositiveRules();
    EncounterAndRollingDps();
    EncounterDpsUsesPlayerActivityWindow();
    AggregatesAndQualityFlags();
    AggregateLimitUsesOtherBucket();
    DamageTotalsSaturateOnOverflow();
    CursorDefersResetAndRecoversRetainedWindow();
    CursorGapQualityAppliesToReplacementEncounter();
    CursorGapWaitsForLaterReplacementEncounter();
    WorldChangeDropsPendingQuality();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All combat stats tests passed\n";
    return 0;
}
