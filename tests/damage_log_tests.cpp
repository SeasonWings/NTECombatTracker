#include "damage_log.hpp"
#include "log_worker.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using nte::combat_stats::DamageEvent;
using nte::combat_stats::Handle;
using nte::damage_log::DamageLogRepository;
using nte::damage_log::EncounterClose;
using nte::damage_log::EncounterOpen;
using nte::damage_log::LoggedDamage;
using nte::damage_log::ReportClose;
using nte::damage_log::ReportOpen;

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

void ClosedEncounterProducesRecoverableJsonlReport() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-log-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const std::string report_id = "11111111-2222-4333-8444-555555555555";
    const std::string encounter_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

    {
        DamageLogRepository repository(root);
        std::string error;
        Check(repository.Recover(error), "empty repository should recover");
        Check(repository.BeginReport(ReportOpen{
                  report_id, Handle{9, 2}, 1'700'000'000'000, "nte-build-test"}, error),
              "report should open");
        std::string active_delete_error;
        Check(!repository.DeleteReport(report_id, active_delete_error),
              "an active report must not be deleted");
        Check(repository.BeginEncounter(EncounterOpen{
                  encounter_id, 1'700'000'000'100, 100'000}, error),
              "encounter should open");

        DamageEvent event;
        event.sequence = 18'446'744'073'709'551'000ULL;
        event.tick_sequence = 91;
        event.flags = 0x0f;
        event.world = {9, 2};
        event.attacker = {10, 3};
        event.victim = {11, 4};
        event.source_id = 18'446'744'073'709'550'999ULL;
        event.display_damage = 102;
        event.basic_damage = 101;
        event.final_damage = 100;
        event.hit_location = {1.25, 2.5, 3.75};
        event.damage_type = 4;
        event.display_type = 5;
        event.reaction_type = 6;
        event.reaction_display_type = 7;
        event.source_name = "Skill \"A\"";
        event.source_display_name = "技能甲";
        event.source_name_origin = "catalog";
        event.name_catalog_version = "2026.08.28";
        event.attacker_name = "Actor/A";
        event.attacker_display_name = "角色甲";
        event.attacker_name_origin = "catalog";
        event.victim_name = "Actor/B";
        event.victim_display_name = "目标乙";
        event.victim_name_origin = "catalog";
        Check(repository.Append(LoggedDamage{
                  event, 1'700'000'000'200, 200'000, true}, error),
              "damage should append");
        EncounterClose close{encounter_id, "inactivity",
            1'700'000'010'100, 10'100'000, 100, 0, 10.0, 10.0, {}};
        close.critical_data_status =
            nte::combat_stats::CriticalDataStatus::Unavailable;
        close.damage_flag_histogram = {{16, 1}};
        close.service_unavailable_intervals.emplace_back(3'000'000, 4'000'000);
        close.missing_sequence_ranges.emplace_back(92, 94);
        close.open_missing_sequence_start = 95;
        close.service_intervals_truncated = true;
        nte::combat_stats::PlayerRow player;
        player.handle = event.attacker;
        player.object_path = event.attacker_name;
        player.label = "Actor/A";
        player.display_name = "角色甲";
        player.name_origin = "catalog";
        player.name_catalog_version = "2026.08.28";
        player.damage = 100;
        player.positive_hits = 1;
        player.critical_hits = 1;
        player.maximum_hit = 100;
        player.maximum_source_id = event.source_id;
        player.maximum_source_name = event.source_name;
        player.maximum_source_display_name = event.source_display_name;
        player.damage_share = 100.0;
        player.encounter_dps = 10.0;
        player.active_dps = 100.0;
        player.rolling_dps = 10.0;
        close.confirmed_controlled_players.push_back(player);
        Check(repository.EndEncounter(close, error),
              "encounter should close");
        Check(repository.CloseReport(ReportClose{
                  "plugin_stop", 1'700'000'010'200, false}, error),
              "report should close");
    }

    const auto report_path = root / "reports" / report_id;
    Check(std::filesystem::exists(root / "catalog.json"), "catalog should exist");
    Check(std::filesystem::exists(report_path / "report.json"), "report metadata should exist");
    Check(std::filesystem::exists(report_path / "actors.jsonl"), "actor dictionary should exist");
    Check(std::filesystem::exists(report_path / "abilities.jsonl"), "ability dictionary should exist");
    Check(std::filesystem::exists(report_path / "encounters.jsonl"), "encounter summary should exist");
    Check(std::filesystem::exists(report_path / "events-000001.jsonl"), "event segment should exist");
    Check(std::filesystem::exists(report_path / "index.json"), "segment index should exist");
    const std::string report_index = ReadAll(report_path / "index.json");
    Check(report_index.find("\"actors_sha256\":\"") != std::string::npos &&
              report_index.find("\"abilities_sha256\":\"") != std::string::npos &&
              report_index.find("\"encounters_sha256\":\"") != std::string::npos,
          "report content hash should cover dictionaries and encounter summaries");

    const std::string events = ReadAll(report_path / "events-000001.jsonl");
    Check(events.find("\"sequence\":\"18446744073709551000\"") != std::string::npos,
          "64-bit sequence should be encoded as a decimal string");
    Check(events.find("\"source_id\":\"18446744073709550999\"") != std::string::npos,
          "64-bit source id should be encoded as a decimal string");
    Check(events.find("\"source_name\":\"Skill \\\"A\\\"\"") != std::string::npos,
          "JSON strings should be escaped");
    Check(events.find("\"source_name_origin\":\"catalog\"") != std::string::npos &&
              events.find("\"attacker_name_origin\":\"catalog\"") != std::string::npos &&
              events.find("\"victim_name_origin\":\"catalog\"") != std::string::npos,
          "events should retain display-name origins for historical priority");
    Check(!events.empty() && events.back() == '\n', "JSONL should end with a newline");
    const std::string encounters = ReadAll(report_path / "encounters.jsonl");
    Check(encounters.find("\"actor_outgoing\"") != std::string::npos &&
              encounters.find("\"ability_stats\"") != std::string::npos &&
              encounters.find("\"actor_ability\"") != std::string::npos &&
              encounters.find("\"actor_target\"") != std::string::npos &&
              encounters.find("\"ability_target\"") != std::string::npos,
          "encounter summary should contain all participant aggregate dimensions");
    Check(encounters.find("\"critical_hits\":\"1\"") != std::string::npos,
          "encounter aggregates should retain critical hit counts");
    Check(encounters.find("\"critical_data_status\":\"unavailable\"") !=
              std::string::npos &&
          encounters.find("\"damage_flag_histogram\":[{\"flags\":16,\"count\":\"1\"}]") !=
              std::string::npos,
          "encounter metadata should explain unavailable critical data with raw flags");
    Check(encounters.find("\"confirmed_controlled_players\":[{\"actor_id\":1") !=
              std::string::npos &&
          encounters.find("\"maximum_ability_id\":1") != std::string::npos &&
          encounters.find("\"damage_share\":100") != std::string::npos,
          "encounter summary should persist report-local controlled player and ability ids");
    Check(encounters.find("\"service_unavailable_intervals\":[{\"start_offset_us\":\"3000000\"") !=
              std::string::npos && encounters.find("\"complete\":false") != std::string::npos,
          "service gaps should be persisted and make the encounter incomplete");
    Check(encounters.find("\"missing_sequence_ranges\":[{\"first\":\"92\",\"last\":\"94\"}]") !=
              std::string::npos,
          "missing sequence ranges should be persisted exactly");
    Check(encounters.find("\"open_missing_sequence_start\":\"95\"") !=
              std::string::npos &&
          encounters.find("\"service_intervals_truncated\":true") !=
              std::string::npos,
          "open cursor gaps and truncated interval lists should be explicit");
    Check(ReadAll(report_path / "actors.jsonl").find(
              "\"resolution_status\":\"resolved\"") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"resolution_status\":\"resolved\"") != std::string::npos,
          "actor and ability dictionaries should persist resolution state");
    Check(ReadAll(report_path / "actors.jsonl").find(
              "\"display_name\":\"角色甲\"") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"display_name\":\"技能甲\"") != std::string::npos,
          "actor and ability dictionaries should persist localized display metadata");
    Check(ReadAll(report_path / "actors.jsonl").find(
              "\"actor_id\":0") != std::string::npos &&
          ReadAll(report_path / "abilities.jsonl").find(
              "\"ability_id\":0") != std::string::npos,
          "dictionary id zero should declare the synthetic Other bucket");

    DamageLogRepository reopened(root);
    std::string recover_error;
    Check(reopened.Recover(recover_error), "closed report should recover");
    const auto history = reopened.History();
    Check(history.size() == 1, "history should contain the report");
    if (!history.empty()) {
        Check(history[0].report_id == report_id && !history[0].open,
              "history should preserve report identity and closed state");
        Check(history[0].world == Handle{9, 2},
              "history should preserve the report World identity");
        Check(history[0].encounter_count == 1 && history[0].event_count == 1,
              "history should expose encounter and event counts");
        Check(history[0].encounters.size() == 1 &&
                  history[0].encounters[0].end_reason == "inactivity" &&
                  history[0].encounters[0].outgoing_damage == 100,
              "recovered history should include encounter summaries");
    }
    {
        std::ofstream marker(root / "keep.txt");
        marker << "keep";
    }
    std::string delete_error;
    Check(!reopened.DeleteReport("../outside", delete_error) &&
              std::filesystem::exists(root / "keep.txt"),
          "invalid report ids must not escape the log root");
    delete_error.clear();
    Check(reopened.DeleteReport(report_id, delete_error) &&
              !std::filesystem::exists(report_path),
          "closed report should be deleted by exact id");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void WorkerDrainsCommandsBeforeStopping() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-worker-test-" + std::to_string(nonce));
    const std::string report_id = "12345678-1234-4abc-8def-123456789abc";
    const std::string encounter_id = "87654321-4321-4cba-8fed-cba987654321";
    nte::damage_log::DamageLogWorker worker(root);
    Check(worker.Start(), "log worker should start");
    Check(worker.Submit(ReportOpen{report_id, {1, 1}, 1000, "build"}),
          "report command should enqueue");
    Check(worker.Submit(EncounterOpen{encounter_id, 1100, 100}),
          "encounter command should enqueue");
    DamageEvent event;
    event.sequence = 1;
    event.world = {1, 1};
    event.attacker = {2, 1};
    event.victim = {3, 1};
    event.source_id = 4;
    event.final_damage = 5;
    Check(worker.Submit(LoggedDamage{event, 1200, 200, true}),
          "event command should enqueue");
    Check(worker.Submit(EncounterClose{
              encounter_id, "inactivity", 2100, 1100, 5, 0, 5.0, 1.0, {}}),
          "encounter close should enqueue");
    std::string flush_error;
    Check(worker.FlushAndWait(std::chrono::seconds(2), flush_error),
          "encounter close should be durably flushed");
    Check(std::filesystem::exists(
              root / "reports" / report_id / "encounters.jsonl"),
          "flush barrier should persist the encounter summary before returning");
    Check(worker.Submit(ReportClose{"plugin_stop", 2200, false}),
          "report close should enqueue");
    worker.Stop();
    Check(std::filesystem::exists(
              root / "reports" / report_id / "events-000001.jsonl"),
          "worker stop should drain queued log commands");
    const auto status = worker.Status();
    Check(!status.running && status.queue_depth == 0 && status.last_error.empty(),
          "stopped worker should report a clean drained state");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void RecoveryPersistsUncleanShutdownState() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-recovery-test-" + std::to_string(nonce));
    const std::string report_id = "99999999-8888-4777-8666-555555555555";
    {
        DamageLogRepository repository(root);
        std::string error;
        Check(repository.Recover(error), "recovery test repository should initialize");
        Check(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
              "recovery test report should open");
    }
    {
        DamageLogRepository repository(root);
        std::string error;
        Check(repository.Recover(error), "open report should recover as unclean");
        const auto history = repository.History();
        Check(history.size() == 1 && !history[0].open && !history[0].complete &&
                  history[0].close_reason == "unclean_shutdown",
              "recovery should expose unclean shutdown in history");
    }
    const auto report = ReadAll(root / "reports" / report_id / "report.json");
    Check(report.find("\"open\":false") != std::string::npos &&
              report.find("\"close_reason\":\"unclean_shutdown\"") != std::string::npos,
          "unclean state should be persisted to report metadata");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void HundredThousandEventsAreSegmentedWithoutLoss() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-damage-volume-test-" + std::to_string(nonce));
    const std::string report_id = "abcdefab-cdef-4abc-8def-abcdefabcdef";
    const std::string encounter_id = "fedcbafe-dcba-4fed-8cba-fedcbafedcba";
    DamageLogRepository repository(root);
    std::string error;
    Check(repository.Recover(error), "volume repository should initialize");
    Check(repository.BeginReport(ReportOpen{report_id, {1, 1}, 1000, "build"}, error),
          "volume report should open");
    Check(repository.BeginEncounter(EncounterOpen{encounter_id, 1000, 0}, error),
          "volume encounter should open");
    DamageEvent event;
    event.world = {1, 1};
    event.attacker = {2, 1};
    event.victim = {3, 1};
    event.source_id = 4;
    event.final_damage = 5;
    event.source_name = "Skill";
    event.attacker_name = "Attacker";
    event.victim_name = "Victim";
    bool appended = true;
    for (std::uint64_t sequence = 1; sequence <= 100'000; ++sequence) {
        event.sequence = sequence;
        event.tick_sequence = sequence / 4;
        appended = repository.Append(LoggedDamage{
            event, 1000 + static_cast<std::int64_t>(sequence), sequence * 1000, true}, error);
        if (!appended) break;
    }
    Check(appended, "100,000 healthy events should append without loss");
    Check(repository.EndEncounter(EncounterClose{
              encounter_id, "inactivity", 101000, 100'000'000,
              500'000, 0, 5000.0, 100.0, {}}, error),
          "volume encounter should close");
    Check(repository.CloseReport(ReportClose{"plugin_stop", 101001, false}, error),
          "volume report should close");
    const auto history = repository.History();
    Check(history.size() == 1 && history[0].event_count == 100'000,
          "history should retain the exact event count");
    const auto report_path = root / "reports" / report_id;
    Check(std::filesystem::exists(report_path / "events-000002.jsonl"),
          "large capture should create multiple event segments");
    const std::string index = ReadAll(report_path / "index.json");
    Check(index.find("\"content_sha256\":\"") != std::string::npos &&
              index.find("events-000002.jsonl") != std::string::npos,
          "segment index should contain hashes and multiple segments");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    ClosedEncounterProducesRecoverableJsonlReport();
    WorkerDrainsCommandsBeforeStopping();
    RecoveryPersistsUncleanShutdownState();
    HundredThousandEventsAreSegmentedWithoutLoss();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All damage log tests passed\n";
    return 0;
}
