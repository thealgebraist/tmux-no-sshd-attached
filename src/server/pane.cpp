#include "src/server/pane.hpp"

#include <algorithm>
#include <sstream>

namespace tmux2::server {

namespace {

void append_utf8_codepoint(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

Pane::Pane(uint32_t pane_id, uint16_t width, uint16_t height)
    : pane_id_(pane_id), width_(width > 0 ? width : 1), height_(height > 0 ? height : 1) {
    rect_ = {0, 0, width_, height_};
    vte_ = std::make_unique<vte::VteScreen>(width_, height_);
    vte_->set_title_callback([this](std::string_view t) {
        title_ = std::string(t);
    });
    vte_->set_output_callback([this](std::string_view response) {
        if (pty_ && !response.empty()) {
            (void)pty_->write_master(response);
        }
    });
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

protocol::CursorState Pane::cursor() const noexcept {
    return vte_ ? vte_->cursor() : protocol::CursorState{};
}

void Pane::resize(uint16_t width, uint16_t height) {
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    if (width == width_ && height == height_) return;

    width_ = width;
    height_ = height;
    rect_.width = width;
    rect_.height = height;

    if (vte_) {
        vte_->resize(width_, height_);
    }

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

void Pane::feed_output(std::string_view data) {
    if (vte_ && !data.empty()) {
        vte_->feed(data);
    }
}

protocol::Cell Pane::get_cell(uint16_t col, uint16_t row) const {
    if (vte_) {
        return vte_->get_cell(col, row);
    }
    return protocol::Cell{};
}

protocol::MsgScreenSnapshot Pane::get_snapshot(uint64_t server_seq) const {
    if (vte_) {
        return vte_->get_snapshot(server_seq, pane_id_);
    }
    protocol::MsgScreenSnapshot snap;
    snap.server_seq = server_seq;
    snap.pane_id = pane_id_;
    snap.cols = width_;
    snap.rows = height_;
    return snap;
}

protocol::PaneRowDiff Pane::get_dirty_diff(uint64_t /*server_seq*/) {
    protocol::PaneRowDiff diff;
    diff.pane_id = pane_id_;
    if (vte_) {
        diff.cursor = vte_->cursor();
        diff.rows = vte_->get_dirty_diffs();
    }
    return diff;
}

void Pane::clear_dirty() noexcept {
    if (vte_) {
        vte_->dirty_tracker().clear();
    }
}

protocol::MsgScrollbackSliceResp Pane::get_scrollback_slice(
    uint64_t start_line, uint32_t line_count) const {
    protocol::MsgScrollbackSliceResp resp;
    resp.pane_id = pane_id_;
    resp.start_line = start_line;

    if (vte_) {
        resp.total_available_lines = vte_->scrollback().total_lines();
        resp.lines = vte_->scrollback().get_slice(start_line, line_count);
    }
    return resp;
}

std::string Pane::capture_text(bool include_history, int start_line, int end_line) const {
    if (!vte_) return "";
    std::ostringstream ss;

    if (include_history) {
        size_t hist_total = vte_->scrollback().total_lines();
        size_t hist_start = (start_line < 0) ? 0 : static_cast<size_t>(start_line);
        size_t hist_end = hist_total;
        if (end_line >= 0 && static_cast<size_t>(end_line) < hist_end) {
            hist_end = static_cast<size_t>(end_line);
        }
        for (size_t i = hist_start; i < hist_end; ++i) {
            auto line_opt = vte_->scrollback().get_line(i);
            if (!line_opt) continue;
            std::string line_str;
            for (const auto& cell : line_opt->cells) {
                if (cell.codepoint == 0 || cell.codepoint == ' ') {
                    line_str += ' ';
                } else {
                    append_utf8_codepoint(line_str, cell.codepoint);
                }
            }
            size_t last = line_str.find_last_not_of(' ');
            if (last != std::string::npos) {
                line_str.erase(last + 1);
            } else {
                line_str.clear();
            }
            ss << line_str << "\n";
        }
    }

    for (uint16_t r = 0; r < height_; ++r) {
        std::string row_str;
        auto row_cells = vte_->get_row(r);
        for (const auto& cell : row_cells) {
            if (cell.codepoint == 0 || cell.codepoint == ' ') {
                row_str += ' ';
            } else {
                append_utf8_codepoint(row_str, cell.codepoint);
            }
        }
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
