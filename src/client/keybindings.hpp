#pragma once

#include "src/client/input_parser.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tmux2::client {

enum class KeyActionType {
    None,
    PrefixActivated,
    PrefixCancelled,
    ForwardKey,
    NewWindow,
    NextWindow,
    PreviousWindow,
    SelectWindow,
    SplitVertical,   // %
    SplitHorizontal, // "
    FocusNextPane,   // o
    FocusDirection,  // Arrows
    ToggleZoom,      // z
    KillPane,        // x
    Detach,          // d
    PromptRenameSession, // $
    RenameSession,
    PreviousSession, // (
    NextSession,     // )
    CommandPrompt,   // :
    ExecuteCommand,
    EnterCopyMode,   // [
    OpenSessionChooser, // s
};

enum class Direction : uint8_t {
    Up,
    Down,
    Left,
    Right
};

struct KeyAction {
    KeyActionType type{KeyActionType::None};
    int param{0};
    std::string str_param;
    Direction direction{Direction::Up};
    std::optional<ClientKeyEvent> forwarded_key;

    bool operator==(const KeyAction&) const = default;
};

class KeybindingsEngine {
public:
    explicit KeybindingsEngine(std::string_view prefix = "C-b");

    void set_prefix(std::string_view prefix);
    [[nodiscard]] std::string_view prefix_str() const noexcept { return prefix_str_; }
    [[nodiscard]] uint32_t prefix_key_code() const noexcept { return prefix_key_code_; }
    [[nodiscard]] uint32_t prefix_modifiers() const noexcept { return prefix_modifiers_; }

    enum class State {
        Normal,
        PrefixActive,
        CommandPrompt,
        RenameSessionPrompt,
    };

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool is_prefix_active() const noexcept { return state_ == State::PrefixActive; }
    [[nodiscard]] bool is_prompt_active() const noexcept {
        return state_ == State::CommandPrompt || state_ == State::RenameSessionPrompt;
    }
    [[nodiscard]] std::string_view prompt_buffer() const noexcept { return prompt_buffer_; }

    // Feed a key event into the engine
    KeyAction handle_key(const ClientKeyEvent& key);

    void cancel_prefix();
    void cancel_prompt();

private:
    std::string prefix_str_{"C-b"};
    uint32_t prefix_key_code_{'b'};
    uint32_t prefix_modifiers_{KeyModifiers::Ctrl};
    State state_{State::Normal};
    std::string prompt_buffer_;

    void parse_prefix(std::string_view prefix);
    bool matches_prefix(const ClientKeyEvent& key) const;
    KeyAction handle_normal(const ClientKeyEvent& key);
    KeyAction handle_prefix_active(const ClientKeyEvent& key);
    KeyAction handle_prompt(const ClientKeyEvent& key, bool is_rename);
};

} // namespace tmux2::client
