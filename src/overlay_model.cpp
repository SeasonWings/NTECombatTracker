#include "overlay_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <ranges>
#include <string_view>

namespace nte::combat_stats::overlay {
namespace {

double Metric(const PlayerRow& row, const SortColumn column) noexcept {
    switch (column) {
    case SortColumn::Character: return 0.0;
    case SortColumn::Damage: return static_cast<double>(row.damage);
    case SortColumn::Share: return row.damage_share;
    case SortColumn::EncounterDps: return row.encounter_dps;
    case SortColumn::CriticalRate: return PlayerCriticalRate(row);
    case SortColumn::MaximumHit: return static_cast<double>(row.maximum_hit);
    case SortColumn::HeadRate: return PlayerHeadRate(row);
    case SortColumn::WeakRate: return PlayerWeakRate(row);
    case SortColumn::ActiveDps: return row.active_dps;
    case SortColumn::RollingDps: return row.rolling_dps;
    }
    return 0.0;
}

}  // namespace

std::vector<PlayerRow> SortPlayers(
    const std::vector<PlayerRow>& players,
    const SortColumn column,
    const bool descending) {
    std::vector<PlayerRow> result = players;
    std::ranges::sort(result, [&](const PlayerRow& left, const PlayerRow& right) {
        if (column == SortColumn::Character) {
            return descending ? left.label > right.label : left.label < right.label;
        }
        const double left_metric = Metric(left, column);
        const double right_metric = Metric(right, column);
        if (std::abs(left_metric - right_metric) > 0.000001) {
            return descending ? left_metric > right_metric : left_metric < right_metric;
        }
        return left.label < right.label;
    });
    return result;
}

LayoutMetrics CalculateLayout(
    const float width,
    const float height,
    const float scale,
    const bool collapsed,
    const std::size_t player_count) noexcept {
    LayoutMetrics result;
    result.width = std::max(0.0F, width);
    result.height = std::max(0.0F, height);
    result.scale = std::clamp(scale, 0.5F, 4.0F);
    const auto reference = OverlayReferenceMetrics();
    result.title_height = reference.title_height * result.scale;
    result.resize_grip = reference.resize_grip * result.scale;
    if (collapsed) return result;
    result.header_height = reference.header_height * result.scale;
    result.row_height = reference.row_height * result.scale;
    result.footer_height = reference.footer_height * result.scale;
    const float available = std::max(
        0.0F, result.height - result.title_height - result.header_height -
            result.footer_height);
    const auto rows = static_cast<std::size_t>(available / result.row_height);
    result.visible_rows = std::min(rows, player_count);
    return result;
}

ReferenceTheme OverlayReferenceTheme() noexcept {
    return {
        0x303030, 0x454245, 0x898989, 0x303030,
        0xF0F0F0, 0xB7B7B7, 0x00B8D4, 0x5D5D5D,
    };
}

ReferenceMetrics OverlayReferenceMetrics() noexcept {
    return {46.0F, 28.0F, 30.0F, 32.0F, 12.0F, 28.0F};
}

TitleBarLayout CalculateTitleBarLayout(
    const float width,
    const float scale,
    const float title_height) noexcept {
    const float safe_scale = std::clamp(scale, 0.5F, 4.0F);
    const float safe_width = std::max(0.0F, width);
    const float safe_height = std::max(0.0F, title_height);
    return {
        {6 * safe_scale, 5 * safe_scale, 38 * safe_scale, safe_height - 5 * safe_scale},
        {40 * safe_scale, 7 * safe_scale, 82 * safe_scale, safe_height - 7 * safe_scale},
        {84 * safe_scale, 0, 146 * safe_scale, safe_height},
        {146 * safe_scale, 0, 252 * safe_scale, safe_height},
        {252 * safe_scale, 0, safe_width - 66 * safe_scale, safe_height},
        {safe_width - 64 * safe_scale, 4 * safe_scale,
            safe_width - 34 * safe_scale, safe_height - 4 * safe_scale},
        {safe_width - 32 * safe_scale, 4 * safe_scale,
            safe_width - 4 * safe_scale, safe_height - 4 * safe_scale},
    };
}

std::uint32_t StablePlayerColor(const std::string_view identity) noexcept {
    constexpr std::array palette{
        0xC48EA1U, 0xC5A654U, 0xB69A48U, 0x8C4266U,
        0x5D667BU, 0x68769DU, 0x56728AU, 0x777777U,
    };
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char value : identity) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return palette[hash % palette.size()];
}

double PlayerBarFraction(
    const std::int64_t damage,
    const std::int64_t maximum_damage) noexcept {
    if (damage <= 0 || maximum_damage <= 0) return 0.0;
    return std::clamp(static_cast<double>(damage) /
        static_cast<double>(maximum_damage), 0.0, 1.0);
}

const std::array<SummaryColumn, 6>& OutgoingSummaryColumns() noexcept {
    static constexpr std::array<SummaryColumn, 6> columns{{
        {L"角色", SortColumn::Character, 1.7F},
        {L"总伤害", SortColumn::Damage, 1.05F},
        {L"DPS", SortColumn::EncounterDps, 0.85F},
        {L"DPS%", SortColumn::Share, 0.8F},
        {L"暴击%", SortColumn::CriticalRate, 0.8F},
        {L"最高伤害", SortColumn::MaximumHit, 1.7F},
    }};
    return columns;
}

bool IsOutgoingSummarySort(const SortColumn column) noexcept {
    const auto& columns = OutgoingSummaryColumns();
    return std::ranges::any_of(columns,
        [column](const auto& value) { return value.sort == column; });
}

long long RoundedPercent(const double value) noexcept {
    if (!std::isfinite(value)) return 0;
    return std::llround(std::clamp(value, 0.0, 100.0));
}

float PlayerNameInset(const float scale) noexcept {
    return OverlayReferenceMetrics().player_name_inset *
        std::clamp(scale, 0.5F, 4.0F);
}

std::wstring FooterSummary(const FooterPage page, const FooterData& data) {
    switch (page) {
    case FooterPage::Live:
        return std::format(L"{}人｜{:.0f}dps", data.live_players, data.live_dps);
    case FooterPage::HistoryList:
        return std::format(L"{}场历史战斗", data.history_count);
    case FooterPage::HistoryDetail:
        return std::format(L"{}人｜{:.0f}dps",
            data.history_players, data.history_dps);
    }
    return {};
}

WindowExtent ScaleWindowExtent(
    const int logical_width,
    const int logical_height,
    const float dpi_scale,
    const float ui_scale) noexcept {
    const float scale = std::clamp(dpi_scale, 0.5F, 4.0F) *
        std::clamp(ui_scale, 0.75F, 2.0F);
    return {
        std::max(1, static_cast<int>(std::lround(
            static_cast<float>(logical_width) * scale))),
        std::max(1, static_cast<int>(std::lround(
            static_cast<float>(logical_height) * scale))),
    };
}

WindowExtent UnscaleWindowExtent(
    const int physical_width,
    const int physical_height,
    const float dpi_scale,
    const float ui_scale) noexcept {
    const float scale = std::clamp(dpi_scale, 0.5F, 4.0F) *
        std::clamp(ui_scale, 0.75F, 2.0F);
    return {
        std::max(1, static_cast<int>(std::lround(
            static_cast<float>(physical_width) / scale))),
        std::max(1, static_cast<int>(std::lround(
            static_cast<float>(physical_height) / scale))),
    };
}

HitRegion HitTest(
    const LayoutMetrics& layout,
    const float x,
    const float y,
    const bool locked) noexcept {
    if (locked) return HitRegion::Client;
    const bool left = x >= 0.0F && x < layout.resize_grip;
    const bool right = x >= layout.width - layout.resize_grip && x <= layout.width;
    const bool top = y >= 0.0F && y < layout.resize_grip;
    const bool bottom = y >= layout.height - layout.resize_grip && y <= layout.height;
    if (left && top) return HitRegion::ResizeTopLeft;
    if (right && top) return HitRegion::ResizeTopRight;
    if (left && bottom) return HitRegion::ResizeBottomLeft;
    if (right && bottom) return HitRegion::ResizeGrip;
    if (left) return HitRegion::ResizeLeft;
    if (right) return HitRegion::ResizeRight;
    if (top) return HitRegion::ResizeTop;
    if (bottom) return HitRegion::ResizeBottom;
    if (y >= 0.0F && y < layout.title_height) return HitRegion::Title;
    if (y >= layout.title_height &&
        y < layout.title_height + layout.header_height) {
        return HitRegion::ColumnHeader;
    }
    return HitRegion::Client;
}

}  // namespace nte::combat_stats::overlay
