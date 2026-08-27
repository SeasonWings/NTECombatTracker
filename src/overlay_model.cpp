#include "overlay_model.hpp"

#include <algorithm>
#include <cmath>
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
    case SortColumn::CriticalRate: return CriticalRate(row);
    case SortColumn::MaximumHit: return static_cast<double>(row.maximum_hit);
    case SortColumn::HeadRate: return HeadRate(row);
    case SortColumn::WeakRate: return WeakRate(row);
    case SortColumn::ActiveDps: return row.active_dps;
    case SortColumn::RollingDps: return row.rolling_dps;
    }
    return 0.0;
}

}  // namespace

double CriticalRate(const PlayerRow& row) noexcept {
    return PlayerCriticalRate(row);
}

double HeadRate(const PlayerRow& row) noexcept {
    return PlayerHeadRate(row);
}

double WeakRate(const PlayerRow& row) noexcept {
    return PlayerWeakRate(row);
}

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
    result.title_height = 34.0F * result.scale;
    result.resize_grip = 12.0F * result.scale;
    if (collapsed) return result;
    result.header_height = 28.0F * result.scale;
    result.row_height = 30.0F * result.scale;
    const float available = std::max(
        0.0F, result.height - result.title_height - result.header_height);
    const auto rows = static_cast<std::size_t>(available / result.row_height);
    result.visible_rows = std::min(rows, player_count);
    return result;
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

HitRegion HitTest(
    const LayoutMetrics& layout,
    const float x,
    const float y,
    const bool locked) noexcept {
    if (locked) return HitRegion::Client;
    if (x >= layout.width - layout.resize_grip &&
        y >= layout.height - layout.resize_grip) {
        return HitRegion::ResizeGrip;
    }
    if (y >= 0.0F && y < layout.title_height) return HitRegion::Title;
    if (y >= layout.title_height &&
        y < layout.title_height + layout.header_height) {
        return HitRegion::ColumnHeader;
    }
    return HitRegion::Client;
}

}  // namespace nte::combat_stats::overlay
