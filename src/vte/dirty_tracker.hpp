#pragma once

#include <cstdint>
#include <vector>

namespace tmux2::vte {

/**
 * Tracks dirty/damaged screen rows per pane for incremental frame diff generation.
 */
class DirtyTracker {
public:
    explicit DirtyTracker(uint16_t rows = 24);

    void resize(uint16_t rows);

    void mark_dirty(uint16_t row) noexcept;
    void mark_range(uint16_t start_row, uint16_t end_row) noexcept;
    void mark_all() noexcept;

    [[nodiscard]] bool is_dirty(uint16_t row) const noexcept;
    [[nodiscard]] bool has_dirty() const noexcept;
    [[nodiscard]] size_t count_dirty() const noexcept;
    [[nodiscard]] std::vector<uint16_t> dirty_rows() const;

    void clear() noexcept;

private:
    uint16_t rows_{24};
    std::vector<bool> dirty_flags_;
    bool has_dirty_{false};
};

} // namespace tmux2::vte
