#include "src/client/renderer.hpp"

#include <algorithm>
#include <format>
#include <span>

namespace tmux2::client {

namespace {

void append_utf8(char32_t cp, std::string& out) {
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

Renderer::Renderer(uint16_t term_cols, uint16_t term_rows)
    : term_cols_(term_cols), term_rows_(term_rows) {
    resize(term_cols_, term_rows_);
}

void Renderer::resize(uint16_t cols, uint16_t rows) {
    term_cols_ = cols > 0 ? cols : 80;
    term_rows_ = rows > 0 ? rows : 24;
    composite_screen_.assign(static_cast<size_t>(term_cols_) * term_rows_, protocol::Cell{});
}

void Renderer::apply_snapshot(const protocol::MsgScreenSnapshot& snapshot) {
    auto& grid = pane_grids_[snapshot.pane_id];
    grid.cols = snapshot.cols;
    grid.rows = snapshot.rows;
    grid.cursor = snapshot.cursor;
    grid.modes = snapshot.modes;
    grid.cells = snapshot.cells;
}

void Renderer::apply_diff(const protocol::MsgFrameDiff& diff) {
    if (!diff.status_line.empty()) {
        custom_status_line_ = diff.status_line;
    }

    for (const auto& p_diff : diff.pane_diffs) {
        auto it = pane_grids_.find(p_diff.pane_id);
        if (it == pane_grids_.end()) {
            continue;
        }
        auto& grid = it->second;
        grid.cursor = p_diff.cursor;

        for (const auto& r_diff : p_diff.rows) {
            if (r_diff.row >= grid.rows) continue;
            size_t row_start = static_cast<size_t>(r_diff.row) * grid.cols;
            size_t col = r_diff.start_col;
            for (size_t i = 0; i < r_diff.cells.size() && (col + i) < grid.cols; ++i) {
                grid.cells[row_start + col + i] = r_diff.cells[i];
            }
        }
    }
}

void Renderer::apply_layout(const protocol::MsgLayoutUpdate& layout) {
    active_pane_id_ = layout.active_pane_id;
    zoomed_pane_id_ = layout.zoomed_pane_id;

    // If no explicit pane rects are configured, default to single pane filling screen (excluding status bar)
    if (pane_rects_.empty() && layout.active_pane_id != 0) {
        pane_rects_ = {
            PaneRect{
                .pane_id = layout.active_pane_id,
                .x = 0,
                .y = 0,
                .width = term_cols_,
                .height = static_cast<uint16_t>(term_rows_ > 1 ? term_rows_ - 1 : 1)
            }
        };
    }
}

void Renderer::set_pane_rects(std::vector<PaneRect> rects) {
    pane_rects_ = std::move(rects);
}

const PaneShadowGrid* Renderer::get_pane_grid(uint32_t pane_id) const {
    auto it = pane_grids_.find(pane_id);
    if (it != pane_grids_.end()) {
        return &it->second;
    }
    return nullptr;
}

PaneShadowGrid* Renderer::get_pane_grid(uint32_t pane_id) {
    auto it = pane_grids_.find(pane_id);
    if (it != pane_grids_.end()) {
        return &it->second;
    }
    return nullptr;
}

void Renderer::draw_pane_content(const PaneRect& rect, const PaneShadowGrid& grid) {
    for (uint16_t r = 0; r < rect.height; ++r) {
        uint16_t target_y = rect.y + r;
        if (target_y >= term_rows_) break;

        for (uint16_t c = 0; c < rect.width; ++c) {
            uint16_t target_x = rect.x + c;
            if (target_x >= term_cols_) break;

            const protocol::Cell* cell = grid.cell_at(c, r);
            if (cell) {
                composite_screen_[static_cast<size_t>(target_y) * term_cols_ + target_x] = *cell;
            } else {
                composite_screen_[static_cast<size_t>(target_y) * term_cols_ + target_x] = protocol::Cell{};
            }
        }
    }
}

void Renderer::draw_split_borders() {
    if (pane_rects_.size() <= 1) return;

    for (const auto& rect : pane_rects_) {
        bool is_active = (rect.pane_id == active_pane_id_);
        protocol::CellStyle border_style;
        if (is_active) {
            border_style.fg = protocol::Color::rgb(0, 255, 0); // Green highlight for active pane
            border_style.flags = protocol::StyleFlags::Bold;
        } else {
            border_style.fg = protocol::Color::indexed(240); // Dim gray for inactive
        }

        // Right vertical border (if not touching screen edge)
        uint16_t right_border_x = rect.x + rect.width;
        if (right_border_x < term_cols_) {
            uint16_t max_y = rect.y + rect.height;
            if (max_y < term_rows_ - 1) {
                max_y += 1; // Include corner intersection
            }
            for (uint16_t y = rect.y; y < max_y && y < term_rows_ - 1; ++y) {
                auto& cell = composite_screen_[static_cast<size_t>(y) * term_cols_ + right_border_x];
                if (cell.codepoint == U'─' || cell.codepoint == U'┼') {
                    cell.codepoint = U'┼';
                } else {
                    cell.codepoint = U'│'; // U+2502 Box Drawings Light Vertical
                }
                cell.width = 1;
                cell.style = border_style;
            }
        }

        // Bottom horizontal border (if not touching status bar)
        uint16_t bottom_border_y = rect.y + rect.height;
        if (bottom_border_y < term_rows_ - 1) {
            uint16_t max_x = rect.x + rect.width;
            if (max_x < term_cols_) {
                max_x += 1; // Include corner intersection
            }
            for (uint16_t x = rect.x; x < max_x && x < term_cols_; ++x) {
                auto& cell = composite_screen_[static_cast<size_t>(bottom_border_y) * term_cols_ + x];
                if (cell.codepoint == U'│' || cell.codepoint == U'┼') {
                    cell.codepoint = U'┼'; // Intersection
                } else {
                    cell.codepoint = U'─'; // U+2500 Box Drawings Light Horizontal
                }
                cell.width = 1;
                cell.style = border_style;
            }
        }
    }
}

void Renderer::draw_status_bar() {
    if (term_rows_ == 0) return;
    uint16_t status_row = term_rows_ - 1;
    size_t row_start = static_cast<size_t>(status_row) * term_cols_;

    protocol::CellStyle status_style;
    status_style.bg = protocol::Color::indexed(236); // Dark gray background
    status_style.fg = protocol::Color::rgb(220, 220, 220);

    protocol::CellStyle active_tab_style;
    active_tab_style.bg = protocol::Color::rgb(0, 120, 215); // Blue
    active_tab_style.fg = protocol::Color::rgb(255, 255, 255);
    active_tab_style.flags = protocol::StyleFlags::Bold;

    // Fill row with spaces with status_style
    for (uint16_t c = 0; c < term_cols_; ++c) {
        composite_screen_[row_start + c] = protocol::Cell{
            .codepoint = U' ',
            .width = 1,
            .style = status_style
        };
    }

    if (!custom_status_line_.empty()) {
        for (size_t i = 0; i < custom_status_line_.size() && i < term_cols_; ++i) {
            composite_screen_[row_start + i] = protocol::Cell{
                .codepoint = static_cast<char32_t>(custom_status_line_[i]),
                .width = 1,
                .style = status_style
            };
        }
        return;
    }

    // Build left side: [session_name]
    std::string left_text = std::format("[{}] ", session_name_);
    size_t col_idx = 0;
    for (char ch : left_text) {
        if (col_idx >= term_cols_) break;
        composite_screen_[row_start + col_idx++] = protocol::Cell{
            .codepoint = static_cast<char32_t>(ch),
            .width = 1,
            .style = status_style
        };
    }

    // Window tabs: e.g. "0:editor* 1:logs "
    for (const auto& tab : window_tabs_) {
        std::string tab_text = std::format("{}:{}{} ",
            tab.index,
            tab.name,
            tab.is_active ? "*" : (tab.is_last ? "-" : ""));

        const auto& style = tab.is_active ? active_tab_style : status_style;
        for (char ch : tab_text) {
            if (col_idx >= term_cols_) break;
            composite_screen_[row_start + col_idx++] = protocol::Cell{
                .codepoint = static_cast<char32_t>(ch),
                .width = 1,
                .style = style
            };
        }
    }

    // Indicators: [prefix], [zoom]
    if (prefix_active_ && col_idx + 8 < term_cols_) {
        protocol::CellStyle prefix_style = status_style;
        prefix_style.fg = protocol::Color::rgb(255, 255, 0); // Yellow
        prefix_style.flags = protocol::StyleFlags::Bold;
        std::string ind = "[prefix] ";
        for (char ch : ind) {
            if (col_idx >= term_cols_) break;
            composite_screen_[row_start + col_idx++] = protocol::Cell{
                .codepoint = static_cast<char32_t>(ch),
                .width = 1,
                .style = prefix_style
            };
        }
    }

    if (zoomed_pane_id_ != 0 && col_idx + 6 < term_cols_) {
        protocol::CellStyle zoom_style = status_style;
        zoom_style.fg = protocol::Color::rgb(255, 128, 0); // Orange
        std::string ind = "[Z] ";
        for (char ch : ind) {
            if (col_idx >= term_cols_) break;
            composite_screen_[row_start + col_idx++] = protocol::Cell{
                .codepoint = static_cast<char32_t>(ch),
                .width = 1,
                .style = zoom_style
            };
        }
    }

    // Right-aligned status banner (e.g. reconnect banner)
    std::string right_text = reconnect_banner_;
    if (!right_text.empty() && right_text.size() < term_cols_) {
        size_t start_r = term_cols_ - right_text.size();
        protocol::CellStyle banner_style = status_style;
        banner_style.bg = protocol::Color::rgb(180, 0, 0); // Red alert for reconnect
        banner_style.fg = protocol::Color::rgb(255, 255, 255);
        banner_style.flags = protocol::StyleFlags::Bold;

        for (size_t i = 0; i < right_text.size(); ++i) {
            composite_screen_[row_start + start_r + i] = protocol::Cell{
                .codepoint = static_cast<char32_t>(right_text[i]),
                .width = 1,
                .style = banner_style
            };
        }
    }
}

void Renderer::compose_frame() {
    // Clear composite screen with empty cells
    composite_screen_.assign(static_cast<size_t>(term_cols_) * term_rows_, protocol::Cell{});
    active_cursor_ = protocol::CursorState{};

    if (zoomed_pane_id_ != 0) {
        auto it = pane_grids_.find(zoomed_pane_id_);
        if (it != pane_grids_.end()) {
            PaneRect zoomed_rect{
                .pane_id = zoomed_pane_id_,
                .x = 0,
                .y = 0,
                .width = term_cols_,
                .height = static_cast<uint16_t>(term_rows_ > 1 ? term_rows_ - 1 : 1)
            };
            draw_pane_content(zoomed_rect, it->second);
            active_cursor_ = it->second.cursor;
        }
    } else {
        // Draw normal panes
        for (const auto& rect : pane_rects_) {
            auto it = pane_grids_.find(rect.pane_id);
            if (it != pane_grids_.end()) {
                draw_pane_content(rect, it->second);
                if (rect.pane_id == active_pane_id_) {
                    active_cursor_ = it->second.cursor;
                    active_cursor_.col += rect.x;
                    active_cursor_.row += rect.y;
                }
            }
        }
        draw_split_borders();
    }

    draw_status_bar();
}

void Renderer::format_cell_sgr(const protocol::CellStyle& style, std::string& out) const {
    out.append("\x1b[0"); // Reset

    if (style.flags & protocol::StyleFlags::Bold) out.append(";1");
    if (style.flags & protocol::StyleFlags::Dim) out.append(";2");
    if (style.flags & protocol::StyleFlags::Italic) out.append(";3");
    if (style.flags & protocol::StyleFlags::Underline) out.append(";4");
    if (style.flags & protocol::StyleFlags::Blink) out.append(";5");
    if (style.flags & protocol::StyleFlags::Reverse) out.append(";7");
    if (style.flags & protocol::StyleFlags::Strikethrough) out.append(";9");

    // Foreground
    if (style.fg.type == protocol::ColorType::Indexed) {
        out.append(std::format(";38;5;{}", style.fg.r_or_idx));
    } else if (style.fg.type == protocol::ColorType::Rgb) {
        out.append(std::format(";38;2;{};{};{}", style.fg.r_or_idx, style.fg.g, style.fg.b));
    }

    // Background
    if (style.bg.type == protocol::ColorType::Indexed) {
        out.append(std::format(";48;5;{}", style.bg.r_or_idx));
    } else if (style.bg.type == protocol::ColorType::Rgb) {
        out.append(std::format(";48;2;{};{};{}", style.bg.r_or_idx, style.bg.g, style.bg.b));
    }

    out.push_back('m');
}

std::string Renderer::render_frame_vt(bool use_decset_2026) const {
    std::string out;
    out.reserve(static_cast<size_t>(term_cols_) * term_rows_ * 8);

    if (use_decset_2026) {
        out.append("\x1b[?2026h"); // Synchronized update start
    }

    protocol::CellStyle current_style{};
    bool style_initialized = false;

    for (uint16_t r = 0; r < term_rows_; ++r) {
        // Position cursor at beginning of row: \x1b[{row};1H (1-based)
        out.append(std::format("\x1b[{};1H", r + 1));

        for (uint16_t c = 0; c < term_cols_; ++c) {
            const auto& cell = composite_screen_[static_cast<size_t>(r) * term_cols_ + c];

            if (!style_initialized || !(cell.style == current_style)) {
                format_cell_sgr(cell.style, out);
                current_style = cell.style;
                style_initialized = true;
            }

            append_utf8(cell.codepoint, out);
        }
    }

    // Restore default attributes
    out.append("\x1b[0m");

    // Position cursor at active cursor location
    if (active_cursor_.visible && active_cursor_.row < term_rows_ && active_cursor_.col < term_cols_) {
        out.append(std::format("\x1b[{};{}H\x1b[?25h", active_cursor_.row + 1, active_cursor_.col + 1));
    } else {
        out.append("\x1b[?25l"); // Hide cursor
    }

    if (use_decset_2026) {
        out.append("\x1b[?2026l"); // Synchronized update end
    }

    return out;
}

const protocol::Cell& Renderer::get_composite_cell(uint16_t col, uint16_t row) const {
    static const protocol::Cell empty_cell{};
    if (col < term_cols_ && row < term_rows_) {
        return composite_screen_[static_cast<size_t>(row) * term_cols_ + col];
    }
    return empty_cell;
}

void Renderer::set_composite_cell(uint16_t col, uint16_t row, const protocol::Cell& cell) {
    if (col < term_cols_ && row < term_rows_) {
        composite_screen_[static_cast<size_t>(row) * term_cols_ + col] = cell;
    }
}

std::string Renderer::get_row_text(uint16_t row) const {
    std::string text;
    if (row >= term_rows_) return text;
    for (uint16_t c = 0; c < term_cols_; ++c) {
        char32_t cp = composite_screen_[static_cast<size_t>(row) * term_cols_ + c].codepoint;
        append_utf8(cp == 0 ? U' ' : cp, text);
    }
    return text;
}

protocol::CursorState Renderer::get_active_cursor() const {
    return active_cursor_;
}

// Base64 lookup table
static constexpr char kB64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Renderer::base64_encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i < input.size()) {
        size_t rem = input.size() - i;
        uint32_t b0 = static_cast<uint8_t>(input[i++]);
        uint32_t b1 = (rem > 1) ? static_cast<uint8_t>(input[i++]) : 0;
        uint32_t b2 = (rem > 2) ? static_cast<uint8_t>(input[i++]) : 0;

        uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(kB64Chars[(triple >> 18) & 0x3F]);
        out.push_back(kB64Chars[(triple >> 12) & 0x3F]);
        out.push_back(rem > 1 ? kB64Chars[(triple >> 6) & 0x3F] : '=');
        out.push_back(rem > 2 ? kB64Chars[triple & 0x3F] : '=');
    }

    return out;
}

std::string Renderer::base64_decode(std::string_view input) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i) T[static_cast<unsigned char>(kB64Chars[i])] = i;

    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break; // End of valid data on padding
        if (T[c] == -1) continue; // Skip whitespace/invalid chars
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string Renderer::make_osc52(std::string_view text, bool clipboard) {
    std::string b64 = base64_encode(text);
    char target = clipboard ? 'c' : 'p';
    return std::format("\x1b]52;{};{}\x07", target, b64);
}

std::string Renderer::handle_passthrough(const protocol::MsgPassthroughEvent& ev) {
    switch (ev.kind) {
        case 1: { // OSC 52 Clipboard
            std::string_view payload(reinterpret_cast<const char*>(ev.payload.data()), ev.payload.size());
            if (payload.starts_with("\x1b]52;")) {
                return std::string(payload);
            }
            return make_osc52(payload);
        }
        case 2: // Bell
            return "\x07";
        case 3: // Desktop Notification
        case 4: // Hyperlink
        default:
            return std::string(reinterpret_cast<const char*>(ev.payload.data()), ev.payload.size());
    }
}

} // namespace tmux2::client
