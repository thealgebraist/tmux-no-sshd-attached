#include "src/vte/dirty_tracker.hpp"

#include <algorithm>

namespace tmux2::vte {

DirtyTracker::DirtyTracker(uint16_t rows)
    : rows_(rows), dirty_flags_(rows, true), has_dirty_(rows > 0) {}

void DirtyTracker::resize(uint16_t rows) {
    rows_ = rows;
    dirty_flags_.assign(rows, true);
    has_dirty_ = (rows > 0);
}

void DirtyTracker::mark_dirty(uint16_t row) noexcept {
    if (row < rows_) {
        dirty_flags_[row] = true;
        has_dirty_ = true;
    }
}

void DirtyTracker::mark_range(uint16_t start_row, uint16_t end_row) noexcept {
    uint16_t clamped_end = std::min(end_row, rows_);
    for (uint16_t r = start_row; r < clamped_end; ++r) {
        dirty_flags_[r] = true;
        has_dirty_ = true;
    }
}

void DirtyTracker::mark_all() noexcept {
    std::fill(dirty_flags_.begin(), dirty_flags_.end(), true);
    has_dirty_ = (rows_ > 0);
}

bool DirtyTracker::is_dirty(uint16_t row) const noexcept {
    if (row < rows_) {
        return dirty_flags_[row];
    }
    return false;
}

bool DirtyTracker::has_dirty() const noexcept {
    return has_dirty_;
}

size_t DirtyTracker::count_dirty() const noexcept {
    size_t count = 0;
    for (bool d : dirty_flags_) {
        if (d) ++count;
    }
    return count;
}

std::vector<uint16_t> DirtyTracker::dirty_rows() const {
    std::vector<uint16_t> result;
    result.reserve(count_dirty());
    for (uint16_t r = 0; r < rows_; ++r) {
        if (dirty_flags_[r]) {
            result.push_back(r);
        }
    }
    return result;
}

void DirtyTracker::clear() noexcept {
    std::fill(dirty_flags_.begin(), dirty_flags_.end(), false);
    has_dirty_ = false;
}

} // namespace tmux2::vte
