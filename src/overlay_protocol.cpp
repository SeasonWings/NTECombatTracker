#include "overlay_protocol.hpp"

#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <system_error>

namespace nte::combat_stats {
namespace {

using Json = nlohmann::json;

template <typename Integer>
std::string Decimal(const Integer value) {
    return std::to_string(value);
}

template <typename Integer>
Integer ParseDecimal(const Json& value, const char* field) {
    if (!value.is_string()) throw std::runtime_error(std::string(field) + " must be a string");
    const std::string& text = value.get_ref<const std::string&>();
    Integer result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string(field) + " is not a decimal integer");
    }
    return result;
}

const char* StateName(const EncounterState state) noexcept {
    switch (state) {
    case EncounterState::Idle: return "idle";
    case EncounterState::Active: return "active";
    case EncounterState::Completed: return "completed";
    }
    return "idle";
}

EncounterState ParseState(const std::string& state) {
    if (state == "idle") return EncounterState::Idle;
    if (state == "active") return EncounterState::Active;
    if (state == "completed") return EncounterState::Completed;
    throw std::runtime_error("encounter.state is invalid");
}

double Rate(const std::uint64_t count, const std::uint64_t total) noexcept {
    return total == 0 ? 0.0 : static_cast<double>(count) * 100.0 /
        static_cast<double>(total);
}

Json PlayerToJson(const PlayerRow& row) {
    return Json{
        {"identity_key", row.identity_key},
        {"handle", {{"id", Decimal(row.handle.id)},
                    {"generation", Decimal(row.handle.generation)}}},
        {"object_path", row.object_path},
        {"label", row.label},
        {"damage", Decimal(row.damage)},
        {"positive_hits", Decimal(row.positive_hits)},
        {"critical_hits", Decimal(row.critical_hits)},
        {"critical_rate", Rate(row.critical_hits, row.positive_hits)},
        {"head_hits", Decimal(row.head_hits)},
        {"head_rate", Rate(row.head_hits, row.positive_hits)},
        {"weak_hits", Decimal(row.weak_hits)},
        {"weak_rate", Rate(row.weak_hits, row.positive_hits)},
        {"maximum_hit", Decimal(row.maximum_hit)},
        {"maximum_ability", {{"source_id", Decimal(row.maximum_source_id)},
                             {"name", row.maximum_source_name}}},
        {"damage_share", row.damage_share},
        {"encounter_dps", row.encounter_dps},
        {"active_dps", row.active_dps},
        {"rolling_dps_10s", row.rolling_dps}};
}

PlayerRow PlayerFromJson(const Json& value) {
    PlayerRow row;
    row.identity_key = value.at("identity_key").get<std::string>();
    const Json& handle = value.at("handle");
    row.handle.id = ParseDecimal<std::uint64_t>(handle.at("id"), "players.handle.id");
    row.handle.generation = ParseDecimal<std::uint64_t>(
        handle.at("generation"), "players.handle.generation");
    row.object_path = value.at("object_path").get<std::string>();
    row.label = value.at("label").get<std::string>();
    row.damage = ParseDecimal<std::int64_t>(value.at("damage"), "players.damage");
    row.positive_hits = ParseDecimal<std::uint64_t>(
        value.at("positive_hits"), "players.positive_hits");
    row.critical_hits = ParseDecimal<std::uint64_t>(
        value.at("critical_hits"), "players.critical_hits");
    row.head_hits = ParseDecimal<std::uint64_t>(
        value.at("head_hits"), "players.head_hits");
    row.weak_hits = ParseDecimal<std::uint64_t>(
        value.at("weak_hits"), "players.weak_hits");
    row.maximum_hit = ParseDecimal<std::int64_t>(
        value.at("maximum_hit"), "players.maximum_hit");
    const Json& ability = value.at("maximum_ability");
    row.maximum_source_id = ParseDecimal<std::uint64_t>(
        ability.at("source_id"), "players.maximum_ability.source_id");
    row.maximum_source_name = ability.at("name").get<std::string>();
    row.damage_share = value.at("damage_share").get<double>();
    row.encounter_dps = value.at("encounter_dps").get<double>();
    row.active_dps = value.at("active_dps").get<double>();
    row.rolling_dps = value.at("rolling_dps_10s").get<double>();
    return row;
}

}  // namespace

std::string SerializeOverlaySnapshot(const NteOverlaySnapshot& snapshot) {
    Json players = Json::array();
    for (const PlayerRow& row : snapshot.combat.players) players.push_back(PlayerToJson(row));
    const Json document{
        {"channel", kOverlayChannel},
        {"schema_version", kOverlaySchemaVersion},
        {"metric_version", kMetricVersion},
        {"message_type", snapshot.producer_stopping ? "producer_stopping" : "snapshot"},
        {"producer_instance_id", snapshot.producer_instance_id},
        {"snapshot_sequence", Decimal(snapshot.snapshot_sequence)},
        {"latest_damage_sequence", Decimal(snapshot.latest_damage_sequence)},
        {"sent_utc_ms", Decimal(snapshot.sent_utc_ms)},
        {"timestamp_basis", "client_receive"},
        {"capture_scope", "host_visible_during_player_encounter"},
        {"capture", {{"available", snapshot.capture_available},
                     {"status", snapshot.capture_status}}},
        {"report_id", snapshot.report_id},
        {"encounter_id", snapshot.encounter_id},
        {"encounter", {{"state", StateName(snapshot.combat.state)},
                       {"duration_seconds", snapshot.combat.duration_seconds},
                       {"confirmed_controlled_count",
                        Decimal(snapshot.combat.controlled_character_count)}}},
        {"summary", {{"outgoing_damage", Decimal(snapshot.combat.outgoing_damage)},
                     {"incoming_damage", Decimal(snapshot.combat.incoming_damage)},
                     {"outgoing_positive_hits",
                      Decimal(snapshot.combat.outgoing_positive_hits)},
                     {"encounter_dps", snapshot.combat.encounter_dps},
                     {"rolling_dps_10s", snapshot.combat.rolling_dps},
                     {"hp", snapshot.combat.hp},
                     {"max_hp", snapshot.combat.max_hp},
                     {"shield", snapshot.combat.shield},
                     {"primary_target", snapshot.primary_target}}},
        {"players", std::move(players)},
        {"quality", {{"partial", snapshot.combat.quality.partial},
                     {"overflow", snapshot.combat.quality.overflow},
                     {"cursor_gap", snapshot.combat.quality.cursor_gap},
                     {"aggregate_limit", snapshot.combat.quality.aggregate_truncated}}},
        {"transport", {{"port", snapshot.websocket_port},
                       {"connected_clients", snapshot.connected_clients},
                       {"published_messages", Decimal(snapshot.published_messages)},
                       {"dropped_messages", Decimal(snapshot.transport_dropped_messages)}}}};
    return document.dump();
}

std::optional<NteOverlaySnapshot> ParseOverlaySnapshot(
    const std::string_view text,
    std::string& error) {
    error.clear();
    if (text.size() > kMaximumOverlayMessageBytes) {
        error = "overlay message exceeds 64 KiB";
        return std::nullopt;
    }
    try {
        const Json document = Json::parse(text.begin(), text.end());
        if (document.at("channel").get<std::string>() != kOverlayChannel) {
            throw std::runtime_error("overlay channel is not supported");
        }
        if (document.at("schema_version").get<std::uint32_t>() !=
            kOverlaySchemaVersion) {
            throw std::runtime_error("overlay schema version is not supported");
        }
        if (document.at("metric_version").get<std::uint32_t>() != kMetricVersion) {
            throw std::runtime_error("metric version is not supported");
        }
        if (document.at("timestamp_basis").get<std::string>() != "client_receive") {
            throw std::runtime_error("timestamp basis is not supported");
        }
        if (document.at("capture_scope").get<std::string>() !=
            "host_visible_during_player_encounter") {
            throw std::runtime_error("capture scope is not supported");
        }
        const std::string message_type = document.at("message_type").get<std::string>();
        if (message_type != "snapshot" && message_type != "producer_stopping") {
            throw std::runtime_error("overlay message type is not supported");
        }
        NteOverlaySnapshot result;
        result.producer_stopping = message_type == "producer_stopping";
        result.producer_instance_id = document.at("producer_instance_id").get<std::string>();
        result.snapshot_sequence = ParseDecimal<std::uint64_t>(
            document.at("snapshot_sequence"), "snapshot_sequence");
        result.latest_damage_sequence = ParseDecimal<std::uint64_t>(
            document.at("latest_damage_sequence"), "latest_damage_sequence");
        result.sent_utc_ms = ParseDecimal<std::int64_t>(
            document.at("sent_utc_ms"), "sent_utc_ms");
        const Json& capture = document.at("capture");
        result.capture_available = capture.at("available").get<bool>();
        result.capture_status = capture.at("status").get<std::string>();
        result.report_id = document.at("report_id").get<std::string>();
        result.encounter_id = document.at("encounter_id").get<std::string>();
        const Json& encounter = document.at("encounter");
        result.combat.state = ParseState(encounter.at("state").get<std::string>());
        result.combat.duration_seconds = encounter.at("duration_seconds").get<double>();
        result.combat.controlled_character_count = ParseDecimal<std::size_t>(
            encounter.at("confirmed_controlled_count"),
            "encounter.confirmed_controlled_count");
        const Json& summary = document.at("summary");
        result.combat.outgoing_damage = ParseDecimal<std::int64_t>(
            summary.at("outgoing_damage"), "summary.outgoing_damage");
        result.combat.incoming_damage = ParseDecimal<std::int64_t>(
            summary.at("incoming_damage"), "summary.incoming_damage");
        result.combat.outgoing_positive_hits = ParseDecimal<std::uint64_t>(
            summary.at("outgoing_positive_hits"), "summary.outgoing_positive_hits");
        result.combat.encounter_dps = summary.at("encounter_dps").get<double>();
        result.combat.rolling_dps = summary.at("rolling_dps_10s").get<double>();
        result.combat.hp = summary.at("hp").get<double>();
        result.combat.max_hp = summary.at("max_hp").get<double>();
        result.combat.shield = summary.at("shield").get<double>();
        result.primary_target = summary.at("primary_target").get<std::string>();
        for (const Json& player : document.at("players")) {
            result.combat.players.push_back(PlayerFromJson(player));
        }
        const Json& quality = document.at("quality");
        result.combat.quality.partial = quality.at("partial").get<bool>();
        result.combat.quality.overflow = quality.at("overflow").get<bool>();
        result.combat.quality.cursor_gap = quality.at("cursor_gap").get<bool>();
        result.combat.quality.aggregate_truncated =
            quality.at("aggregate_limit").get<bool>();
        const Json& transport = document.at("transport");
        result.websocket_port = transport.at("port").get<std::uint16_t>();
        result.connected_clients = transport.at("connected_clients").get<std::uint32_t>();
        result.published_messages = ParseDecimal<std::uint64_t>(
            transport.at("published_messages"), "transport.published_messages");
        result.transport_dropped_messages = ParseDecimal<std::uint64_t>(
            transport.at("dropped_messages"), "transport.dropped_messages");
        return result;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

}  // namespace nte::combat_stats
