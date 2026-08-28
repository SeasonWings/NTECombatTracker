#include "damage_log.hpp"
#include "history_reader.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

namespace {

using nte::combat_stats::DamageEvent;
using nte::combat_stats::Handle;
using nte::combat_stats::PlayerRow;
using nte::damage_log::DamageLogRepository;
using nte::damage_log::EncounterClose;
using nte::damage_log::EncounterOpen;
using nte::damage_log::LoggedDamage;
using nte::damage_log::ReportClose;
using nte::damage_log::ReportOpen;
using nte::combat_stats::overlay::CombatLogReader;
using nte::combat_stats::overlay::DetailDirection;

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void ClosedLogsCanBeBrowsedAndDrilledDown() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-history-reader-test-" + std::to_string(nonce));
    const std::string report_id = "11111111-2222-4333-8444-555555555555";
    const std::string encounter_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const Handle world{1, 1};
    const Handle player{2, 1};
    const Handle enemy{3, 1};
    {
        DamageLogRepository repository(root);
        std::string error;
        Check(repository.Recover(error), "reader fixture repository should initialize");
        Check(repository.BeginReport(ReportOpen{
            report_id, world, 1'700'000'000'000, "build"}, error),
            "reader fixture report should open");
        Check(repository.BeginEncounter(EncounterOpen{
            encounter_id, 1'700'000'001'000, 1'000'000}, error),
            "reader fixture encounter should open");
        DamageEvent first;
        first.sequence = 10;
        first.tick_sequence = 20;
        first.flags = 0x02;
        first.world = world;
        first.attacker = player;
        first.victim = enemy;
        first.source_id = 77;
        first.final_damage = 120;
        first.basic_damage = 100;
        first.display_damage = 121;
        first.source_name = "SkillA";
        first.attacker_name = "/Game/Player/player_C";
        first.victim_name = "/Game/Enemy/enemy_C";
        Check(repository.Append(LoggedDamage{
            first, 1'700'000'001'250, 1'250'000, true}, error),
            "first fixture event should append");
        auto second = first;
        second.sequence = 11;
        second.final_damage = 80;
        second.flags = 0;
        Check(repository.Append(LoggedDamage{
            second, 1'700'000'001'750, 1'750'000, true}, error),
            "second fixture event should append");
        PlayerRow row;
        row.handle = player;
        row.object_path = first.attacker_name;
        row.label = "player_C";
        row.damage = 200;
        row.positive_hits = 2;
        row.critical_hits = 1;
        row.maximum_hit = 120;
        row.maximum_source_id = 77;
        row.maximum_source_name = "SkillA";
        row.damage_share = 100.0;
        row.encounter_dps = 100.0;
        EncounterClose close{encounter_id, "manual_end",
            1'700'000'003'000, 3'000'000, 200, 0, 100.0, 2.0, {}};
        close.confirmed_controlled_players.push_back(row);
        Check(repository.EndEncounter(close, error), "fixture encounter should close");
        Check(repository.CloseReport(ReportClose{
            "plugin_stop", 1'700'000'003'100, false}, error),
            "fixture report should close");
    }

    CombatLogReader reader(root);
    std::string error;
    const auto history = reader.History(error);
    Check(error.empty() && history.size() == 1,
        "closed encounter should appear in history");
    if (!history.empty()) {
        Check(history[0].encounter_id == encounter_id &&
                  history[0].end_reason == "manual_end" &&
                  history[0].outgoing_damage == 200,
              "history should expose encounter identity and summary");
    }
    const auto detail = reader.Encounter(report_id, encounter_id, error);
    Check(detail.has_value() && error.empty(), "closed encounter detail should load");
    if (detail) {
        Check(detail->players.size() == 1 && detail->players[0].handle == player,
            "report-local actor ids should resolve back to player handles");
        Check(detail->events.size() == 2 && detail->events[0].sequence == 10,
            "all matching raw events should load in sequence order");
        const auto sources = nte::combat_stats::overlay::GroupDamageSources(
            detail->events, player, DetailDirection::Outgoing);
        Check(sources.size() == 1 && sources[0].source_id == 77 &&
                  sources[0].damage == 200 && sources[0].hits == 2 &&
                  sources[0].maximum_hit == 120,
              "player events should aggregate by damage source");
        const auto hits = nte::combat_stats::overlay::FilterDamageEvents(
            detail->events, player, DetailDirection::Outgoing, sources[0]);
        Check(hits.size() == 2 && hits[1].received_utc_ms == 1'700'000'001'750,
            "source selection should expose timestamped individual hits");

        auto renamed_events = detail->events;
        renamed_events[1].source = "SkillB";
        const auto renamed_sources = nte::combat_stats::overlay::GroupDamageSources(
            renamed_events, player, DetailDirection::Outgoing);
        Check(renamed_sources.size() == 2,
            "the same source id with different names should remain distinct");
        if (renamed_sources.size() == 2) {
            const auto renamed_hits = nte::combat_stats::overlay::FilterDamageEvents(
                renamed_events, player, DetailDirection::Outgoing,
                renamed_sources[0]);
            Check(renamed_hits.size() == 1,
                "source drill-down should retain the selected source name");
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    ClosedLogsCanBeBrowsedAndDrilledDown();
    if (failures != 0) return 1;
    std::cout << "All history reader tests passed\n";
    return 0;
}
