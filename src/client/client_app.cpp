#include "src/client/client_app.hpp"

#include <algorithm>
#include <format>

namespace tmux2::client {

ClientApp::ClientApp(uint16_t term_cols, uint16_t term_rows)
    : renderer_(term_cols, term_rows) {}

void ClientApp::set_attached_session(uint32_t session_id, std::string name) {
    attached_session_id_ = session_id;
    attached_session_name_ = std::move(name);
    renderer_.set_session_name(attached_session_name_);
}

HitTestResult ClientApp::hit_test(uint16_t col, uint16_t row) const {
    HitTestResult res;
    uint16_t term_cols = renderer_.term_cols();
    uint16_t term_rows = renderer_.term_rows();

    if (term_rows > 0 && row == term_rows - 1) {
        // Status bar row
        size_t offset = renderer_.session_name().size() + 3; // "[name] "
        for (const auto& tab : renderer_.window_tabs()) {
            std::string tab_str = std::format("{}:{}{} ",
                tab.index,
                tab.name,
                tab.is_active ? "*" : (tab.is_last ? "-" : ""));
            size_t next_offset = offset + tab_str.size();
            if (col >= offset && col < next_offset) {
                res.type = HitType::WindowTab;
                res.window_index = tab.index;
                return res;
            }
            offset = next_offset;
        }
        res.type = HitType::None;
        return res;
    }

    // Check borders between panes
    const auto& rects = renderer_.pane_rects();
    for (const auto& r1 : rects) {
        // Vertical border on right edge of r1
        uint16_t border_x = r1.x + r1.width;
        if (col == border_x && row >= r1.y && row < r1.y + r1.height) {
            for (const auto& r2 : rects) {
                if (r2.x == border_x + 1 && row >= r2.y && row < r2.y + r2.height) {
                    res.type = HitType::VerticalBorder;
                    res.left_pane_id = r1.pane_id;
                    res.right_pane_id = r2.pane_id;
                    return res;
                }
            }
        }

        // Horizontal border on bottom edge of r1
        uint16_t border_y = r1.y + r1.height;
        if (row == border_y && col >= r1.x && col < r1.x + r1.width) {
            for (const auto& r2 : rects) {
                if (r2.y == border_y + 1 && col >= r2.x && col < r2.x + r2.width) {
                    res.type = HitType::HorizontalBorder;
                    res.top_pane_id = r1.pane_id;
                    res.bottom_pane_id = r2.pane_id;
                    return res;
                }
            }
        }
    }

    // Check inside panes
    for (const auto& rect : rects) {
        if (col >= rect.x && col < rect.x + rect.width &&
            row >= rect.y && row < rect.y + rect.height) {
            res.type = HitType::Pane;
            res.pane_id = rect.pane_id;
            res.local_col = col - rect.x;
            res.local_row = row - rect.y;
            return res;
        }
    }

    return res;
}

bool ClientApp::resize_border_drag(const HitTestResult& border, int16_t delta_x, int16_t delta_y) {
    auto rects = renderer_.pane_rects();

    if (border.type == HitType::VerticalBorder) {
        if (delta_x == 0) return false;
        PaneRect* left = nullptr;
        PaneRect* right = nullptr;
        for (auto& r : rects) {
            if (r.pane_id == border.left_pane_id) left = &r;
            if (r.pane_id == border.right_pane_id) right = &r;
        }
        if (!left || !right) return false;

        if (delta_x > 0) {
            int16_t max_shift = right->width > 2 ? static_cast<int16_t>(right->width - 2) : 0;
            int16_t shift = std::min(delta_x, max_shift);
            if (shift <= 0) return false;
            left->width += shift;
            right->x += shift;
            right->width -= shift;
        } else {
            int16_t max_shift = left->width > 2 ? static_cast<int16_t>(left->width - 2) : 0;
            int16_t shift = std::min(static_cast<int16_t>(-delta_x), max_shift);
            if (shift <= 0) return false;
            left->width -= shift;
            right->x -= shift;
            right->width += shift;
        }
        renderer_.set_pane_rects(std::move(rects));
        return true;
    }

    if (border.type == HitType::HorizontalBorder) {
        if (delta_y == 0) return false;
        PaneRect* top = nullptr;
        PaneRect* bottom = nullptr;
        for (auto& r : rects) {
            if (r.pane_id == border.top_pane_id) top = &r;
            if (r.pane_id == border.bottom_pane_id) bottom = &r;
        }
        if (!top || !bottom) return false;

        if (delta_y > 0) {
            int16_t max_shift = bottom->height > 1 ? static_cast<int16_t>(bottom->height - 1) : 0;
            int16_t shift = std::min(delta_y, max_shift);
            if (shift <= 0) return false;
            top->height += shift;
            bottom->y += shift;
            bottom->height -= shift;
        } else {
            int16_t max_shift = top->height > 1 ? static_cast<int16_t>(top->height - 1) : 0;
            int16_t shift = std::min(static_cast<int16_t>(-delta_y), max_shift);
            if (shift <= 0) return false;
            top->height -= shift;
            bottom->y -= shift;
            bottom->height += shift;
        }
        renderer_.set_pane_rects(std::move(rects));
        return true;
    }

    return false;
}

ClientApp::ProcessInputResult ClientApp::process_input_bytes(std::span<const uint8_t> bytes) {
    auto events = input_parser_.parse(bytes);
    ProcessInputResult agg;
    for (const auto& ev : events) {
        auto r = process_input_event(ev);
        agg.outbound_messages.insert(agg.outbound_messages.end(),
            std::make_move_iterator(r.outbound_messages.begin()),
            std::make_move_iterator(r.outbound_messages.end()));
        agg.terminal_output.append(r.terminal_output);
        if (r.should_redraw) agg.should_redraw = true;
        if (r.should_detach) agg.should_detach = true;
    }
    return agg;
}

ClientApp::ProcessInputResult ClientApp::process_input_event(const InputEvent& event) {
    if (std::holds_alternative<ClientKeyEvent>(event)) {
        return handle_key_event(std::get<ClientKeyEvent>(event));
    } else if (std::holds_alternative<ClientMouseEvent>(event)) {
        return handle_mouse_event(std::get<ClientMouseEvent>(event));
    } else if (std::holds_alternative<ClientPasteEvent>(event)) {
        return handle_paste_event(std::get<ClientPasteEvent>(event));
    }
    return ProcessInputResult{};
}

ClientApp::ProcessInputResult ClientApp::handle_key_event(const ClientKeyEvent& key) {
    ProcessInputResult out;

    // 1. If Session Chooser is active
    if (session_chooser_.is_active()) {
        auto switch_req = session_chooser_.handle_key(key);
        if (switch_req) {
            out.outbound_messages.push_back(*switch_req);
        }
        out.should_redraw = true;
        return out;
    }

    // 2. If Copy Mode is active
    if (copy_mode_.is_active()) {
        auto res = copy_mode_.handle_key(key);
        if (res.yanked_osc52) {
            out.terminal_output.append(*res.yanked_osc52);
        }
        if (res.slice_req) {
            out.outbound_messages.push_back(*res.slice_req);
        }
        out.should_redraw = true;
        return out;
    }

    // 3. Normal mode / Keybindings engine
    KeyAction action = keybindings_.handle_key(key);
    renderer_.set_prefix_indicator(keybindings_.is_prefix_active());

    switch (action.type) {
        case KeyActionType::None:
            break;
        case KeyActionType::PrefixActivated:
            out.should_redraw = true;
            break;
        case KeyActionType::PrefixCancelled:
            out.should_redraw = true;
            break;
        case KeyActionType::OpenSessionChooser:
            session_chooser_.open(cached_sessions_);
            renderer_.set_prefix_indicator(false);
            out.should_redraw = true;
            break;
        case KeyActionType::EnterCopyMode: {
            uint32_t active_p = renderer_.active_pane();
            uint16_t w = 80, h = 24;
            for (const auto& r : renderer_.pane_rects()) {
                if (r.pane_id == active_p) {
                    w = r.width;
                    h = r.height;
                    break;
                }
            }
            copy_mode_.enter(active_p, w, h);
            renderer_.set_prefix_indicator(false);
            out.should_redraw = true;
            break;
        }
        case KeyActionType::Detach: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgDetachReq req{
                .session_id = attached_session_id_,
                .reason = "client detached"
            };
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            out.should_detach = true;
            break;
        }
        case KeyActionType::NewWindow: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "new-window"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::NextWindow: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "next-window"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::PreviousWindow: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "previous-window"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::SelectWindow: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, std::format("select-window -t {}", action.param)};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::SplitVertical: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "split-window -v"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::SplitHorizontal: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "split-window -h"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::FocusNextPane: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "select-pane -t :.+"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::FocusDirection: {
            std::string dir_str = (action.direction == Direction::Up) ? "-U" :
                                  (action.direction == Direction::Down) ? "-D" :
                                  (action.direction == Direction::Left) ? "-L" : "-R";
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, std::format("select-pane {}", dir_str)};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::ToggleZoom: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "resize-pane -Z"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::KillPane: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "kill-pane"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::PromptRenameSession:
        case KeyActionType::CommandPrompt:
            out.should_redraw = true;
            break;
        case KeyActionType::RenameSession: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, std::format("rename-session {}", action.str_param)};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            out.should_redraw = true;
            break;
        }
        case KeyActionType::PreviousSession: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "switch-client -p"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::NextSession: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, "switch-client -n"};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            break;
        }
        case KeyActionType::ExecuteCommand: {
            uint64_t seq = reconnect_buf_.next_seq();
            protocol::MsgCommandRpcReq req{seq, action.str_param};
            reconnect_buf_.push(seq, req);
            out.outbound_messages.push_back(req);
            out.should_redraw = true;
            break;
        }
        case KeyActionType::ForwardKey: {
            if (action.forwarded_key) {
                uint64_t seq = reconnect_buf_.next_seq();
                auto p_key = InputParser::to_protocol_key(*action.forwarded_key, seq, renderer_.active_pane());
                reconnect_buf_.push(seq, p_key);
                out.outbound_messages.push_back(p_key);
            }
            break;
        }
    }

    return out;
}

ClientApp::ProcessInputResult ClientApp::handle_mouse_event(const ClientMouseEvent& mouse) {
    ProcessInputResult out;
    auto hit = hit_test(mouse.col, mouse.row);

    // 1. Status bar window tab click
    if (hit.type == HitType::WindowTab && mouse.kind == ClientMouseEvent::Kind::Press) {
        uint64_t seq = reconnect_buf_.next_seq();
        protocol::MsgCommandRpcReq req{seq, std::format("select-window -t {}", hit.window_index)};
        reconnect_buf_.push(seq, req);
        out.outbound_messages.push_back(req);
        return out;
    }

    // 2. Split border drag resizing
    if (mouse.kind == ClientMouseEvent::Kind::Press &&
        (hit.type == HitType::VerticalBorder || hit.type == HitType::HorizontalBorder)) {
        is_dragging_border_ = true;
        drag_target_ = hit;
        drag_start_x_ = mouse.col;
        drag_start_y_ = mouse.row;
        return out;
    }

    if (is_dragging_border_) {
        if (mouse.kind == ClientMouseEvent::Kind::Move) {
            int16_t dx = static_cast<int16_t>(mouse.col) - static_cast<int16_t>(drag_start_x_);
            int16_t dy = static_cast<int16_t>(mouse.row) - static_cast<int16_t>(drag_start_y_);
            if (resize_border_drag(drag_target_, dx, dy)) {
                drag_start_x_ = mouse.col;
                drag_start_y_ = mouse.row;
                out.should_redraw = true;
            }
            return out;
        } else if (mouse.kind == ClientMouseEvent::Kind::Release) {
            is_dragging_border_ = false;
            out.should_redraw = true;
            return out;
        }
    }

    // 3. Pane hit
    if (hit.type == HitType::Pane) {
        // Switch active pane on click
        if (mouse.kind == ClientMouseEvent::Kind::Press && hit.pane_id != renderer_.active_pane()) {
            renderer_.set_active_pane(hit.pane_id);
            out.should_redraw = true;
        }

        // Wheel scroll handling
        if (mouse.kind == ClientMouseEvent::Kind::WheelUp || mouse.kind == ClientMouseEvent::Kind::WheelDown) {
            const auto* grid = renderer_.get_pane_grid(hit.pane_id);
            bool alt_or_mouse_reporting = grid && (grid->modes & 1); // Mode bit 0 = alt-screen / mouse reporting

            if (!alt_or_mouse_reporting) {
                // Enter Copy Mode and scroll
                if (!copy_mode_.is_active()) {
                    uint16_t w = 80, h = 24;
                    for (const auto& r : renderer_.pane_rects()) {
                        if (r.pane_id == hit.pane_id) {
                            w = r.width;
                            h = r.height;
                            break;
                        }
                    }
                    copy_mode_.enter(hit.pane_id, w, h);
                }

                ClientKeyEvent sim_key;
                sim_key.key_code = (mouse.kind == ClientMouseEvent::Kind::WheelUp) ? 'k' : 'j';
                auto r = copy_mode_.handle_key(sim_key);
                if (r.slice_req) {
                    out.outbound_messages.push_back(*r.slice_req);
                }
                out.should_redraw = true;
                return out;
            }
        }

        // Forward mouse event to pane
        ClientMouseEvent translated_mouse = mouse;
        translated_mouse.col = hit.local_col;
        translated_mouse.row = hit.local_row;

        uint64_t seq = reconnect_buf_.next_seq();
        auto p_mouse = InputParser::to_protocol_mouse(translated_mouse, seq, hit.pane_id);
        reconnect_buf_.push(seq, p_mouse);
        out.outbound_messages.push_back(p_mouse);
    }

    return out;
}

ClientApp::ProcessInputResult ClientApp::handle_paste_event(const ClientPasteEvent& paste) {
    ProcessInputResult out;
    uint64_t seq = reconnect_buf_.next_seq();
    auto p_paste = InputParser::to_protocol_paste(paste, seq, renderer_.active_pane());
    reconnect_buf_.push(seq, p_paste);
    out.outbound_messages.push_back(p_paste);
    return out;
}

ClientApp::ServerMsgResult ClientApp::handle_server_message(const protocol::ProtocolMessage& msg) {
    ServerMsgResult res;

    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, protocol::MsgServerHello>) {
            reconnect_buf_.ack(m.last_client_seq_acked);
            cached_sessions_ = m.session_list;
        } else if constexpr (std::is_same_v<T, protocol::MsgListSessionsResp>) {
            cached_sessions_ = m.sessions;
            if (session_chooser_.is_active()) {
                session_chooser_.open(cached_sessions_);
                res.should_redraw = true;
            }
        } else if constexpr (std::is_same_v<T, protocol::MsgLayoutUpdate>) {
            renderer_.apply_layout(m);
            res.should_redraw = true;
        } else if constexpr (std::is_same_v<T, protocol::MsgScreenSnapshot>) {
            renderer_.apply_snapshot(m);
            res.should_redraw = true;
        } else if constexpr (std::is_same_v<T, protocol::MsgFrameDiff>) {
            reconnect_buf_.ack(m.acked_client_seq);
            renderer_.apply_diff(m);
            res.should_redraw = true;
        } else if constexpr (std::is_same_v<T, protocol::MsgScrollbackSliceResp>) {
            copy_mode_.receive_slice_resp(m);
            res.should_redraw = true;
        } else if constexpr (std::is_same_v<T, protocol::MsgPassthroughEvent>) {
            res.terminal_output = Renderer::handle_passthrough(m);
        } else if constexpr (std::is_same_v<T, protocol::MsgSwitchSessionResp>) {
            if (m.success) {
                attached_session_id_ = m.session_id;
                res.should_redraw = true;
            }
        }
    }, msg);

    return res;
}

std::vector<protocol::ProtocolMessage> ClientApp::prepare_reconnect_replay(uint64_t last_server_acked_seq) {
    auto unacked = reconnect_buf_.get_unacked(last_server_acked_seq);
    std::vector<protocol::ProtocolMessage> replay;
    replay.reserve(unacked.size());
    for (auto& item : unacked) {
        replay.push_back(std::move(item.message));
    }
    return replay;
}

std::string ClientApp::render_frame(bool use_decset_2026) {
    renderer_.compose_frame();

    if (session_chooser_.is_active()) {
        // Blend modal session chooser onto shadow screen
        // Create a copy of screen buffer from renderer composite
        std::vector<protocol::Cell> composite(static_cast<size_t>(renderer_.term_cols()) * renderer_.term_rows());
        for (uint16_t r = 0; r < renderer_.term_rows(); ++r) {
            for (uint16_t c = 0; c < renderer_.term_cols(); ++c) {
                composite[static_cast<size_t>(r) * renderer_.term_cols() + c] = renderer_.get_composite_cell(c, r);
            }
        }
        session_chooser_.render_overlay(composite, renderer_.term_cols(), renderer_.term_rows());
        // For testing / rendering, update composite cells
        // To keep it simple, apply composite back or render directly
    }

    return renderer_.render_frame_vt(use_decset_2026);
}

} // namespace tmux2::client
