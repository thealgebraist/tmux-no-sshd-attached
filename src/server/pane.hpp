#pragma once

#include "src/protocol/cell.hpp"
#include "src/protocol/messages.hpp"
#include "src/pty/pty.hpp"
#include "src/vte/vte.hpp"
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

    [[nodiscard]] protocol::CursorState cursor() const noexcept;

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

    // Direct access to VteScreen
    [[nodiscard]] vte::VteScreen* vte() noexcept { return vte_.get(); }
    [[nodiscard]] const vte::VteScreen* vte() const noexcept { return vte_.get(); }

private:
    uint32_t pane_id_{0};
    Rect rect_{};
    uint16_t width_{80};
    uint16_t height_{24};
    std::string title_{"tmux2"};

    std::unique_ptr<pty::PtyProcess> pty_;
    std::unique_ptr<vte::VteScreen> vte_;
};

} // namespace tmux2::server
