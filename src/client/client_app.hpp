#pragma once

#include "src/client/cli_parser.hpp"
#include "src/client/copy_mode.hpp"
#include "src/client/input_parser.hpp"
#include "src/client/keybindings.hpp"
#include "src/client/reconnect_buffer.hpp"
#include "src/client/renderer.hpp"
#include "src/client/session_chooser.hpp"
#include "src/client/tty_raw.hpp"
#include "src/protocol/messages.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tmux2::client {

enum class HitType {
    None,
    Pane,
    VerticalBorder,
    HorizontalBorder,
    WindowTab,
};

struct HitTestResult {
    HitType type{HitType::None};
    uint32_t pane_id{0};
    uint16_t local_col{0};
    uint16_t local_row{0};
    uint32_t left_pane_id{0};
    uint32_t right_pane_id{0};
    uint32_t top_pane_id{0};
    uint32_t bottom_pane_id{0};
    uint32_t window_index{0};

    bool operator==(const HitTestResult&) const = default;
};

class ClientApp {
public:
    explicit ClientApp(uint16_t term_cols = 80, uint16_t term_rows = 24);

    // Component access
    [[nodiscard]] Renderer& renderer() noexcept { return renderer_; }
    [[nodiscard]] const Renderer& renderer() const noexcept { return renderer_; }

    [[nodiscard]] KeybindingsEngine& keybindings() noexcept { return keybindings_; }
    [[nodiscard]] const KeybindingsEngine& keybindings() const noexcept { return keybindings_; }

    [[nodiscard]] SessionChooser& session_chooser() noexcept { return session_chooser_; }
    [[nodiscard]] const SessionChooser& session_chooser() const noexcept { return session_chooser_; }

    [[nodiscard]] CopyMode& copy_mode() noexcept { return copy_mode_; }
    [[nodiscard]] const CopyMode& copy_mode() const noexcept { return copy_mode_; }

    [[nodiscard]] ReconnectBuffer& reconnect_buffer() noexcept { return reconnect_buf_; }
    [[nodiscard]] const ReconnectBuffer& reconnect_buffer() const noexcept { return reconnect_buf_; }

    [[nodiscard]] InputParser& input_parser() noexcept { return input_parser_; }

    // Hit-testing
    [[nodiscard]] HitTestResult hit_test(uint16_t col, uint16_t row) const;

    // Split resizing via mouse drag
    bool resize_border_drag(const HitTestResult& border, int16_t delta_x, int16_t delta_y);

    // Input processing: returns messages to send to server and any local terminal outputs
    struct ProcessInputResult {
        std::vector<protocol::ProtocolMessage> outbound_messages;
        std::string terminal_output; // e.g. OSC 52, bell, redraw
        bool should_redraw{false};
        bool should_detach{false};
    };

    ProcessInputResult process_input_bytes(std::span<const uint8_t> bytes);
    ProcessInputResult process_input_event(const InputEvent& event);

    // Server protocol messages handling
    struct ServerMsgResult {
        std::string terminal_output; // e.g. redrawn frame or OSC 52
        bool should_redraw{false};
        std::optional<protocol::MsgScrollbackSliceReq> slice_req;
    };

    ServerMsgResult handle_server_message(const protocol::ProtocolMessage& msg);

    // Reconnect & catch-up
    [[nodiscard]] std::vector<protocol::ProtocolMessage> prepare_reconnect_replay(uint64_t last_server_acked_seq);

    // Render current composite screen
    [[nodiscard]] std::string render_frame(bool use_decset_2026 = true);

    void set_attached_session(uint32_t session_id, std::string name);
    [[nodiscard]] uint32_t attached_session_id() const noexcept { return attached_session_id_; }
    [[nodiscard]] std::string_view attached_session_name() const noexcept { return attached_session_name_; }

private:
    Renderer renderer_;
    KeybindingsEngine keybindings_;
    SessionChooser session_chooser_;
    CopyMode copy_mode_;
    ReconnectBuffer reconnect_buf_;
    InputParser input_parser_;

    uint32_t attached_session_id_{0};
    std::string attached_session_name_{"main"};
    std::vector<protocol::SessionSummary> cached_sessions_;

    // Drag tracking state
    bool is_dragging_border_{false};
    HitTestResult drag_target_{};
    uint16_t drag_start_x_{0};
    uint16_t drag_start_y_{0};

    ProcessInputResult handle_key_event(const ClientKeyEvent& key);
    ProcessInputResult handle_mouse_event(const ClientMouseEvent& mouse);
    ProcessInputResult handle_paste_event(const ClientPasteEvent& paste);
};

} // namespace tmux2::client
