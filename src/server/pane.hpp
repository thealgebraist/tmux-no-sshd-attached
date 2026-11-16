#pragma once

#include "src/protocol/cell.hpp"
#include "src/protocol/messages.hpp"
#include "src/pty/pty.hpp"
#include "src/server/layout.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tmux2::server {

class Pane {
public:
    explicit Pane(uint32_t pane_id, uint16_t width = 80, uint16_t height = 24);
    ~Pane() = default;

    // Factory methods
    static std::shared_ptr<Pane> create(
        uint32_t pane_id, uint16_t width = 80, uint16_t height = 24,
        const pty::PtyOptions& options = {});

    static std::shared_ptr<Pane> create_mock(
        uint32_t pane_id, uint16_t width = 80, uint16_t height = 24);

    // Geometry & identity
    [[nodiscard]] uint32_t pane_id() const noexcept { return pane_id_; }
    [[nodiscard]] const Rect& rect() const noexcept { return rect_; }
    void set_rect(const Rect& r);

    [[nodiscard]] uint16_t width() const noexcept { return width_; }
    [[nodiscard]] uint16_t height() const noexcept { return height_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    void set_title(std::string_view title) { title_ = title; }

    [[nodiscard]] const protocol::CursorState& cursor() const noexcept { return cursor_; }

    // Resize
    void resize(uint16_t width, uint16_t height);

    // Terminal I/O
    void write_input(std::string_view data);
    void feed_output(std::string_view data);
    bool read_pty(); // Reads available data from master PTY and feeds output

    // PTY status
    [[nodiscard]] bool has_pty() const noexcept { return pty_ != nullptr; }
    [[nodiscard]] int master_fd() const noexcept { return pty_ ? pty_->master_fd() : -1; }
    [[nodiscard]] bool is_alive() const noexcept;
    void send_signal(int sig);

    // Grid queries & diffs
    [[nodiscard]] protocol::Cell get_cell(uint16_t col, uint16_t row) const;
    [[nodiscard]] protocol::MsgScreenSnapshot get_snapshot(uint64_t server_seq = 0) const;
    [[nodiscard]] protocol::PaneRowDiff get_dirty_diff(uint64_t server_seq = 0);
    void clear_dirty() noexcept;

    // Scrollback & capture
    [[nodiscard]] protocol::MsgScrollbackSliceResp get_scrollback_slice(
        uint64_t start_line, uint32_t line_count) const;
    [[nodiscard]] std::string capture_text(bool include_history = false, int start_line = 0, int end_line = -1) const;

    // Direct access to internal pty process
    [[nodiscard]] pty::PtyProcess* pty() noexcept { return pty_.get(); }
    void set_pty(std::unique_ptr<pty::PtyProcess> pty) { pty_ = std::move(pty); }

private:
    void ensure_grid_size();
    void scroll_up();
    void mark_row_dirty(uint16_t row);
    void process_escape_sequence(std::string_view seq);
    void process_csi(std::string_view params, char cmd);
    void process_osc(std::string_view osc_content);

    uint32_t pane_id_{0};
    Rect rect_{};
    uint16_t width_{80};
    uint16_t height_{24};
    std::string title_{"tmux2"};

    std::unique_ptr<pty::PtyProcess> pty_;

    protocol::CursorState cursor_{};
    std::vector<protocol::Cell> grid_;
    std::vector<bool> dirty_rows_;
    std::vector<protocol::ScrollbackLine> scrollback_;

    // Current style attribute state
    uint16_t current_flags_{protocol::StyleFlags::None};
    protocol::Color current_fg_{protocol::Color::default_color()};
    protocol::Color current_bg_{protocol::Color::default_color()};

    // Parser state machine
    enum class ParserState {
        Normal,
        Escape,
        Csi,
        Osc,
    };
    ParserState parser_state_{ParserState::Normal};
    std::string escape_buf_;
};

} // namespace tmux2::server
