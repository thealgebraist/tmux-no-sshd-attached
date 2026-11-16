#pragma once

#include "src/protocol/messages.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace tmux2::client {

namespace KeyModifiers {
    inline constexpr uint32_t None  = 0;
    inline constexpr uint32_t Shift = 1 << 0;
    inline constexpr uint32_t Alt   = 1 << 1;
    inline constexpr uint32_t Ctrl  = 1 << 2;
}

namespace SpecialKey {
    inline constexpr uint32_t Enter     = 13;
    inline constexpr uint32_t Escape    = 27;
    inline constexpr uint32_t Tab       = 9;
    inline constexpr uint32_t BackTab   = 1000;
    inline constexpr uint32_t Backspace = 127;
    inline constexpr uint32_t Up        = 1001;
    inline constexpr uint32_t Down      = 1002;
    inline constexpr uint32_t Right     = 1003;
    inline constexpr uint32_t Left      = 1004;
    inline constexpr uint32_t Home      = 1005;
    inline constexpr uint32_t End       = 1006;
    inline constexpr uint32_t PageUp    = 1007;
    inline constexpr uint32_t PageDown  = 1008;
    inline constexpr uint32_t Insert    = 1009;
    inline constexpr uint32_t Delete    = 1010;
    inline constexpr uint32_t F1        = 1011;
    inline constexpr uint32_t F2        = 1012;
    inline constexpr uint32_t F3        = 1013;
    inline constexpr uint32_t F4        = 1014;
    inline constexpr uint32_t F5        = 1015;
    inline constexpr uint32_t F6        = 1016;
    inline constexpr uint32_t F7        = 1017;
    inline constexpr uint32_t F8        = 1018;
    inline constexpr uint32_t F9        = 1019;
    inline constexpr uint32_t F10       = 1020;
    inline constexpr uint32_t F11       = 1021;
    inline constexpr uint32_t F12       = 1022;
}

struct ClientKeyEvent {
    uint32_t key_code{0};
    uint32_t modifiers{KeyModifiers::None};
    std::vector<uint8_t> raw_bytes;

    bool operator==(const ClientKeyEvent&) const = default;
};

struct ClientMouseEvent {
    enum class Kind : uint8_t {
        Press = 0,
        Release = 1,
        Move = 2,
        WheelUp = 3,
        WheelDown = 4,
    };

    enum class Button : uint8_t {
        None = 0,
        Left = 1,
        Middle = 2,
        Right = 3,
    };

    Kind kind{Kind::Press};
    Button button{Button::Left};
    uint16_t col{0};
    uint16_t row{0};
    uint32_t modifiers{KeyModifiers::None};

    bool operator==(const ClientMouseEvent&) const = default;
};

struct ClientPasteEvent {
    bool bracketed{true};
    std::vector<uint8_t> data;

    bool operator==(const ClientPasteEvent&) const = default;
};

using InputEvent = std::variant<ClientKeyEvent, ClientMouseEvent, ClientPasteEvent>;

class InputParser {
public:
    InputParser() = default;

    // Parse a chunk of input bytes, returning all fully parsed events.
    // Handles fragmented sequences across calls.
    std::vector<InputEvent> parse(std::span<const uint8_t> bytes);
    std::vector<InputEvent> parse(std::string_view str);

    // Flushes any pending unparsed buffer (e.g. standalone ESC)
    std::vector<InputEvent> flush();

    // Reset internal state
    void reset();

    // Helper conversions to protocol structs
    static protocol::MsgKeyEvent to_protocol_key(const ClientKeyEvent& ev, uint64_t client_seq, uint32_t target_pane);
    static protocol::MsgMouseEvent to_protocol_mouse(const ClientMouseEvent& ev, uint64_t client_seq, uint32_t pane_id);
    static protocol::MsgPasteEvent to_protocol_paste(const ClientPasteEvent& ev, uint64_t client_seq, uint32_t pane_id);

private:
    std::vector<uint8_t> buffer_;
    bool in_paste_mode_{false};
    std::vector<uint8_t> paste_buffer_;

    size_t try_parse_event(std::span<const uint8_t> span, std::vector<InputEvent>& out);
    size_t try_parse_escape(std::span<const uint8_t> span, std::vector<InputEvent>& out);
    size_t try_parse_sgr_mouse(std::span<const uint8_t> span, std::vector<InputEvent>& out);
    size_t try_parse_csi(std::span<const uint8_t> span, std::vector<InputEvent>& out);
    size_t try_parse_ss3(std::span<const uint8_t> span, std::vector<InputEvent>& out);
};

} // namespace tmux2::client
