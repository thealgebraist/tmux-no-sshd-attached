#include "src/client/tty_raw.hpp"

#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

namespace tmux2::client {

TtyRaw::TtyRaw(int stdin_fd, int stdout_fd)
    : stdin_fd_(stdin_fd), stdout_fd_(stdout_fd) {}

TtyRaw::~TtyRaw() {
    disable_raw_mode();
    if (mouse_active_) {
        disable_mouse();
    }
    if (bracketed_paste_active_) {
        disable_bracketed_paste();
    }
    if (cursor_hidden_) {
        show_cursor();
    }
    if (alt_screen_active_) {
        leave_alt_screen();
    }
}

TtyRaw::TtyRaw(TtyRaw&& other) noexcept
    : stdin_fd_(other.stdin_fd_),
      stdout_fd_(other.stdout_fd_),
      orig_termios_(other.orig_termios_),
      has_orig_termios_(other.has_orig_termios_),
      is_raw_(other.is_raw_),
      alt_screen_active_(other.alt_screen_active_),
      mouse_active_(other.mouse_active_),
      bracketed_paste_active_(other.bracketed_paste_active_),
      cursor_hidden_(other.cursor_hidden_) {
    other.is_raw_ = false;
    other.has_orig_termios_ = false;
    other.alt_screen_active_ = false;
    other.mouse_active_ = false;
    other.bracketed_paste_active_ = false;
    other.cursor_hidden_ = false;
}

TtyRaw& TtyRaw::operator=(TtyRaw&& other) noexcept {
    if (this != &other) {
        disable_raw_mode();
        if (mouse_active_) disable_mouse();
        if (bracketed_paste_active_) disable_bracketed_paste();
        if (cursor_hidden_) show_cursor();
        if (alt_screen_active_) leave_alt_screen();

        stdin_fd_ = other.stdin_fd_;
        stdout_fd_ = other.stdout_fd_;
        orig_termios_ = other.orig_termios_;
        has_orig_termios_ = other.has_orig_termios_;
        is_raw_ = other.is_raw_;
        alt_screen_active_ = other.alt_screen_active_;
        mouse_active_ = other.mouse_active_;
        bracketed_paste_active_ = other.bracketed_paste_active_;
        cursor_hidden_ = other.cursor_hidden_;

        other.is_raw_ = false;
        other.has_orig_termios_ = false;
        other.alt_screen_active_ = false;
        other.mouse_active_ = false;
        other.bracketed_paste_active_ = false;
        other.cursor_hidden_ = false;
    }
    return *this;
}

bool TtyRaw::is_a_tty() const noexcept {
    return ::isatty(stdin_fd_) != 0;
}

bool TtyRaw::enable_raw_mode() {
    if (is_raw_) {
        return true;
    }
    if (!is_a_tty()) {
        return false;
    }

    if (::tcgetattr(stdin_fd_, &orig_termios_) != 0) {
        return false;
    }
    has_orig_termios_ = true;

    struct termios raw = orig_termios_;
    // Input flags: disable break condition, CR-to-NL, parity check, strip 8th bit, flow control
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // Output flags: disable post-processing (\n to \r\n, etc.)
    raw.c_oflag &= ~(OPOST);
    // Control flags: 8 bits per byte
    raw.c_cflag |= (CS8);
    // Local flags: disable echo, canonical mode, extended input, signals (SIGINT, SIGTSTP)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    // Read at least 1 byte, non-blocking timeout 0
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (::tcsetattr(stdin_fd_, TCSAFLUSH, &raw) != 0) {
        return false;
    }

    is_raw_ = true;
    return true;
}

void TtyRaw::disable_raw_mode() {
    if (!is_raw_ || !has_orig_termios_) {
        return;
    }
    ::tcsetattr(stdin_fd_, TCSAFLUSH, &orig_termios_);
    is_raw_ = false;
}

WindowSize TtyRaw::get_window_size() const {
    WindowSize ws{80, 24, 0, 0};
    struct winsize w{};
    if (::ioctl(stdout_fd_, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
        ws.cols = w.ws_col;
        ws.rows = w.ws_row;
        ws.xpixel = w.ws_xpixel;
        ws.ypixel = w.ws_ypixel;
        return ws;
    }

    // Fallback to COLUMNS and LINES environment variables
    const char* cols_env = std::getenv("COLUMNS");
    const char* lines_env = std::getenv("LINES");
    if (cols_env) {
        int c = std::atoi(cols_env);
        if (c > 0) ws.cols = static_cast<uint16_t>(c);
    }
    if (lines_env) {
        int r = std::atoi(lines_env);
        if (r > 0) ws.rows = static_cast<uint16_t>(r);
    }
    return ws;
}

void TtyRaw::enter_alt_screen() {
    write_raw("\x1b[?1049h");
    alt_screen_active_ = true;
}

void TtyRaw::leave_alt_screen() {
    write_raw("\x1b[?1049l");
    alt_screen_active_ = false;
}

void TtyRaw::enable_mouse() {
    // 1000h: Normal mouse tracking, 1002h: Button event tracking, 1006h: SGR extended coordinates
    write_raw("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    mouse_active_ = true;
}

void TtyRaw::disable_mouse() {
    write_raw("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
    mouse_active_ = false;
}

void TtyRaw::enable_bracketed_paste() {
    write_raw("\x1b[?2004h");
    bracketed_paste_active_ = true;
}

void TtyRaw::disable_bracketed_paste() {
    write_raw("\x1b[?2004l");
    bracketed_paste_active_ = false;
}

void TtyRaw::hide_cursor() {
    write_raw("\x1b[?25l");
    cursor_hidden_ = true;
}

void TtyRaw::show_cursor() {
    write_raw("\x1b[?25h");
    cursor_hidden_ = false;
}

void TtyRaw::write_raw(std::string_view bytes) {
    if (bytes.empty() || stdout_fd_ < 0) return;
    const char* ptr = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        ssize_t written = ::write(stdout_fd_, ptr, remaining);
        if (written <= 0) break;
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }
}

} // namespace tmux2::client
