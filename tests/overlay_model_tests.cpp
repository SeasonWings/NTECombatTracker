#include "overlay_model.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace nte::combat_stats;
using namespace nte::combat_stats::overlay;

void Check(const bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

PlayerRow Player(std::string label, const std::int64_t damage,
                 const std::uint64_t crits, const std::uint64_t hits,
                 const double active_dps) {
    PlayerRow row;
    row.label = std::move(label);
    row.damage = damage;
    row.critical_hits = crits;
    row.positive_hits = hits;
    row.active_dps = active_dps;
    return row;
}

void SortsEachSupportedMetricStably() {
    const std::vector players{
        Player("Beta", 100, 2, 4, 70.0),
        Player("Alpha", 200, 1, 4, 10.0),
        Player("Gamma", 200, 3, 4, 40.0),
    };
    const auto damage = SortPlayers(players, SortColumn::Damage, true);
    Check(damage[0].label == "Alpha" && damage[1].label == "Gamma",
          "equal metrics use character label as deterministic tie breaker");
    const auto crit = SortPlayers(players, SortColumn::CriticalRate, true);
    Check(crit[0].label == "Gamma" && std::abs(CriticalRate(crit[0]) - 75.0) < 0.01,
          "critical rate is sortable");
    const auto active = SortPlayers(players, SortColumn::ActiveDps, false);
    Check(active[0].label == "Alpha" && active[2].label == "Beta",
          "ascending active DPS is sortable");
}

void LayoutScalesAndCapsRows() {
    const auto normal = CalculateLayout(520.0F, 220.0F, 1.0F, false, 10);
    Check(normal.title_height == 34.0F && normal.header_height == 28.0F,
          "default title and header dimensions match the design");
    Check(normal.visible_rows == 5, "rows are clipped to available client height");
    const auto dpi = CalculateLayout(780.0F, 330.0F, 1.5F, false, 10);
    Check(dpi.visible_rows == 5 && dpi.resize_grip == 18.0F,
          "DPI scaling preserves logical row count and hit targets");
    const auto dpi200 = CalculateLayout(1040.0F, 440.0F, 2.0F, false, 10);
    Check(dpi200.visible_rows == 5 && dpi200.title_height == 68.0F,
          "200 percent DPI preserves logical layout proportions");
    const auto collapsed = CalculateLayout(520.0F, 220.0F, 1.0F, true, 10);
    Check(collapsed.visible_rows == 0 && collapsed.header_height == 0.0F,
          "collapsed layout only exposes the title bar");
}

void HitTestingRespectsLockAndResizeGrip() {
    const auto layout = CalculateLayout(520.0F, 220.0F, 1.0F, false, 2);
    Check(HitTest(layout, 100.0F, 10.0F, false) == HitRegion::Title,
          "unlocked title bar can move the window");
    Check(HitTest(layout, 100.0F, 40.0F, false) == HitRegion::ColumnHeader,
          "column header is interactive");
    Check(HitTest(layout, 519.0F, 219.0F, false) == HitRegion::ResizeGrip,
          "bottom-right corner can resize the window");
    Check(HitTest(layout, 100.0F, 10.0F, true) == HitRegion::Client,
          "locked overlay does not expose interactive hit regions");
}

}  // namespace

int main() {
    SortsEachSupportedMetricStably();
    LayoutScalesAndCapsRows();
    HitTestingRespectsLockAndResizeGrip();
    std::cout << "All overlay model tests passed\n";
}
