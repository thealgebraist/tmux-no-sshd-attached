#pragma once

#include "src/client/input_parser.hpp"
#include "src/protocol/cell.hpp"
#include "src/protocol/messages.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tmux2::client {

struct CopyPosition {
    int64_t line{0}; // 0 = oldest line in history, total_lines - 1 = newest line
    uint16_t col{0};

    auto operator<=>(const CopyPosition&) const = default;
};

class CopyMode {
public:
    CopyMode() = default;

    void enter(uint32_t pane_id, uint16_t view_cols, uint16_t view_rows, uint64_t initial_total_lines = 0);
    void exit();
    [[nodiscard]] bool is_active() const noexcept { return active_; }

    [[nodiscard]] uint32_t pane_id() const noexcept { return pane_id_; }
    [[nodiscard]] uint64_t total_lines() const noexcept { return total_lines_; }
    [[nodiscard]] int64_t viewport_top_line() const noexcept { return viewport_top_line_; }
    [[nodiscard]] CopyPosition cursor() const noexcept { return cursor_; }
    [[nodiscard]] bool is_selecting() const noexcept { return selecting_; }
    [[nodiscard]] std::string_view search_query() const noexcept { return search_query_; }

    // History caching & paging
    void receive_slice_resp(const protocol::MsgScrollbackSliceResp& resp);
    [[nodiscard]] std::optional<protocol::MsgScrollbackSliceReq> check_missing_slices() const;
    void add_local_history_lines(uint64_t start_line, std::vector<protocol::ScrollbackLine> lines);

    // Navigation and key handling
    struct KeyResult {
        bool exited{false};
        std::optional<std::string> yanked_osc52;
        std::optional<protocol::MsgScrollbackSliceReq> slice_req;
    };

    KeyResult handle_key(const ClientKeyEvent& key);

    // Extraction & OSC 52
    [[nodiscard]] std::string get_selected_text() const;
    [[nodiscard]] std::string yank();

    // Render copy mode overlay on a pane's visible grid
    void render_view(std::vector<protocol::Cell>& dst_cells, uint16_t width, uint16_t height) const;

private:
    bool active_{false};
    uint32_t pane_id_{0};
    uint16_t view_cols_{80};
    uint16_t view_rows_{24};

    uint64_t total_lines_{0};
    int64_t viewport_top_line_{0};
    CopyPosition cursor_{0, 0};

    bool selecting_{false};
    CopyPosition sel_start_{0, 0};

    bool search_mode_{false};
    std::string search_query_;

    std::map<uint64_t, protocol::ScrollbackLine> history_cache_;

    void clamp_cursor();
    void scroll_viewport_to_cursor();
    void execute_search(bool forward = true);
};

} // namespace tmux2::client
