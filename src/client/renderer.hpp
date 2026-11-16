#pragma once

#include "src/protocol/cell.hpp"
#include "src/protocol/messages.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tmux2::client {

struct PaneRect {
    uint32_t pane_id{0};
    uint16_t x{0};
    uint16_t y{0};
    uint16_t width{0};
    uint16_t height{0};

    constexpr bool operator==(const PaneRect&) const = default;
};

struct PaneShadowGrid {
    uint16_t cols{0};
    uint16_t rows{0};
    std::vector<protocol::Cell> cells;
    protocol::CursorState cursor{};
    uint32_t modes{0};

    [[nodiscard]] const protocol::Cell* cell_at(uint16_t c, uint16_t r) const noexcept {
        if (c < cols && r < rows) {
            return &cells[r * cols + c];
        }
        return nullptr;
    }

    protocol::Cell* cell_at(uint16_t c, uint16_t r) noexcept {
        if (c < cols && r < rows) {
            return &cells[r * cols + c];
        }
        return nullptr;
    }
};

struct WindowTabInfo {
    uint32_t index{0};
    std::string name;
    bool is_active{false};
    bool is_last{false};

    bool operator==(const WindowTabInfo&) const = default;
};

class Renderer {
public:
    Renderer(uint16_t term_cols = 80, uint16_t term_rows = 24);

    void resize(uint16_t cols, uint16_t rows);
    [[nodiscard]] uint16_t term_cols() const noexcept { return term_cols_; }
    [[nodiscard]] uint16_t term_rows() const noexcept { return term_rows_; }

    // Protocol state updates
    void apply_snapshot(const protocol::MsgScreenSnapshot& snapshot);
    void apply_diff(const protocol::MsgFrameDiff& diff);
    void apply_layout(const protocol::MsgLayoutUpdate& layout);

    // Layout configuration
    void set_pane_rects(std::vector<PaneRect> rects);
    [[nodiscard]] const std::vector<PaneRect>& pane_rects() const noexcept { return pane_rects_; }
    void set_active_pane(uint32_t pane_id) noexcept { active_pane_id_ = pane_id; }
    [[nodiscard]] uint32_t active_pane() const noexcept { return active_pane_id_; }
    void set_zoomed_pane(uint32_t pane_id) noexcept { zoomed_pane_id_ = pane_id; }
    [[nodiscard]] uint32_t zoomed_pane() const noexcept { return zoomed_pane_id_; }

    // Status bar state
    void set_session_name(std::string name) { session_name_ = std::move(name); }
    [[nodiscard]] std::string_view session_name() const noexcept { return session_name_; }
    void set_window_tabs(std::vector<WindowTabInfo> tabs) { window_tabs_ = std::move(tabs); }
    [[nodiscard]] const std::vector<WindowTabInfo>& window_tabs() const noexcept { return window_tabs_; }
    void set_reconnect_banner(std::string banner) { reconnect_banner_ = std::move(banner); }
    [[nodiscard]] std::string_view reconnect_banner() const noexcept { return reconnect_banner_; }
    void set_prefix_indicator(bool active) noexcept { prefix_active_ = active; }
    [[nodiscard]] bool prefix_indicator() const noexcept { return prefix_active_; }
    void set_custom_status_line(std::string status) { custom_status_line_ = std::move(status); }

    // Query shadow grid
    [[nodiscard]] const PaneShadowGrid* get_pane_grid(uint32_t pane_id) const;
    PaneShadowGrid* get_pane_grid(uint32_t pane_id);

    // Render composite frame
    void compose_frame(); // Builds composite screen buffer
    [[nodiscard]] std::string render_frame_vt(bool use_decset_2026 = true) const;

    // Direct access to composite frame for testing and overlays
    [[nodiscard]] const protocol::Cell& get_composite_cell(uint16_t col, uint16_t row) const;
    void set_composite_cell(uint16_t col, uint16_t row, const protocol::Cell& cell);
    [[nodiscard]] std::vector<protocol::Cell>& composite_screen() noexcept { return composite_screen_; }
    [[nodiscard]] const std::vector<protocol::Cell>& composite_screen() const noexcept { return composite_screen_; }
    void set_active_cursor(const protocol::CursorState& cursor) noexcept { active_cursor_ = cursor; }
    [[nodiscard]] std::string get_row_text(uint16_t row) const;
    [[nodiscard]] protocol::CursorState get_active_cursor() const;

    // Clipboard & OSC 52 helpers
    static std::string base64_encode(std::string_view input);
    static std::string base64_decode(std::string_view input);
    static std::string make_osc52(std::string_view text, bool clipboard = true);
    static std::string handle_passthrough(const protocol::MsgPassthroughEvent& ev);

private:
    uint16_t term_cols_{80};
    uint16_t term_rows_{24};

    std::map<uint32_t, PaneShadowGrid> pane_grids_;
    std::vector<PaneRect> pane_rects_;
    uint32_t active_pane_id_{0};
    uint32_t zoomed_pane_id_{0};

    std::string session_name_{"tmux2"};
    std::vector<WindowTabInfo> window_tabs_;
    std::string reconnect_banner_;
    std::string custom_status_line_;
    bool prefix_active_{false};

    // Composite screen grid: size term_cols_ * term_rows_
    std::vector<protocol::Cell> composite_screen_;
    protocol::CursorState active_cursor_{};

    void draw_pane_content(const PaneRect& rect, const PaneShadowGrid& grid);
    void draw_split_borders();
    void draw_status_bar();
    void format_cell_sgr(const protocol::CellStyle& style, std::string& out) const;
};

} // namespace tmux2::client
