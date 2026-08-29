#include "combat_stats_engine.hpp"
#include "app_settings.hpp"
#include "damage_cursor.hpp"
#include "event_damage_source_resolver.hpp"
#include "log_worker.hpp"
#include "resource_name_resolver.hpp"
#include "overlay_publish_policy.hpp"
#include "overlay_protocol.hpp"
#include "overlay_control.hpp"

#include <anomaly/sdk/anomaly_sdk.h>
#include <anomaly/sdk/cpp.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <shellapi.h>

namespace {

using nte::combat_stats::CombatStatsEngine;
using nte::combat_stats::EncounterState;
using nte::combat_stats::Handle;
using nte::combat_stats::Snapshot;
using nte::combat_stats::TimePoint;
using nte::combat_stats::NteOverlaySnapshot;

constexpr std::size_t kMaximumDrainPerUpdate = 256;
constexpr std::size_t kMaximumResolvedNameBytes = 4096;
constexpr std::size_t kMaximumNameCacheEntries = 4096;
constexpr std::size_t kMaximumServiceGapIntervals = 64;
constexpr std::size_t kMaximumPathBytes = 32768;

struct NameCacheEntry {
    std::string value;
    std::uint32_t attempts{};
    std::uint64_t retry_after_sequence{};
};

struct RenderState {
    std::shared_ptr<const NteOverlaySnapshot> snapshot;
    nte::damage_log::LogWorkerStatus log;
    std::string log_root;
    std::string overlay_error;
    std::string name_resource_error;
    nte::settings::OverlaySettings overlay_settings;
    bool overlay_enabled{true};
    std::string settings_status;
    std::string settings_error;
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
    std::unordered_map<Handle, NameCacheEntry, nte::combat_stats::HandleHash>
        participant_names;
    std::mutex mutex;
    RenderState render;
    Handle observed_world;
    std::optional<std::uint64_t> pending_cursor_gap_first;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pending_missing_sequence_ranges;
    bool pending_missing_ranges_truncated{};
    const AnomalyWebSocketServiceV1* websocket{};
    const AnomalyCoreServiceV1* core{};
    std::string producer_instance_id;
    std::uint64_t overlay_snapshot_sequence{};
    std::uint64_t local_overlay_drops{};
    nte::combat_stats::OverlayPublishPolicy overlay_policy;
    bool overlay_launch_attempted{};
    bool overlay_restart_pending{};
    bool toggle_overlay_requested{};
    bool open_log_root_requested{};
    std::string requested_log_root;
    std::optional<nte::settings::OverlaySettings> requested_overlay_settings;
    nte::settings::OverlaySettings overlay_settings;
    std::array<char, kMaximumPathBytes> log_root_input{};
    std::chrono::steady_clock::time_point next_settings_refresh{};
    std::string settings_status;
    std::string settings_error;
    AnomalyWebSocketServerInfoV1 overlay_server_info{sizeof(AnomalyWebSocketServerInfoV1)};
    std::string overlay_error;
    std::string control_status;
    HANDLE manual_end_event{};
    HANDLE plugin_stopping_event{};
    nte::combat_stats::ResourceNameResolver name_resolver;
    std::string name_resource_error;
} g_context;

NteOverlaySnapshot MakeOverlaySnapshot(
    Snapshot stats,
    std::uint32_t service_status,
    std::uint64_t latest_sequence,
    const std::string& report_id,
    const std::string& encounter_id,
    std::int64_t utc_ms,
    bool stopping,
    std::uint64_t snapshot_sequence);
std::optional<std::filesystem::path> PluginDirectory();

void EnsureNameResources() {
    if (!g_context.name_resolver.version().empty() &&
        g_context.name_resource_error.empty()) return;
    const auto directory = PluginDirectory();
    if (!directory) {
        g_context.name_resource_error = "插件目录不可用，名称回退到技术路径";
        g_context.name_resolver = {};
        return;
    }
    g_context.name_resolver = nte::combat_stats::ResourceNameResolver::Load(
        *directory / "res", g_context.name_resource_error);
}

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

void ResetPendingCaptureGaps() {
    g_context.pending_cursor_gap_first.reset();
    g_context.pending_missing_sequence_ranges.clear();
    g_context.pending_missing_ranges_truncated = false;
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

std::int64_t UtcNowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
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
    g_context.control_status.clear();
    auto& session = g_context.log_session;
    if (session.report_id.empty()) {
        session.report_id = nte::damage_log::GenerateUuidV4();
        if (session.report_id.empty()) return false;
        session.world = world;
        session.report_started = now;
        session.report_started_utc_ms = utc_ms;
        const std::string game_build = ResolveBuildId(host);
        EnsureNameResources();
        if (!g_context.log_worker->Submit(nte::damage_log::ReportOpen{
                session.report_id, world, utc_ms, game_build,
                "0.4.0", ResolveRuntimeVersion(host)})) return false;
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

void ResetEncounterSession() {
    auto& session = g_context.log_session;
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

bool FinishEncounterLog(
    const char* reason,
    const NteOverlaySnapshot& overlay_snapshot,
    const TimePoint now,
    const std::int64_t utc_ms) {
    const Snapshot& snapshot = overlay_snapshot.combat;
    auto& session = g_context.log_session;
    if (!g_context.log_worker || session.encounter_id.empty()) return false;
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
    nte::damage_log::EncounterClose close{
        session.encounter_id, reason, utc_ms, closed_offset_us,
        snapshot.outgoing_damage, snapshot.incoming_damage,
        snapshot.encounter_dps, snapshot.duration_seconds, quality,
        session.dropped_events, session.dropped_first_sequence,
        session.dropped_last_sequence, {}, session.service_unavailable_intervals,
        session.missing_sequence_ranges, session.open_missing_sequence_start,
        session.service_gap_truncated, session.missing_ranges_truncated,
        snapshot.players};
    close.critical_data_status = snapshot.critical_data_status;
    close.damage_flag_histogram = snapshot.damage_flag_histogram;
    if (!g_context.log_worker->Submit(std::move(close))) return false;
    std::string flush_error;
    const bool flushed = g_context.log_worker->FlushAndWait(
        std::chrono::seconds(5), flush_error);
    ResetEncounterSession();
    return flushed;
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

bool FinishReportLog(const char* reason, const std::int64_t utc_ms) {
    auto& session = g_context.log_session;
    if (session.report_id.empty()) return true;
    if (!g_context.log_worker || !g_context.log_worker->Submit(
            nte::damage_log::ReportClose{reason, utc_ms, false})) {
        return false;
    }
    std::string flush_error;
    const bool flushed = g_context.log_worker->FlushAndWait(
        std::chrono::seconds(5), flush_error);
    session = {};
    g_context.participant_names.clear();
    ResetPendingCaptureGaps();
    return flushed;
}

const char* CaptureStatusName(const std::uint32_t status) noexcept {
    switch (status) {
    case ANOMALY_STATUS_V1_OK: return "available";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "unavailable";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "permission_denied";
    default: return "error";
    }
}

std::uint32_t CaptureStatusCode(const NteOverlaySnapshot* snapshot) noexcept {
    if (snapshot == nullptr) return ANOMALY_STATUS_V1_UNAVAILABLE;
    if (snapshot->capture_status == "available") return ANOMALY_STATUS_V1_OK;
    if (snapshot->capture_status == "permission_denied") {
        return ANOMALY_STATUS_V1_PERMISSION_DENIED;
    }
    if (snapshot->capture_status == "unavailable") return ANOMALY_STATUS_V1_UNAVAILABLE;
    return ANOMALY_STATUS_V1_FAILED;
}

NteOverlaySnapshot MakeOverlaySnapshot(
    Snapshot stats,
    const std::uint32_t service_status,
    const std::uint64_t latest_sequence,
    const std::string& report_id,
    const std::string& encounter_id,
    const std::int64_t utc_ms,
    const bool stopping,
    const std::uint64_t snapshot_sequence) {
    NteOverlaySnapshot::Data data;
    data.producer_instance_id = g_context.producer_instance_id;
    data.snapshot_sequence = snapshot_sequence;
    data.latest_damage_sequence = latest_sequence;
    data.sent_utc_ms = utc_ms;
    data.producer_stopping = stopping;
    data.capture_available = service_status == ANOMALY_STATUS_V1_OK;
    data.capture_status = CaptureStatusName(service_status);
    data.control_status = g_context.control_status;
    data.report_id = report_id;
    data.encounter_id = encounter_id;
    if (!stats.outgoing_targets.empty()) {
        data.primary_target = stats.outgoing_targets.front().label;
    }
    const auto& server = g_context.overlay_server_info;
    data.websocket_port = server.port;
    data.connected_clients = server.connected_clients;
    data.published_messages = server.published_messages;
    data.transport_dropped_messages =
        server.dropped_messages + g_context.local_overlay_drops;
    data.combat = std::move(stats);
    return NteOverlaySnapshot(std::move(data));
}

std::optional<std::filesystem::path> PluginDirectory() {
    if (g_context.core == nullptr || g_context.core->plugin_directory == nullptr) {
        return std::nullopt;
    }
    std::size_t size{};
    if (!IsSizingStatus(g_context.core->plugin_directory(
            g_context.core->user, nullptr, &size)) || size <= 1 || size > 32768) {
        return std::nullopt;
    }
    std::string utf8(size, '\0');
    if (g_context.core->plugin_directory(
            g_context.core->user, utf8.data(), &size).code != ANOMALY_STATUS_V1_OK) {
        return std::nullopt;
    }
    if (const auto end = utf8.find('\0'); end != std::string::npos) utf8.resize(end);
    if (utf8.empty()) return std::nullopt;
    const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_size <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), wide.data(), wide_size) != wide_size) {
        return std::nullopt;
    }
    return std::filesystem::path(std::move(wide));
}

bool LaunchOverlay(
    const std::uint16_t port,
    const bool restart,
    std::string& error_message) {
    error_message.clear();
    if (port == 0) {
        error_message = "WebSocket listener is unavailable";
        return false;
    }
    const auto package = PluginDirectory();
    if (!package) {
        error_message = "Plugin directory is unavailable";
        return false;
    }
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(*package, error);
    if (error) {
        error_message = "Plugin directory could not be resolved";
        return false;
    }
    const std::filesystem::path executable = std::filesystem::weakly_canonical(
        root / L"NteCombatTracker.exe", error);
    if (error || executable.parent_path() != root ||
        !std::filesystem::is_regular_file(executable, error) || error) {
        error_message = "NteCombatTracker.exe is missing or outside the plugin package";
        return false;
    }
    std::wstring command = L"\"" + executable.wstring() + L"\" --port " +
        std::to_wstring(port) + L" --host-pid " +
        std::to_wstring(GetCurrentProcessId());
    if (restart) command += L" --restart";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr,
            FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, root.c_str(), &startup, &process)) {
        error_message = "CreateProcessW failed: " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

HWND OverlayWindow() {
    return FindWindowW(nte::combat_stats::overlay::kWindowClassName,
        nte::combat_stats::overlay::kWindowTitle);
}

void NotifyOverlaySettingsChanged() {
    const HWND window = OverlayWindow();
    const UINT message = RegisterWindowMessageW(
        nte::combat_stats::overlay::kReloadSettingsMessageName);
    if (window != nullptr && message != 0) PostMessageW(window, message, 0, 0);
}

void CloseOverlayWindow() {
    const HWND window = OverlayWindow();
    const UINT message = RegisterWindowMessageW(
        nte::combat_stats::overlay::kCloseOverlayMessageName);
    if (window != nullptr && message != 0) PostMessageW(window, message, 0, 0);
}

void CopyLogRootToInput(const std::filesystem::path& path) {
    const std::string utf8 = nte::settings::PathToUtf8(path);
    std::scoped_lock lock(g_context.mutex);
    std::fill(g_context.log_root_input.begin(), g_context.log_root_input.end(), '\0');
    const std::size_t count = (std::min)(
        utf8.size(), g_context.log_root_input.size() - 1);
    std::copy_n(utf8.data(), count, g_context.log_root_input.data());
}

bool WaitUntilReady(nte::damage_log::DamageLogWorker& worker, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = worker.Status();
        if (status.ready) return true;
        if (!status.running || !status.last_error.empty()) {
            error = status.last_error.empty() ? "日志线程未运行" : status.last_error;
            return false;
        }
        Sleep(10);
    }
    error = "等待日志目录初始化超时";
    return false;
}

bool SwitchLogRoot(
    const std::filesystem::path& requested,
    const TimePoint now,
    const std::int64_t utc_ms,
    std::string& error) {
    error.clear();
    if (!nte::settings::PrepareLogRoot(requested, error)) return false;
    std::error_code filesystem_error;
    const auto normalized = std::filesystem::weakly_canonical(requested, filesystem_error);
    const auto target = filesystem_error ? requested.lexically_normal() : normalized;
    filesystem_error.clear();
    const auto current_normalized = std::filesystem::weakly_canonical(
        g_context.log_root, filesystem_error);
    const auto current = filesystem_error ?
        g_context.log_root.lexically_normal() : current_normalized;
    if (target == current) {
        if (!nte::settings::SavePluginSettings(
                nte::settings::PluginSettingsPath(), {target}, error)) return false;
        g_context.log_root = target;
        CopyLogRootToInput(target);
        return true;
    }
    auto candidate = std::make_unique<nte::damage_log::DamageLogWorker>(target);
    if (!candidate->Start() || !WaitUntilReady(*candidate, error)) {
        candidate->Stop();
        if (error.empty()) error = "无法启动新目录的日志线程";
        return false;
    }
    auto pending_settings = nte::settings::StagePluginSettings(
        nte::settings::PluginSettingsPath(), {target}, error);
    if (!pending_settings) {
        candidate->Stop();
        return false;
    }

    const bool active = g_context.engine.state() == EncounterState::Active;
    const bool has_open_encounter = !g_context.log_session.encounter_id.empty();
    if (has_open_encounter && !FinishEncounterLog("storage_path_changed",
            MakeOverlaySnapshot(g_context.engine.BuildSnapshot(now),
                CaptureStatusCode(g_context.render.snapshot.get()),
                g_context.render.snapshot ?
                    g_context.render.snapshot->latest_damage_sequence : 0,
                g_context.log_session.report_id,
                g_context.log_session.encounter_id, utc_ms, false,
                g_context.overlay_snapshot_sequence), now, utc_ms)) {
        candidate->Stop();
        if (active) {
            g_context.engine.Reset();
            g_context.cursor.RequestReset();
            ResetPendingCaptureGaps();
        }
        error = "无法完整关闭当前战斗日志，目录未切换";
        return false;
    }
    if (!FinishReportLog("storage_path_changed", utc_ms)) {
        candidate->Stop();
        if (active) {
            g_context.engine.Reset();
            g_context.cursor.RequestReset();
            ResetPendingCaptureGaps();
        }
        error = "无法完整关闭旧 Report，目录未切换";
        return false;
    }

    if (!pending_settings->Commit(error)) {
        candidate->Stop();
        if (active) {
            g_context.engine.Reset();
            g_context.cursor.RequestReset();
            ResetPendingCaptureGaps();
        }
        return false;
    }

    auto previous = std::move(g_context.log_worker);
    g_context.log_worker = std::move(candidate);
    g_context.log_root = target;
    if (previous) previous->Stop();
    if (active) {
        g_context.engine.Reset();
        g_context.cursor.RequestReset();
        ResetPendingCaptureGaps();
    }
    CopyLogRootToInput(target);
    return true;
}

AnomalyWebSocketServerInfoV1 QueryOverlayServerInfo() {
    AnomalyWebSocketServerInfoV1 info{sizeof(info)};
    if (g_context.websocket == nullptr || g_context.websocket->server_info == nullptr ||
        g_context.websocket->server_info(g_context.websocket->user, &info).code !=
            ANOMALY_STATUS_V1_OK) {
        info.port = 0;
    }
    return info;
}

std::uint64_t OverlayUrgencyRevision(
    const Snapshot& stats,
    const std::uint32_t service_status,
    const std::string& report_id,
    const std::string& encounter_id,
    const bool stopping) noexcept {
    std::uint64_t value = 1469598103934665603ULL;
    const auto mix = [&value](const std::uint64_t item) {
        value ^= item;
        value *= 1099511628211ULL;
    };
    mix(service_status);
    mix(static_cast<std::uint64_t>(stats.state));
    mix(stats.quality.partial);
    mix(stats.quality.overflow);
    mix(stats.quality.cursor_gap);
    mix(stats.quality.aggregate_truncated);
    mix(stopping);
    for (const unsigned char character : report_id) mix(character);
    mix(0xffU);
    for (const unsigned char character : encounter_id) mix(character);
    return value;
}

void PublishOverlay(
    const Snapshot& stats,
    const std::uint32_t service_status,
    const std::uint64_t latest_sequence,
    const std::string& report_id,
    const std::string& encounter_id,
    const std::int64_t utc_ms,
    const TimePoint now,
    const bool stopping = false,
    const bool force = false) {
    if (!g_context.overlay_policy.ShouldPoll(now, force || stopping)) return;
    g_context.overlay_server_info = QueryOverlayServerInfo();
    const auto& server = g_context.overlay_server_info;
    g_context.overlay_policy.ObserveServer(now, server.connected_clients);
    if (!stopping && server.port == 0) {
        g_context.overlay_error = "WebSocket listener is unavailable";
        return;
    }
    if (!stopping && server.port != 0 && !g_context.overlay_launch_attempted) {
        g_context.overlay_launch_attempted = true;
        static_cast<void>(LaunchOverlay(
            server.port, g_context.overlay_restart_pending, g_context.overlay_error));
        g_context.overlay_restart_pending = false;
    }
    if (g_context.websocket == nullptr || g_context.websocket->publish_text == nullptr ||
        server.port == 0) return;
    const std::uint64_t state_revision = OverlayUrgencyRevision(
        stats, service_status, report_id, encounter_id, stopping);
    if (!g_context.overlay_policy.ShouldPublish(
            now, state_revision, force || stopping)) return;
    const NteOverlaySnapshot message = MakeOverlaySnapshot(
        stats, service_status, latest_sequence, report_id, encounter_id,
        utc_ms, stopping, ++g_context.overlay_snapshot_sequence);
    const std::string json = nte::combat_stats::SerializeOverlaySnapshot(message);
    if (json.size() > nte::combat_stats::kMaximumOverlayMessageBytes ||
        g_context.websocket->publish_text(
            g_context.websocket->user, anomaly::sdk::StringView(json)).code !=
            ANOMALY_STATUS_V1_OK) {
        ++g_context.local_overlay_drops;
    }
    g_context.overlay_policy.RecordPublished(now, state_revision);
}

void ResetState() {
    g_context.engine.Reset();
    g_context.cursor.Clear();
    g_context.log_session = {};
    g_context.participant_names.clear();
    g_context.observed_world = {};
    ResetPendingCaptureGaps();
    g_context.producer_instance_id.clear();
    g_context.overlay_snapshot_sequence = 0;
    g_context.local_overlay_drops = 0;
    g_context.overlay_policy.Reset();
    g_context.overlay_launch_attempted = false;
    g_context.overlay_restart_pending = false;
    g_context.toggle_overlay_requested = false;
    g_context.open_log_root_requested = false;
    g_context.requested_log_root.clear();
    g_context.requested_overlay_settings.reset();
    g_context.overlay_settings = {};
    g_context.log_root_input.fill('\0');
    g_context.next_settings_refresh = {};
    g_context.settings_status.clear();
    g_context.settings_error.clear();
    g_context.overlay_server_info = {sizeof(AnomalyWebSocketServerInfoV1)};
    g_context.overlay_error.clear();
    g_context.control_status.clear();
    std::scoped_lock lock(g_context.mutex);
    g_context.render = {};
}

void CloseControlHandles() {
    if (g_context.manual_end_event != nullptr) {
        CloseHandle(g_context.manual_end_event);
        g_context.manual_end_event = nullptr;
    }
    if (g_context.plugin_stopping_event != nullptr) {
        CloseHandle(g_context.plugin_stopping_event);
        g_context.plugin_stopping_event = nullptr;
    }
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
        FinishEncounterLog("inactivity", MakeOverlaySnapshot(
            g_context.engine.BuildSnapshot(now),
            CaptureStatusCode(g_context.render.snapshot.get()),
            g_context.render.snapshot ?
                g_context.render.snapshot->latest_damage_sequence : 0,
            g_context.log_session.report_id,
            g_context.log_session.encounter_id, utc_ms, false,
            g_context.overlay_snapshot_sequence), now, utc_ms);
    }

    const bool manual_end_requested = g_context.manual_end_event != nullptr &&
        WaitForSingleObject(g_context.manual_end_event, 0) == WAIT_OBJECT_0;
    bool toggle_overlay_requested{};
    bool open_log_root_requested{};
    std::string requested_log_root;
    std::optional<nte::settings::OverlaySettings> requested_overlay_settings;
    {
        std::scoped_lock lock(g_context.mutex);
        toggle_overlay_requested = std::exchange(
            g_context.toggle_overlay_requested, false);
        open_log_root_requested = std::exchange(
            g_context.open_log_root_requested, false);
        requested_log_root = std::move(g_context.requested_log_root);
        g_context.requested_log_root.clear();
        requested_overlay_settings = std::move(
            g_context.requested_overlay_settings);
        g_context.requested_overlay_settings.reset();
    }

    if (requested_overlay_settings) {
        std::string error;
        nte::settings::OverlaySettings merged;
        if (nte::settings::UpdateOverlayPreferences(
                nte::settings::OverlaySettingsPath(),
                *requested_overlay_settings, merged, error)) {
            g_context.overlay_settings = merged;
            g_context.settings_status = "Overlay 设置已应用";
            g_context.settings_error.clear();
            NotifyOverlaySettingsChanged();
        } else {
            g_context.settings_error = "保存 Overlay 设置失败：" + error;
        }
    }

    if (toggle_overlay_requested) {
        const auto action = nte::combat_stats::ResolveOverlayToggleAction(
            g_context.overlay_policy.enabled(),
            !g_context.overlay_error.empty());
        if (action == nte::combat_stats::OverlayToggleAction::Retry) {
            g_context.overlay_launch_attempted = false;
            g_context.overlay_error.clear();
            g_context.settings_status = "正在重试启动 Overlay";
        } else if (action == nte::combat_stats::OverlayToggleAction::Disable) {
            g_context.overlay_policy.SetEnabled(false);
            g_context.overlay_launch_attempted = false;
            CloseOverlayWindow();
            g_context.settings_status = "Overlay 已关闭（下次启动仍会自动开启）";
        } else {
            g_context.overlay_policy.SetEnabled(true);
            g_context.overlay_launch_attempted = false;
            g_context.overlay_restart_pending = false;
            g_context.overlay_error.clear();
            g_context.settings_status = "正在启动 Overlay";
        }
        g_context.settings_error.clear();
    }

    if (!requested_log_root.empty()) {
        const auto path = nte::settings::PathFromUtf8(requested_log_root);
        std::string error;
        if (!path) {
            g_context.settings_error = "存储路径不是有效的 UTF-8 路径";
        } else if (SwitchLogRoot(*path, now, utc_ms, error)) {
            g_context.settings_status = "统计数据存储路径已应用";
            g_context.settings_error = std::move(error);
        } else {
            g_context.settings_error = std::move(error);
        }
    }

    if (open_log_root_requested) {
        std::string error;
        if (!nte::settings::PrepareLogRoot(g_context.log_root, error)) {
            g_context.settings_error = "无法打开数据文件夹：" + error;
        } else if (reinterpret_cast<std::intptr_t>(ShellExecuteW(
                nullptr, L"open", g_context.log_root.c_str(), nullptr,
                nullptr, SW_SHOWNORMAL)) <= 32) {
            g_context.settings_error = "无法打开数据文件夹";
        }
    }

    if (now >= g_context.next_settings_refresh &&
        !requested_overlay_settings) {
        nte::settings::OverlaySettings refreshed = g_context.overlay_settings;
        std::string error;
        if (nte::settings::LoadOverlaySettings(
                nte::settings::OverlaySettingsPath(), refreshed, error)) {
            g_context.overlay_settings = refreshed;
        } else if (!error.empty()) {
            g_context.settings_error = "读取 Overlay 设置失败：" + error;
        }
        g_context.next_settings_refresh = now + std::chrono::seconds(1);
    }

    if (manual_end_requested) {
        bool may_reset = true;
        if (!g_context.log_session.encounter_id.empty()) {
            may_reset = FinishEncounterLog(
                "manual_end",
                MakeOverlaySnapshot(
                g_context.engine.BuildSnapshot(now),
                CaptureStatusCode(g_context.render.snapshot.get()),
                g_context.render.snapshot ?
                    g_context.render.snapshot->latest_damage_sequence : 0,
                g_context.log_session.report_id,
                g_context.log_session.encounter_id, utc_ms, false,
                g_context.overlay_snapshot_sequence), now, utc_ms);
        }
        g_context.control_status = may_reset
            ? "manual_end_completed" : "manual_end_failed";
        if (may_reset) {
            g_context.engine.Reset();
            g_context.cursor.RequestReset();
            ResetPendingCaptureGaps();
        }
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
                ResetPendingCaptureGaps();
            }
            g_context.observed_world = observed_world;
            if (!g_context.log_session.report_id.empty() &&
                observed_world != g_context.log_session.world) {
                if (!g_context.log_session.encounter_id.empty()) {
                    FinishEncounterLog("world_changed", MakeOverlaySnapshot(
                        g_context.engine.BuildSnapshot(now), service_status, latest,
                        g_context.log_session.report_id,
                        g_context.log_session.encounter_id, utc_ms, false,
                        g_context.overlay_snapshot_sequence), now, utc_ms);
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
                    normalized.attacker_name = ResolveParticipantCached(
                        combat.get(), event.attacker, event.sequence);
                    normalized.victim_name = ResolveParticipantCached(
                        combat.get(), event.victim, event.sequence);
                    const auto source_display =
                        nte::combat_stats::ResolveEventDamageSource(
                            event.source_id,
                            [service = combat.get()](const std::uint64_t source_id) {
                                return ResolveSource(service, source_id);
                            },
                            g_context.name_resolver);
                    normalized.source_name = source_display.technical_name;
                    const auto attacker_display = g_context.name_resolver.ResolveParticipant(
                        normalized.attacker_name);
                    const auto victim_display = g_context.name_resolver.ResolveParticipant(
                        normalized.victim_name);
                    normalized.source_display_name = source_display.display_name;
                    normalized.source_name_origin = std::string(
                        nte::combat_stats::DisplayNameOriginName(source_display.origin));
                    normalized.name_resource_version = source_display.resource_version;
                    normalized.attacker_display_name = attacker_display.display_name;
                    normalized.attacker_name_origin = std::string(
                        nte::combat_stats::DisplayNameOriginName(attacker_display.origin));
                    normalized.victim_display_name = victim_display.display_name;
                    normalized.victim_name_origin = std::string(
                        nte::combat_stats::DisplayNameOriginName(victim_display.origin));
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
    Snapshot stats = g_context.engine.BuildSnapshot(now);
    if (g_context.log_worker) next.log = g_context.log_worker->Status();
    next.log_root = nte::settings::PathToUtf8(g_context.log_root);
    const std::string report_id = g_context.log_session.report_id;
    const std::string encounter_id = g_context.log_session.encounter_id;
    PublishOverlay(stats, service_status, latest, report_id,
        encounter_id, utc_ms, now);
    next.snapshot = std::make_shared<const NteOverlaySnapshot>(
        MakeOverlaySnapshot(std::move(stats), service_status, latest,
            report_id, encounter_id, utc_ms, false,
            g_context.overlay_snapshot_sequence));
    next.overlay_error = g_context.overlay_error;
    next.name_resource_error = g_context.name_resource_error;
    next.overlay_settings = g_context.overlay_settings;
    next.overlay_enabled = g_context.overlay_policy.enabled();
    next.settings_status = g_context.settings_status;
    next.settings_error = g_context.settings_error;
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

void Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr ||
        ui->end_window == nullptr || ui->text == nullptr) return;
    RenderState render;
    std::array<char, kMaximumPathBytes> path_input{};
    {
        std::scoped_lock lock(g_context.mutex);
        render = g_context.render;
        path_input = g_context.log_root_input;
    }
    int open = 1;
    anomaly::sdk::UiWindow window(ui, "NTE Combat Tracker", &open);
    if (!window) return;

    Text(ui, "Overlay 设置");
    if (ui->button != nullptr && ui->button(
            ui->user, anomaly::sdk::StringView(
                !render.overlay_error.empty() ? "重试启动 Overlay" :
                    (render.overlay_enabled ? "关闭 Overlay" : "开启 Overlay")),
            0.0F, 0.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.toggle_overlay_requested = true;
    }

    auto settings = render.overlay_settings;
    bool settings_changed{};
    if (ui->checkbox != nullptr) {
        const auto checkbox = [&](const char* label, bool& value) {
            int checked = value ? 1 : 0;
            if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &checked)) {
                value = checked != 0;
                settings_changed = true;
            }
        };
        checkbox("锁定 / 点击穿透", settings.locked);
        checkbox("折叠", settings.collapsed);
        checkbox("置顶", settings.topmost);
        checkbox("离战隐藏", settings.hide_idle);
    }
    if (ui->slider_float != nullptr) {
        settings_changed = ui->slider_float(ui->user,
            anomaly::sdk::StringView("透明度"), &settings.opacity,
            0.4F, 1.0F) != 0 || settings_changed;
        settings_changed = ui->slider_float(ui->user,
            anomaly::sdk::StringView("界面缩放"), &settings.ui_scale,
            0.75F, 2.0F) != 0 || settings_changed;
    }
    if (settings_changed) {
        std::scoped_lock lock(g_context.mutex);
        g_context.requested_overlay_settings = settings;
    }

    if (ui->separator != nullptr) ui->separator(ui->user);
    Text(ui, "统计数据存储");
    const bool input_available = HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::input_text)>(
            ui, offsetof(AnomalyUiServiceV1, input_text)) &&
        ui->input_text != nullptr;
    if (input_available) {
        if (ui->input_text(ui->user, anomaly::sdk::StringView("存储路径"),
                path_input.data(), path_input.size(), 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.log_root_input = path_input;
        }
    } else {
        TextFormat(ui, "存储路径：%s", render.log_root.c_str());
    }
    if (ui->button != nullptr && ui->button(
            ui->user, anomaly::sdk::StringView("应用路径"), 0.0F, 0.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.requested_log_root = path_input.data();
    }
    if (HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::same_line)>(
            ui, offsetof(AnomalyUiServiceV1, same_line)) &&
        ui->same_line != nullptr) {
        ui->same_line(ui->user, 0.0F, -1.0F);
    }
    if (ui->button != nullptr && ui->button(
            ui->user, anomaly::sdk::StringView("打开数据文件夹"), 0.0F, 0.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.open_log_root_requested = true;
    }
    if (!render.settings_status.empty()) {
        TextFormat(ui, "%s", render.settings_status.c_str());
    }
    if (!render.settings_error.empty()) {
        TextFormat(ui, "设置错误：%s", render.settings_error.c_str());
    }
    if (!render.overlay_error.empty()) {
        TextFormat(ui, "Overlay 启动错误：%s", render.overlay_error.c_str());
    }
    if (!render.log.last_error.empty()) {
        TextFormat(ui, "存储错误：%s", render.log.last_error.c_str());
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    const anomaly::sdk::Host services(host);
    const auto ui = services.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    const auto core = services.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    const auto websocket = services.Query<AnomalyWebSocketServiceV1>(
        ANOMALY_WEBSOCKET_SERVICE_V1_ID, ANOMALY_WEBSOCKET_SERVICE_V1_VERSION);
    g_context.host = host;
    g_context.ui = ui.get();
    g_context.core = core.get();
    g_context.websocket = websocket.get();
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
    EnsureNameResources();
    CloseControlHandles();
    g_context.manual_end_event = CreateEventW(nullptr, FALSE, FALSE,
        nte::combat_stats::overlay::kManualEndEventName);
    g_context.plugin_stopping_event = CreateEventW(nullptr, FALSE, FALSE,
        nte::combat_stats::overlay::kPluginStoppingEventName);
    if (g_context.manual_end_event != nullptr) ResetEvent(g_context.manual_end_event);
    if (g_context.plugin_stopping_event != nullptr) ResetEvent(g_context.plugin_stopping_event);
    g_context.producer_instance_id = nte::damage_log::GenerateUuidV4();
    const auto default_log_root = nte::damage_log::DefaultLogRoot();
    std::string settings_error;
    const auto plugin_settings = nte::settings::LoadPluginSettings(
        nte::settings::PluginSettingsPath(), default_log_root, settings_error);
    g_context.log_root = plugin_settings.log_root;
    std::string prepare_error;
    if (!nte::settings::PrepareLogRoot(g_context.log_root, prepare_error)) {
        if (!settings_error.empty()) settings_error += "；";
        settings_error += "配置目录不可用：" + prepare_error;
        g_context.log_root = default_log_root;
        std::string fallback_error;
        if (!nte::settings::PrepareLogRoot(g_context.log_root, fallback_error)) {
            settings_error += "；默认目录不可用：" + fallback_error;
        }
    }
    if (!settings_error.empty()) g_context.settings_error = settings_error;
    CopyLogRootToInput(g_context.log_root);
    std::string overlay_settings_error;
    static_cast<void>(nte::settings::LoadOverlaySettings(
        nte::settings::OverlaySettingsPath(),
        g_context.overlay_settings, overlay_settings_error));
    if (!overlay_settings_error.empty()) {
        g_context.settings_error = overlay_settings_error;
    }
    if (!g_context.log_root.empty()) {
        g_context.log_worker = std::make_unique<nte::damage_log::DamageLogWorker>(
            g_context.log_root);
        static_cast<void>(g_context.log_worker->Start());
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    if (g_context.plugin_stopping_event != nullptr) {
        SetEvent(g_context.plugin_stopping_event);
    }
    const auto now = nte::combat_stats::Clock::now();
    const auto utc_ms = UtcNowMilliseconds();
    RenderState render;
    {
        std::scoped_lock lock(g_context.mutex);
        render = g_context.render;
    }
    if (!g_context.producer_instance_id.empty()) {
        PublishOverlay(g_context.engine.BuildSnapshot(now),
            CaptureStatusCode(render.snapshot.get()),
            render.snapshot ? render.snapshot->latest_damage_sequence : 0,
            g_context.log_session.report_id,
            g_context.log_session.encounter_id, utc_ms, now, true, true);
    }
    if (!g_context.log_session.encounter_id.empty()) {
        FinishEncounterLog("plugin_stop", MakeOverlaySnapshot(
            g_context.engine.BuildSnapshot(now), CaptureStatusCode(render.snapshot.get()),
            render.snapshot ? render.snapshot->latest_damage_sequence : 0,
            g_context.log_session.report_id,
            g_context.log_session.encounter_id, utc_ms, true,
            g_context.overlay_snapshot_sequence), now, utc_ms);
    }
    FinishReportLog("plugin_stop", utc_ms);
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    ResetState();
    CloseControlHandles();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    if (g_context.plugin_stopping_event != nullptr) {
        SetEvent(g_context.plugin_stopping_event);
    }
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    ResetState();
    CloseControlHandles();
    g_context.host = nullptr;
    g_context.ui = nullptr;
    g_context.core = nullptr;
    g_context.websocket = nullptr;
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
        anomaly::sdk::StringView("NTE Combat Tracker"),
        anomaly::sdk::StringView("Shikari"),
        anomaly::sdk::StringView("0.4.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
