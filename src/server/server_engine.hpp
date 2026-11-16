#pragma once

#include "src/protocol/messages.hpp"
#include "src/server/client_state.hpp"
#include "src/server/command_dsl.hpp"
#include "src/server/session_manager.hpp"
#include <tmux2/reactor/reactor.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tmux2::server {

using MessageSender = std::function<void(uint64_t client_id, const protocol::ProtocolMessage& msg)>;

class ServerEngine {
public:
    explicit ServerEngine(MessageSender sender = nullptr, bool mock_pty = false);
    ~ServerEngine() = default;

    // Sender callback configuration
    void set_message_sender(MessageSender sender) { sender_ = std::move(sender); }

    // SessionManager & Client access
    [[nodiscard]] SessionManager& session_manager() noexcept { return session_manager_; }
    [[nodiscard]] const SessionManager& session_manager() const noexcept { return session_manager_; }

    [[nodiscard]] std::shared_ptr<ClientState> get_client(uint64_t client_id) const;
    std::shared_ptr<ClientState> register_client(
        uint64_t client_id,
        const std::array<uint8_t, 16>& instance_id,
        std::string_view cert_fingerprint);
    void unregister_client(uint64_t client_id);

    // Protocol message handler
    void handle_message(uint64_t client_id, const protocol::ProtocolMessage& msg);

    // Broadcast frame diffs and layout updates for a session
    void broadcast_session_layout(uint32_t session_id);
    void broadcast_session_frames(uint32_t session_id, std::string_view status_line = "");

    // PTY output polling & reactor integration
    bool poll_panes();
    void attach_to_reactor(tmux2::reactor::Reactor& reactor);

    // Mock PTY mode (for hermetic unit tests)
    void set_mock_pty(bool mock) noexcept { mock_pty_ = mock; }
    [[nodiscard]] bool mock_pty() const noexcept { return mock_pty_; }

    // Sequence generator
    uint64_t next_server_seq() noexcept { return next_server_seq_++; }

    // HMAC key for resume token
    [[nodiscard]] std::span<const uint8_t> hmac_key() const noexcept { return hmac_key_; }
    void set_hmac_key(std::span<const uint8_t> key);

private:
    void send_to_client(uint64_t client_id, const protocol::ProtocolMessage& msg);

    void handle_client_hello(uint64_t client_id, const protocol::MsgClientHello& req);
    void handle_list_sessions(uint64_t client_id, const protocol::MsgListSessionsReq& req);
    void handle_new_session(uint64_t client_id, const protocol::MsgNewSessionReq& req);
    void handle_attach_session(uint64_t client_id, const protocol::MsgAttachSessionReq& req);
    void handle_switch_session(uint64_t client_id, const protocol::MsgSwitchSessionReq& req);
    void handle_detach(uint64_t client_id, const protocol::MsgDetachReq& req);
    void handle_kill_session(uint64_t client_id, const protocol::MsgKillSessionReq& req);
    void handle_command_rpc(uint64_t client_id, const protocol::MsgCommandRpcReq& req);
    void handle_key_event(uint64_t client_id, const protocol::MsgKeyEvent& ev);
    void handle_mouse_event(uint64_t client_id, const protocol::MsgMouseEvent& ev);
    void handle_paste_event(uint64_t client_id, const protocol::MsgPasteEvent& ev);
    void handle_client_resize(uint64_t client_id, const protocol::MsgClientResize& ev);
    void handle_scrollback_slice(uint64_t client_id, const protocol::MsgScrollbackSliceReq& req);

    SessionManager session_manager_;
    std::unordered_map<uint64_t, std::shared_ptr<ClientState>> clients_;
    MessageSender sender_;
    bool mock_pty_{false};

    uint64_t next_server_seq_{1};
    uint64_t next_assigned_client_id_{1};
    std::array<uint8_t, 32> hmac_key_{};
};

} // namespace tmux2::server
