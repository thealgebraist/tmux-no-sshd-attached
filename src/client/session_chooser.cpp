#include "src/client/session_chooser.hpp"

#include <algorithm>
#include <format>

namespace tmux2::client {

void SessionChooser::open(std::vector<protocol::SessionSummary> sessions) {
    sessions_ = std::move(sessions);
    selected_index_ = 0;
    filter_.clear();
    active_ = true;
}

void SessionChooser::close() {
    active_ = false;
    filter_.clear();
}

std::vector<protocol::SessionSummary> SessionChooser::filtered_sessions() const {
    if (filter_.empty()) {
        return sessions_;
    }
    std::vector<protocol::SessionSummary> res;
    for (const auto& s : sessions_) {
        if (s.name.find(filter_) != std::string::npos ||
            s.preview_summary.find(filter_) != std::string::npos) {
            res.push_back(s);
        }
    }
    return res;
}

void SessionChooser::clamp_selected_index() {
    auto list = filtered_sessions();
    if (list.empty()) {
        selected_index_ = 0;
    } else if (selected_index_ >= list.size()) {
        selected_index_ = list.size() - 1;
    }
}

std::optional<protocol::MsgSwitchSessionReq> SessionChooser::handle_key(const ClientKeyEvent& key) {
    if (!active_) return std::nullopt;

    auto list = filtered_sessions();

    if (key.key_code == SpecialKey::Escape ||
        (key.key_code == 'c' && (key.modifiers & KeyModifiers::Ctrl)) ||
        (key.key_code == 'q' && key.modifiers == KeyModifiers::None && filter_.empty())) {
        close();
        return std::nullopt;
    }

    if (key.key_code == SpecialKey::Down || (key.key_code == 'j' && key.modifiers == KeyModifiers::None && filter_.empty())) {
        if (!list.empty()) {
            selected_index_ = (selected_index_ + 1) % list.size();
        }
        return std::nullopt;
    }

    if (key.key_code == SpecialKey::Up || (key.key_code == 'k' && key.modifiers == KeyModifiers::None && filter_.empty())) {
        if (!list.empty()) {
            selected_index_ = (selected_index_ == 0) ? list.size() - 1 : selected_index_ - 1;
        }
        return std::nullopt;
    }

    if (key.key_code == SpecialKey::Enter) {
        if (!list.empty() && selected_index_ < list.size()) {
            const auto& chosen = list[selected_index_];
            protocol::MsgSwitchSessionReq req{
                .session_id = chosen.session_id,
                .session_name = chosen.name
            };
            close();
            return req;
        }
        close();
        return std::nullopt;
    }

    if (key.key_code == SpecialKey::Backspace) {
        if (!filter_.empty()) {
            filter_.pop_back();
            clamp_selected_index();
        }
        return std::nullopt;
    }

    // Filter characters
    if ((key.modifiers == KeyModifiers::None || key.modifiers == KeyModifiers::Shift) &&
        key.key_code >= 32 && key.key_code <= 126) {
        filter_.push_back(static_cast<char>(key.key_code));
        clamp_selected_index();
        return std::nullopt;
    }

    return std::nullopt;
}

void SessionChooser::render_overlay(std::vector<protocol::Cell>& screen, uint16_t term_cols, uint16_t term_rows) const {
    if (!active_ || term_cols < 20 || term_rows < 10) return;

    uint16_t box_w = std::min<uint16_t>(term_cols - 4, 60);
    uint16_t box_h = std::min<uint16_t>(term_rows - 4, 16);
    uint16_t start_x = (term_cols - box_w) / 2;
    uint16_t start_y = (term_rows - box_h) / 2;

    protocol::CellStyle box_style;
    box_style.fg = protocol::Color::rgb(200, 200, 255);
    box_style.bg = protocol::Color::rgb(20, 20, 30);

    protocol::CellStyle border_style = box_style;
    border_style.fg = protocol::Color::rgb(100, 150, 255);
    border_style.flags = protocol::StyleFlags::Bold;

    protocol::CellStyle selected_style;
    selected_style.fg = protocol::Color::rgb(255, 255, 255);
    selected_style.bg = protocol::Color::rgb(40, 80, 160);
    selected_style.flags = protocol::StyleFlags::Bold;

    // Fill box background
    for (uint16_t y = 0; y < box_h; ++y) {
        for (uint16_t x = 0; x < box_w; ++x) {
            size_t idx = static_cast<size_t>(start_y + y) * term_cols + (start_x + x);
            if (idx < screen.size()) {
                screen[idx] = protocol::Cell{
                    .codepoint = U' ',
                    .width = 1,
                    .style = box_style
                };
            }
        }
    }

    // Draw borders
    for (uint16_t x = 0; x < box_w; ++x) {
        size_t top_idx = static_cast<size_t>(start_y) * term_cols + (start_x + x);
        size_t bot_idx = static_cast<size_t>(start_y + box_h - 1) * term_cols + (start_x + x);
        if (top_idx < screen.size()) screen[top_idx].codepoint = U'─', screen[top_idx].style = border_style;
        if (bot_idx < screen.size()) screen[bot_idx].codepoint = U'─', screen[bot_idx].style = border_style;
    }
    for (uint16_t y = 0; y < box_h; ++y) {
        size_t left_idx = static_cast<size_t>(start_y + y) * term_cols + start_x;
        size_t right_idx = static_cast<size_t>(start_y + y) * term_cols + (start_x + box_w - 1);
        if (left_idx < screen.size()) screen[left_idx].codepoint = U'│', screen[left_idx].style = border_style;
        if (right_idx < screen.size()) screen[right_idx].codepoint = U'│', screen[right_idx].style = border_style;
    }
    // Corners
    screen[static_cast<size_t>(start_y) * term_cols + start_x].codepoint = U'┌';
    screen[static_cast<size_t>(start_y) * term_cols + (start_x + box_w - 1)].codepoint = U'┐';
    screen[static_cast<size_t>(start_y + box_h - 1) * term_cols + start_x].codepoint = U'└';
    screen[static_cast<size_t>(start_y + box_h - 1) * term_cols + (start_x + box_w - 1)].codepoint = U'┘';

    // Header Title
    std::string title = " [ Sessions (Enter: switch, q: cancel) ] ";
    if (title.size() < box_w) {
        size_t tx = start_x + (box_w - title.size()) / 2;
        for (size_t i = 0; i < title.size(); ++i) {
            size_t idx = static_cast<size_t>(start_y) * term_cols + tx + i;
            if (idx < screen.size()) {
                screen[idx] = protocol::Cell{
                    .codepoint = static_cast<char32_t>(title[i]),
                    .width = 1,
                    .style = border_style
                };
            }
        }
    }

    // Filter display if any
    uint16_t row_cur = start_y + 1;
    if (!filter_.empty() && row_cur < start_y + box_h - 1) {
        std::string filter_line = std::format(" Filter: {}_", filter_);
        for (size_t i = 0; i < filter_line.size() && (start_x + 1 + i) < (start_x + box_w - 1); ++i) {
            size_t idx = static_cast<size_t>(row_cur) * term_cols + (start_x + 1 + i);
            if (idx < screen.size()) {
                screen[idx] = protocol::Cell{
                    .codepoint = static_cast<char32_t>(filter_line[i]),
                    .width = 1,
                    .style = border_style
                };
            }
        }
        row_cur++;
    }

    // List sessions
    auto list = filtered_sessions();
    size_t max_items = (start_y + box_h - 2) - row_cur;

    for (size_t i = 0; i < list.size() && i < max_items; ++i) {
        const auto& s = list[i];
        bool is_sel = (i == selected_index_);
        const auto& style = is_sel ? selected_style : box_style;

        std::string line = std::format(" {} {} ({} win, {} att) - {}",
            is_sel ? ">" : " ",
            s.name,
            s.window_count,
            s.attached_clients,
            s.preview_summary.empty() ? "active" : s.preview_summary);

        uint16_t line_y = row_cur + i;
        for (uint16_t col = 1; col < box_w - 1; ++col) {
            char32_t cp = (col - 1 < line.size()) ? static_cast<char32_t>(line[col - 1]) : U' ';
            size_t idx = static_cast<size_t>(line_y) * term_cols + (start_x + col);
            if (idx < screen.size()) {
                screen[idx] = protocol::Cell{
                    .codepoint = cp,
                    .width = 1,
                    .style = style
                };
            }
        }
    }
}

} // namespace tmux2::client
