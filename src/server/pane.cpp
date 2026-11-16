#include "src/server/pane.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace tmux2::server {

Pane::Pane(uint32_t pane_id, uint16_t width, uint16_t height)
    : pane_id_(pane_id), width_(width > 0 ? width : 1), height_(height > 0 ? height : 1) {
    rect_ = {0, 0, width_, height_};
    ensure_grid_size();
}

std::shared_ptr<Pane> Pane::create(
    uint32_t pane_id, uint16_t width, uint16_t height, const pty::PtyOptions& options) {
    auto pane = std::make_shared<Pane>(pane_id, width, height);

    pty::PtyOptions opt = options;
    opt.cols = pane->width();
    opt.rows = pane->height();

    auto spawn_res = pty::PtyProcess::spawn(opt);
    if (spawn_res) {
        pane->set_pty(std::make_unique<pty::PtyProcess>(std::move(*spawn_res)));
    }
    return pane;
}

std::shared_ptr<Pane> Pane::create_mock(uint32_t pane_id, uint16_t width, uint16_t height) {
    return std::make_shared<Pane>(pane_id, width, height);
}

void Pane::set_rect(const Rect& r) {
    rect_ = r;
    if (rect_.width > 0 && rect_.height > 0 && (rect_.width != width_ || rect_.height != height_)) {
        resize(rect_.width, rect_.height);
    }
}

void Pane::ensure_grid_size() {
    size_t total = static_cast<size_t>(width_) * height_;
    grid_.assign(total, protocol::Cell{});
    dirty_rows_.assign(height_, true);
}

void Pane::resize(uint16_t width, uint16_t height) {
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    if (width == width_ && height == height_) return;

    std::vector<protocol::Cell> new_grid(static_cast<size_t>(width) * height, protocol::Cell{});

    // Copy overlapping cells
    uint16_t min_w = std::min(width_, width);
    uint16_t min_h = std::min(height_, height);

    for (uint16_t r = 0; r < min_h; ++r) {
        for (uint16_t c = 0; c < min_w; ++c) {
            new_grid[static_cast<size_t>(r) * width + c] = grid_[static_cast<size_t>(r) * width_ + c];
        }
    }

    width_ = width;
    height_ = height;
    rect_.width = width;
    rect_.height = height;
    grid_ = std::move(new_grid);
    dirty_rows_.assign(height_, true);

    if (cursor_.col >= width_) cursor_.col = width_ - 1;
    if (cursor_.row >= height_) cursor_.row = height_ - 1;

    if (pty_) {
        pty_->resize(width_, height_);
    }
}

void Pane::write_input(std::string_view data) {
    if (pty_ && !data.empty()) {
        (void)pty_->write_master(data);
    }
}

bool Pane::read_pty() {
    if (!pty_) return false;
    char buf[4096];
    bool read_any = false;
    while (true) {
        auto res = pty_->read_master(std::span<char>(buf, sizeof(buf)));
        if (!res) {
            break;
        }
        if (*res == 0) break;
        read_any = true;
        feed_output(std::string_view(buf, *res));
    }
    return read_any;
}

bool Pane::is_alive() const noexcept {
    if (pty_) {
        return pty_->is_alive();
    }
    return true;
}

void Pane::send_signal(int sig) {
    if (pty_) {
        pty_->send_signal(sig);
    }
}

void Pane::mark_row_dirty(uint16_t row) {
    if (row < dirty_rows_.size()) {
        dirty_rows_[row] = true;
    }
}

void Pane::scroll_up() {
    protocol::ScrollbackLine line;
    line.wrapped = false;
    line.cells.reserve(width_);
    for (uint16_t c = 0; c < width_; ++c) {
        line.cells.push_back(grid_[c]);
    }
    scrollback_.push_back(std::move(line));

    // Shift rows up
    for (size_t i = 0; i < static_cast<size_t>(height_ - 1) * width_; ++i) {
        grid_[i] = grid_[i + width_];
    }

    // Blank out last row
    size_t last_row_start = static_cast<size_t>(height_ - 1) * width_;
    for (size_t c = 0; c < width_; ++c) {
        grid_[last_row_start + c] = protocol::Cell{};
    }

    // All rows are now dirty
    std::fill(dirty_rows_.begin(), dirty_rows_.end(), true);
}

void Pane::feed_output(std::string_view data) {
    for (size_t i = 0; i < data.size(); ++i) {
        char ch = data[i];

        switch (parser_state_) {
            case ParserState::Normal: {
                if (ch == '\x1b') {
                    parser_state_ = ParserState::Escape;
                    escape_buf_.clear();
                } else if (ch == '\r') {
                    cursor_.col = 0;
                } else if (ch == '\n') {
                    if (cursor_.row + 1 >= height_) {
                        scroll_up();
                    } else {
                        cursor_.row++;
                    }
                    mark_row_dirty(cursor_.row);
                } else if (ch == '\b') {
                    if (cursor_.col > 0) cursor_.col--;
                } else if (ch == '\t') {
                    cursor_.col = static_cast<uint16_t>((cursor_.col + 8) & ~7);
                    if (cursor_.col >= width_) cursor_.col = width_ - 1;
                } else if (static_cast<unsigned char>(ch) >= 32) {
                    if (cursor_.col >= width_) {
                        cursor_.col = 0;
                        if (cursor_.row + 1 >= height_) {
                            scroll_up();
                        } else {
                            cursor_.row++;
                        }
                    }

                    // Decode UTF-8 code point
                    char32_t codepoint = static_cast<unsigned char>(ch);
                    if ((ch & 0x80) != 0) {
                        size_t needed = 0;
                        if ((ch & 0xE0) == 0xC0) { codepoint = ch & 0x1F; needed = 1; }
                        else if ((ch & 0xF0) == 0xE0) { codepoint = ch & 0x0F; needed = 2; }
                        else if ((ch & 0xF8) == 0xF0) { codepoint = ch & 0x07; needed = 3; }

                        while (needed > 0 && (i + 1) < data.size()) {
                            ++i;
                            codepoint = (codepoint << 6) | (static_cast<unsigned char>(data[i]) & 0x3F);
                            --needed;
                        }
                    }

                    protocol::Cell cell{codepoint, 1, protocol::CellStyle{current_flags_, current_fg_, current_bg_}};
                    size_t idx = static_cast<size_t>(cursor_.row) * width_ + cursor_.col;
                    if (idx < grid_.size()) {
                        grid_[idx] = cell;
                        mark_row_dirty(cursor_.row);
                    }
                    cursor_.col++;
                }
                break;
            }

            case ParserState::Escape: {
                if (ch == '[') {
                    parser_state_ = ParserState::Csi;
                    escape_buf_.clear();
                } else if (ch == ']') {
                    parser_state_ = ParserState::Osc;
                    escape_buf_.clear();
                } else {
                    // Two-character escape sequence (e.g. ESC M, ESC 7, ESC 8)
                    if (ch == 'M') { // Reverse Index
                        if (cursor_.row == 0) {
                            // Scroll down
                        } else {
                            cursor_.row--;
                        }
                    } else if (ch == '7') { // Save cursor
                    } else if (ch == '8') { // Restore cursor
                    }
                    parser_state_ = ParserState::Normal;
                }
                break;
            }

            case ParserState::Csi: {
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '@' || ch == '`') {
                    process_csi(escape_buf_, ch);
                    parser_state_ = ParserState::Normal;
                } else {
                    escape_buf_.push_back(ch);
                    if (escape_buf_.size() > 64) {
                        parser_state_ = ParserState::Normal;
                    }
                }
                break;
            }

            case ParserState::Osc: {
                if (ch == '\x07' || (ch == '\\' && !escape_buf_.empty() && escape_buf_.back() == '\x1b')) {
                    if (ch == '\\' && !escape_buf_.empty()) escape_buf_.pop_back();
                    process_osc(escape_buf_);
                    parser_state_ = ParserState::Normal;
                } else {
                    escape_buf_.push_back(ch);
                    if (escape_buf_.size() > 512) {
                        parser_state_ = ParserState::Normal;
                    }
                }
                break;
            }
        }
    }
}

void Pane::process_csi(std::string_view params, char cmd) {
    // Parse integer arguments
    std::vector<int> args;
    std::string_view rem = params;
    while (!rem.empty()) {
        size_t semi = rem.find(';');
        std::string_view part = (semi == std::string_view::npos) ? rem : rem.substr(0, semi);
        int val = 0;
        if (!part.empty()) {
            std::from_chars(part.data(), part.data() + part.size(), val);
        }
        args.push_back(val);
        if (semi == std::string_view::npos) break;
        rem.remove_prefix(semi + 1);
    }
    if (args.empty()) args.push_back(0);

    switch (cmd) {
        case 'm': { // SGR
            for (size_t i = 0; i < args.size(); ++i) {
                int code = args[i];
                if (code == 0) {
                    current_flags_ = protocol::StyleFlags::None;
                    current_fg_ = protocol::Color::default_color();
                    current_bg_ = protocol::Color::default_color();
                } else if (code == 1) {
                    current_flags_ |= protocol::StyleFlags::Bold;
                } else if (code == 2) {
                    current_flags_ |= protocol::StyleFlags::Dim;
                } else if (code == 3) {
                    current_flags_ |= protocol::StyleFlags::Italic;
                } else if (code == 4) {
                    current_flags_ |= protocol::StyleFlags::Underline;
                } else if (code == 7) {
                    current_flags_ |= protocol::StyleFlags::Reverse;
                } else if (code >= 30 && code <= 37) {
                    current_fg_ = protocol::Color::indexed(static_cast<uint8_t>(code - 30));
                } else if (code == 39) {
                    current_fg_ = protocol::Color::default_color();
                } else if (code >= 40 && code <= 47) {
                    current_bg_ = protocol::Color::indexed(static_cast<uint8_t>(code - 40));
                } else if (code == 49) {
                    current_bg_ = protocol::Color::default_color();
                } else if (code >= 90 && code <= 97) {
                    current_fg_ = protocol::Color::indexed(static_cast<uint8_t>(code - 90 + 8));
                } else if (code >= 100 && code <= 107) {
                    current_bg_ = protocol::Color::indexed(static_cast<uint8_t>(code - 100 + 8));
                } else if ((code == 38 || code == 48) && i + 1 < args.size()) {
                    bool is_fg = (code == 38);
                    int mode = args[++i];
                    if (mode == 5 && i + 1 < args.size()) {
                        uint8_t idx = static_cast<uint8_t>(args[++i]);
                        if (is_fg) current_fg_ = protocol::Color::indexed(idx);
                        else current_bg_ = protocol::Color::indexed(idx);
                    } else if (mode == 2 && i + 3 < args.size()) {
                        uint8_t r = static_cast<uint8_t>(args[++i]);
                        uint8_t g = static_cast<uint8_t>(args[++i]);
                        uint8_t b = static_cast<uint8_t>(args[++i]);
                        if (is_fg) current_fg_ = protocol::Color::rgb(r, g, b);
                        else current_bg_ = protocol::Color::rgb(r, g, b);
                    }
                }
            }
            break;
        }

        case 'H':
        case 'f': { // Cursor position
            int r = args.size() > 0 && args[0] > 0 ? args[0] - 1 : 0;
            int c = args.size() > 1 && args[1] > 0 ? args[1] - 1 : 0;
            cursor_.row = static_cast<uint16_t>(std::clamp(r, 0, height_ - 1));
            cursor_.col = static_cast<uint16_t>(std::clamp(c, 0, width_ - 1));
            mark_row_dirty(cursor_.row);
            break;
        }

        case 'A': { // Cursor up
            int count = args[0] > 0 ? args[0] : 1;
            cursor_.row = static_cast<uint16_t>(cursor_.row >= count ? cursor_.row - count : 0);
            mark_row_dirty(cursor_.row);
            break;
        }

        case 'B': { // Cursor down
            int count = args[0] > 0 ? args[0] : 1;
            cursor_.row = static_cast<uint16_t>(std::min(height_ - 1, cursor_.row + count));
            mark_row_dirty(cursor_.row);
            break;
        }

        case 'C': { // Cursor forward
            int count = args[0] > 0 ? args[0] : 1;
            cursor_.col = static_cast<uint16_t>(std::min(width_ - 1, cursor_.col + count));
            break;
        }

        case 'D': { // Cursor backward
            int count = args[0] > 0 ? args[0] : 1;
            cursor_.col = static_cast<uint16_t>(cursor_.col >= count ? cursor_.col - count : 0);
            break;
        }

        case 'J': { // Erase in display
            int mode = args[0];
            if (mode == 0) { // Cursor to end
                size_t start = static_cast<size_t>(cursor_.row) * width_ + cursor_.col;
                for (size_t k = start; k < grid_.size(); ++k) grid_[k] = protocol::Cell{};
                for (size_t r = cursor_.row; r < height_; ++r) mark_row_dirty(static_cast<uint16_t>(r));
            } else if (mode == 1) { // Start to cursor
                size_t end = static_cast<size_t>(cursor_.row) * width_ + cursor_.col;
                for (size_t k = 0; k <= end && k < grid_.size(); ++k) grid_[k] = protocol::Cell{};
                for (size_t r = 0; r <= cursor_.row; ++r) mark_row_dirty(static_cast<uint16_t>(r));
            } else if (mode == 2 || mode == 3) { // Whole screen
                ensure_grid_size();
            }
            break;
        }

        case 'K': { // Erase in line
            int mode = args[0];
            size_t row_start = static_cast<size_t>(cursor_.row) * width_;
            if (mode == 0) { // Cursor to end of line
                for (size_t c = cursor_.col; c < width_; ++c) grid_[row_start + c] = protocol::Cell{};
            } else if (mode == 1) { // Start of line to cursor
                for (size_t c = 0; c <= cursor_.col && c < width_; ++c) grid_[row_start + c] = protocol::Cell{};
            } else if (mode == 2) { // Entire line
                for (size_t c = 0; c < width_; ++c) grid_[row_start + c] = protocol::Cell{};
            }
            mark_row_dirty(cursor_.row);
            break;
        }
    }
}

void Pane::process_osc(std::string_view content) {
    // OSC 0;title or 2;title
    if (content.starts_with("0;") || content.starts_with("2;")) {
        title_ = std::string(content.substr(2));
    }
}

protocol::Cell Pane::get_cell(uint16_t col, uint16_t row) const {
    if (col < width_ && row < height_) {
        return grid_[static_cast<size_t>(row) * width_ + col];
    }
    return protocol::Cell{};
}

protocol::MsgScreenSnapshot Pane::get_snapshot(uint64_t server_seq) const {
    protocol::MsgScreenSnapshot snap;
    snap.server_seq = server_seq;
    snap.pane_id = pane_id_;
    snap.cols = width_;
    snap.rows = height_;
    snap.cursor = cursor_;
    snap.modes = 0;
    snap.cells = grid_;
    return snap;
}

protocol::PaneRowDiff Pane::get_dirty_diff(uint64_t /*server_seq*/) {
    protocol::PaneRowDiff diff;
    diff.pane_id = pane_id_;
    diff.cursor = cursor_;

    for (uint16_t r = 0; r < height_; ++r) {
        if (dirty_rows_[r]) {
            protocol::CellRowDiff rd;
            rd.row = r;
            rd.start_col = 0;
            rd.cells.reserve(width_);
            size_t row_start = static_cast<size_t>(r) * width_;
            for (uint16_t c = 0; c < width_; ++c) {
                rd.cells.push_back(grid_[row_start + c]);
            }
            diff.rows.push_back(std::move(rd));
            dirty_rows_[r] = false;
        }
    }
    return diff;
}

void Pane::clear_dirty() noexcept {
    std::fill(dirty_rows_.begin(), dirty_rows_.end(), false);
}

protocol::MsgScrollbackSliceResp Pane::get_scrollback_slice(
    uint64_t start_line, uint32_t line_count) const {
    protocol::MsgScrollbackSliceResp resp;
    resp.pane_id = pane_id_;
    resp.start_line = start_line;
    resp.total_available_lines = scrollback_.size();

    if (start_line < scrollback_.size()) {
        size_t count = std::min<size_t>(line_count, scrollback_.size() - start_line);
        resp.lines.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            resp.lines.push_back(scrollback_[start_line + i]);
        }
    }
    return resp;
}

std::string Pane::capture_text(bool include_history, int start_line, int end_line) const {
    std::ostringstream ss;

    if (include_history) {
        size_t hist_start = (start_line < 0) ? 0 : static_cast<size_t>(start_line);
        size_t hist_end = scrollback_.size();
        if (end_line >= 0 && static_cast<size_t>(end_line) < hist_end) {
            hist_end = static_cast<size_t>(end_line);
        }
        for (size_t i = hist_start; i < hist_end; ++i) {
            for (const auto& cell : scrollback_[i].cells) {
                if (cell.codepoint == 0 || cell.codepoint == ' ') ss << ' ';
                else {
                    char buf[8] = {};
                    // Convert unicode codepoint to UTF-8
                    if (cell.codepoint < 0x80) {
                        buf[0] = static_cast<char>(cell.codepoint);
                    } else if (cell.codepoint < 0x800) {
                        buf[0] = static_cast<char>(0xC0 | (cell.codepoint >> 6));
                        buf[1] = static_cast<char>(0x80 | (cell.codepoint & 0x3F));
                    } else {
                        buf[0] = static_cast<char>(0xE0 | (cell.codepoint >> 12));
                        buf[1] = static_cast<char>(0x80 | ((cell.codepoint >> 6) & 0x3F));
                        buf[2] = static_cast<char>(0x80 | (cell.codepoint & 0x3F));
                    }
                    ss << buf;
                }
            }
            ss << "\n";
        }
    }

    // Visible grid
    for (uint16_t r = 0; r < height_; ++r) {
        size_t row_start = static_cast<size_t>(r) * width_;
        std::string row_str;
        for (uint16_t c = 0; c < width_; ++c) {
            const auto& cell = grid_[row_start + c];
            if (cell.codepoint == 0 || cell.codepoint == ' ') {
                row_str += ' ';
            } else if (cell.codepoint < 0x80) {
                row_str += static_cast<char>(cell.codepoint);
            } else {
                char buf[8] = {};
                if (cell.codepoint < 0x800) {
                    buf[0] = static_cast<char>(0xC0 | (cell.codepoint >> 6));
                    buf[1] = static_cast<char>(0x80 | (cell.codepoint & 0x3F));
                } else {
                    buf[0] = static_cast<char>(0xE0 | (cell.codepoint >> 12));
                    buf[1] = static_cast<char>(0x80 | ((cell.codepoint >> 6) & 0x3F));
                    buf[2] = static_cast<char>(0x80 | (cell.codepoint & 0x3F));
                }
                row_str += buf;
            }
        }
        // Trim trailing spaces
        size_t last = row_str.find_last_not_of(' ');
        if (last != std::string::npos) {
            row_str.erase(last + 1);
        } else {
            row_str.clear();
        }
        ss << row_str << "\n";
    }

    return ss.str();
}

} // namespace tmux2::server
