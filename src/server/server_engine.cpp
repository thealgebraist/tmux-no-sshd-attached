#include "src/server/server_engine.hpp"

#include <openssl/rand.h>

namespace tmux2::server {

ServerEngine::ServerEngine(MessageSender sender, bool mock_pty)
    : sender_(std::move(sender)), mock_pty_(mock_pty) {
    if (RAND_bytes(hmac_key_.data(), static_cast<int>(hmac_key_.size())) != 1) {
        // Fallback deterministic fill if RAND_bytes fails
        for (size_t i = 0; i < hmac_key_.size(); ++i) {
            hmac_key_[i] = static_cast<uint8_t>(0xA5 ^ i);
        }
    }
}

void ServerEngine::set_hmac_key(std::span<const uint8_t> key) {
    size_t copy_len = std::min(key.size(), hmac_key_.size());
    std::copy_n(key.data(), copy_len, hmac_key_.data());
}

std::shared_ptr<ClientState> ServerEngine::get_client(uint64_t client_id) const {
    auto it = clients_.find(client_id);
    if (it != clients_.end()) return it->second;
    return nullptr;
}

std::shared_ptr<ClientState> ServerEngine::register_client(
    uint64_t client_id,
    const std::array<uint8_t, 16>& instance_id,
    std::string_view cert_fingerprint) {
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        return it->second;
    }
    auto state = std::make_shared<ClientState>(client_id, instance_id, cert_fingerprint);
    clients_[client_id] = state;
    return state;
}

void ServerEngine::unregister_client(uint64_t client_id) {
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        uint32_t sid = it->second->attached_session_id();
        if (sid != 0) {
            auto s = session_manager_.find_session(sid);
            if (s) {
                s->detach_client(client_id);
            }
        }
        clients_.erase(it);
    }
}

void ServerEngine::send_to_client(uint64_t client_id, const protocol::ProtocolMessage& msg) {
    if (sender_) {
        sender_(client_id, msg);
    }
}

void ServerEngine::handle_message(uint64_t client_id, const protocol::ProtocolMessage& msg) {
    std::visit([this, client_id](const auto& concrete_msg) {
        using T = std::decay_t<decltype(concrete_msg)>;

        if constexpr (std::is_same_v<T, protocol::MsgClientHello>) {
            handle_client_hello(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgListSessionsReq>) {
            handle_list_sessions(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgNewSessionReq>) {
            handle_new_session(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgAttachSessionReq>) {
            handle_attach_session(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgSwitchSessionReq>) {
            handle_switch_session(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgDetachReq>) {
            handle_detach(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgKillSessionReq>) {
            handle_kill_session(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgCommandRpcReq>) {
            handle_command_rpc(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgKeyEvent>) {
            handle_key_event(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgMouseEvent>) {
            handle_mouse_event(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgPasteEvent>) {
            handle_paste_event(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgClientResize>) {
            handle_client_resize(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgScrollbackSliceReq>) {
            handle_scrollback_slice(client_id, concrete_msg);
        } else if constexpr (std::is_same_v<T, protocol::MsgHeartbeatPing>) {
            protocol::MsgHeartbeatPong pong;
            pong.seq = concrete_msg.seq;
            pong.timestamp_ms = concrete_msg.timestamp_ms;
            send_to_client(client_id, pong);
        }
    }, msg);
}

void ServerEngine::handle_client_hello(uint64_t client_id, const protocol::MsgClientHello& req) {
    auto client = get_client(client_id);
    if (!client) {
        client = register_client(client_id, req.client_instance_id, "default-cert-fp");
    }

    client->set_term_caps(req.term_caps);

    bool is_resume_valid = false;
    if (!req.resume_token.empty()) {
        is_resume_valid = ClientState::validate_resume_token(
            req.resume_token, client_id, client->cert_fingerprint(), hmac_key_);
    }

    if (!is_resume_valid) {
        std::string token = ClientState::generate_resume_token(
            client_id, client->cert_fingerprint(), hmac_key_);
        client->set_resume_token(token);
    }

    protocol::MsgServerHello resp;
    resp.server_version = 1;
    resp.assigned_client_id = client_id;
    resp.resume_token = client->resume_token();
    resp.last_client_seq_acked = client->last_client_seq_acked();
    resp.session_list = session_manager_.list_sessions();

    send_to_client(client_id, resp);

    // If valid resumption, attempt delta replay
    if (is_resume_valid) {
        uint32_t sid = client->attached_session_id();
        auto session = session_manager_.find_session(sid);
        if (session) {
            if (req.last_srv_seq > 0 && client->can_delta_replay(req.last_srv_seq)) {
                auto replays = client->get_replay_diffs(req.last_srv_seq);
                for (const auto& diff : replays) {
                    send_to_client(client_id, diff);
                }
            } else {
                // Snapshot fallback
                broadcast_session_layout(sid);
                auto win = session->active_window();
                if (win) {
                    for (const auto& [pid, pane] : win->panes()) {
                        send_to_client(client_id, pane->get_snapshot(next_server_seq()));
                    }
                }
            }
        }
    } else if (!req.target.empty()) {
        // Automatic attach on initial ClientHello if target specified
        protocol::MsgAttachSessionReq attach_req;
        attach_req.session_name = req.target;
        handle_attach_session(client_id, attach_req);
    }
}

void ServerEngine::handle_list_sessions(uint64_t client_id, const protocol::MsgListSessionsReq& req) {
    protocol::MsgListSessionsResp resp;
    resp.sessions = session_manager_.list_sessions(req.filter);
    send_to_client(client_id, resp);
}

void ServerEngine::handle_new_session(uint64_t client_id, const protocol::MsgNewSessionReq& req) {
    auto session = session_manager_.new_session(req, 80, 24, mock_pty_);

    protocol::MsgNewSessionResp resp;
    if (session) {
        resp.success = true;
        resp.session_id = session->session_id();
        auto win = session->active_window();
        if (win) {
            resp.window_id = win->window_id();
            resp.pane_id = win->active_pane_id();
        }
    } else {
        resp.success = false;
        resp.error_msg = "Failed to create session";
    }

    send_to_client(client_id, resp);

    if (session && !req.detached) {
        protocol::MsgAttachSessionReq attach_req;
        attach_req.session_id = session->session_id();
        handle_attach_session(client_id, attach_req);
    }
}

void ServerEngine::handle_attach_session(uint64_t client_id, const protocol::MsgAttachSessionReq& req) {
    auto client = get_client(client_id);
    if (!client) {
        client = register_client(client_id, {}, "default-cert-fp");
    }

    auto session = session_manager_.find_session_or_default(req.session_id, req.session_name);
    if (!session) {
        // Auto-create default session if none exist
        protocol::MsgNewSessionReq new_req;
        new_req.name = req.session_name.empty() ? "default" : req.session_name;
        session = session_manager_.new_session(new_req, req.cols, req.rows, mock_pty_);
    }

    protocol::MsgAttachSessionResp resp;
    if (!session) {
        resp.success = false;
        resp.error_msg = "Session not found";
        send_to_client(client_id, resp);
        return;
    }

    uint16_t cols = req.cols > 0 ? req.cols : 80;
    uint16_t rows = req.rows > 0 ? req.rows : 24;

    // Attach client to session
    auto detached_others = session->attach_client(client_id, req.read_only, cols, rows);
    for (uint64_t detached_id : detached_others) {
        protocol::MsgDetachNotice notice;
        notice.reason = "Detached by another client attachment (-d)";
        send_to_client(detached_id, notice);
    }

    client->set_attached_session_id(session->session_id());
    client->set_read_only(req.read_only);
    client->set_dimensions(cols, rows);

    auto win = session->active_window();
    resp.success = true;
    resp.session_id = session->session_id();
    if (win) {
        resp.window_id = win->window_id();
        resp.layout_tree_json = win->layout().to_json();
    }

    send_to_client(client_id, resp);

    // Send layout update & screen snapshots to client
    if (win) {
        send_to_client(client_id, win->get_layout_update(next_server_seq(), session->session_id()));
        for (const auto& [pid, pane] : win->panes()) {
            send_to_client(client_id, pane->get_snapshot(next_server_seq()));
        }
    }
}

void ServerEngine::handle_switch_session(uint64_t client_id, const protocol::MsgSwitchSessionReq& req) {
    auto client = get_client(client_id);
    if (!client) return;

    uint32_t old_sid = client->attached_session_id();
    auto old_session = session_manager_.find_session(old_sid);
    if (old_session) {
        old_session->detach_client(client_id);
    }

    auto new_session = session_manager_.find_session_or_default(req.session_id, req.session_name);
    protocol::MsgSwitchSessionResp resp;
    if (!new_session) {
        resp.success = false;
        resp.error_msg = "Target session not found";
        send_to_client(client_id, resp);
        return;
    }

    new_session->attach_client(client_id, client->is_read_only(), client->cols(), client->rows());
    client->set_attached_session_id(new_session->session_id());

    auto win = new_session->active_window();
    resp.success = true;
    resp.session_id = new_session->session_id();
    if (win) {
        resp.window_id = win->window_id();
    }
    send_to_client(client_id, resp);

    if (win) {
        send_to_client(client_id, win->get_layout_update(next_server_seq(), new_session->session_id()));
        for (const auto& [pid, pane] : win->panes()) {
            send_to_client(client_id, pane->get_snapshot(next_server_seq()));
        }
    }
}

void ServerEngine::handle_detach(uint64_t client_id, const protocol::MsgDetachReq& req) {
    auto client = get_client(client_id);
    if (!client) return;

    uint32_t sid = req.session_id != 0 ? req.session_id : client->attached_session_id();
    auto session = session_manager_.find_session(sid);
    if (session) {
        session->detach_client(client_id);
    }
    client->set_attached_session_id(0);
}

void ServerEngine::handle_kill_session(uint64_t client_id, const protocol::MsgKillSessionReq& req) {
    auto session = session_manager_.find_session(req.session_id);
    protocol::MsgKillSessionResp resp;

    if (!session) {
        resp.success = false;
        resp.error_msg = "Session not found";
        send_to_client(client_id, resp);
        return;
    }

    // Detach all attached clients
    for (const auto& att : session->attached_clients()) {
        protocol::MsgDetachNotice notice;
        notice.reason = "Session terminated";
        send_to_client(att.client_id, notice);
        auto c = get_client(att.client_id);
        if (c) c->set_attached_session_id(0);
    }

    session_manager_.kill_session(req.session_id);
    resp.success = true;
    send_to_client(client_id, resp);
}

void ServerEngine::handle_command_rpc(uint64_t client_id, const protocol::MsgCommandRpcReq& req) {
    auto client = get_client(client_id);
    uint32_t sid = client ? client->attached_session_id() : 0;

    auto resp = CommandDsl::execute(session_manager_, req.command_dsl, req.rpc_id, sid, mock_pty_);
    send_to_client(client_id, resp);

    // If active session was modified, broadcast layout and frames
    if (sid != 0) {
        broadcast_session_layout(sid);
        broadcast_session_frames(sid);
    }
}

void ServerEngine::handle_key_event(uint64_t client_id, const protocol::MsgKeyEvent& ev) {
    auto client = get_client(client_id);
    if (!client) return;

    if (!client->deduplicate_client_seq(ev.client_seq)) {
        return; // Deduplicated duplicate input
    }

    if (client->is_read_only()) {
        return; // Spectator client
    }

    auto session = session_manager_.find_session(client->attached_session_id());
    if (!session) return;

    // Trigger size policy interaction (window-size = latest dynamic resize)
    bool resized = session->client_interacted(client_id, client->cols(), client->rows());
    if (resized) {
        broadcast_session_layout(session->session_id());
    }

    auto [s, pane] = session_manager_.find_pane(ev.target_pane);
    if (!pane) {
        auto win = session->active_window();
        if (win) pane = win->active_pane();
    }

    if (pane) {
        if (!ev.raw_bytes.empty()) {
            std::string_view data(reinterpret_cast<const char*>(ev.raw_bytes.data()), ev.raw_bytes.size());
            pane->write_input(data);
            if (!pane->has_pty()) {
                pane->feed_output(data);
            }
        } else if (ev.key_code > 0) {
            char ch = static_cast<char>(ev.key_code);
            pane->write_input(std::string_view(&ch, 1));
            if (!pane->has_pty()) {
                pane->feed_output(std::string_view(&ch, 1));
            }
        }
        pane->read_pty();
        broadcast_session_frames(session->session_id());
    }
}

void ServerEngine::handle_mouse_event(uint64_t client_id, const protocol::MsgMouseEvent& ev) {
    auto client = get_client(client_id);
    if (!client || client->is_read_only()) return;
    if (!client->deduplicate_client_seq(ev.client_seq)) return;

    auto session = session_manager_.find_session(client->attached_session_id());
    if (!session) return;

    session->client_interacted(client_id, client->cols(), client->rows());
}

void ServerEngine::handle_paste_event(uint64_t client_id, const protocol::MsgPasteEvent& ev) {
    auto client = get_client(client_id);
    if (!client || client->is_read_only()) return;
    if (!client->deduplicate_client_seq(ev.client_seq)) return;

    auto session = session_manager_.find_session(client->attached_session_id());
    if (!session) return;

    auto win = session->active_window();
    auto pane = win ? win->active_pane() : nullptr;
    if (pane && !ev.data.empty()) {
        std::string_view data(reinterpret_cast<const char*>(ev.data.data()), ev.data.size());
        if (ev.bracketed) {
            pane->write_input("\x1b[200~");
            pane->write_input(data);
            pane->write_input("\x1b[201~");
        } else {
            pane->write_input(data);
        }
        if (!pane->has_pty()) {
            pane->feed_output(data);
        }
        pane->read_pty();
        broadcast_session_frames(session->session_id());
    }
}

void ServerEngine::handle_client_resize(uint64_t client_id, const protocol::MsgClientResize& ev) {
    auto client = get_client(client_id);
    if (!client) return;

    client->set_dimensions(ev.cols, ev.rows);

    auto session = session_manager_.find_session(client->attached_session_id());
    if (!session) return;

    if (!client->is_read_only()) {
        session->client_interacted(client_id, ev.cols, ev.rows);
        broadcast_session_layout(session->session_id());
        broadcast_session_frames(session->session_id());
    }
}

void ServerEngine::handle_scrollback_slice(uint64_t client_id, const protocol::MsgScrollbackSliceReq& req) {
    auto [session, pane] = session_manager_.find_pane(req.pane_id);
    if (pane) {
        auto resp = pane->get_scrollback_slice(req.start_line, req.line_count);
        send_to_client(client_id, resp);
    }
}

void ServerEngine::broadcast_session_layout(uint32_t session_id) {
    auto session = session_manager_.find_session(session_id);
    if (!session) return;

    auto win = session->active_window();
    if (!win) return;

    uint64_t seq = next_server_seq();
    auto update = win->get_layout_update(seq, session_id);

    for (const auto& att : session->attached_clients()) {
        send_to_client(att.client_id, update);
    }
}

void ServerEngine::broadcast_session_frames(uint32_t session_id, std::string_view status_line) {
    auto session = session_manager_.find_session(session_id);
    if (!session) return;

    auto win = session->active_window();
    if (!win) return;

    uint64_t seq = next_server_seq();

    protocol::MsgFrameDiff diff;
    diff.server_seq = seq;
    diff.status_line = status_line.empty() ? session->preview_summary() : std::string(status_line);

    for (const auto& [pid, pane] : win->panes()) {
        auto pdiff = pane->get_dirty_diff(seq);
        if (!pdiff.rows.empty()) {
            diff.pane_diffs.push_back(std::move(pdiff));
        }
    }

    if (diff.pane_diffs.empty()) {
        return; // Nothing changed
    }

    for (const auto& att : session->attached_clients()) {
        auto client = get_client(att.client_id);
        if (client) {
            diff.acked_client_seq = client->last_client_seq_acked();
            client->record_frame_diff(diff);
            client->set_last_srv_seq(seq);
        }
        send_to_client(att.client_id, diff);
    }
}

bool ServerEngine::poll_panes() {
    bool any_read = false;
    for (const auto& [sid, session] : session_manager_.sessions()) {
        bool session_has_output = false;
        for (const auto& win : session->windows()) {
            for (const auto& [pid, pane] : win->panes()) {
                if (pane->has_pty()) {
                    if (pane->read_pty()) {
                        session_has_output = true;
                        any_read = true;
                    }
                }
            }
        }
        if (session_has_output) {
            broadcast_session_frames(sid);
        }
    }
    return any_read;
}

void ServerEngine::attach_to_reactor(tmux2::reactor::Reactor& reactor) {
    for (const auto& [sid, session] : session_manager_.sessions()) {
        for (const auto& win : session->windows()) {
            for (const auto& [pid, pane] : win->panes()) {
                int fd = pane->master_fd();
                if (fd >= 0 && !reactor.has_fd(fd)) {
                    (void)reactor.register_fd(fd, tmux2::reactor::EventFlags::Readable,
                        [this, sid = sid, pane = pane](int /*fd*/, tmux2::reactor::EventFlags /*events*/) {
                            if (pane->read_pty()) {
                                broadcast_session_frames(sid);
                            }
                        });
                }
            }
        }
    }
}

} // namespace tmux2::server
