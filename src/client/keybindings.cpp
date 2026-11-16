#include "src/client/keybindings.hpp"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include <cctype>

namespace tmux2::client {

KeybindingsEngine::KeybindingsEngine(std::string_view prefix) {
    set_prefix(prefix);
}

void KeybindingsEngine::parse_prefix(std::string_view prefix) {
    prefix_str_ = std::string(prefix);
    prefix_modifiers_ = KeyModifiers::None;
    prefix_key_code_ = 0;

    if (prefix.empty()) {
        prefix_key_code_ = 'b';
        prefix_modifiers_ = KeyModifiers::Ctrl;
        return;
    }

    if (prefix.starts_with("C-") || prefix.starts_with("^")) {
        prefix_modifiers_ |= KeyModifiers::Ctrl;
        char c = prefix.starts_with("C-") ? prefix[2] : prefix[1];
        prefix_key_code_ = std::tolower(static_cast<unsigned char>(c));
    } else if (prefix.starts_with("M-")) {
        prefix_modifiers_ |= KeyModifiers::Alt;
        prefix_key_code_ = std::tolower(static_cast<unsigned char>(prefix[2]));
    } else {
        prefix_key_code_ = static_cast<unsigned char>(prefix[0]);
    }
}

void KeybindingsEngine::set_prefix(std::string_view prefix) {
    parse_prefix(prefix);
}

bool KeybindingsEngine::matches_prefix(const ClientKeyEvent& key) const {
    // Check key code and modifiers
    if (key.modifiers == prefix_modifiers_ && key.key_code == prefix_key_code_) {
        return true;
    }
    // Also if key was raw 0x02 for Ctrl-b, raw 0x01 for Ctrl-a, etc.
    if ((prefix_modifiers_ & KeyModifiers::Ctrl) && !key.raw_bytes.empty()) {
        uint8_t expected_raw = (prefix_key_code_ >= 'a' && prefix_key_code_ <= 'z')
                                   ? (prefix_key_code_ - 'a' + 1)
                                   : (prefix_key_code_ >= 'A' && prefix_key_code_ <= 'Z')
                                         ? (prefix_key_code_ - 'A' + 1)
                                         : 0;
        if (key.raw_bytes.size() == 1 && key.raw_bytes[0] == expected_raw) {
            return true;
        }
    }
    return false;
}

KeyAction KeybindingsEngine::handle_key(const ClientKeyEvent& key) {
    switch (state_) {
        case State::Normal:
            return handle_normal(key);
        case State::PrefixActive:
            return handle_prefix_active(key);
        case State::CommandPrompt:
            return handle_prompt(key, false);
        case State::RenameSessionPrompt:
            return handle_prompt(key, true);
    }
    return KeyAction{.type = KeyActionType::None};
}

KeyAction KeybindingsEngine::handle_normal(const ClientKeyEvent& key) {
    if (matches_prefix(key)) {
        state_ = State::PrefixActive;
        return KeyAction{.type = KeyActionType::PrefixActivated};
    }
    return KeyAction{
        .type = KeyActionType::ForwardKey,
        .forwarded_key = key
    };
}

KeyAction KeybindingsEngine::handle_prefix_active(const ClientKeyEvent& key) {
    // If prefix key pressed again, send literal prefix key
    if (matches_prefix(key)) {
        state_ = State::Normal;
        return KeyAction{
            .type = KeyActionType::ForwardKey,
            .forwarded_key = key
        };
    }

    state_ = State::Normal; // Almost all prefix actions reset state back to Normal (or to prompt)

    switch (key.key_code) {
        case 'c':
            return KeyAction{.type = KeyActionType::NewWindow};
        case 'n':
            return KeyAction{.type = KeyActionType::NextWindow};
        case 'p':
            return KeyAction{.type = KeyActionType::PreviousWindow};
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return KeyAction{
                .type = KeyActionType::SelectWindow,
                .param = static_cast<int>(key.key_code - '0')
            };
        case '%':
            return KeyAction{.type = KeyActionType::SplitVertical};
        case '"':
            return KeyAction{.type = KeyActionType::SplitHorizontal};
        case 'o':
            return KeyAction{.type = KeyActionType::FocusNextPane};
        case SpecialKey::Up:
            return KeyAction{.type = KeyActionType::FocusDirection, .direction = Direction::Up};
        case SpecialKey::Down:
            return KeyAction{.type = KeyActionType::FocusDirection, .direction = Direction::Down};
        case SpecialKey::Left:
            return KeyAction{.type = KeyActionType::FocusDirection, .direction = Direction::Left};
        case SpecialKey::Right:
            return KeyAction{.type = KeyActionType::FocusDirection, .direction = Direction::Right};
        case 'z':
            return KeyAction{.type = KeyActionType::ToggleZoom};
        case 'x':
            return KeyAction{.type = KeyActionType::KillPane};
        case 'd':
            return KeyAction{.type = KeyActionType::Detach};
        case '$':
            state_ = State::RenameSessionPrompt;
            prompt_buffer_.clear();
            return KeyAction{.type = KeyActionType::PromptRenameSession};
        case '(':
            return KeyAction{.type = KeyActionType::PreviousSession};
        case ')':
            return KeyAction{.type = KeyActionType::NextSession};
        case ':':
            state_ = State::CommandPrompt;
            prompt_buffer_.clear();
            return KeyAction{.type = KeyActionType::CommandPrompt};
        case '[':
            return KeyAction{.type = KeyActionType::EnterCopyMode};
        case 's':
            return KeyAction{.type = KeyActionType::OpenSessionChooser};
        default:
            return KeyAction{.type = KeyActionType::PrefixCancelled};
    }
}

KeyAction KeybindingsEngine::handle_prompt(const ClientKeyEvent& key, bool is_rename) {
    if (key.key_code == SpecialKey::Escape) {
        state_ = State::Normal;
        prompt_buffer_.clear();
        return KeyAction{.type = KeyActionType::None};
    }

    if (key.key_code == SpecialKey::Enter) {
        state_ = State::Normal;
        std::string res = std::move(prompt_buffer_);
        prompt_buffer_.clear();
        if (is_rename) {
            return KeyAction{
                .type = KeyActionType::RenameSession,
                .str_param = std::move(res)
            };
        } else {
            return KeyAction{
                .type = KeyActionType::ExecuteCommand,
                .str_param = std::move(res)
            };
        }
    }

    if (key.key_code == SpecialKey::Backspace) {
        if (!prompt_buffer_.empty()) {
            prompt_buffer_.pop_back();
        }
        return KeyAction{.type = KeyActionType::None};
    }

    // Printable characters
    if (key.modifiers == KeyModifiers::None || key.modifiers == KeyModifiers::Shift) {
        if (key.key_code >= 32 && key.key_code <= 126) {
            prompt_buffer_.push_back(static_cast<char>(key.key_code));
        }
    }

    return KeyAction{.type = KeyActionType::None};
}

void KeybindingsEngine::cancel_prefix() {
    if (state_ == State::PrefixActive) {
        state_ = State::Normal;
    }
}

void KeybindingsEngine::cancel_prompt() {
    state_ = State::Normal;
    prompt_buffer_.clear();
}

} // namespace tmux2::client
