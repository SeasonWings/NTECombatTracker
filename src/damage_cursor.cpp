#include "damage_cursor.hpp"

namespace nte::combat_stats {

void DamageCursor::RequestReset() noexcept {
    reset_pending_ = true;
}

CursorFrameDecision DamageCursor::BeginFrame(
    const std::optional<std::uint64_t> latest_sequence) noexcept {
    recovery_attempted_ = false;
    if (!latest_sequence.has_value()) return {};
    if (reset_pending_) {
        value_ = *latest_sequence;
        reset_pending_ = false;
        return {.reset_applied = true};
    }
    if (*latest_sequence < value_) {
        value_ = 0;
        return {.cursor_gap = true};
    }
    return {};
}

bool DamageCursor::RecoverFromNotFound(const std::uint64_t latest_sequence) noexcept {
    if (recovery_attempted_ || value_ == 0 || latest_sequence <= value_) return false;
    value_ = 0;
    recovery_attempted_ = true;
    return true;
}

bool DamageCursor::Accept(const std::uint64_t sequence) noexcept {
    if (sequence <= value_) return false;
    value_ = sequence;
    return true;
}

void DamageCursor::Clear() noexcept {
    value_ = 0;
    reset_pending_ = false;
    recovery_attempted_ = false;
}

std::uint64_t DamageCursor::value() const noexcept {
    return value_;
}

bool DamageCursor::reset_pending() const noexcept {
    return reset_pending_;
}

}  // namespace nte::combat_stats

