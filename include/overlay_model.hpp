#pragma once

#include "overlay_protocol.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
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
    float footer_height{};
    float resize_grip{};
    std::size_t visible_rows{};
};

struct ReferenceTheme {
    std::uint32_t title_background{};
    std::uint32_t header_background{};
    std::uint32_t content_background{};
    std::uint32_t footer_background{};
    std::uint32_t text{};
    std::uint32_t muted_text{};
    std::uint32_t accent{};
    std::uint32_t separator{};
};

struct ReferenceMetrics {
    float title_height{};
    float header_height{};
    float row_height{};
    float footer_height{};
    float resize_grip{};
    float player_name_inset{};
};

struct ViewRect {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

struct TitleBarLayout {
    ViewRect history;
    ViewRect manual_end;
    ViewRect time;
    ViewRect title;
    ViewRect target;
    ViewRect collapse;
    ViewRect menu;
};

struct SummaryColumn {
    std::wstring_view label;
    SortColumn sort;
    float weight;
};

enum class FooterPage : std::uint8_t { Live, HistoryList, HistoryDetail };

struct FooterData {
    std::size_t live_players{};
    double live_dps{};
    std::size_t history_count{};
    std::size_t history_players{};
    double history_dps{};
};

struct WindowExtent {
    int width{};
    int height{};
};

enum class HitRegion : std::uint8_t {
    Client,
    Title,
    ColumnHeader,
    ResizeLeft,
    ResizeRight,
    ResizeTop,
    ResizeBottom,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeGrip,
};

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
[[nodiscard]] ReferenceTheme OverlayReferenceTheme() noexcept;
[[nodiscard]] ReferenceMetrics OverlayReferenceMetrics() noexcept;
[[nodiscard]] TitleBarLayout CalculateTitleBarLayout(
    float width, float scale, float title_height) noexcept;
[[nodiscard]] std::uint32_t StablePlayerColor(std::string_view identity) noexcept;
[[nodiscard]] double PlayerBarFraction(
    std::int64_t damage, std::int64_t maximum_damage) noexcept;
[[nodiscard]] const std::array<SummaryColumn, 6>& OutgoingSummaryColumns() noexcept;
[[nodiscard]] bool IsOutgoingSummarySort(SortColumn column) noexcept;
[[nodiscard]] long long RoundedPercent(double value) noexcept;
[[nodiscard]] float PlayerNameInset(float scale) noexcept;
[[nodiscard]] std::wstring FooterSummary(FooterPage page, const FooterData& data);

}  // namespace nte::combat_stats::overlay
