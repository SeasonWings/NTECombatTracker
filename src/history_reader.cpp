#include "history_reader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <map>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <tuple>

namespace nte::combat_stats::overlay {
namespace {

using Json = nlohmann::json;

template <typename Integer>
Integer Decimal(const Json& value) {
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<Integer>();
    }
    if (!value.is_string()) throw std::runtime_error("decimal field is not a string");
    const auto& text = value.get_ref<const std::string&>();
    Integer result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error("decimal field is invalid");
    }
    return result;
}

bool IsUuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t index{}; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

bool IsEventSegment(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    if (name.size() != 19 || !name.starts_with("events-") || !name.ends_with(".jsonl")) {
        return false;
    }
    return std::ranges::all_of(name.substr(7, 6), [](const char character) {
        return character >= '0' && character <= '9';
    });
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream content;
    content << stream.rdbuf();
    if (!stream.eof() && stream.fail()) return std::nullopt;
    return content.str();
}

HistoricalEncounter ParseSummary(
    const Json& value,
    const std::string& report_id,
    const std::string& game_build = {}) {
    HistoricalEncounter result;
    result.report_id = report_id;
    result.encounter_id = value.at("encounter_id").get<std::string>();
    result.game_build = game_build;
    result.started_utc_ms = value.at("started_utc_ms").get<std::int64_t>();
    result.closed_utc_ms = value.at("closed_utc_ms").get<std::int64_t>();
    result.duration_seconds = value.at("duration_seconds").get<double>();
    result.outgoing_damage = Decimal<std::int64_t>(value.at("outgoing_damage"));
    result.incoming_damage = Decimal<std::int64_t>(value.at("incoming_damage"));
    result.encounter_dps = value.at("encounter_dps").get<double>();
    result.end_reason = value.at("end_reason").get<std::string>();
    result.complete = value.value("complete", false);
    const std::string critical_status = value.value(
        "critical_data_status", std::string{"unavailable"});
    if (critical_status == "available") {
        result.critical_data_status = CriticalDataStatus::Available;
    } else if (critical_status == "partial") {
        result.critical_data_status = CriticalDataStatus::Partial;
    } else {
        result.critical_data_status = CriticalDataStatus::Unavailable;
    }
    if (const auto histogram = value.find("damage_flag_histogram");
        histogram != value.end()) {
        for (const auto& item : *histogram) {
            result.damage_flag_histogram.push_back({
                item.at("flags").get<std::uint32_t>(),
                Decimal<std::uint64_t>(item.at("count"))});
        }
    }
    return result;
}

std::string ShortName(const std::string& value);

struct ActorName {
    Handle handle;
    std::string technical_name;
    std::string display_name;
    std::string name_origin;
    std::string resource_version;
};

struct AbilityName {
    std::uint64_t source_id{};
    std::string technical_name;
    std::string display_name;
    std::string name_origin;
    std::string resource_version;
};

std::unordered_map<std::uint32_t, ActorName> LoadActors(
    const std::filesystem::path& report) {
    std::unordered_map<std::uint32_t, ActorName> result;
    std::ifstream stream(report / "actors.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        const Json value = Json::parse(line);
        const std::uint32_t id = value.at("actor_id").get<std::uint32_t>();
        const auto& handle = value.at("handle");
        const std::string technical = value.value(
            "technical_name", value.at("path").get<std::string>());
        result.emplace(id, ActorName{
            Handle{Decimal<std::uint64_t>(handle.at("id")),
                   Decimal<std::uint64_t>(handle.at("generation"))},
            technical, value.value("display_name", ShortName(technical)),
            value.value("name_origin", std::string{"technical"}),
            value.value("name_resource_version",
                value.value("catalog_version", std::string{}))});
    }
    return result;
}

std::unordered_map<std::uint32_t, AbilityName> LoadAbilities(
    const std::filesystem::path& report) {
    std::unordered_map<std::uint32_t, AbilityName> result;
    std::ifstream stream(report / "abilities.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        const Json value = Json::parse(line);
        const std::string technical = value.value(
            "technical_name", value.at("name").get<std::string>());
        result.emplace(value.at("ability_id").get<std::uint32_t>(), AbilityName{
            Decimal<std::uint64_t>(value.at("source_id")),
            technical, value.value("display_name", ShortName(technical)),
            value.value("name_origin", std::string{"technical"}),
            value.value("name_resource_version",
                value.value("catalog_version", std::string{}))});
    }
    return result;
}

CombatEventDetail ParseEvent(const Json& value) {
    CombatEventDetail result;
    result.encounter_id = value.at("encounter_id").get<std::string>();
    result.sequence = Decimal<std::uint64_t>(value.at("sequence"));
    result.tick_sequence = Decimal<std::uint64_t>(value.at("tick_sequence"));
    result.flags = value.at("flags").get<std::uint32_t>();
    result.source_id = Decimal<std::uint64_t>(value.at("source_id"));
    const auto& attacker = value.at("attacker");
    const auto& victim = value.at("victim");
    result.attacker = {Decimal<std::uint64_t>(attacker.at("id")),
        Decimal<std::uint64_t>(attacker.at("generation"))};
    result.victim = {Decimal<std::uint64_t>(victim.at("id")),
        Decimal<std::uint64_t>(victim.at("generation"))};
    result.final_damage = Decimal<std::int64_t>(value.at("final_damage"));
    result.basic_damage = Decimal<std::int64_t>(value.at("basic_damage"));
    result.display_damage = Decimal<std::int64_t>(value.at("display_damage"));
    result.received_utc_ms = value.at("received_utc_ms").get<std::int64_t>();
    result.received_offset_us = Decimal<std::uint64_t>(value.at("received_offset_us"));
    result.source = value.at("source_name").get<std::string>();
    result.source_display_name = value.value("source_display_name", ShortName(result.source));
    result.source_name_origin = value.value("source_name_origin", std::string{"technical"});
    result.source_resource_version = value.value(
        "name_resource_version",
        value.value("name_catalog_version", std::string{}));
    result.attacker_name = value.value("attacker_name", std::string{});
    result.attacker_display_name = value.value(
        "attacker_display_name", ShortName(result.attacker_name));
    result.attacker_name_origin = value.value(
        "attacker_name_origin", std::string{"technical"});
    result.attacker_resource_version = value.value(
        "name_resource_version",
        value.value("name_catalog_version", std::string{}));
    result.victim_name = value.value("victim_name", std::string{});
    result.victim_display_name = value.value(
        "victim_display_name", ShortName(result.victim_name));
    result.victim_name_origin = value.value(
        "victim_name_origin", std::string{"technical"});
    result.victim_resource_version = value.value(
        "name_resource_version",
        value.value("name_catalog_version", std::string{}));
    return result;
}

std::string ShortName(const std::string& value) {
    const auto slash = value.find_last_of("/\\");
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

}  // namespace

CombatLogReader::CombatLogReader(std::filesystem::path root) : root_(std::move(root)) {}

std::vector<HistoricalEncounter> CombatLogReader::History(std::string& error) const {
    error.clear();
    std::vector<HistoricalEncounter> result;
    try {
        const auto catalog_text = ReadFile(root_ / "catalog.json");
        if (!catalog_text) {
            std::error_code filesystem_error;
            if (std::filesystem::exists(root_, filesystem_error) && !filesystem_error) {
                error = "history catalog is temporarily unavailable";
            }
            return result;
        }
        const Json catalog = Json::parse(*catalog_text);
        for (const auto& report : catalog.at("reports")) {
            const std::string report_id = report.at("report_id").get<std::string>();
            if (!IsUuid(report_id)) continue;
            std::string game_build;
            if (const auto report_text = ReadFile(
                    root_ / "reports" / report_id / "report.json")) {
                try {
                    game_build = Json::parse(*report_text).value(
                        "game_build", std::string{});
                } catch (const std::exception& exception) {
                    error = exception.what();
                }
            }
            std::ifstream encounters(root_ / "reports" / report_id / "encounters.jsonl",
                std::ios::binary);
            std::string line;
            while (std::getline(encounters, line)) {
                if (line.empty()) continue;
                try {
                    auto summary = ParseSummary(Json::parse(line), report_id, game_build);
                    if (IsUuid(summary.encounter_id)) result.push_back(std::move(summary));
                } catch (const std::exception& exception) {
                    error = exception.what();
                }
            }
        }
        std::ranges::sort(result, [](const auto& left, const auto& right) {
            if (left.started_utc_ms != right.started_utc_ms) {
                return left.started_utc_ms > right.started_utc_ms;
            }
            return left.encounter_id < right.encounter_id;
        });
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return result;
}

std::vector<CombatEventDetail> CombatLogReader::Events(
    const std::string& report_id,
    const std::string& encounter_id,
    std::string& error) const {
    error.clear();
    std::vector<CombatEventDetail> result;
    if (!IsUuid(report_id) || !IsUuid(encounter_id)) {
        error = "invalid report or encounter id";
        return result;
    }
    try {
        const auto directory = root_ / "reports" / report_id;
        const auto actors = LoadActors(directory);
        const auto abilities = LoadAbilities(directory);
        std::unordered_map<Handle, ActorName, HandleHash> actor_names;
        for (const auto& [id, actor] : actors) {
            static_cast<void>(id);
            actor_names.insert_or_assign(actor.handle, actor);
        }
        std::unordered_map<std::uint64_t, AbilityName> ability_names;
        for (const auto& [id, ability] : abilities) {
            static_cast<void>(id);
            ability_names.insert_or_assign(ability.source_id, ability);
        }
        std::error_code filesystem_error;
        std::vector<std::filesystem::path> segments;
        for (const auto& entry : std::filesystem::directory_iterator(
                 directory, filesystem_error)) {
            if (!filesystem_error && entry.is_regular_file() && IsEventSegment(entry.path())) {
                segments.push_back(entry.path());
            }
        }
        if (filesystem_error) throw std::runtime_error(filesystem_error.message());
        std::ranges::sort(segments);
        for (const auto& segment : segments) {
            std::ifstream stream(segment, std::ios::binary);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty()) continue;
                try {
                    auto event = ParseEvent(Json::parse(line));
                    if (event.encounter_id == encounter_id) {
                        if (const auto attacker = actor_names.find(event.attacker);
                            attacker != actor_names.end()) {
                            event.attacker_name = attacker->second.technical_name;
                            if (event.attacker_name_origin == "technical" &&
                                attacker->second.name_origin != "technical") {
                                event.attacker_display_name = attacker->second.display_name;
                                event.attacker_name_origin = attacker->second.name_origin;
                                event.attacker_resource_version =
                                    attacker->second.resource_version;
                            }
                        }
                        if (const auto victim = actor_names.find(event.victim);
                            victim != actor_names.end()) {
                            event.victim_name = victim->second.technical_name;
                            if (event.victim_name_origin == "technical" &&
                                victim->second.name_origin != "technical") {
                                event.victim_display_name = victim->second.display_name;
                                event.victim_name_origin = victim->second.name_origin;
                                event.victim_resource_version = victim->second.resource_version;
                            }
                        }
                        if (const auto source = ability_names.find(event.source_id);
                            source != ability_names.end() &&
                            event.source_name_origin == "technical" &&
                            source->second.name_origin != "technical") {
                            event.source_display_name = source->second.display_name;
                            event.source_name_origin = source->second.name_origin;
                            event.source_resource_version = source->second.resource_version;
                        }
                        result.push_back(std::move(event));
                    }
                } catch (const std::exception& exception) {
                    error = exception.what();
                }
            }
        }
        std::ranges::sort(result, {}, &CombatEventDetail::sequence);
        result.erase(std::unique(result.begin(), result.end(), [](const auto& left,
                         const auto& right) { return left.sequence == right.sequence; }),
            result.end());
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return result;
}

std::optional<EncounterDetail> CombatLogReader::Encounter(
    const std::string& report_id,
    const std::string& encounter_id,
    std::string& error) const {
    error.clear();
    if (!IsUuid(report_id) || !IsUuid(encounter_id)) {
        error = "invalid report or encounter id";
        return std::nullopt;
    }
    try {
        const auto directory = root_ / "reports" / report_id;
        std::ifstream stream(directory / "encounters.jsonl", std::ios::binary);
        std::string line;
        Json selected;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            const Json value = Json::parse(line);
            if (value.at("encounter_id").get<std::string>() == encounter_id) {
                selected = value;
                break;
            }
        }
        if (selected.is_null()) {
            error = "encounter not found";
            return std::nullopt;
        }
        EncounterDetail detail;
        std::string game_build;
        if (const auto report_text = ReadFile(directory / "report.json")) {
            game_build = Json::parse(*report_text).value("game_build", std::string{});
        }
        detail.summary = ParseSummary(selected, report_id, game_build);
        const auto actors = LoadActors(directory);
        const auto abilities = LoadAbilities(directory);
        std::unordered_map<std::uint32_t, std::size_t> player_indices;
        for (const auto& item : selected.at("confirmed_controlled_players")) {
            PlayerRow player;
            const auto actor_id = item.at("actor_id").get<std::uint32_t>();
            if (const auto actor = actors.find(actor_id); actor != actors.end()) {
                player.handle = actor->second.handle;
                player.object_path = actor->second.technical_name;
                player.display_name = actor->second.display_name;
                player.name_origin = actor->second.name_origin;
                player.name_resource_version = actor->second.resource_version;
            }
            player.identity_key = "path:" + player.object_path;
            player.label = item.value("label", player.display_name.empty()
                ? ShortName(player.object_path) : player.display_name);
            if (player.display_name.empty()) player.display_name = player.label;
            player.name_origin = item.value("name_origin", player.name_origin.empty()
                ? std::string{"recorded"} : player.name_origin);
            player.name_resource_version = item.value(
                "name_resource_version",
                item.value("name_catalog_version", player.name_resource_version));
            player.damage = Decimal<std::int64_t>(item.at("damage"));
            player.positive_hits = Decimal<std::uint64_t>(item.at("positive_hits"));
            player.critical_hits = Decimal<std::uint64_t>(item.at("critical_hits"));
            player.head_hits = Decimal<std::uint64_t>(item.at("head_hits"));
            player.weak_hits = Decimal<std::uint64_t>(item.at("weak_hits"));
            player.damage_share = item.at("damage_share").get<double>();
            player.encounter_dps = item.at("encounter_dps").get<double>();
            player.active_dps = item.at("active_dps").get<double>();
            player.rolling_dps = item.at("rolling_dps_10s").get<double>();
            player.maximum_hit = Decimal<std::int64_t>(item.at("maximum_hit"));
            const auto ability_id = item.at("maximum_ability_id").get<std::uint32_t>();
            if (const auto ability = abilities.find(ability_id); ability != abilities.end()) {
                player.maximum_source_id = ability->second.source_id;
                player.maximum_source_name = ability->second.technical_name;
                player.maximum_source_display_name = ability->second.display_name;
                player.maximum_source_name_origin = ability->second.name_origin;
                player.maximum_source_resource_version =
                    ability->second.resource_version;
            }
            player_indices.emplace(actor_id, detail.players.size());
            detail.players.push_back(std::move(player));
        }
        if (const auto incoming = selected.find("actor_incoming"); incoming != selected.end()) {
            for (const auto& item : *incoming) {
                const auto actor_id = item.at("actor_id").get<std::uint32_t>();
                const auto player = player_indices.find(actor_id);
                if (player == player_indices.end()) continue;
                auto& row = detail.players[player->second];
                row.incoming_damage = Decimal<std::int64_t>(item.at("damage"));
                row.incoming_positive_hits = Decimal<std::uint64_t>(item.at("hits"));
                row.maximum_incoming_hit = Decimal<std::int64_t>(item.at("maximum_hit"));
                row.incoming_share = detail.summary.incoming_damage > 0
                    ? static_cast<double>(row.incoming_damage) * 100.0 /
                        static_cast<double>(detail.summary.incoming_damage)
                    : 0.0;
            }
        }
        detail.events = Events(report_id, encounter_id, error);
        if (error.empty() && !detail.events.empty()) {
            CanonicalizePlayerEvents(detail.events, detail.players);
        }
        if (error.empty()) {
            detail.summary.critical_data_status = CriticalDataStatusForEvents(
                detail.events, detail.players, detail.summary.complete);
        }
        if (error.empty()) RecomputePlayerHitCounts(
            detail.events, detail.players, detail.summary.complete);
        return detail;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

CriticalDataStatus CriticalDataStatusForEvents(
    const std::vector<CombatEventDetail>& events,
    const std::vector<PlayerRow>& players,
    const bool complete) noexcept {
    std::uint64_t positive_hits{};
    std::uint64_t metadata_hits{};
    std::uint64_t explicit_critical_hits{};
    for (const auto& event : events) {
        if (!IsPositiveDamage(event.final_damage)) continue;
        const bool outgoing = std::ranges::any_of(players, [&](const PlayerRow& player) {
            return event.attacker == player.handle;
        });
        if (!outgoing) continue;
        ++positive_hits;
        if ((event.flags & kDamageFlagClientPresented) != 0) ++metadata_hits;
        if ((event.flags & kDamageFlagCritical) != 0) ++explicit_critical_hits;
    }
    if (!complete) return CriticalDataStatus::Partial;
    if (metadata_hits != 0 || explicit_critical_hits != 0) {
        return CriticalDataStatus::Partial;
    }
    return CriticalDataStatus::Unavailable;
}

void CanonicalizePlayerEvents(
    std::vector<CombatEventDetail>& events,
    const std::vector<PlayerRow>& players) {
    for (auto& event : events) {
        for (const auto& player : players) {
            if (!player.object_path.empty() && event.attacker_name == player.object_path) {
                event.attacker = player.handle;
            }
            if (!player.object_path.empty() && event.victim_name == player.object_path) {
                event.victim = player.handle;
            }
        }
    }
}

void RecomputePlayerHitCounts(
    const std::vector<CombatEventDetail>& events,
    std::vector<PlayerRow>& players,
    const bool complete) {
    if (!complete || events.empty()) return;
    for (auto& player : players) {
        std::uint64_t positive_hits{};
        std::uint64_t critical_hits{};
        for (const auto& event : events) {
            if (event.attacker != player.handle ||
                !IsPositiveDamage(event.final_damage)) continue;
            ++positive_hits;
            if (IsCriticalPositiveDamage(event.final_damage, event.flags)) ++critical_hits;
        }
        player.positive_hits = positive_hits;
        player.critical_hits = (std::min)(critical_hits, positive_hits);
    }
}

std::vector<DamageSourceSummary> GroupDamageSources(
    const std::vector<CombatEventDetail>& events,
    const Handle player,
    const DetailDirection direction) {
    struct Key {
        std::uint64_t source_id{};
        Handle participant;
        std::string source_name;
        bool operator<(const Key& right) const {
            return std::tie(source_id, participant.id, participant.generation, source_name) <
                std::tie(right.source_id, right.participant.id,
                    right.participant.generation, right.source_name);
        }
    };
    std::map<Key, DamageSourceSummary> rows;
    for (const auto& event : events) {
        const bool matches = direction == DetailDirection::Outgoing
            ? event.attacker == player : event.victim == player;
        if (!matches || event.final_damage <= 0) continue;
        const Handle participant = direction == DetailDirection::Incoming
            ? event.attacker : Handle{};
        auto& row = rows[{event.source_id, participant, event.source}];
        row.source_id = event.source_id;
        row.participant = participant;
        row.source_name = event.source;
        row.label = direction == DetailDirection::Incoming
            ? (event.attacker_display_name.empty()
                ? ShortName(event.attacker_name) : event.attacker_display_name) + " / " +
              (event.source_display_name.empty()
                ? ShortName(event.source) : event.source_display_name)
            : (event.source_display_name.empty()
                ? ShortName(event.source) : event.source_display_name);
        row.damage += event.final_damage;
        ++row.hits;
        row.maximum_hit = (std::max)(row.maximum_hit, event.final_damage);
    }
    std::vector<DamageSourceSummary> result;
    for (auto& [key, row] : rows) {
        static_cast<void>(key);
        result.push_back(std::move(row));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.damage != right.damage) return left.damage > right.damage;
        return left.label < right.label;
    });
    return result;
}

std::vector<CombatEventDetail> FilterDamageEvents(
    const std::vector<CombatEventDetail>& events,
    const Handle player,
    const DetailDirection direction,
    const DamageSourceSummary& source) {
    std::vector<CombatEventDetail> result;
    for (const auto& event : events) {
        const bool participant_matches = direction == DetailDirection::Outgoing
            ? event.attacker == player
            : event.victim == player && event.attacker == source.participant;
        if (participant_matches && event.source_id == source.source_id &&
            event.source == source.source_name &&
            event.final_damage > 0) {
            result.push_back(event);
        }
    }
    std::ranges::sort(result, {}, &CombatEventDetail::sequence);
    return result;
}

}  // namespace nte::combat_stats::overlay
