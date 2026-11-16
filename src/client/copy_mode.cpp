#include "src/client/copy_mode.hpp"
#include "src/client/renderer.hpp"

#include <algorithm>
#include <format>

namespace tmux2::client {

namespace {

void append_utf8_char(char32_t cp, std::string& out) {
    if (cp == 0) {
        out.push_back(' ');
        return;
    }
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
    } else {
        out.push_back('?');
    }
}

} // namespace

void CopyMode::enter(uint32_t pane_id, uint16_t view_cols, uint16_t view_rows, uint64_t initial_total_lines) {
    pane_id_ = pane_id;
    view_cols_ = view_cols > 0 ? view_cols : 80;
    view_rows_ = view_rows > 0 ? view_rows : 24;
    total_lines_ = std::max<uint64_t>(initial_total_lines, view_rows_);

    viewport_top_line_ = (total_lines_ >= view_rows_) ? static_cast<int64_t>(total_lines_ - view_rows_) : 0;
    cursor_ = CopyPosition{
        .line = static_cast<int64_t>(total_lines_ > 0 ? total_lines_ - 1 : 0),
        .col = 0
    };

    active_ = true;
    selecting_ = false;
    line_selection_ = false;
    search_mode_ = false;
    search_forward_ = true;
    search_query_.clear();
}

void CopyMode::exit() {
    active_ = false;
    selecting_ = false;
    line_selection_ = false;
    search_mode_ = false;
}

void CopyMode::receive_slice_resp(const protocol::MsgScrollbackSliceResp& resp) {
    if (resp.pane_id != pane_id_) return;
    uint64_t old_total = total_lines_;
    total_lines_ = std::max(total_lines_, resp.total_available_lines);

    for (size_t i = 0; i < resp.lines.size(); ++i) {
        history_cache_[resp.start_line + i] = resp.lines[i];
    }

    if (cursor_.line == static_cast<int64_t>(old_total - 1) && total_lines_ > old_total) {
        cursor_.line = static_cast<int64_t>(total_lines_ - 1);
        scroll_viewport_to_cursor();
    }
}

void CopyMode::add_local_history_lines(uint64_t start_line, std::vector<protocol::ScrollbackLine> lines) {
    for (size_t i = 0; i < lines.size(); ++i) {
        history_cache_[start_line + i] = std::move(lines[i]);
    }
    if (start_line + lines.size() > total_lines_) {
        total_lines_ = start_line + lines.size();
    }
}

std::optional<protocol::MsgScrollbackSliceReq> CopyMode::check_missing_slices() const {
    if (!active_ || total_lines_ == 0) return std::nullopt;

    uint64_t start = static_cast<uint64_t>(std::max<int64_t>(0, viewport_top_line_));
    uint32_t count = view_rows_;

    bool missing = false;
    for (uint64_t l = start; l < start + count && l < total_lines_; ++l) {
        if (!history_cache_.contains(l)) {
            missing = true;
            break;
        }
    }

    if (missing) {
        return protocol::MsgScrollbackSliceReq{
            .pane_id = pane_id_,
            .start_line = start,
            .line_count = count
        };
    }
    return std::nullopt;
}

void CopyMode::clamp_cursor() {
    if (cursor_.line < 0) cursor_.line = 0;
    if (total_lines_ > 0 && cursor_.line >= static_cast<int64_t>(total_lines_)) {
        cursor_.line = static_cast<int64_t>(total_lines_ - 1);
    }
    if (cursor_.col >= view_cols_) {
        cursor_.col = view_cols_ > 0 ? view_cols_ - 1 : 0;
    }
}

void CopyMode::scroll_viewport_to_cursor() {
    if (cursor_.line < viewport_top_line_) {
        viewport_top_line_ = cursor_.line;
    } else if (cursor_.line >= viewport_top_line_ + view_rows_) {
        viewport_top_line_ = cursor_.line - view_rows_ + 1;
    }
    if (viewport_top_line_ < 0) viewport_top_line_ = 0;
}

CopyMode::KeyResult CopyMode::handle_key(const ClientKeyEvent& key) {
    KeyResult res;
    if (!active_) {
        res.exited = true;
        return res;
    }

    if (search_mode_) {
        if (key.key_code == SpecialKey::Escape) {
            search_mode_ = false;
            return res;
        }
        if (key.key_code == SpecialKey::Enter) {
            search_mode_ = false;
            execute_search(search_forward_);
            res.slice_req = check_missing_slices();
            return res;
        }
        if (key.key_code == SpecialKey::Backspace) {
            if (!search_query_.empty()) search_query_.pop_back();
            return res;
        }
        if ((key.modifiers == KeyModifiers::None || key.modifiers == KeyModifiers::Shift) &&
            key.key_code >= 32 && key.key_code <= 126) {
            search_query_.push_back(static_cast<char>(key.key_code));
            return res;
        }
        return res;
    }

    // Normal Copy Mode navigation
    if (key.key_code == SpecialKey::Escape || key.key_code == 'q') {
        exit();
        res.exited = true;
        return res;
    }

    // Cursor movement: h / Left
    if (key.key_code == 'h' || key.key_code == SpecialKey::Left) {
        if (cursor_.col > 0) cursor_.col--;
    }
    // l / Right
    else if (key.key_code == 'l' || key.key_code == SpecialKey::Right) {
        if (cursor_.col + 1 < view_cols_) cursor_.col++;
    }
    // j / Down
    else if (key.key_code == 'j' || key.key_code == SpecialKey::Down) {
        cursor_.line++;
        clamp_cursor();
        scroll_viewport_to_cursor();
    }
    // k / Up
    else if (key.key_code == 'k' || key.key_code == SpecialKey::Up) {
        cursor_.line--;
        clamp_cursor();
        scroll_viewport_to_cursor();
    }
    // PageUp / Ctrl-b / Ctrl-u
    else if (key.key_code == SpecialKey::PageUp ||
             (key.key_code == 'b' && (key.modifiers & KeyModifiers::Ctrl)) ||
             (key.key_code == 'u' && (key.modifiers & KeyModifiers::Ctrl))) {
        int64_t step = std::max<int64_t>(1, view_rows_ / 2);
        cursor_.line -= step;
        viewport_top_line_ -= step;
        clamp_cursor();
        scroll_viewport_to_cursor();
    }
    // PageDown / Ctrl-f / Ctrl-d
    else if (key.key_code == SpecialKey::PageDown ||
             (key.key_code == 'f' && (key.modifiers & KeyModifiers::Ctrl)) ||
             (key.key_code == 'd' && (key.modifiers & KeyModifiers::Ctrl))) {
        int64_t step = std::max<int64_t>(1, view_rows_ / 2);
        cursor_.line += step;
        viewport_top_line_ += step;
        clamp_cursor();
        scroll_viewport_to_cursor();
    }
    // g (top)
    else if (key.key_code == 'g') {
        cursor_.line = 0;
        cursor_.col = 0;
        viewport_top_line_ = 0;
    }
    // G (bottom)
    else if (key.key_code == 'G' || (key.key_code == 'g' && (key.modifiers & KeyModifiers::Shift))) {
        cursor_.line = total_lines_ > 0 ? static_cast<int64_t>(total_lines_ - 1) : 0;
        cursor_.col = 0;
        scroll_viewport_to_cursor();
    }
    // Space or v: toggle character selection
    else if (key.key_code == ' ' || (key.key_code == 'v' && (key.modifiers == KeyModifiers::None))) {
        if (!selecting_ || line_selection_) {
            selecting_ = true;
            line_selection_ = false;
            sel_start_ = cursor_;
        } else {
            selecting_ = false;
            line_selection_ = false;
        }
    }
    // V or Shift-v: toggle line selection
    else if (key.key_code == 'V' || (key.key_code == 'v' && (key.modifiers & KeyModifiers::Shift))) {
        if (!selecting_ || !line_selection_) {
            selecting_ = true;
            line_selection_ = true;
            sel_start_ = cursor_;
        } else {
            selecting_ = false;
            line_selection_ = false;
        }
    }
    // Enter or y: Yank selection
    else if (key.key_code == SpecialKey::Enter || key.key_code == 'y') {
        res.yanked_osc52 = yank();
        exit();
        res.exited = true;
        return res;
    }
    // /: search forward
    else if (key.key_code == '/') {
        search_mode_ = true;
        search_forward_ = true;
        search_query_.clear();
    }
    // ?: search backward
    else if (key.key_code == '?') {
        search_mode_ = true;
        search_forward_ = false;
        search_query_.clear();
    }
    // n: next match (in search direction)
    else if (key.key_code == 'n') {
        execute_search(search_forward_);
    }
    // N: previous match (opposite to search direction)
    else if (key.key_code == 'N') {
        execute_search(!search_forward_);
    }

    res.slice_req = check_missing_slices();
    return res;
}

void CopyMode::execute_search(bool forward) {
    if (search_query_.empty() || total_lines_ == 0) return;

    int64_t start_line = forward ? cursor_.line + 1 : cursor_.line - 1;
    int64_t end_line = forward ? static_cast<int64_t>(total_lines_) : -1;
    int64_t step = forward ? 1 : -1;

    for (int64_t l = start_line; l != end_line; l += step) {
        auto it = history_cache_.find(static_cast<uint64_t>(l));
        if (it != history_cache_.end()) {
            std::string line_str;
            for (const auto& c : it->second.cells) {
                append_utf8_char(c.codepoint, line_str);
            }
            auto pos = line_str.find(search_query_);
            if (pos != std::string::npos) {
                cursor_.line = l;
                cursor_.col = static_cast<uint16_t>(pos);
                scroll_viewport_to_cursor();
                return;
            }
        }
    }
}

std::string CopyMode::get_selected_text() const {
    if (total_lines_ == 0) return "";

    CopyPosition start = selecting_ ? std::min(sel_start_, cursor_) : cursor_;
    CopyPosition end = selecting_ ? std::max(sel_start_, cursor_) : cursor_;

    if (line_selection_) {
        start.col = 0;
        end.col = view_cols_ > 0 ? static_cast<uint16_t>(view_cols_ - 1) : 0;
    }

    std::string result;
    for (int64_t l = start.line; l <= end.line; ++l) {
        auto it = history_cache_.find(static_cast<uint64_t>(l));
        if (it == history_cache_.end()) {
            continue;
        }

        const auto& line_cells = it->second.cells;
        uint16_t c_start = (l == start.line) ? start.col : 0;
        uint16_t c_end = (l == end.line) ? end.col : static_cast<uint16_t>(line_cells.size());

        std::string line_str;
        for (uint16_t c = c_start; c <= c_end && c < line_cells.size(); ++c) {
            append_utf8_char(line_cells[c].codepoint, line_str);
        }

        // Trim trailing spaces from line
        while (!line_str.empty() && line_str.back() == ' ') {
            line_str.pop_back();
        }

        result.append(line_str);
        if (l < end.line) {
            result.push_back('\n');
        }
    }

    return result;
}

std::string CopyMode::yank() {
    std::string text = get_selected_text();
    return Renderer::make_osc52(text, true);
}

void CopyMode::render_view(std::vector<protocol::Cell>& dst_cells, uint16_t width, uint16_t height) const {
    if (!active_ || width == 0 || height == 0) return;
    dst_cells.assign(static_cast<size_t>(width) * height, protocol::Cell{});

    CopyPosition sel_min = std::min(sel_start_, cursor_);
    CopyPosition sel_max = std::max(sel_start_, cursor_);
    if (line_selection_) {
        sel_min.col = 0;
        sel_max.col = width > 0 ? static_cast<uint16_t>(width - 1) : 0;
    }

    protocol::CellStyle sel_style;
    sel_style.bg = protocol::Color::rgb(60, 100, 180);
    sel_style.fg = protocol::Color::rgb(255, 255, 255);

    protocol::CellStyle cursor_style;
    cursor_style.flags = protocol::StyleFlags::Reverse | protocol::StyleFlags::Bold;

    // Render lines from history_cache_
    for (uint16_t r = 0; r < height; ++r) {
        int64_t line_idx = viewport_top_line_ + r;
        auto it = history_cache_.find(static_cast<uint64_t>(line_idx));

        for (uint16_t c = 0; c < width; ++c) {
            protocol::Cell cell{.codepoint = U' ', .width = 1};
            if (it != history_cache_.end() && c < it->second.cells.size()) {
                cell = it->second.cells[c];
            }

            CopyPosition cur_pos{line_idx, c};
            if (selecting_ && cur_pos >= sel_min && cur_pos <= sel_max) {
                cell.style = sel_style;
            }

            if (cur_pos.line == cursor_.line && cur_pos.col == cursor_.col) {
                cell.style = cursor_style;
            }

            dst_cells[static_cast<size_t>(r) * width + c] = cell;
        }
    }

    // Top-right banner: [Copy Mode: {line}/{total}]
    std::string banner = search_mode_
        ? std::format("[Search: {}_]", search_query_)
        : std::format("[Copy Mode: {}/{}]", cursor_.line + 1, total_lines_);

    if (banner.size() < width) {
        size_t start_c = width - banner.size();
        protocol::CellStyle banner_style;
        banner_style.bg = protocol::Color::rgb(200, 100, 0); // Orange
        banner_style.fg = protocol::Color::rgb(255, 255, 255);
        banner_style.flags = protocol::StyleFlags::Bold;

        for (size_t i = 0; i < banner.size(); ++i) {
            dst_cells[start_c + i] = protocol::Cell{
                .codepoint = static_cast<char32_t>(banner[i]),
                .width = 1,
                .style = banner_style
            };
        }
    }
}

} // namespace tmux2::client
