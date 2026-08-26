#include "combat_stats_engine.hpp"
#include "damage_cursor.hpp"

#include <anomaly/sdk/anomaly_sdk.h>
#include <anomaly/sdk/cpp.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace {

using nte::combat_stats::AggregateRow;
using nte::combat_stats::CombatStatsEngine;
using nte::combat_stats::EncounterState;
using nte::combat_stats::EventDirection;
using nte::combat_stats::Handle;
using nte::combat_stats::Snapshot;

constexpr std::size_t kMaximumDrainPerUpdate = 256;
constexpr std::size_t kMaximumResolvedNameBytes = 4096;
constexpr std::size_t kMaximumRecentRows = 100;

struct RenderState {
    Snapshot stats;
    std::uint32_t service_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    std::uint64_t latest_sequence{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyUiServiceV1* ui{};
    CombatStatsEngine engine;
    nte::combat_stats::DamageCursor cursor;
    std::mutex mutex;
    RenderState render;
    bool reset_requested{};
} g_context;

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

void ResetState() {
    g_context.engine.Reset();
    g_context.cursor.Clear();
    std::scoped_lock lock(g_context.mutex);
    g_context.render = {};
    g_context.reset_requested = false;
}

void Update(void* context, double) {
    if (context != &g_context || g_context.host == nullptr) return;
    const auto now = nte::combat_stats::Clock::now();
    const anomaly::sdk::Host host(g_context.host);
    const auto combat = host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);

    bool reset_requested{};
    {
        std::scoped_lock lock(g_context.mutex);
        reset_requested = g_context.reset_requested;
        g_context.reset_requested = false;
    }
    if (reset_requested) g_context.cursor.RequestReset();

    std::uint32_t service_status = ANOMALY_STATUS_V1_UNAVAILABLE;
    std::uint64_t latest{};
    bool sampled_partial{};
    bool sampled_overflow{};
    bool cursor_gap{};
    if (CombatMethodsAvailable(combat.get())) {
        latest = combat->latest_damage_sequence(combat->user);
        const auto cursor_decision = g_context.cursor.BeginFrame(latest);
        cursor_gap = cursor_decision.cursor_gap;
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
        if (valid_combatant) {
            g_context.engine.ObserveCombatant({
                ConvertHandle(combatant.world), ConvertHandle(combatant.character),
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
                        break;
                    }
                    nte::combat_stats::DamageEvent normalized;
                    normalized.sequence = event.sequence;
                    normalized.world = ConvertHandle(event.world);
                    normalized.attacker = ConvertHandle(event.attacker);
                    normalized.victim = ConvertHandle(event.victim);
                    normalized.source_id = event.source_id;
                    normalized.final_damage = event.final_damage;
                    if (!g_context.engine.IsRelevant(
                            normalized.world, normalized.attacker, normalized.victim)) {
                        continue;
                    }
                    normalized.source_name = ResolveSource(combat.get(), event.source_id);
                    normalized.attacker_name = ResolveParticipant(combat.get(), event.attacker);
                    normalized.victim_name = ResolveParticipant(combat.get(), event.victim);
                    static_cast<void>(g_context.engine.AcceptDamage(normalized, now));
                    continue;
                }
                if (event_status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                    g_context.cursor.RecoverFromNotFound(latest)) {
                    cursor_gap = true;
                    continue;
                }
                if (event_status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
                    service_status = event_status.code;
                }
                break;
            }
        }
        if (sampled_partial || cursor_gap) g_context.engine.MarkPartial(cursor_gap);
        if (sampled_overflow) g_context.engine.MarkOverflow();
    } else {
        static_cast<void>(g_context.cursor.BeginFrame(std::nullopt));
    }

    g_context.engine.Advance(now);
    RenderState next;
    next.stats = g_context.engine.BuildSnapshot(now);
    next.service_status = service_status;
    next.latest_sequence = latest;
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
    if (stats.quality.partial || stats.quality.overflow ||
        stats.quality.cursor_gap || stats.quality.aggregate_truncated) {
        TextFormat(ui, "数据质量：%s%s%s%s",
            stats.quality.partial ? "PARTIAL " : "",
            stats.quality.overflow ? "OVERFLOW " : "",
            stats.quality.cursor_gap ? "CURSOR_GAP " : "",
            stats.quality.aggregate_truncated ? "AGGREGATE_LIMIT" : "");
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
                    ? "双向" : (event.direction == EventDirection::Outgoing ? "输出" : "承伤");
                TextFormat(ui, "#%llu [%s] %lld%s  %s  %s -> %s",
                    static_cast<unsigned long long>(event.sequence), direction,
                    static_cast<long long>(event.final_damage),
                    event.final_damage <= 0 ? " (不计DPS)" : "",
                    ShortName(event.source).c_str(), ShortName(event.attacker).c_str(),
                    ShortName(event.victim).c_str());
            }
            if (count == 0) Text(ui, "暂无当前玩家相关事件");
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
    ResetState();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    ResetState();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
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
        anomaly::sdk::StringView("0.1.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
