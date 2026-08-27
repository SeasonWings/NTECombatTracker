#include "combat_stats_engine.hpp"
#include "damage_cursor.hpp"
#include "log_worker.hpp"

#include <anomaly/sdk/anomaly_sdk.h>
#include <anomaly/sdk/cpp.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using nte::combat_stats::AggregateRow;
using nte::combat_stats::CombatStatsEngine;
using nte::combat_stats::EncounterState;
using nte::combat_stats::EventDirection;
using nte::combat_stats::Handle;
using nte::combat_stats::Snapshot;
using nte::combat_stats::TimePoint;

constexpr std::size_t kMaximumDrainPerUpdate = 256;
constexpr std::size_t kMaximumResolvedNameBytes = 4096;
constexpr std::size_t kMaximumRecentRows = 100;
constexpr std::size_t kMaximumNameCacheEntries = 4096;
constexpr std::size_t kMaximumServiceGapIntervals = 64;

struct NameCacheEntry {
    std::string value;
    std::uint32_t attempts{};
    std::uint64_t retry_after_sequence{};
};

struct RenderState {
    Snapshot stats;
    std::uint32_t service_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    std::uint64_t latest_sequence{};
    nte::damage_log::LogWorkerStatus log;
    std::string log_root;
    std::string report_id;
    std::string encounter_id;
};

struct LogSession {
    Handle world;
    std::string report_id;
    std::string encounter_id;
    TimePoint report_started{};
    std::int64_t report_started_utc_ms{};
    std::int64_t encounter_started_utc_ms{};
    std::uint64_t encounter_started_offset_us{};
    std::uint64_t dropped_events{};
    std::uint64_t dropped_first_sequence{};
    std::uint64_t dropped_last_sequence{};
    std::optional<std::uint64_t> service_unavailable_started_us;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> service_unavailable_intervals;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> missing_sequence_ranges;
    std::optional<std::uint64_t> open_missing_sequence_start;
    bool missing_ranges_truncated{};
    bool service_gap_truncated{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyUiServiceV1* ui{};
    CombatStatsEngine engine;
    nte::combat_stats::DamageCursor cursor;
    std::unique_ptr<nte::damage_log::DamageLogWorker> log_worker;
    std::filesystem::path log_root;
    LogSession log_session;
    std::unordered_map<std::uint64_t, NameCacheEntry> source_names;
    std::unordered_map<Handle, NameCacheEntry, nte::combat_stats::HandleHash>
        participant_names;
    std::mutex mutex;
    RenderState render;
    bool reset_requested{};
    std::string delete_request;
    std::string pending_delete;
    Handle observed_world;
    std::optional<std::uint64_t> pending_cursor_gap_first;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pending_missing_sequence_ranges;
    bool pending_missing_ranges_truncated{};
    std::unordered_set<std::string> expanded_reports;
} g_context;

bool AppendRange(
    std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
    const std::uint64_t first,
    const std::uint64_t last) {
    if (first > last) return true;
    if (!ranges.empty() && ranges.back().second !=
            (std::numeric_limits<std::uint64_t>::max)() &&
        ranges.back().second + 1 >= first) {
        ranges.back().second = (std::max)(ranges.back().second, last);
    } else if (ranges.size() < kMaximumServiceGapIntervals) {
        ranges.emplace_back(first, last);
    } else {
        return false;
    }
    return true;
}

void RecordMissingRange(const std::uint64_t first, const std::uint64_t last) {
    if (g_context.log_session.encounter_id.empty()) {
        if (!AppendRange(g_context.pending_missing_sequence_ranges, first, last)) {
            g_context.pending_missing_ranges_truncated = true;
        }
    } else {
        if (!AppendRange(g_context.log_session.missing_sequence_ranges, first, last)) {
            g_context.log_session.missing_ranges_truncated = true;
        }
    }
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool CombatMethodsAvailable(const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
               decltype(AnomalyNteCombatServiceV1::participant_path_utf8)>(
               service, offsetof(AnomalyNteCombatServiceV1, participant_path_utf8)) &&
        service->current_combatant != nullptr &&
        service->latest_damage_sequence != nullptr &&
        service->next_damage_event != nullptr &&
        service->statistics != nullptr &&
        service->source_name_utf8 != nullptr &&
        service->participant_path_utf8 != nullptr;
}

Handle ConvertHandle(const AnomalyGenerationHandleV1 handle) noexcept {
    return {handle.id, handle.generation};
}

bool IsSizingStatus(const AnomalyStatusV1 status) noexcept {
    return status.code == ANOMALY_STATUS_V1_OK ||
        status.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL;
}

template <typename Resolver>
std::string ResolveName(Resolver resolver, const char* fallback) {
    std::size_t size{};
    if (!IsSizingStatus(resolver(nullptr, &size)) ||
        size <= 1 || size > kMaximumResolvedNameBytes) {
        return fallback;
    }
    std::string value(size, '\0');
    if (resolver(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return fallback;
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    return value.empty() ? fallback : value;
}

std::string ResolveSource(
    const AnomalyNteCombatServiceV1* service,
    const std::uint64_t source_id) {
    if (source_id == 0) return "Unknown source";
    return ResolveName(
        [service, source_id](char* destination, std::size_t* size) {
            return service->source_name_utf8(
                service->user, source_id, destination, size);
        }, "Unknown source");
}

std::string ResolveParticipant(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyGenerationHandleV1 participant) {
    if (participant.id == 0) return "Unknown participant";
    return ResolveName(
        [service, participant](char* destination, std::size_t* size) {
            return service->participant_path_utf8(
                service->user, participant, destination, size);
        }, "Unknown participant");
}

std::string ResolveSourceCached(
    const AnomalyNteCombatServiceV1* service,
    const std::uint64_t source_id,
    const std::uint64_t sequence) {
    if (source_id == 0) return "Unknown source";
    auto found = g_context.source_names.find(source_id);
    if (found != g_context.source_names.end()) {
        if (!found->second.value.empty()) return found->second.value;
        if (found->second.attempts >= 3 || sequence < found->second.retry_after_sequence) {
            return "Unknown source";
        }
    } else if (g_context.source_names.size() >= kMaximumNameCacheEntries) {
        return "Unknown source";
    } else {
        found = g_context.source_names.emplace(source_id, NameCacheEntry{}).first;
    }
    const std::string resolved = ResolveSource(service, source_id);
    if (!resolved.starts_with("Unknown")) found->second.value = resolved;
    ++found->second.attempts;
    found->second.retry_after_sequence = sequence + 64;
    return resolved;
}

std::string ResolveParticipantCached(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyGenerationHandleV1 participant,
    const std::uint64_t sequence) {
    const Handle handle = ConvertHandle(participant);
    if (handle.id == 0) return "Unknown participant";
    auto found = g_context.participant_names.find(handle);
    if (found != g_context.participant_names.end()) {
        if (!found->second.value.empty()) return found->second.value;
        if (found->second.attempts >= 3 || sequence < found->second.retry_after_sequence) {
            return "Unknown participant";
        }
    } else if (g_context.participant_names.size() >= kMaximumNameCacheEntries) {
        return "Unknown participant";
    } else {
        found = g_context.participant_names.emplace(handle, NameCacheEntry{}).first;
    }
    const std::string resolved = ResolveParticipant(service, participant);
    if (!resolved.starts_with("Unknown")) found->second.value = resolved;
    ++found->second.attempts;
    found->second.retry_after_sequence = sequence + 64;
    return resolved;
}

std::string ShortName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    const std::size_t position = slash == std::string::npos
        ? dot
        : (dot == std::string::npos ? slash : (std::max)(slash, dot));
    if (position == std::string::npos || position + 1 >= path.size()) return path;
    return path.substr(position + 1);
}

const char* StateName(const EncounterState state) noexcept {
    switch (state) {
    case EncounterState::Idle: return "等待战斗";
    case EncounterState::Active: return "战斗中";
    case EncounterState::Completed: return "已结束";
    }
    return "未知";
}

const char* StatusName(const std::uint32_t status) noexcept {
    switch (status) {
    case ANOMALY_STATUS_V1_OK: return "可用";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "不可用（Profile 或服务未就绪）";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "权限不足";
    default: return "采集异常";
    }
}

std::int64_t UtcNowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string FormatUtcDate(const std::int64_t utc_ms) {
    const auto point = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(utc_ms));
    const std::time_t time = std::chrono::system_clock::to_time_t(point);
    std::tm utc{};
    if (gmtime_s(&utc, &time) != 0) return std::to_string(utc_ms);
    std::array<char, 32> value{};
    std::strftime(value.data(), value.size(), "%Y-%m-%d %H:%M:%S UTC", &utc);
    return value.data();
}

std::uint64_t ReportOffsetMicroseconds(const TimePoint now) {
    if (g_context.log_session.report_started == TimePoint{}) return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - g_context.log_session.report_started).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

std::string ResolveBuildId(const anomaly::sdk::Host& host) {
    const auto service = host.Query<AnomalyNteBuildServiceV1>(
        ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION);
    if (!service || service->build_id == nullptr) return "unknown";
    return ResolveName([service = service.get()](char* destination, std::size_t* size) {
        return service->build_id(service->user, destination, size);
    }, "unknown");
}

std::string ResolveRuntimeVersion(const anomaly::sdk::Host& host) {
    const auto service = host.Query<AnomalyRuntimeInfoServiceV1>(
        ANOMALY_RUNTIME_INFO_SERVICE_V1_ID, ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION);
    if (!service || service->runtime_version_utf8 == nullptr) return "unknown";
    return ResolveName([service = service.get()](char* destination, std::size_t* size) {
        return service->runtime_version_utf8(service->user, destination, size);
    }, "unknown");
}

bool OpenEncounterLog(
    const anomaly::sdk::Host& host,
    const Handle world,
    const TimePoint now,
    const std::int64_t utc_ms) {
    if (!g_context.log_worker) return false;
    auto& session = g_context.log_session;
    if (session.report_id.empty()) {
        session.report_id = nte::damage_log::GenerateUuidV4();
        if (session.report_id.empty()) return false;
        session.world = world;
        session.report_started = now;
        session.report_started_utc_ms = utc_ms;
        if (!g_context.log_worker->Submit(nte::damage_log::ReportOpen{
                session.report_id, world, utc_ms, ResolveBuildId(host),
                "0.2.0", ResolveRuntimeVersion(host)})) return false;
    }
    session.encounter_id = nte::damage_log::GenerateUuidV4();
    if (session.encounter_id.empty()) return false;
    session.encounter_started_utc_ms = utc_ms;
    session.encounter_started_offset_us = ReportOffsetMicroseconds(now);
    session.dropped_events = 0;
    session.dropped_first_sequence = 0;
    session.dropped_last_sequence = 0;
    session.service_unavailable_started_us.reset();
    session.service_unavailable_intervals.clear();
    session.missing_sequence_ranges.clear();
    session.missing_sequence_ranges = std::move(
        g_context.pending_missing_sequence_ranges);
    g_context.pending_missing_sequence_ranges.clear();
    session.open_missing_sequence_start = std::exchange(
        g_context.pending_cursor_gap_first, std::nullopt);
    session.missing_ranges_truncated = g_context.pending_missing_ranges_truncated;
    g_context.pending_missing_ranges_truncated = false;
    session.service_gap_truncated = false;
    return g_context.log_worker->Submit(nte::damage_log::EncounterOpen{
        session.encounter_id, utc_ms, session.encounter_started_offset_us});
}

void FinishEncounterLog(
    const char* reason,
    const Snapshot& snapshot,
    const TimePoint now,
    const std::int64_t utc_ms) {
    auto& session = g_context.log_session;
    if (!g_context.log_worker || session.encounter_id.empty()) return;
    auto quality = snapshot.quality;
    const std::uint64_t closed_offset_us = ReportOffsetMicroseconds(now);
    if (session.service_unavailable_started_us) {
        if (session.service_unavailable_intervals.size() < kMaximumServiceGapIntervals) {
            session.service_unavailable_intervals.emplace_back(
                *session.service_unavailable_started_us, closed_offset_us);
        } else {
            session.service_gap_truncated = true;
        }
        session.service_unavailable_started_us.reset();
    }
    if (session.dropped_events != 0 || !session.service_unavailable_intervals.empty() ||
        session.service_gap_truncated || session.open_missing_sequence_start ||
        session.missing_ranges_truncated) quality.partial = true;
    static_cast<void>(g_context.log_worker->Submit(nte::damage_log::EncounterClose{
        session.encounter_id, reason, utc_ms, closed_offset_us,
        snapshot.outgoing_damage, snapshot.incoming_damage,
        snapshot.encounter_dps, snapshot.duration_seconds, quality,
        session.dropped_events, session.dropped_first_sequence,
        session.dropped_last_sequence, {}, session.service_unavailable_intervals,
        session.missing_sequence_ranges, session.open_missing_sequence_start,
        session.service_gap_truncated, session.missing_ranges_truncated}));
    session.encounter_id.clear();
    session.encounter_started_utc_ms = 0;
    session.encounter_started_offset_us = 0;
    session.dropped_events = 0;
    session.dropped_first_sequence = 0;
    session.dropped_last_sequence = 0;
    session.service_unavailable_started_us.reset();
    session.service_unavailable_intervals.clear();
    session.missing_sequence_ranges.clear();
    session.open_missing_sequence_start.reset();
    session.missing_ranges_truncated = false;
    session.service_gap_truncated = false;
}

void UpdateServiceGap(const bool available, const TimePoint now) {
    auto& session = g_context.log_session;
    if (session.encounter_id.empty()) return;
    const std::uint64_t offset = ReportOffsetMicroseconds(now);
    if (!available) {
        if (!session.service_unavailable_started_us) {
            session.service_unavailable_started_us = offset;
        }
        return;
    }
    if (!session.service_unavailable_started_us) return;
    if (session.service_unavailable_intervals.size() < kMaximumServiceGapIntervals) {
        session.service_unavailable_intervals.emplace_back(
            *session.service_unavailable_started_us, offset);
    } else {
        session.service_gap_truncated = true;
    }
    session.service_unavailable_started_us.reset();
}

void FinishReportLog(const char* reason, const std::int64_t utc_ms) {
    auto& session = g_context.log_session;
    if (g_context.log_worker && !session.report_id.empty()) {
        static_cast<void>(g_context.log_worker->Submit(
            nte::damage_log::ReportClose{reason, utc_ms, false}));
    }
    session = {};
    g_context.source_names.clear();
    g_context.participant_names.clear();
    g_context.pending_cursor_gap_first.reset();
    g_context.pending_missing_sequence_ranges.clear();
    g_context.pending_missing_ranges_truncated = false;
}

void ResetState() {
    g_context.engine.Reset();
    g_context.cursor.Clear();
    g_context.log_session = {};
    g_context.source_names.clear();
    g_context.participant_names.clear();
    g_context.observed_world = {};
    g_context.pending_cursor_gap_first.reset();
    g_context.pending_missing_sequence_ranges.clear();
    g_context.pending_missing_ranges_truncated = false;
    std::scoped_lock lock(g_context.mutex);
    g_context.render = {};
    g_context.reset_requested = false;
    g_context.delete_request.clear();
    g_context.pending_delete.clear();
    g_context.expanded_reports.clear();
}

void Update(void* context, double) {
    if (context != &g_context || g_context.host == nullptr) return;
    const auto now = nte::combat_stats::Clock::now();
    const std::int64_t utc_ms = UtcNowMilliseconds();
    const anomaly::sdk::Host host(g_context.host);
    const auto combat = host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);

    bool preflight_available{};
    if (CombatMethodsAvailable(combat.get())) {
        AnomalyNteCombatantSnapshotV1 preflight{sizeof(preflight)};
        const auto preflight_status = combat->current_combatant(
            combat->user, &preflight);
        preflight_available = preflight_status.code == ANOMALY_STATUS_V1_OK &&
            (preflight.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 &&
            (preflight.flags & ANOMALY_NTE_COMBATANT_V1_STALE) == 0;
    }
    UpdateServiceGap(preflight_available, now);
    g_context.engine.Advance(now);
    if (!g_context.log_session.encounter_id.empty() &&
        g_context.engine.state() == EncounterState::Completed) {
        FinishEncounterLog("inactivity", g_context.engine.BuildSnapshot(now), now, utc_ms);
    }

    bool reset_requested{};
    std::string delete_request;
    {
        std::scoped_lock lock(g_context.mutex);
        reset_requested = g_context.reset_requested;
        g_context.reset_requested = false;
        delete_request = std::move(g_context.delete_request);
        g_context.delete_request.clear();
    }
    if (!delete_request.empty() && g_context.log_worker) {
        static_cast<void>(g_context.log_worker->DeleteReport(std::move(delete_request)));
    }
    if (reset_requested) {
        if (!g_context.log_session.encounter_id.empty()) {
            FinishEncounterLog("manual_reset", g_context.engine.BuildSnapshot(now), now, utc_ms);
        }
        g_context.engine.Reset();
        g_context.cursor.RequestReset();
        g_context.pending_cursor_gap_first.reset();
        g_context.pending_missing_sequence_ranges.clear();
        g_context.pending_missing_ranges_truncated = false;
    }

    std::uint32_t service_status = ANOMALY_STATUS_V1_UNAVAILABLE;
    std::uint64_t latest{};
    bool sampled_partial{};
    bool sampled_overflow{};
    bool cursor_gap{};
    bool capture_available{};
    if (CombatMethodsAvailable(combat.get())) {
        latest = combat->latest_damage_sequence(combat->user);
        const std::uint64_t cursor_before_frame = g_context.cursor.value();
        const auto cursor_decision = g_context.cursor.BeginFrame(latest);
        cursor_gap = cursor_decision.cursor_gap;
        if (cursor_decision.cursor_gap && latest < cursor_before_frame) {
            RecordMissingRange(latest + 1, cursor_before_frame);
        }
        if (cursor_decision.reset_applied) {
            g_context.engine.Reset();
        }

        AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
        const AnomalyStatusV1 status = combat->current_combatant(
            combat->user, &combatant);
        service_status = status.code;
        const bool valid_combatant = status.code == ANOMALY_STATUS_V1_OK &&
            (combatant.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 &&
            (combatant.flags & ANOMALY_NTE_COMBATANT_V1_STALE) == 0;
        capture_available = valid_combatant;
        if (valid_combatant) {
            const Handle observed_world = ConvertHandle(combatant.world);
            if (g_context.observed_world.id != 0 &&
                observed_world != g_context.observed_world) {
                g_context.pending_cursor_gap_first.reset();
                g_context.pending_missing_sequence_ranges.clear();
                g_context.pending_missing_ranges_truncated = false;
            }
            g_context.observed_world = observed_world;
            if (!g_context.log_session.report_id.empty() &&
                observed_world != g_context.log_session.world) {
                if (!g_context.log_session.encounter_id.empty()) {
                    FinishEncounterLog("world_changed",
                        g_context.engine.BuildSnapshot(now), now, utc_ms);
                }
                FinishReportLog("world_changed", utc_ms);
            }
            g_context.engine.ObserveCombatant({
                observed_world, ConvertHandle(combatant.character),
                combatant.hp, combatant.max_hp, combatant.shield}, now);
            if ((combatant.flags & ANOMALY_NTE_COMBATANT_V1_PARTIAL) != 0) {
                sampled_partial = true;
            }
            for (const std::uint32_t direction : {
                     ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_ATTACKER,
                     ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_VICTIM}) {
                AnomalyNteCombatStatisticsRequestV1 request{sizeof(request)};
                request.world = combatant.world;
                request.character = combatant.character;
                request.direction = direction;
                AnomalyNteCombatStatisticsV1 statistics{sizeof(statistics)};
                if (combat->statistics(combat->user, &request, &statistics).code ==
                    ANOMALY_STATUS_V1_OK) {
                    if ((statistics.flags & ANOMALY_NTE_COMBAT_STATISTICS_V1_PARTIAL) != 0) {
                        sampled_partial = true;
                    }
                    if ((statistics.flags & ANOMALY_NTE_COMBAT_STATISTICS_V1_OVERFLOW) != 0) {
                        sampled_overflow = true;
                    }
                }
            }
        }

        if (valid_combatant) {
            for (std::size_t count{}; count < kMaximumDrainPerUpdate; ++count) {
                if (g_context.cursor.value() == latest) break;
                AnomalyNteDamageEventV1 event{sizeof(event)};
                const AnomalyStatusV1 event_status = combat->next_damage_event(
                    combat->user, g_context.cursor.value(), &event);
                if (event_status.code == ANOMALY_STATUS_V1_OK) {
                    if (!g_context.cursor.Accept(event.sequence)) {
                        service_status = ANOMALY_STATUS_V1_FAILED;
                        capture_available = false;
                        break;
                    }
                    auto& open_gap = g_context.log_session.encounter_id.empty()
                        ? g_context.pending_cursor_gap_first
                        : g_context.log_session.open_missing_sequence_start;
                    if (open_gap) {
                        if (event.sequence > *open_gap) {
                            RecordMissingRange(*open_gap,
                                event.sequence - 1);
                        }
                        open_gap.reset();
                    }
                    nte::combat_stats::DamageEvent normalized;
                    normalized.sequence = event.sequence;
                    normalized.tick_sequence = event.tick_sequence;
                    normalized.flags = event.flags;
                    normalized.world = ConvertHandle(event.world);
                    normalized.attacker = ConvertHandle(event.attacker);
                    normalized.victim = ConvertHandle(event.victim);
                    normalized.source_id = event.source_id;
                    normalized.display_damage = event.display_damage;
                    normalized.basic_damage = event.basic_damage;
                    normalized.final_damage = event.final_damage;
                    normalized.hit_location = {event.hit_location[0],
                        event.hit_location[1], event.hit_location[2]};
                    normalized.damage_type = event.damage_type;
                    normalized.display_type = event.display_type;
                    normalized.reaction_type = event.reaction_type;
                    normalized.reaction_display_type = event.reaction_display_type;
                    const bool player_related = g_context.engine.IsRelevant(
                        normalized.world, normalized.attacker, normalized.victim);
                    if (g_context.engine.state() != EncounterState::Active &&
                        player_related && !OpenEncounterLog(
                            host, normalized.world, now, utc_ms)) {
                        g_context.engine.MarkPartial(false);
                    }
                    normalized.received_utc_ms = utc_ms;
                    normalized.received_offset_us = ReportOffsetMicroseconds(now);
                    normalized.source_name = ResolveSourceCached(
                        combat.get(), event.source_id, event.sequence);
                    normalized.attacker_name = ResolveParticipantCached(
                        combat.get(), event.attacker, event.sequence);
                    normalized.victim_name = ResolveParticipantCached(
                        combat.get(), event.victim, event.sequence);
                    if (g_context.engine.AcceptDamage(normalized, now) &&
                        g_context.log_worker &&
                        !g_context.log_session.encounter_id.empty() &&
                        !g_context.log_worker->Submit(nte::damage_log::LoggedDamage{
                            normalized, utc_ms, normalized.received_offset_us,
                            player_related})) {
                        auto& session = g_context.log_session;
                        ++session.dropped_events;
                        if (session.dropped_first_sequence == 0) {
                            session.dropped_first_sequence = normalized.sequence;
                        }
                        session.dropped_last_sequence = normalized.sequence;
                        if (!AppendRange(session.missing_sequence_ranges,
                                normalized.sequence, normalized.sequence)) {
                            session.missing_ranges_truncated = true;
                        }
                        g_context.engine.MarkPartial(false);
                        g_context.engine.MarkOverflow();
                    }
                    continue;
                }
                if (event_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
                    const std::uint64_t missing_after = g_context.cursor.value();
                    if (g_context.cursor.RecoverFromNotFound(latest)) {
                        if (missing_after !=
                            (std::numeric_limits<std::uint64_t>::max)()) {
                            if (g_context.log_session.encounter_id.empty()) {
                                g_context.pending_cursor_gap_first = missing_after + 1;
                            } else {
                                g_context.log_session.open_missing_sequence_start =
                                    missing_after + 1;
                            }
                        }
                        cursor_gap = true;
                        continue;
                    }
                }
                if (event_status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
                    service_status = event_status.code;
                    capture_available = false;
                }
                break;
            }
        }
        if (sampled_partial || cursor_gap) g_context.engine.MarkPartial(cursor_gap);
        if (sampled_overflow) g_context.engine.MarkOverflow();
    } else {
        static_cast<void>(g_context.cursor.BeginFrame(std::nullopt));
    }

    UpdateServiceGap(capture_available && service_status == ANOMALY_STATUS_V1_OK, now);

    RenderState next;
    next.stats = g_context.engine.BuildSnapshot(now);
    next.service_status = service_status;
    next.latest_sequence = latest;
    if (g_context.log_worker) next.log = g_context.log_worker->Status();
    next.log_root = g_context.log_root.string();
    next.report_id = g_context.log_session.report_id;
    next.encounter_id = g_context.log_session.encounter_id;
    std::scoped_lock lock(g_context.mutex);
    g_context.render = std::move(next);
}

void Text(const AnomalyUiServiceV1* ui, const std::string& value) {
    ui->text(ui->user, anomaly::sdk::StringView(value));
}

template <typename... Args>
void TextFormat(const AnomalyUiServiceV1* ui, const char* format, Args... args) {
    std::array<char, 512> buffer{};
    std::snprintf(buffer.data(), buffer.size(), format, args...);
    Text(ui, buffer.data());
}

void DrawAggregateTable(
    const AnomalyUiServiceV1* ui,
    const char* id,
    const std::vector<AggregateRow>& rows,
    const std::int64_t total) {
    if (rows.empty()) {
        Text(ui, "暂无数据");
        return;
    }
    const bool table_available = HasField<AnomalyUiServiceV1,
        decltype(AnomalyUiServiceV1::end_table)>(
        ui, offsetof(AnomalyUiServiceV1, end_table));
    if (!table_available || ui->begin_table == nullptr || ui->table_next_row == nullptr ||
        ui->table_next_column == nullptr || ui->end_table == nullptr) {
        for (const auto& row : rows) {
            TextFormat(ui, "%s  %lld  %llu hits", row.label.c_str(),
                static_cast<long long>(row.damage),
                static_cast<unsigned long long>(row.positive_hits));
        }
        return;
    }
    if (!ui->begin_table(ui->user, anomaly::sdk::StringView(id), 6,
            ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT, 0.0F, 0.0F)) return;
    for (const auto& row : rows) {
        ui->table_next_row(ui->user);
        ui->table_next_column(ui->user); Text(ui, ShortName(row.label));
        ui->table_next_column(ui->user); TextFormat(ui, "%lld", static_cast<long long>(row.damage));
        ui->table_next_column(ui->user); TextFormat(ui, "%.1f%%", total > 0 ? row.damage * 100.0 / total : 0.0);
        ui->table_next_column(ui->user); TextFormat(ui, "%llu", static_cast<unsigned long long>(row.positive_hits));
        ui->table_next_column(ui->user); TextFormat(ui, "%.1f", row.positive_hits ? static_cast<double>(row.damage) / row.positive_hits : 0.0);
        ui->table_next_column(ui->user); TextFormat(ui, "%lld", static_cast<long long>(row.maximum_hit));
    }
    ui->end_table(ui->user);
}

void DrawOverview(const AnomalyUiServiceV1* ui, const RenderState& render) {
    const Snapshot& stats = render.stats;
    TextFormat(ui, "状态：%s  持续：%.1f 秒  上场角色：%zu",
        StateName(stats.state), stats.duration_seconds, stats.controlled_character_count);
    TextFormat(ui, "输出：%lld  遭遇 DPS：%.1f  10秒 DPS：%.1f",
        static_cast<long long>(stats.outgoing_damage), stats.encounter_dps, stats.rolling_dps);
    TextFormat(ui, "输出命中：%llu  平均：%.1f  最大：%lld",
        static_cast<unsigned long long>(stats.outgoing_positive_hits),
        stats.outgoing_positive_hits ? static_cast<double>(stats.outgoing_damage) / stats.outgoing_positive_hits : 0.0,
        static_cast<long long>(stats.maximum_outgoing_hit));
    TextFormat(ui, "承伤：%lld  次数：%llu  非正值事件：%llu",
        static_cast<long long>(stats.incoming_damage),
        static_cast<unsigned long long>(stats.incoming_positive_hits),
        static_cast<unsigned long long>(stats.non_positive_events));
    TextFormat(ui, "HP：%.1f / %.1f  护盾：%.1f", stats.hp, stats.max_hp, stats.shield);
    TextFormat(ui, "采集服务：%s  最新序列：%llu", StatusName(render.service_status),
        static_cast<unsigned long long>(render.latest_sequence));
    TextFormat(ui, "日志：%s  队列：%zu  丢失：%llu",
        render.log.running ? (render.log.ready ? "记录就绪" : "正在初始化") : "未运行",
        render.log.queue_depth,
        static_cast<unsigned long long>(render.log.dropped_events));
    if (!render.report_id.empty()) {
        std::uint64_t active_bytes{};
        for (const auto& report : render.log.history) {
            if (report.report_id == render.report_id) {
                active_bytes = report.total_bytes;
                break;
            }
        }
        TextFormat(ui, "Report：%s  Encounter：%s",
            render.report_id.c_str(), render.encounter_id.empty() ? "等待下一场" :
            render.encounter_id.c_str());
        TextFormat(ui, "当前日志大小：%.2f MiB",
            static_cast<double>(active_bytes) / (1024.0 * 1024.0));
    }
    TextFormat(ui, "日志目录：%s", render.log_root.c_str());
    if (!render.log.last_error.empty()) {
        TextFormat(ui, "写入错误：%s", render.log.last_error.c_str());
    }
    if (stats.quality.partial || stats.quality.overflow ||
        stats.quality.cursor_gap || stats.quality.aggregate_truncated) {
        TextFormat(ui, "数据质量：%s%s%s%s",
            stats.quality.partial ? "PARTIAL " : "",
            stats.quality.overflow ? "OVERFLOW " : "",
            stats.quality.cursor_gap ? "CURSOR_GAP " : "",
            stats.quality.aggregate_truncated ? "AGGREGATE_LIMIT" : "");
    }
}

void DrawHistory(const AnomalyUiServiceV1* ui, const RenderState& render) {
    if (!render.log.ready && render.log.history.empty()) {
        Text(ui, "历史索引正在加载或不可用");
        return;
    }
    if (render.log.history.empty()) {
        Text(ui, "暂无已保存战斗日志");
        return;
    }
    const std::size_t count = (std::min<std::size_t>)(50, render.log.history.size());
    for (std::size_t index{}; index < count; ++index) {
        const auto& report = render.log.history[index];
        const std::string date = FormatUtcDate(report.started_utc_ms);
        TextFormat(ui,
            "Report %s  World %llu:%llu  %s  %s  遭遇：%llu  事件：%llu  %.2f MiB  %s",
            report.report_id.c_str(),
            static_cast<unsigned long long>(report.world.id),
            static_cast<unsigned long long>(report.world.generation),
            date.c_str(),
            report.open ? "OPEN" : report.close_reason.c_str(),
            static_cast<unsigned long long>(report.encounter_count),
            static_cast<unsigned long long>(report.event_count),
            static_cast<double>(report.total_bytes) / (1024.0 * 1024.0),
            report.complete ? "完整" : "不完整");
        bool expanded{};
        {
            std::scoped_lock lock(g_context.mutex);
            expanded = g_context.expanded_reports.contains(report.report_id);
        }
        if (ui->button != nullptr) {
            const std::string toggle = std::string(expanded ? "收起" : "展开") +
                "##expand-" + report.report_id;
            if (ui->button(ui->user, anomaly::sdk::StringView(toggle), 0.0F, 0.0F)) {
                std::scoped_lock lock(g_context.mutex);
                if (expanded) g_context.expanded_reports.erase(report.report_id);
                else g_context.expanded_reports.insert(report.report_id);
                expanded = !expanded;
            }
        }
        if (expanded) for (const auto& encounter : report.encounters) {
            TextFormat(ui, "  %s  %.1f秒  输出 %lld  承伤 %lld  DPS %.1f  %s  %s",
                encounter.encounter_id.c_str(), encounter.duration_seconds,
                static_cast<long long>(encounter.outgoing_damage),
                static_cast<long long>(encounter.incoming_damage),
                encounter.encounter_dps, encounter.end_reason.c_str(),
                encounter.complete ? "完整" : "不完整");
        }
        if (!report.open && ui->button != nullptr) {
            bool pending{};
            {
                std::scoped_lock lock(g_context.mutex);
                pending = g_context.pending_delete == report.report_id;
            }
            if (!pending) {
                const std::string label = "删除此 Report##" + report.report_id;
                if (ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F)) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.pending_delete = report.report_id;
                }
            } else {
                const std::string confirm = "确认永久删除##" + report.report_id;
                const std::string cancel = "取消删除##" + report.report_id;
                if (ui->button(ui->user, anomaly::sdk::StringView(confirm), 0.0F, 0.0F)) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.delete_request = report.report_id;
                    g_context.pending_delete.clear();
                }
                if (ui->button(ui->user, anomaly::sdk::StringView(cancel), 0.0F, 0.0F)) {
                    std::scoped_lock lock(g_context.mutex);
                    g_context.pending_delete.clear();
                }
            }
        }
    }
}

void Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr ||
        ui->end_window == nullptr || ui->text == nullptr) return;
    RenderState render;
    {
        std::scoped_lock lock(g_context.mutex);
        render = g_context.render;
    }
    int open = 1;
    anomaly::sdk::UiWindow window(ui, "NTE Combat Stats", &open);
    if (!window) return;
    if (ui->button != nullptr && ui->button(
            ui->user, anomaly::sdk::StringView("重置统计"), 0.0F, 0.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.reset_requested = true;
    }

    const bool tabs = HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::end_tab_bar)>(
            ui, offsetof(AnomalyUiServiceV1, end_tab_bar)) &&
        ui->begin_tab_bar != nullptr && ui->begin_tab_item != nullptr &&
        ui->end_tab_item != nullptr && ui->end_tab_bar != nullptr;
    if (!tabs) {
        DrawOverview(ui, render);
        return;
    }
    if (ui->begin_tab_bar(ui->user, anomaly::sdk::StringView("combat-tabs"), 0)) {
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView("总览"), nullptr, 0, 1)) {
            DrawOverview(ui, render);
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView("伤害来源"), nullptr, 0, 1)) {
            DrawAggregateTable(ui, "sources", render.stats.outgoing_sources, render.stats.outgoing_damage);
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView("参与者"), nullptr, 0, 1)) {
            Text(ui, "输出目标");
            DrawAggregateTable(ui, "targets", render.stats.outgoing_targets, render.stats.outgoing_damage);
            Text(ui, "承伤来源");
            DrawAggregateTable(ui, "attackers", render.stats.incoming_sources, render.stats.incoming_damage);
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView("最近事件"), nullptr, 0, 1)) {
            const std::size_t count = (std::min)(kMaximumRecentRows, render.stats.recent_events.size());
            for (std::size_t offset{}; offset < count; ++offset) {
                const auto& event = render.stats.recent_events[render.stats.recent_events.size() - 1 - offset];
                const char* direction = event.direction == EventDirection::Both
                    ? "双向" : (event.direction == EventDirection::Outgoing ? "输出" :
                        (event.direction == EventDirection::Incoming ? "承伤" : "可见"));
                const bool critical = (event.flags & ANOMALY_NTE_DAMAGE_V1_CRITICAL) != 0;
                const bool head_hit = (event.flags & ANOMALY_NTE_DAMAGE_V1_HEAD_HIT) != 0;
                const bool weak = (event.flags & ANOMALY_NTE_DAMAGE_V1_WEAK_UNBALANCE) != 0;
                TextFormat(ui, "+%.3fs #%llu/t%llu [%s] final=%lld basic=%lld display=%lld%s%s%s%s  %s  %s -> %s",
                    static_cast<double>(event.received_offset_us) / 1'000'000.0,
                    static_cast<unsigned long long>(event.sequence),
                    static_cast<unsigned long long>(event.tick_sequence),
                    direction,
                    static_cast<long long>(event.final_damage),
                    static_cast<long long>(event.basic_damage),
                    static_cast<long long>(event.display_damage),
                    event.final_damage <= 0 ? " (不计DPS)" : "",
                    critical ? " 暴击" : "", head_hit ? " 爆头" : "",
                    weak ? " 弱点" : "",
                    ShortName(event.source).c_str(), ShortName(event.attacker).c_str(),
                    ShortName(event.victim).c_str());
            }
            if (count == 0) Text(ui, "暂无当前玩家相关事件");
            ui->end_tab_item(ui->user);
        }
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView("历史"), nullptr, 0, 1)) {
            DrawHistory(ui, render);
            ui->end_tab_item(ui->user);
        }
        ui->end_tab_bar(ui->user);
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    const auto ui = anomaly::sdk::Host(host).Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_context.host = host;
    g_context.ui = ui.get();
    ResetState();
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    ResetState();
    g_context.log_root = nte::damage_log::DefaultLogRoot();
    if (!g_context.log_root.empty()) {
        g_context.log_worker = std::make_unique<nte::damage_log::DamageLogWorker>(
            g_context.log_root);
        static_cast<void>(g_context.log_worker->Start());
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    const auto now = nte::combat_stats::Clock::now();
    const auto utc_ms = UtcNowMilliseconds();
    if (!g_context.log_session.encounter_id.empty()) {
        FinishEncounterLog("plugin_stop", g_context.engine.BuildSnapshot(now), now, utc_ms);
    }
    FinishReportLog("plugin_stop", utc_ms);
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    ResetState();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    ResetState();
    g_context.host = nullptr;
    g_context.ui = nullptr;
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("local.nte.combat-stats"),
        anomaly::sdk::StringView("NTE Combat Stats"),
        anomaly::sdk::StringView("Shikari"),
        anomaly::sdk::StringView("0.2.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
