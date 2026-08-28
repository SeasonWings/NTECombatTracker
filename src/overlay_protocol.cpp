#include "overlay_protocol.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

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

const char* DirectionName(const EventDirection direction) noexcept {
    switch (direction) {
    case EventDirection::Visible: return "visible";
    case EventDirection::Outgoing: return "outgoing";
    case EventDirection::Incoming: return "incoming";
    case EventDirection::Both: return "both";
    }
    return "visible";
}

EventDirection ParseDirection(const std::string& direction) {
    if (direction == "visible") return EventDirection::Visible;
    if (direction == "outgoing") return EventDirection::Outgoing;
    if (direction == "incoming") return EventDirection::Incoming;
    if (direction == "both") return EventDirection::Both;
    throw std::runtime_error("recent_events.direction is invalid");
}

EncounterState ParseState(const std::string& state) {
    if (state == "idle") return EncounterState::Idle;
    if (state == "active") return EncounterState::Active;
    if (state == "completed") return EncounterState::Completed;
    throw std::runtime_error("encounter.state is invalid");
}

void ValidateRate(const Json& value, const double expected, const char* field) {
    const double actual = value.get<double>();
    if (!std::isfinite(actual) || std::abs(actual - expected) > 0.000001) {
        throw std::runtime_error(std::string(field) + " is inconsistent with hit counts");
    }
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
        {"critical_rate", PlayerCriticalRate(row)},
        {"head_hits", Decimal(row.head_hits)},
        {"head_rate", PlayerHeadRate(row)},
        {"weak_hits", Decimal(row.weak_hits)},
        {"weak_rate", PlayerWeakRate(row)},
        {"maximum_hit", Decimal(row.maximum_hit)},
        {"maximum_ability", {{"source_id", Decimal(row.maximum_source_id)},
                             {"name", row.maximum_source_name}}},
        {"damage_share", row.damage_share},
        {"encounter_dps", row.encounter_dps},
        {"active_dps", row.active_dps},
        {"rolling_dps_10s", row.rolling_dps},
        {"incoming_metrics", {
            {"damage", Decimal(row.incoming_damage)},
            {"positive_hits", Decimal(row.incoming_positive_hits)},
            {"maximum_hit", Decimal(row.maximum_incoming_hit)},
            {"share", row.incoming_share}}}};
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
    if (row.critical_hits > row.positive_hits) {
        throw std::runtime_error(
            "players.critical_hits must not exceed players.positive_hits");
    }
    row.head_hits = ParseDecimal<std::uint64_t>(
        value.at("head_hits"), "players.head_hits");
    row.weak_hits = ParseDecimal<std::uint64_t>(
        value.at("weak_hits"), "players.weak_hits");
    ValidateRate(value.at("critical_rate"), PlayerCriticalRate(row),
        "players.critical_rate");
    ValidateRate(value.at("head_rate"), PlayerHeadRate(row),
        "players.head_rate");
    ValidateRate(value.at("weak_rate"), PlayerWeakRate(row),
        "players.weak_rate");
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
    if (const auto incoming = value.find("incoming_metrics"); incoming != value.end()) {
        row.incoming_damage = ParseDecimal<std::int64_t>(
            incoming->at("damage"), "players.incoming_metrics.damage");
        row.incoming_positive_hits = ParseDecimal<std::uint64_t>(
            incoming->at("positive_hits"), "players.incoming_metrics.positive_hits");
        row.maximum_incoming_hit = ParseDecimal<std::int64_t>(
            incoming->at("maximum_hit"), "players.incoming_metrics.maximum_hit");
        row.incoming_share = incoming->at("share").get<double>();
    }
    return row;
}

Json RecentEventToJson(const RecentEvent& event) {
    return Json{
        {"sequence", Decimal(event.sequence)},
        {"tick_sequence", Decimal(event.tick_sequence)},
        {"flags", event.flags},
        {"direction", DirectionName(event.direction)},
        {"source_id", Decimal(event.source_id)},
        {"attacker_handle", {{"id", Decimal(event.attacker_handle.id)},
                             {"generation", Decimal(event.attacker_handle.generation)}}},
        {"victim_handle", {{"id", Decimal(event.victim_handle.id)},
                           {"generation", Decimal(event.victim_handle.generation)}}},
        {"final_damage", Decimal(event.final_damage)},
        {"display_damage", Decimal(event.display_damage)},
        {"basic_damage", Decimal(event.basic_damage)},
        {"received_utc_ms", Decimal(event.received_utc_ms)},
        {"received_offset_us", Decimal(event.received_offset_us)},
        {"source", event.source}, {"attacker", event.attacker}, {"victim", event.victim}};
}

RecentEvent RecentEventFromJson(const Json& value) {
    RecentEvent event;
    event.sequence = ParseDecimal<std::uint64_t>(value.at("sequence"),
        "recent_events.sequence");
    event.tick_sequence = ParseDecimal<std::uint64_t>(value.at("tick_sequence"),
        "recent_events.tick_sequence");
    event.flags = value.at("flags").get<std::uint32_t>();
    event.direction = ParseDirection(value.at("direction").get<std::string>());
    event.source_id = ParseDecimal<std::uint64_t>(value.at("source_id"),
        "recent_events.source_id");
    const auto parse_handle = [](const Json& handle, const char* prefix) {
        return Handle{
            ParseDecimal<std::uint64_t>(handle.at("id"), prefix),
            ParseDecimal<std::uint64_t>(handle.at("generation"), prefix)};
    };
    event.attacker_handle = parse_handle(value.at("attacker_handle"),
        "recent_events.attacker_handle");
    event.victim_handle = parse_handle(value.at("victim_handle"),
        "recent_events.victim_handle");
    event.final_damage = ParseDecimal<std::int64_t>(value.at("final_damage"),
        "recent_events.final_damage");
    event.display_damage = ParseDecimal<std::int64_t>(value.at("display_damage"),
        "recent_events.display_damage");
    event.basic_damage = ParseDecimal<std::int64_t>(value.at("basic_damage"),
        "recent_events.basic_damage");
    event.received_utc_ms = ParseDecimal<std::int64_t>(value.at("received_utc_ms"),
        "recent_events.received_utc_ms");
    event.received_offset_us = ParseDecimal<std::uint64_t>(value.at("received_offset_us"),
        "recent_events.received_offset_us");
    event.source = value.at("source").get<std::string>();
    event.attacker = value.at("attacker").get<std::string>();
    event.victim = value.at("victim").get<std::string>();
    return event;
}

}  // namespace

NteOverlaySnapshot::NteOverlaySnapshot(Data data)
    : producer_instance_id(std::move(data.producer_instance_id)),
      snapshot_sequence(data.snapshot_sequence),
      latest_damage_sequence(data.latest_damage_sequence),
      sent_utc_ms(data.sent_utc_ms),
      producer_stopping(data.producer_stopping),
      capture_available(data.capture_available),
      capture_status(std::move(data.capture_status)),
      control_status(std::move(data.control_status)),
      report_id(std::move(data.report_id)),
      encounter_id(std::move(data.encounter_id)),
      primary_target(std::move(data.primary_target)),
      websocket_port(data.websocket_port),
      connected_clients(data.connected_clients),
      published_messages(data.published_messages),
      transport_dropped_messages(data.transport_dropped_messages),
      combat(std::move(data.combat)) {}

std::string SerializeOverlaySnapshot(const NteOverlaySnapshot& snapshot) {
    Json players = Json::array();
    for (const PlayerRow& row : snapshot.combat.players) players.push_back(PlayerToJson(row));
    Json recent_events = Json::array();
    constexpr std::size_t kMaximumRecentEvents = 128;
    const std::size_t first = snapshot.combat.recent_events.size() > kMaximumRecentEvents
        ? snapshot.combat.recent_events.size() - kMaximumRecentEvents : 0;
    for (std::size_t index = first; index < snapshot.combat.recent_events.size(); ++index) {
        recent_events.push_back(RecentEventToJson(snapshot.combat.recent_events[index]));
    }
    Json document{
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
        {"control_status", snapshot.control_status},
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
        {"recent_events", std::move(recent_events)},
        {"quality", {{"partial", snapshot.combat.quality.partial},
                     {"overflow", snapshot.combat.quality.overflow},
                     {"cursor_gap", snapshot.combat.quality.cursor_gap},
                     {"aggregate_limit", snapshot.combat.quality.aggregate_truncated}}},
        {"transport", {{"port", snapshot.websocket_port},
                       {"connected_clients", snapshot.connected_clients},
                       {"published_messages", Decimal(snapshot.published_messages)},
                       {"dropped_messages", Decimal(snapshot.transport_dropped_messages)}}}};
    std::string result = document.dump();
    while (result.size() > kMaximumOverlayMessageBytes &&
           !document["recent_events"].empty()) {
        document["recent_events"].erase(document["recent_events"].begin());
        result = document.dump();
    }
    return result;
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
        NteOverlaySnapshot::Data result;
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
        if (const auto status = document.find("control_status"); status != document.end()) {
            result.control_status = status->get<std::string>();
        }
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
        if (const auto events = document.find("recent_events"); events != document.end()) {
            for (const Json& event : *events) {
                result.combat.recent_events.push_back(RecentEventFromJson(event));
            }
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
        return NteOverlaySnapshot(std::move(result));
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

}  // namespace nte::combat_stats
