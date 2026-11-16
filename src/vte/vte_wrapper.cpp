#include "src/vte/vte_wrapper.hpp"

#include <vterm.h>
#include <algorithm>
#include <utility>

namespace tmux2::vte {

namespace {

extern "C" {

static int vte_damage_cb(VTermRect rect, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    self->on_damage(rect.start_row, rect.end_row, rect.start_col, rect.end_col);
    return 1;
}

static int vte_moverect_cb(VTermRect dest, VTermRect src, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    self->on_moverect(dest.start_row, dest.end_row, src.start_row, src.end_row);
    return 1;
}

static int vte_movecursor_cb(VTermPos pos, VTermPos oldpos, int visible, void* user) {
    (void)oldpos;
    auto* self = static_cast<VteScreen*>(user);
    self->on_movecursor(pos.row, pos.col, visible != 0);
    return 1;
}

static int vte_settermprop_cb(VTermProp prop, VTermValue* val, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    self->on_settermprop(static_cast<int>(prop), val);
    return 1;
}

static int vte_bell_cb(void* user) {
    auto* self = static_cast<VteScreen*>(user);
    self->on_bell();
    return 1;
}

static int vte_resize_cb(int rows, int cols, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    self->on_resize(rows, cols);
    return 1;
}

static int vte_sb_pushline_cb(int cols, const VTermScreenCell* cells, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    return self->on_sb_pushline(cols, cells);
}

static int vte_sb_popline_cb(int cols, VTermScreenCell* cells, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    return self->on_sb_popline(cols, cells);
}

static int vte_sb_clear_cb(void* user) {
    auto* self = static_cast<VteScreen*>(user);
    self->on_sb_clear();
    return 1;
}

static int vte_osc_cb(int command, VTermStringFragment frag, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    std::string_view s(frag.str ? frag.str : "", frag.len);
    self->on_osc(command, s, frag.initial != 0, frag.final != 0);
    return 1;
}

static int vte_selection_set_cb(VTermSelectionMask mask, VTermStringFragment frag, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    std::string_view s(frag.str ? frag.str : "", frag.len);
    return self->on_selection_set(static_cast<int>(mask), s, frag.initial != 0, frag.final != 0);
}

static int vte_selection_query_cb(VTermSelectionMask mask, void* user) {
    (void)mask;
    (void)user;
    return 0;
}

static void vte_output_cb(const char* s, size_t len, void* user) {
    auto* self = static_cast<VteScreen*>(user);
    self->on_output(s, len);
}

} // extern "C"

static const VTermScreenCallbacks kScreenCallbacks = {
    .damage = vte_damage_cb,
    .moverect = vte_moverect_cb,
    .movecursor = vte_movecursor_cb,
    .settermprop = vte_settermprop_cb,
    .bell = vte_bell_cb,
    .resize = vte_resize_cb,
    .sb_pushline = vte_sb_pushline_cb,
    .sb_popline = vte_sb_popline_cb,
    .sb_clear = vte_sb_clear_cb,
};

static const VTermStateFallbacks kStateFallbacks = {
    .control = nullptr,
    .csi = nullptr,
    .osc = vte_osc_cb,
    .dcs = nullptr,
    .apc = nullptr,
    .pm = nullptr,
    .sos = nullptr,
};

static const VTermSelectionCallbacks kSelectionCallbacks = {
    .set = vte_selection_set_cb,
    .query = vte_selection_query_cb,
};

std::string base64_encode(std::string_view in) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    uint32_t val = 0;
    int valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (out.size() % 4) {
        out.push_back('=');
    }
    return out;
}

void append_utf8(std::string& out, char32_t cp) {
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

VteScreen::VteScreen(uint16_t cols, uint16_t rows, size_t scrollback_max_lines)
    : cols_(cols > 0 ? cols : 80),
      rows_(rows > 0 ? rows : 24),
      dirty_tracker_(rows_),
      scrollback_(scrollback_max_lines),
      cached_frame_cells_(rows_, std::vector<protocol::Cell>(cols_)) {
    cursor_.col = 0;
    cursor_.row = 0;
    cursor_.visible = true;
    cursor_.shape = 0;
    init_vterm();
}

VteScreen::~VteScreen() {
    cleanup_vterm();
}

VteScreen::VteScreen(VteScreen&& other) noexcept
    : cols_(other.cols_),
      rows_(other.rows_),
      vt_(std::exchange(other.vt_, nullptr)),
      screen_(std::exchange(other.screen_, nullptr)),
      state_(std::exchange(other.state_, nullptr)),
      dirty_tracker_(std::move(other.dirty_tracker_)),
      scrollback_(std::move(other.scrollback_)),
      cursor_(other.cursor_),
      cursor_blink_(other.cursor_blink_),
      is_altscreen_(other.is_altscreen_),
      mouse_mode_(other.mouse_mode_),
      focus_reporting_(other.focus_reporting_),
      title_(std::move(other.title_)),
      cached_frame_cells_(std::move(other.cached_frame_cells_)),
      osc_buffer_(std::move(other.osc_buffer_)),
      active_osc_command_(other.active_osc_command_),
      selection_buffer_(std::move(other.selection_buffer_)),
      selection_accum_(std::move(other.selection_accum_)),
      active_selection_mask_(other.active_selection_mask_),
      bell_callback_(std::move(other.bell_callback_)),
      title_callback_(std::move(other.title_callback_)),
      osc52_callback_(std::move(other.osc52_callback_)),
      output_callback_(std::move(other.output_callback_)) {
    if (screen_) {
        vterm_screen_set_callbacks(screen_, &kScreenCallbacks, this);
        vterm_screen_set_unrecognised_fallbacks(screen_, &kStateFallbacks, this);
    }
    if (state_) {
        vterm_state_set_selection_callbacks(state_, &kSelectionCallbacks, this,
                                            selection_buffer_.data(), selection_buffer_.size());
    }
    if (vt_) {
        vterm_output_set_callback(vt_, vte_output_cb, this);
    }
}

VteScreen& VteScreen::operator=(VteScreen&& other) noexcept {
    if (this != &other) {
        cleanup_vterm();

        cols_ = other.cols_;
        rows_ = other.rows_;
        vt_ = std::exchange(other.vt_, nullptr);
        screen_ = std::exchange(other.screen_, nullptr);
        state_ = std::exchange(other.state_, nullptr);
        dirty_tracker_ = std::move(other.dirty_tracker_);
        scrollback_ = std::move(other.scrollback_);
        cursor_ = other.cursor_;
        cursor_blink_ = other.cursor_blink_;
        is_altscreen_ = other.is_altscreen_;
        mouse_mode_ = other.mouse_mode_;
        focus_reporting_ = other.focus_reporting_;
        title_ = std::move(other.title_);
        cached_frame_cells_ = std::move(other.cached_frame_cells_);
        osc_buffer_ = std::move(other.osc_buffer_);
        active_osc_command_ = other.active_osc_command_;
        selection_buffer_ = std::move(other.selection_buffer_);
        selection_accum_ = std::move(other.selection_accum_);
        active_selection_mask_ = other.active_selection_mask_;
        bell_callback_ = std::move(other.bell_callback_);
        title_callback_ = std::move(other.title_callback_);
        osc52_callback_ = std::move(other.osc52_callback_);
        output_callback_ = std::move(other.output_callback_);

        if (screen_) {
            vterm_screen_set_callbacks(screen_, &kScreenCallbacks, this);
            vterm_screen_set_unrecognised_fallbacks(screen_, &kStateFallbacks, this);
        }
        if (state_) {
            vterm_state_set_selection_callbacks(state_, &kSelectionCallbacks, this,
                                                selection_buffer_.data(), selection_buffer_.size());
        }
        if (vt_) {
            vterm_output_set_callback(vt_, vte_output_cb, this);
        }
    }
    return *this;
}

void VteScreen::init_vterm() {
    vt_ = vterm_new(rows_, cols_);
    if (!vt_) return;

    vterm_set_utf8(vt_, 1);
    screen_ = vterm_obtain_screen(vt_);
    state_ = vterm_obtain_state(vt_);

    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_ROW);

    vterm_screen_set_callbacks(screen_, &kScreenCallbacks, this);
    vterm_screen_set_unrecognised_fallbacks(screen_, &kStateFallbacks, this);
    selection_buffer_.resize(65536);
    vterm_state_set_selection_callbacks(state_, &kSelectionCallbacks, this,
                                        selection_buffer_.data(), selection_buffer_.size());
    vterm_output_set_callback(vt_, vte_output_cb, this);

    vterm_screen_reset(screen_, 1);
}

void VteScreen::cleanup_vterm() noexcept {
    if (vt_) {
        vterm_free(vt_);
        vt_ = nullptr;
        screen_ = nullptr;
        state_ = nullptr;
    }
}

size_t VteScreen::feed(std::string_view str) {
    if (!vt_ || str.empty()) return 0;
    return vterm_input_write(vt_, str.data(), str.size());
}

size_t VteScreen::feed(std::span<const std::byte> bytes) {
    return feed(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void VteScreen::resize(uint16_t cols, uint16_t rows) {
    if (cols == 0 || rows == 0) return;
    cols_ = cols;
    rows_ = rows;
    if (vt_) {
        vterm_set_size(vt_, rows_, cols_);
    }
    dirty_tracker_.resize(rows_);
    cached_frame_cells_.assign(rows_, std::vector<protocol::Cell>(cols_));
}

void VteScreen::flush_damage() {
    if (screen_) {
        vterm_screen_flush_damage(screen_);
    }
}

protocol::Cell VteScreen::convert_cell(const VTermScreenCell& sc) const {
    char32_t cp = sc.chars[0] != 0 ? sc.chars[0] : U' ';
    uint8_t w = static_cast<uint8_t>(sc.width > 0 ? sc.width : 0);

    uint16_t flags = protocol::StyleFlags::None;
    if (sc.attrs.bold) flags |= protocol::StyleFlags::Bold;
    if (sc.attrs.italic) flags |= protocol::StyleFlags::Italic;
    if (sc.attrs.underline) flags |= protocol::StyleFlags::Underline;
    if (sc.attrs.blink) flags |= protocol::StyleFlags::Blink;
    if (sc.attrs.reverse) flags |= protocol::StyleFlags::Reverse;
    if (sc.attrs.conceal) flags |= protocol::StyleFlags::Hidden;
    if (sc.attrs.strike) flags |= protocol::StyleFlags::Strikethrough;

    protocol::Color fg = protocol::Color::default_color();
    if (VTERM_COLOR_IS_DEFAULT_FG(&sc.fg)) {
        fg = protocol::Color::default_color();
    } else if (VTERM_COLOR_IS_INDEXED(&sc.fg)) {
        fg = protocol::Color::indexed(sc.fg.indexed.idx);
    } else if (VTERM_COLOR_IS_RGB(&sc.fg)) {
        fg = protocol::Color::rgb(sc.fg.rgb.red, sc.fg.rgb.green, sc.fg.rgb.blue);
    }

    protocol::Color bg = protocol::Color::default_color();
    if (VTERM_COLOR_IS_DEFAULT_BG(&sc.bg)) {
        bg = protocol::Color::default_color();
    } else if (VTERM_COLOR_IS_INDEXED(&sc.bg)) {
        bg = protocol::Color::indexed(sc.bg.indexed.idx);
    } else if (VTERM_COLOR_IS_RGB(&sc.bg)) {
        bg = protocol::Color::rgb(sc.bg.rgb.red, sc.bg.rgb.green, sc.bg.rgb.blue);
    }

    protocol::CellStyle style{
        .flags = flags,
        .fg = fg,
        .bg = bg
    };

    return protocol::Cell{
        .codepoint = cp,
        .width = w,
        .style = style
    };
}

protocol::Cell VteScreen::get_cell(uint16_t col, uint16_t row) const {
    if (!screen_ || col >= cols_ || row >= rows_) {
        return protocol::Cell{};
    }

    VTermPos pos{
        .row = static_cast<int>(row),
        .col = static_cast<int>(col)
    };
    VTermScreenCell sc{};
    if (vterm_screen_get_cell(screen_, pos, &sc) != 0) {
        return convert_cell(sc);
    }
    return protocol::Cell{};
}

std::vector<protocol::Cell> VteScreen::get_row(uint16_t row) const {
    std::vector<protocol::Cell> row_cells;
    if (row >= rows_) return row_cells;

    row_cells.reserve(cols_);
    for (uint16_t c = 0; c < cols_; ++c) {
        row_cells.push_back(get_cell(c, row));
    }
    return row_cells;
}

std::string VteScreen::get_text(uint16_t start_col, uint16_t start_row, uint16_t end_col, uint16_t end_row) const {
    std::string text;
    for (uint16_t r = start_row; r <= end_row && r < rows_; ++r) {
        uint16_t c_start = (r == start_row) ? start_col : 0;
        uint16_t c_end = (r == end_row) ? end_col : cols_ - 1;
        for (uint16_t c = c_start; c <= c_end && c < cols_; ++c) {
            auto cell = get_cell(c, r);
            if (cell.width > 0) {
                append_utf8(text, cell.codepoint);
            }
        }
        if (r < end_row) {
            text.push_back('\n');
        }
    }
    return text;
}

protocol::MsgScreenSnapshot VteScreen::get_snapshot(uint64_t server_seq, uint32_t pane_id) const {
    protocol::MsgScreenSnapshot snap;
    snap.server_seq = server_seq;
    snap.pane_id = pane_id;
    snap.cols = cols_;
    snap.rows = rows_;
    snap.cursor = cursor_;
    snap.modes = 0;
    if (is_altscreen_) snap.modes |= 0x01;
    if (focus_reporting_) snap.modes |= 0x02;

    snap.cells.reserve(cols_ * rows_);
    for (uint16_t r = 0; r < rows_; ++r) {
        for (uint16_t c = 0; c < cols_; ++c) {
            snap.cells.push_back(get_cell(c, r));
        }
    }

    return snap;
}

std::vector<protocol::CellRowDiff> VteScreen::get_dirty_diffs() {
    flush_damage();
    std::vector<protocol::CellRowDiff> diffs;

    auto dirty_list = dirty_tracker_.dirty_rows();
    if (dirty_list.empty()) {
        return diffs;
    }

    if (cached_frame_cells_.size() != rows_) {
        cached_frame_cells_.assign(rows_, std::vector<protocol::Cell>(cols_));
    }

    for (uint16_t r : dirty_list) {
        if (r >= rows_) continue;
        auto current_row = get_row(r);
        auto& cached_row = cached_frame_cells_[r];
        if (cached_row.size() != cols_) {
            cached_row.assign(cols_, protocol::Cell{});
        }

        // Find first differing column
        uint16_t first_diff = cols_;
        for (uint16_t c = 0; c < cols_; ++c) {
            if (current_row[c] != cached_row[c]) {
                first_diff = c;
                break;
            }
        }

        if (first_diff == cols_) {
            // Row has no visible cell change
            continue;
        }

        // Find last differing column
        uint16_t last_diff = first_diff;
        for (int c = static_cast<int>(cols_) - 1; c >= static_cast<int>(first_diff); --c) {
            if (current_row[c] != cached_row[c]) {
                last_diff = static_cast<uint16_t>(c);
                break;
            }
        }

        uint16_t count = (last_diff - first_diff) + 1;
        protocol::CellRowDiff row_diff;
        row_diff.row = r;
        row_diff.start_col = first_diff;
        row_diff.cells.assign(current_row.begin() + first_diff, current_row.begin() + first_diff + count);

        cached_row = std::move(current_row);
        diffs.push_back(std::move(row_diff));
    }

    dirty_tracker_.clear();
    return diffs;
}

void VteScreen::mouse_move(int col, int row, int modifiers) {
    if (vt_) {
        vterm_mouse_move(vt_, row, col, static_cast<VTermModifier>(modifiers));
    }
}

void VteScreen::mouse_button(int button, bool pressed, int modifiers) {
    if (vt_) {
        vterm_mouse_button(vt_, button, pressed, static_cast<VTermModifier>(modifiers));
    }
}

void VteScreen::keyboard_unichar(char32_t c, int modifiers) {
    if (vt_) {
        vterm_keyboard_unichar(vt_, static_cast<uint32_t>(c), static_cast<VTermModifier>(modifiers));
    }
}

void VteScreen::keyboard_key(int key, int modifiers) {
    if (vt_) {
        vterm_keyboard_key(vt_, static_cast<VTermKey>(key), static_cast<VTermModifier>(modifiers));
    }
}

void VteScreen::on_damage(int start_row, int end_row, int start_col, int end_col) {
    (void)start_col;
    (void)end_col;
    dirty_tracker_.mark_range(static_cast<uint16_t>(start_row), static_cast<uint16_t>(end_row));
}

void VteScreen::on_moverect(int dest_start_row, int dest_end_row, int src_start_row, int src_end_row) {
    (void)src_start_row;
    (void)src_end_row;
    dirty_tracker_.mark_range(static_cast<uint16_t>(dest_start_row), static_cast<uint16_t>(dest_end_row));
}

void VteScreen::on_movecursor(int row, int col, bool visible) {
    cursor_.row = static_cast<uint16_t>(row >= 0 ? row : 0);
    cursor_.col = static_cast<uint16_t>(col >= 0 ? col : 0);
    cursor_.visible = visible;
}

void VteScreen::on_settermprop(int prop, const void* val) {
    const auto* v = static_cast<const VTermValue*>(val);
    switch (static_cast<VTermProp>(prop)) {
        case VTERM_PROP_CURSORVISIBLE:
            cursor_.visible = (v->boolean != 0);
            break;
        case VTERM_PROP_CURSORBLINK:
            cursor_blink_ = (v->boolean != 0);
            break;
        case VTERM_PROP_ALTSCREEN:
            is_altscreen_ = (v->boolean != 0);
            dirty_tracker_.mark_all();
            break;
        case VTERM_PROP_CURSORSHAPE:
            if (v->number == VTERM_PROP_CURSORSHAPE_BLOCK) {
                cursor_.shape = 0;
            } else if (v->number == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
                cursor_.shape = 1;
            } else if (v->number == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
                cursor_.shape = 2;
            }
            break;
        case VTERM_PROP_MOUSE:
            mouse_mode_ = v->number;
            break;
        case VTERM_PROP_FOCUSREPORT:
            focus_reporting_ = (v->boolean != 0);
            break;
        case VTERM_PROP_TITLE:
            if (v->string.str) {
                title_.assign(v->string.str, v->string.len);
                if (title_callback_) {
                    title_callback_(title_);
                }
            }
            break;
        default:
            break;
    }
}

void VteScreen::on_bell() {
    if (bell_callback_) {
        bell_callback_();
    }
}

void VteScreen::on_resize(int rows, int cols) {
    cols_ = static_cast<uint16_t>(cols > 0 ? cols : 80);
    rows_ = static_cast<uint16_t>(rows > 0 ? rows : 24);
    dirty_tracker_.resize(rows_);
    cached_frame_cells_.assign(rows_, std::vector<protocol::Cell>(cols_));
}

int VteScreen::on_sb_pushline(int cols, const VTermScreenCell* cells) {
    protocol::ScrollbackLine line;
    line.wrapped = false;
    line.cells.reserve(cols);
    for (int i = 0; i < cols; ++i) {
        line.cells.push_back(convert_cell(cells[i]));
    }
    scrollback_.push_line(std::move(line));
    return 1;
}

int VteScreen::on_sb_popline(int cols, VTermScreenCell* cells) {
    auto popped = scrollback_.pop_line();
    if (!popped) {
        return 0;
    }

    for (int i = 0; i < cols; ++i) {
        if (i < static_cast<int>(popped->cells.size())) {
            const auto& src = popped->cells[i];
            cells[i].chars[0] = src.codepoint;
            cells[i].chars[1] = 0;
            cells[i].width = src.width;
            cells[i].attrs.bold = (src.style.flags & protocol::StyleFlags::Bold) != 0;
            cells[i].attrs.italic = (src.style.flags & protocol::StyleFlags::Italic) != 0;
            cells[i].attrs.underline = (src.style.flags & protocol::StyleFlags::Underline) != 0 ? 1 : 0;
            cells[i].attrs.blink = (src.style.flags & protocol::StyleFlags::Blink) != 0;
            cells[i].attrs.reverse = (src.style.flags & protocol::StyleFlags::Reverse) != 0;
            cells[i].attrs.conceal = (src.style.flags & protocol::StyleFlags::Hidden) != 0;
            cells[i].attrs.strike = (src.style.flags & protocol::StyleFlags::Strikethrough) != 0;

            if (src.style.fg.type == protocol::ColorType::Indexed) {
                cells[i].fg.type = VTERM_COLOR_INDEXED;
                cells[i].fg.indexed.idx = src.style.fg.r_or_idx;
            } else if (src.style.fg.type == protocol::ColorType::Rgb) {
                cells[i].fg.type = VTERM_COLOR_RGB;
                cells[i].fg.rgb.red = src.style.fg.r_or_idx;
                cells[i].fg.rgb.green = src.style.fg.g;
                cells[i].fg.rgb.blue = src.style.fg.b;
            } else {
                cells[i].fg.type = VTERM_COLOR_DEFAULT_FG;
            }

            if (src.style.bg.type == protocol::ColorType::Indexed) {
                cells[i].bg.type = VTERM_COLOR_INDEXED;
                cells[i].bg.indexed.idx = src.style.bg.r_or_idx;
            } else if (src.style.bg.type == protocol::ColorType::Rgb) {
                cells[i].bg.type = VTERM_COLOR_RGB;
                cells[i].bg.rgb.red = src.style.bg.r_or_idx;
                cells[i].bg.rgb.green = src.style.bg.g;
                cells[i].bg.rgb.blue = src.style.bg.b;
            } else {
                cells[i].bg.type = VTERM_COLOR_DEFAULT_BG;
            }
        } else {
            cells[i].chars[0] = ' ';
            cells[i].chars[1] = 0;
            cells[i].width = 1;
            cells[i].attrs = {};
            cells[i].fg.type = VTERM_COLOR_DEFAULT_FG;
            cells[i].bg.type = VTERM_COLOR_DEFAULT_BG;
        }
    }
    return 1;
}

void VteScreen::on_sb_clear() {
    scrollback_.clear();
}

void VteScreen::on_osc(int command, std::string_view frag_str, bool initial, bool final_frag) {
    if (initial) {
        osc_buffer_.clear();
        active_osc_command_ = command;
    }

    osc_buffer_.append(frag_str);

    if (final_frag) {
        if (active_osc_command_ == 52) {
            auto semi = osc_buffer_.find(';');
            if (semi != std::string::npos) {
                std::string_view selection(osc_buffer_.data(), semi);
                std::string_view data(osc_buffer_.data() + semi + 1, osc_buffer_.size() - semi - 1);
                if (osc52_callback_) {
                    osc52_callback_(selection, data);
                }
            } else {
                if (osc52_callback_) {
                    osc52_callback_("c", osc_buffer_);
                }
            }
        }
        osc_buffer_.clear();
        active_osc_command_ = -1;
    }
}

int VteScreen::on_selection_set(int mask, std::string_view frag_str, bool initial, bool final_frag) {
    if (initial) {
        selection_accum_.clear();
        active_selection_mask_ = mask;
    }
    selection_accum_.append(frag_str);
    if (final_frag) {
        std::string_view sel = "c";
        if (active_selection_mask_ & VTERM_SELECTION_PRIMARY) {
            sel = "p";
        } else if (active_selection_mask_ & VTERM_SELECTION_SECONDARY) {
            sel = "q";
        } else if (active_selection_mask_ & VTERM_SELECTION_SELECT) {
            sel = "s";
        } else if (active_selection_mask_ & VTERM_SELECTION_CLIPBOARD) {
            sel = "c";
        }
        if (osc52_callback_) {
            osc52_callback_(sel, base64_encode(selection_accum_));
        }
        selection_accum_.clear();
        active_selection_mask_ = 0;
    }
    return 1;
}

void VteScreen::on_output(const char* s, size_t len) {
    if (output_callback_ && s && len > 0) {
        output_callback_(std::string_view(s, len));
    }
}

} // namespace tmux2::vte
