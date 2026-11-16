#pragma once

#include <cstdint>
#include <string_view>
#include <termios.h>

namespace tmux2::client {

struct WindowSize {
    uint16_t cols{80};
    uint16_t rows{24};
    uint32_t xpixel{0};
    uint32_t ypixel{0};

    constexpr bool operator==(const WindowSize&) const = default;
};

class TtyRaw {
public:
    explicit TtyRaw(int stdin_fd = 0, int stdout_fd = 1);
    ~TtyRaw();

    TtyRaw(const TtyRaw&) = delete;
    TtyRaw& operator=(const TtyRaw&) = delete;
    TtyRaw(TtyRaw&& other) noexcept;
    TtyRaw& operator=(TtyRaw&& other) noexcept;

    // Raw mode toggle
    bool enable_raw_mode();
    void disable_raw_mode();
    [[nodiscard]] bool is_raw() const noexcept { return is_raw_; }
    [[nodiscard]] bool is_a_tty() const noexcept;

    // Terminal window dimensions
    [[nodiscard]] WindowSize get_window_size() const;

    // Terminal modes (escapes)
    void enter_alt_screen();
    void leave_alt_screen();
    [[nodiscard]] bool is_alt_screen() const noexcept { return alt_screen_active_; }

    void enable_mouse();
    void disable_mouse();
    [[nodiscard]] bool is_mouse_enabled() const noexcept { return mouse_active_; }

    void enable_bracketed_paste();
    void disable_bracketed_paste();
    [[nodiscard]] bool is_bracketed_paste_enabled() const noexcept { return bracketed_paste_active_; }

    void hide_cursor();
    void show_cursor();
    [[nodiscard]] bool is_cursor_hidden() const noexcept { return cursor_hidden_; }

    // Direct write to stdout
    void write_raw(std::string_view bytes);

private:
    int stdin_fd_{0};
    int stdout_fd_{1};
    struct termios orig_termios_{};
    bool has_orig_termios_{false};
    bool is_raw_{false};
    bool alt_screen_active_{false};
    bool mouse_active_{false};
    bool bracketed_paste_active_{false};
    bool cursor_hidden_{false};
};

} // namespace tmux2::client
