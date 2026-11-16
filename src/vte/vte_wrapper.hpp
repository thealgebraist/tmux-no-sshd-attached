#pragma once

#include "src/protocol/cell.hpp"
#include "src/protocol/messages.hpp"
#include "src/vte/dirty_tracker.hpp"
#include "src/vte/scrollback_buffer.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vterm.h>

namespace tmux2::vte {

class VteScreen {
public:
    VteScreen(uint16_t cols, uint16_t rows, size_t scrollback_max_lines = kDefaultScrollbackCapacity);
    ~VteScreen();

    // Non-copyable, movable
    VteScreen(const VteScreen&) = delete;
    VteScreen& operator=(const VteScreen&) = delete;
    VteScreen(VteScreen&& other) noexcept;
    VteScreen& operator=(VteScreen&& other) noexcept;

    /**
     * Feeds raw byte stream (e.g. from PTY master) into libvterm emulator.
     * Returns number of bytes processed.
     */
    size_t feed(std::string_view str);
    size_t feed(std::span<const std::byte> bytes);

    template <size_t N = std::dynamic_extent>
    size_t feed(std::span<const char, N> bytes) {
        return feed(std::string_view(bytes.data(), bytes.size()));
    }

    template <size_t N = std::dynamic_extent>
    size_t feed(std::span<char, N> bytes) {
        return feed(std::string_view(bytes.data(), bytes.size()));
    }

    /**
     * Resizes the virtual terminal screen.
     */
    void resize(uint16_t cols, uint16_t rows);

    /**
     * Flushes pending damage events in libvterm to dirty tracker.
     */
    void flush_damage();

    // Screen geometry
    [[nodiscard]] uint16_t cols() const noexcept { return cols_; }
    [[nodiscard]] uint16_t rows() const noexcept { return rows_; }

    // Cursor and terminal modes
    [[nodiscard]] protocol::CursorState cursor() const noexcept { return cursor_; }
    [[nodiscard]] bool is_altscreen() const noexcept { return is_altscreen_; }
    [[nodiscard]] bool cursor_blink() const noexcept { return cursor_blink_; }
    [[nodiscard]] int mouse_mode() const noexcept { return mouse_mode_; }
    [[nodiscard]] bool focus_reporting() const noexcept { return focus_reporting_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }

    // Direct cell queries
    [[nodiscard]] protocol::Cell get_cell(uint16_t col, uint16_t row) const;
    [[nodiscard]] std::vector<protocol::Cell> get_row(uint16_t row) const;
    [[nodiscard]] std::string get_text(uint16_t start_col, uint16_t start_row, uint16_t end_col, uint16_t end_row) const;

    // Full snapshot & incremental dirty row diff generation
    [[nodiscard]] protocol::MsgScreenSnapshot get_snapshot(uint64_t server_seq = 0, uint32_t pane_id = 0) const;
    std::vector<protocol::CellRowDiff> get_dirty_diffs();

    // Dirty tracker access
    [[nodiscard]] DirtyTracker& dirty_tracker() noexcept { return dirty_tracker_; }
    [[nodiscard]] const DirtyTracker& dirty_tracker() const noexcept { return dirty_tracker_; }

    // Scrollback ring buffer access
    [[nodiscard]] ScrollbackBuffer& scrollback() noexcept { return scrollback_; }
    [[nodiscard]] const ScrollbackBuffer& scrollback() const noexcept { return scrollback_; }

    // Callbacks
    using BellCallback = std::function<void()>;
    using TitleCallback = std::function<void(std::string_view)>;
    using Osc52Callback = std::function<void(std::string_view selection, std::string_view base64_payload)>;
    using OutputCallback = std::function<void(std::string_view response)>;

    void set_bell_callback(BellCallback cb) { bell_callback_ = std::move(cb); }
    void set_title_callback(TitleCallback cb) { title_callback_ = std::move(cb); }
    void set_osc52_callback(Osc52Callback cb) { osc52_callback_ = std::move(cb); }
    void set_output_callback(OutputCallback cb) { output_callback_ = std::move(cb); }

    // Mouse and keyboard injection into emulator state machine
    void mouse_move(int col, int row, int modifiers = 0);
    void mouse_button(int button, bool pressed, int modifiers = 0);
    void keyboard_unichar(char32_t c, int modifiers = 0);
    void keyboard_key(int key, int modifiers = 0);

    // Converts libvterm cell to protocol cell
    [[nodiscard]] protocol::Cell convert_cell(const VTermScreenCell& sc) const;

    // Internal libvterm hooks (called by C trampolines)
    void on_damage(int start_row, int end_row, int start_col, int end_col);
    void on_moverect(int dest_start_row, int dest_end_row, int src_start_row, int src_end_row);
    void on_movecursor(int row, int col, bool visible);
    void on_settermprop(int prop, const void* val);
    void on_bell();
    void on_resize(int rows, int cols);
    int on_sb_pushline(int cols, const VTermScreenCell* cells);
    int on_sb_popline(int cols, VTermScreenCell* cells);
    void on_sb_clear();
    void on_osc(int command, std::string_view frag_str, bool initial, bool final_frag);
    int on_selection_set(int mask, std::string_view frag_str, bool initial, bool final_frag);
    void on_output(const char* s, size_t len);

private:
    void init_vterm();
    void cleanup_vterm() noexcept;

    uint16_t cols_{80};
    uint16_t rows_{24};

    VTerm* vt_{nullptr};
    VTermScreen* screen_{nullptr};
    VTermState* state_{nullptr};

    DirtyTracker dirty_tracker_;
    ScrollbackBuffer scrollback_;

    protocol::CursorState cursor_{};
    bool cursor_blink_{false};
    bool is_altscreen_{false};
    int mouse_mode_{0};
    bool focus_reporting_{false};
    std::string title_;

    // Cached previous frame cells for minimal diff calculation
    std::vector<std::vector<protocol::Cell>> cached_frame_cells_;

    // OSC accumulator
    std::string osc_buffer_;
    int active_osc_command_{-1};

    // Selection buffer for OSC 52
    std::vector<char> selection_buffer_;
    std::string selection_accum_;
    int active_selection_mask_{0};

    // User callbacks
    BellCallback bell_callback_;
    TitleCallback title_callback_;
    Osc52Callback osc52_callback_;
    OutputCallback output_callback_;
};

} // namespace tmux2::vte
