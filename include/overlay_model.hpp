#pragma once

#include "overlay_protocol.hpp"

#include <cstdint>
#include <vector>

namespace nte::combat_stats::overlay {

enum class SortColumn : std::uint8_t {
    Character,
    Damage,
    Share,
    EncounterDps,
    CriticalRate,
    MaximumHit,
    HeadRate,
    WeakRate,
    ActiveDps,
    RollingDps,
};

struct ColumnVisibility {
    bool head_rate{};
    bool weak_rate{};
    bool active_dps{};
    bool rolling_dps{};
};

struct LayoutMetrics {
    float width{};
    float height{};
    float scale{1.0F};
    float title_height{};
    float header_height{};
    float row_height{};
    float resize_grip{};
    std::size_t visible_rows{};
};

struct WindowExtent {
    int width{};
    int height{};
};

enum class HitRegion : std::uint8_t { Client, Title, ColumnHeader, ResizeGrip };

[[nodiscard]] std::vector<PlayerRow> SortPlayers(
    const std::vector<PlayerRow>& players,
    SortColumn column,
    bool descending);
[[nodiscard]] LayoutMetrics CalculateLayout(
    float width,
    float height,
    float scale,
    bool collapsed,
    std::size_t player_count) noexcept;
[[nodiscard]] WindowExtent ScaleWindowExtent(
    int logical_width,
    int logical_height,
    float dpi_scale,
    float ui_scale) noexcept;
[[nodiscard]] WindowExtent UnscaleWindowExtent(
    int physical_width,
    int physical_height,
    float dpi_scale,
    float ui_scale) noexcept;
[[nodiscard]] HitRegion HitTest(
    const LayoutMetrics& layout,
    float x,
    float y,
    bool locked) noexcept;

}  // namespace nte::combat_stats::overlay
