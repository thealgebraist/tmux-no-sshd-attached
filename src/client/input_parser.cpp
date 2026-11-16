#include "src/client/input_parser.hpp"

#include <cctype>
#include <charconv>
#include <string_view>

namespace tmux2::client {

std::vector<InputEvent> InputParser::parse(std::string_view str) {
    auto ptr = reinterpret_cast<const uint8_t*>(str.data());
    return parse(std::span<const uint8_t>(ptr, str.size()));
}

std::vector<InputEvent> InputParser::parse(std::span<const uint8_t> bytes) {
    std::vector<InputEvent> events;
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    size_t offset = 0;
    while (offset < buffer_.size()) {
        std::span<const uint8_t> rem(buffer_.data() + offset, buffer_.size() - offset);

        if (in_paste_mode_) {
            // Look for \x1b[201~
            static constexpr uint8_t paste_end[] = {0x1b, '[', '2', '0', '1', '~'};
            auto it = std::search(rem.begin(), rem.end(), std::begin(paste_end), std::end(paste_end));
            if (it != rem.end()) {
                size_t prefix_len = std::distance(rem.begin(), it);
                paste_buffer_.insert(paste_buffer_.end(), rem.begin(), rem.begin() + prefix_len);
                events.push_back(ClientPasteEvent{
                    .bracketed = true,
                    .data = std::move(paste_buffer_)
                });
                paste_buffer_.clear();
                in_paste_mode_ = false;
                offset += prefix_len + sizeof(paste_end);
            } else {
                // Keep partial end marker safe in buffer, copy rest into paste_buffer_
                if (rem.size() >= sizeof(paste_end)) {
                    size_t safe_copy = rem.size() - (sizeof(paste_end) - 1);
                    paste_buffer_.insert(paste_buffer_.end(), rem.begin(), rem.begin() + safe_copy);
                    offset += safe_copy;
                }
                break;
            }
            continue;
        }

        size_t consumed = try_parse_event(rem, events);
        if (consumed == 0) {
            // Need more bytes to complete sequence
            break;
        }
        offset += consumed;
    }

    if (offset > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
    }

    return events;
}

std::vector<InputEvent> InputParser::flush() {
    std::vector<InputEvent> events;
    if (in_paste_mode_) {
        // Emit whatever was collected as paste
        if (!paste_buffer_.empty() || !buffer_.empty()) {
            paste_buffer_.insert(paste_buffer_.end(), buffer_.begin(), buffer_.end());
            events.push_back(ClientPasteEvent{
                .bracketed = true,
                .data = std::move(paste_buffer_)
            });
            paste_buffer_.clear();
            buffer_.clear();
        }
        in_paste_mode_ = false;
        return events;
    }

    if (buffer_.empty()) {
        return events;
    }

    // If buffer contains a lone ESC
    if (buffer_.size() == 1 && buffer_[0] == 0x1b) {
        events.push_back(ClientKeyEvent{
            .key_code = SpecialKey::Escape,
            .modifiers = KeyModifiers::None,
            .raw_bytes = {0x1b}
        });
        buffer_.clear();
        return events;
    }

    // Try parsing character by character
    for (uint8_t b : buffer_) {
        events.push_back(ClientKeyEvent{
            .key_code = b,
            .modifiers = KeyModifiers::None,
            .raw_bytes = {b}
        });
    }
    buffer_.clear();
    return events;
}

void InputParser::reset() {
    buffer_.clear();
    paste_buffer_.clear();
    in_paste_mode_ = false;
}

size_t InputParser::try_parse_event(std::span<const uint8_t> span, std::vector<InputEvent>& out) {
    if (span.empty()) return 0;

    uint8_t first = span[0];

    // Escape sequence
    if (first == 0x1b) {
        return try_parse_escape(span, out);
    }

    // Control characters (0x00 - 0x1F, except 0x1b which was handled)
    if (first == 0x00) {
        out.push_back(ClientKeyEvent{
            .key_code = ' ',
            .modifiers = KeyModifiers::Ctrl,
            .raw_bytes = {0x00}
        });
        return 1;
    }
    if (first == 0x09) {
        out.push_back(ClientKeyEvent{
            .key_code = SpecialKey::Tab,
            .modifiers = KeyModifiers::None,
            .raw_bytes = {0x09}
        });
        return 1;
    }
    if (first == 0x0a || first == 0x0d) {
        out.push_back(ClientKeyEvent{
            .key_code = SpecialKey::Enter,
            .modifiers = KeyModifiers::None,
            .raw_bytes = {first}
        });
        return 1;
    }
    if (first == 0x7f || first == 0x08) {
        out.push_back(ClientKeyEvent{
            .key_code = SpecialKey::Backspace,
            .modifiers = KeyModifiers::None,
            .raw_bytes = {first}
        });
        return 1;
    }
    if (first >= 0x01 && first <= 0x1a) {
        uint32_t char_code = 'a' + (first - 1);
        out.push_back(ClientKeyEvent{
            .key_code = char_code,
            .modifiers = KeyModifiers::Ctrl,
            .raw_bytes = {first}
        });
        return 1;
    }

    // Standard UTF-8 multibyte sequence or printable ASCII
    if ((first & 0x80) == 0) {
        out.push_back(ClientKeyEvent{
            .key_code = first,
            .modifiers = KeyModifiers::None,
            .raw_bytes = {first}
        });
        return 1;
    }

    // Multi-byte UTF-8
    size_t needed = 0;
    uint32_t cp = 0;
    if ((first & 0xE0) == 0xC0) {
        needed = 2;
        cp = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        needed = 3;
        cp = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        needed = 4;
        cp = first & 0x07;
    } else {
        // Invalid leading byte, consume 1 byte
        out.push_back(ClientKeyEvent{
            .key_code = first,
            .modifiers = KeyModifiers::None,
            .raw_bytes = {first}
        });
        return 1;
    }

    if (span.size() < needed) {
        return 0; // wait for more bytes
    }

    for (size_t i = 1; i < needed; ++i) {
        if ((span[i] & 0xC0) != 0x80) {
            // Malformed UTF-8, emit single byte
            out.push_back(ClientKeyEvent{
                .key_code = first,
                .modifiers = KeyModifiers::None,
                .raw_bytes = {first}
            });
            return 1;
        }
        cp = (cp << 6) | (span[i] & 0x3F);
    }

    std::vector<uint8_t> raw(span.begin(), span.begin() + needed);
    out.push_back(ClientKeyEvent{
        .key_code = cp,
        .modifiers = KeyModifiers::None,
        .raw_bytes = std::move(raw)
    });
    return needed;
}

size_t InputParser::try_parse_escape(std::span<const uint8_t> span, std::vector<InputEvent>& out) {
    if (span.size() == 1) {
        return 0; // need more bytes to disambiguate lone ESC vs escape sequence
    }

    uint8_t second = span[1];

    // CSI: \x1b[
    if (second == '[') {
        if (span.size() >= 3 && span[2] == '<') {
            return try_parse_sgr_mouse(span, out);
        }
        return try_parse_csi(span, out);
    }

    // SS3: \x1bO
    if (second == 'O') {
        return try_parse_ss3(span, out);
    }

    // Alt + key (e.g. \x1b + 'a' -> Alt-a)
    if (span.size() >= 2) {
        uint8_t k = span[1];
        if (k != 0x1b && k >= 0x20 && k <= 0x7e) {
            out.push_back(ClientKeyEvent{
                .key_code = k,
                .modifiers = KeyModifiers::Alt,
                .raw_bytes = {0x1b, k}
            });
            return 2;
        }
    }

    return 0;
}

size_t InputParser::try_parse_sgr_mouse(std::span<const uint8_t> span, std::vector<InputEvent>& out) {
    // Expected format: \x1b[< {btn} ; {col} ; {row} (M|m)
    if (span.size() < 6) return 0;

    size_t end_idx = 3;
    while (end_idx < span.size() && span[end_idx] != 'M' && span[end_idx] != 'm') {
        ++end_idx;
    }
    if (end_idx >= span.size()) return 0; // Incomplete

    std::string_view payload(reinterpret_cast<const char*>(span.data() + 3), end_idx - 3);
    char term_char = static_cast<char>(span[end_idx]);

    // Parse button;col;row
    auto semi1 = payload.find(';');
    if (semi1 == std::string_view::npos) return 0;
    auto semi2 = payload.find(';', semi1 + 1);
    if (semi2 == std::string_view::npos) return 0;

    int btn = 0;
    int col = 0;
    int row = 0;
    std::from_chars(payload.data(), payload.data() + semi1, btn);
    std::from_chars(payload.data() + semi1 + 1, payload.data() + semi2, col);
    std::from_chars(payload.data() + semi2 + 1, payload.data() + payload.size(), row);

    ClientMouseEvent ev;
    ev.col = (col > 0) ? static_cast<uint16_t>(col - 1) : 0;
    ev.row = (row > 0) ? static_cast<uint16_t>(row - 1) : 0;

    if (btn & 4) ev.modifiers |= KeyModifiers::Shift;
    if (btn & 8) ev.modifiers |= KeyModifiers::Alt;
    if (btn & 16) ev.modifiers |= KeyModifiers::Ctrl;

    if (btn & 64) {
        // Wheel event
        if (btn & 1) {
            ev.kind = ClientMouseEvent::Kind::WheelDown;
        } else {
            ev.kind = ClientMouseEvent::Kind::WheelUp;
        }
        ev.button = ClientMouseEvent::Button::None;
    } else if (term_char == 'm') {
        ev.kind = ClientMouseEvent::Kind::Release;
        int b = btn & 3;
        if (b == 0) ev.button = ClientMouseEvent::Button::Left;
        else if (b == 1) ev.button = ClientMouseEvent::Button::Middle;
        else if (b == 2) ev.button = ClientMouseEvent::Button::Right;
        else ev.button = ClientMouseEvent::Button::None;
    } else if (btn & 32) {
        ev.kind = ClientMouseEvent::Kind::Move;
        int b = btn & 3;
        if (b == 0) ev.button = ClientMouseEvent::Button::Left;
        else if (b == 1) ev.button = ClientMouseEvent::Button::Middle;
        else if (b == 2) ev.button = ClientMouseEvent::Button::Right;
        else ev.button = ClientMouseEvent::Button::None;
    } else {
        ev.kind = ClientMouseEvent::Kind::Press;
        int b = btn & 3;
        if (b == 0) ev.button = ClientMouseEvent::Button::Left;
        else if (b == 1) ev.button = ClientMouseEvent::Button::Middle;
        else if (b == 2) ev.button = ClientMouseEvent::Button::Right;
        else ev.button = ClientMouseEvent::Button::None;
    }

    out.push_back(ev);
    return end_idx + 1;
}

size_t InputParser::try_parse_csi(std::span<const uint8_t> span, std::vector<InputEvent>& out) {
    if (span.size() < 3) return 0;

    // Check bracketed paste start: \x1b[200~
    static constexpr uint8_t paste_start[] = {0x1b, '[', '2', '0', '0', '~'};
    if (span.size() >= sizeof(paste_start) &&
        std::equal(std::begin(paste_start), std::end(paste_start), span.begin())) {
        in_paste_mode_ = true;
        paste_buffer_.clear();
        return sizeof(paste_start);
    }

    // Find the terminal byte of CSI: 0x40-0x7E ('@' - '~')
    size_t i = 2;
    while (i < span.size() && (span[i] < 0x40 || span[i] > 0x7E)) {
        ++i;
    }
    if (i >= span.size()) {
        return 0; // Incomplete CSI
    }

    uint8_t final_char = span[i];
    std::string_view params(reinterpret_cast<const char*>(span.data() + 2), i - 2);
    std::vector<uint8_t> raw(span.begin(), span.begin() + i + 1);

    // Parse modifiers if present (e.g. 1;5A -> Ctrl+Up)
    uint32_t mods = KeyModifiers::None;
    auto semi = params.find(';');
    if (semi != std::string_view::npos) {
        int mod_code = 1;
        std::from_chars(params.data() + semi + 1, params.data() + params.size(), mod_code);
        if (mod_code == 2) mods |= KeyModifiers::Shift;
        else if (mod_code == 3) mods |= KeyModifiers::Alt;
        else if (mod_code == 4) mods |= KeyModifiers::Shift | KeyModifiers::Alt;
        else if (mod_code == 5) mods |= KeyModifiers::Ctrl;
        else if (mod_code == 6) mods |= KeyModifiers::Shift | KeyModifiers::Ctrl;
        else if (mod_code == 7) mods |= KeyModifiers::Alt | KeyModifiers::Ctrl;
        else if (mod_code == 8) mods |= KeyModifiers::Shift | KeyModifiers::Alt | KeyModifiers::Ctrl;
    }

    uint32_t key = 0;
    if (final_char == 'A') key = SpecialKey::Up;
    else if (final_char == 'B') key = SpecialKey::Down;
    else if (final_char == 'C') key = SpecialKey::Right;
    else if (final_char == 'D') key = SpecialKey::Left;
    else if (final_char == 'H') key = SpecialKey::Home;
    else if (final_char == 'F') key = SpecialKey::End;
    else if (final_char == 'Z') {
        key = SpecialKey::BackTab;
        mods |= KeyModifiers::Shift;
    } else if (final_char == '~') {
        int code = 0;
        std::from_chars(params.data(), params.data() + (semi != std::string_view::npos ? semi : params.size()), code);
        switch (code) {
            case 1: key = SpecialKey::Home; break;
            case 2: key = SpecialKey::Insert; break;
            case 3: key = SpecialKey::Delete; break;
            case 4: key = SpecialKey::End; break;
            case 5: key = SpecialKey::PageUp; break;
            case 6: key = SpecialKey::PageDown; break;
            case 7: key = SpecialKey::Home; break;
            case 8: key = SpecialKey::End; break;
            case 11: key = SpecialKey::F1; break;
            case 12: key = SpecialKey::F2; break;
            case 13: key = SpecialKey::F3; break;
            case 14: key = SpecialKey::F4; break;
            case 15: key = SpecialKey::F5; break;
            case 17: key = SpecialKey::F6; break;
            case 18: key = SpecialKey::F7; break;
            case 19: key = SpecialKey::F8; break;
            case 20: key = SpecialKey::F9; break;
            case 21: key = SpecialKey::F10; break;
            case 23: key = SpecialKey::F11; break;
            case 24: key = SpecialKey::F12; break;
            default: break;
        }
    }

    if (key != 0) {
        out.push_back(ClientKeyEvent{
            .key_code = key,
            .modifiers = mods,
            .raw_bytes = std::move(raw)
        });
        return i + 1;
    }

    // Unrecognized CSI, consume it so we don't hang
    return i + 1;
}

size_t InputParser::try_parse_ss3(std::span<const uint8_t> span, std::vector<InputEvent>& out) {
    if (span.size() < 3) return 0;
    uint8_t k = span[2];
    uint32_t key = 0;
    if (k == 'P') key = SpecialKey::F1;
    else if (k == 'Q') key = SpecialKey::F2;
    else if (k == 'R') key = SpecialKey::F3;
    else if (k == 'S') key = SpecialKey::F4;
    else if (k == 'H') key = SpecialKey::Home;
    else if (k == 'F') key = SpecialKey::End;

    if (key != 0) {
        out.push_back(ClientKeyEvent{
            .key_code = key,
            .modifiers = KeyModifiers::None,
            .raw_bytes = {span[0], span[1], span[2]}
        });
        return 3;
    }
    return 3;
}

protocol::MsgKeyEvent InputParser::to_protocol_key(const ClientKeyEvent& ev, uint64_t client_seq, uint32_t target_pane) {
    return protocol::MsgKeyEvent{
        .client_seq = client_seq,
        .target_pane = target_pane,
        .key_code = ev.key_code,
        .modifiers = ev.modifiers,
        .raw_bytes = ev.raw_bytes
    };
}

protocol::MsgMouseEvent InputParser::to_protocol_mouse(const ClientMouseEvent& ev, uint64_t client_seq, uint32_t pane_id) {
    return protocol::MsgMouseEvent{
        .client_seq = client_seq,
        .pane_id = pane_id,
        .kind = static_cast<uint8_t>(ev.kind),
        .button = static_cast<uint8_t>(ev.button),
        .col = ev.col,
        .row = ev.row,
        .modifiers = ev.modifiers
    };
}

protocol::MsgPasteEvent InputParser::to_protocol_paste(const ClientPasteEvent& ev, uint64_t client_seq, uint32_t pane_id) {
    return protocol::MsgPasteEvent{
        .client_seq = client_seq,
        .pane_id = pane_id,
        .bracketed = ev.bracketed,
        .data = ev.data
    };
}

} // namespace tmux2::client
