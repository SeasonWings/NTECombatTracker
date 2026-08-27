#include "overlay_protocol.hpp"

#include <chrono>
#include <iostream>
#include <limits>

namespace {

using nte::combat_stats::NteOverlaySnapshot;

int failures{};

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

NteOverlaySnapshot ExampleSnapshot() {
    NteOverlaySnapshot message;
    message.producer_instance_id = "6ee28b4d-7039-4bc4-b068-1468f280745d";
    message.snapshot_sequence = std::numeric_limits<std::uint64_t>::max();
    message.latest_damage_sequence = std::numeric_limits<std::uint64_t>::max() - 1;
    message.sent_utc_ms = 1'725'000'000'123;
    message.capture_available = true;
    message.capture_status = "available";
    message.report_id = "report-id";
    message.encounter_id = "encounter-id";
    message.primary_target = "训练目标·甲";
    message.websocket_port = 14514;
    message.connected_clients = 1;
    message.published_messages = 22;
    message.transport_dropped_messages = 3;
    message.combat.state = nte::combat_stats::EncounterState::Active;
    message.combat.duration_seconds = 12.5;
    message.combat.outgoing_damage = std::numeric_limits<std::int64_t>::max();
    message.combat.incoming_damage = 45;
    message.combat.outgoing_positive_hits = 2;
    message.combat.encounter_dps = 1234.5;
    message.combat.rolling_dps = 500.25;
    message.combat.hp = 88.0;
    message.combat.max_hp = 100.0;
    message.combat.shield = 9.0;
    message.combat.quality.partial = true;
    nte::combat_stats::PlayerRow player;
    player.identity_key = "path:/Game/角色/player_001_C";
    player.handle = {std::numeric_limits<std::uint64_t>::max(), 7};
    player.object_path = "/Game/角色/player_001_C";
    player.label = "角色甲";
    player.damage = 700;
    player.positive_hits = 2;
    player.critical_hits = 1;
    player.head_hits = 1;
    player.weak_hits = 0;
    player.maximum_hit = 500;
    player.maximum_source_id = std::numeric_limits<std::uint64_t>::max() - 2;
    player.maximum_source_name = "终结技\"A\"";
    player.damage_share = 70.0;
    player.encounter_dps = 56.0;
    player.active_dps = 70.0;
    player.rolling_dps = 40.0;
    message.combat.players.push_back(std::move(player));
    message.combat.controlled_character_count = 1;
    return message;
}

void RoundTripPreservesVersionedNteSnapshot() {
    const auto expected = ExampleSnapshot();
    const std::string json = nte::combat_stats::SerializeOverlaySnapshot(expected);
    Check(json.find("CombatData") == std::string::npos,
          "NTE protocol must not emit ACT CombatData");
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    Check(parsed.has_value(), "valid snapshot should parse");
    Check(error.empty(), "valid snapshot should not produce an error");
    if (!parsed) return;
    Check(parsed->snapshot_sequence == expected.snapshot_sequence,
          "uint64 snapshot sequence must retain decimal precision");
    Check(parsed->combat.outgoing_damage == expected.combat.outgoing_damage,
          "int64 damage must retain decimal precision");
    Check(parsed->primary_target == expected.primary_target,
          "Unicode target names must round-trip");
    Check(parsed->combat.players.size() == 1,
          "player rows should round-trip");
    if (!parsed->combat.players.empty()) {
        const auto& player = parsed->combat.players.front();
        Check(player.handle.id == std::numeric_limits<std::uint64_t>::max(),
              "generation handle ids must retain precision");
        Check(player.maximum_source_name == "终结技\"A\"",
              "ability names must use correct JSON escaping");
    }
}

void ParserRejectsWrongProtocolAndAcceptsUnknownFields() {
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::size_t insertion = json.find('{') + 1;
    json.insert(insertion, "\"future_field\":{\"ignored\":true},");
    std::string error;
    Check(nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unknown fields should be ignored");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string channel = "local.nte.combat-stats.overlay";
    json.replace(json.find(channel), channel.size(), "foreign.channel");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "foreign channels must be rejected");
}

void ParserRejectsOversizedMessages() {
    std::string oversized(64U * 1024U + 1U, 'x');
    std::string error;
    Check(!nte::combat_stats::ParseOverlaySnapshot(oversized, error).has_value(),
          "messages over 64 KiB must be rejected");
}

void ParserRejectsForeignSemantics() {
    std::string error;
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string valid_type = "\"message_type\":\"snapshot\"";
    json.replace(json.find(valid_type), valid_type.size(),
        "\"message_type\":\"foreign\"");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unknown message types should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string basis = "\"timestamp_basis\":\"client_receive\"";
    json.replace(json.find(basis), basis.size(),
        "\"timestamp_basis\":\"server\"");
    Check(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "non-client timestamp semantics should be rejected");
}

}  // namespace

int main() {
    RoundTripPreservesVersionedNteSnapshot();
    ParserRejectsWrongProtocolAndAcceptsUnknownFields();
    ParserRejectsOversizedMessages();
    ParserRejectsForeignSemantics();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All overlay protocol tests passed\n";
    return 0;
}
