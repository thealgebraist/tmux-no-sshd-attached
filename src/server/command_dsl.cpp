#include "src/server/command_dsl.hpp"
#include "src/server/client_state.hpp"

#include <charconv>
#include <sstream>

namespace tmux2::server {

std::vector<std::string> CommandDsl::tokenize(std::string_view command_str) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    for (size_t i = 0; i < command_str.size(); ++i) {
        char ch = command_str[i];

        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }

        if (ch == '\\' && !in_single_quote) {
            escaped = true;
            continue;
        }

        if (ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if (ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if ((ch == ' ' || ch == '\t') && !in_single_quote && !in_double_quote) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}

static std::shared_ptr<Session> resolve_session(
    SessionManager& mgr,
    std::string_view target,
    uint32_t current_session_id) {
    if (target.empty()) {
        if (current_session_id != 0) {
            auto s = mgr.find_session(current_session_id);
            if (s) return s;
        }
        if (mgr.session_count() > 0) {
            return mgr.sessions().begin()->second;
        }
        return nullptr;
    }

    // Target might be "session_name:window.pane"
    size_t colon = target.find(':');
    std::string_view sname = (colon != std::string_view::npos) ? target.substr(0, colon) : target;

    if (!sname.empty()) {
        auto by_name = mgr.find_session_by_name(sname);
        if (by_name) return by_name;

        uint32_t sid = 0;
        auto [ptr, ec] = std::from_chars(sname.data(), sname.data() + sname.size(), sid);
        if (ec == std::errc() && sid != 0) {
            auto by_id = mgr.find_session(sid);
            if (by_id) return by_id;
        }
    }

    if (current_session_id != 0) {
        return mgr.find_session(current_session_id);
    }
    return nullptr;
}

static std::pair<std::shared_ptr<Window>, std::shared_ptr<Pane>> resolve_window_and_pane(
    std::shared_ptr<Session> session,
    std::string_view target) {
    if (!session) return {nullptr, nullptr};

    if (target.empty()) {
        auto win = session->active_window();
        auto pane = win ? win->active_pane() : nullptr;
        return {win, pane};
    }

    if (target.find('.') == std::string_view::npos && target.find(':') == std::string_view::npos) {
        uint32_t pid = 0;
        if (target.starts_with('%')) {
            std::from_chars(target.data() + 1, target.data() + target.size(), pid);
        } else {
            std::from_chars(target.data(), target.data() + target.size(), pid);
        }
        if (pid != 0) {
            auto awin = session->active_window();
            if (awin) {
                auto p = awin->find_pane(pid);
                if (p) {
                    return {awin, p};
                }
            }
        }
    }

    size_t colon = target.find(':');
    std::string_view win_pane_str = (colon != std::string_view::npos) ? target.substr(colon + 1) : target;

    size_t dot = win_pane_str.find('.');
    std::string_view win_part = (dot != std::string_view::npos) ? win_pane_str.substr(0, dot) : win_pane_str;
    std::string_view pane_part = (dot != std::string_view::npos) ? win_pane_str.substr(dot + 1) : "";

    std::shared_ptr<Window> win;
    if (!win_part.empty()) {
        uint32_t wid_or_idx = 0;
        auto [ptr, ec] = std::from_chars(win_part.data(), win_part.data() + win_part.size(), wid_or_idx);
        if (ec == std::errc()) {
            if (wid_or_idx < session->window_count()) {
                win = session->windows()[wid_or_idx];
            } else {
                win = session->find_window(wid_or_idx);
            }
        }
        if (!win) {
            for (const auto& w : session->windows()) {
                if (w->name() == win_part) {
                    win = w;
                    break;
                }
            }
        }
    }

    if (!win) {
        win = session->active_window();
    }

    std::shared_ptr<Pane> pane;
    if (win && !pane_part.empty()) {
        uint32_t pid = 0;
        auto [ptr, ec] = std::from_chars(pane_part.data(), pane_part.data() + pane_part.size(), pid);
        if (ec == std::errc()) {
            pane = win->find_pane(pid);
        }
    }

    if (win && !pane) {
        pane = win->active_pane();
    }

    return {win, pane};
}

protocol::MsgCommandRpcResp CommandDsl::execute(
    SessionManager& mgr,
    std::string_view command_str,
    uint64_t rpc_id,
    uint32_t current_session_id,
    bool mock_pty,
    ClientState* client) {
    protocol::MsgCommandRpcResp resp;
    resp.rpc_id = rpc_id;
    resp.exit_code = 0;

    auto tokens = tokenize(command_str);
    if (tokens.empty()) {
        return resp;
    }

    const std::string& cmd = tokens[0];

    if (cmd == "new-session" || cmd == "new") {
        protocol::MsgNewSessionReq req;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-d") {
                req.detached = true;
            } else if (tokens[i] == "-s" && i + 1 < tokens.size()) {
                req.name = tokens[++i];
            } else if (tokens[i] == "-n" && i + 1 < tokens.size()) {
                req.initial_window_name = tokens[++i];
            } else if (tokens[i] == "-c" && i + 1 < tokens.size()) {
                req.cwd = tokens[++i];
            } else if (req.command.empty()) {
                req.command = tokens[i];
            }
        }
        auto s = mgr.new_session(req, 80, 24, mock_pty);
        if (s) {
            resp.stdout_data = s->name() + "\n";
        } else {
            resp.exit_code = 1;
            resp.stderr_data = "Failed to create session\n";
        }
        return resp;
    }

    if (cmd == "list-sessions" || cmd == "ls") {
        auto list = mgr.list_sessions();
        std::ostringstream ss;
        for (const auto& s : list) {
            ss << s.name << ": " << s.window_count << " windows"
               << " (created " << s.created_at << ")"
               << (s.attached_clients > 0 ? " (attached)" : "") << "\n";
        }
        resp.stdout_data = ss.str();
        return resp;
    }

    if (cmd == "attach-session" || cmd == "attach" || cmd == "a") {
        std::string target;
        bool detach_others = false;
        bool read_only = false;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (tokens[i] == "-d") {
                detach_others = true;
            } else if (tokens[i] == "-r") {
                read_only = true;
            } else if (target.empty() && !tokens[i].starts_with("-")) {
                target = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        if (!s) {
            resp.exit_code = 1;
            resp.stderr_data = "Session not found: " + target + "\n";
            return resp;
        }
        if (detach_others) {
            auto others = s->attached_clients();
            for (const auto& att : others) {
                if (!client || att.client_id != client->client_id()) {
                    s->detach_client(att.client_id);
                }
            }
        }
        if (client) {
            s->attach_client(
                client->client_id(),
                read_only,
                client->cols() > 0 ? client->cols() : 80,
                client->rows() > 0 ? client->rows() : 24,
                detach_others
            );
            client->set_attached_session_id(s->session_id());
            client->set_read_only(read_only);
        } else if (read_only) {
            for (const auto& att : s->attached_clients()) {
                s->attach_client(att.client_id, true, att.cols, att.rows);
            }
        }
        return resp;
    }

    if (cmd == "kill-session") {
        std::string target;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (target.empty()) {
                target = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        if (!s) {
            resp.exit_code = 1;
            resp.stderr_data = "Session not found: " + target + "\n";
            return resp;
        }
        mgr.kill_session(s->session_id());
        return resp;
    }

    if (cmd == "rename-session") {
        std::string target;
        std::string new_name;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (new_name.empty()) {
                new_name = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        if (!s || new_name.empty()) {
            resp.exit_code = 1;
            resp.stderr_data = "Usage: rename-session [-t target] new_name\n";
            return resp;
        }
        mgr.rename_session(s->session_id(), new_name);
        return resp;
    }

    if (cmd == "new-window" || cmd == "neww") {
        std::string target;
        std::string win_name;
        std::string cwd;
        std::string command;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (tokens[i] == "-n" && i + 1 < tokens.size()) {
                win_name = tokens[++i];
            } else if (tokens[i] == "-c" && i + 1 < tokens.size()) {
                cwd = tokens[++i];
            } else if (command.empty()) {
                command = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        if (!s) {
            resp.exit_code = 1;
            resp.stderr_data = "Session not found\n";
            return resp;
        }

        uint32_t wid = mgr.allocate_window_id();
        uint32_t pid = mgr.allocate_pane_id();
        if (win_name.empty()) {
            win_name = std::to_string(s->window_count());
        }

        if (mock_pty) {
            s->create_window_mock(wid, win_name, pid);
        } else {
            pty::PtyOptions opt;
            opt.cwd = cwd;
            opt.command = command;
            s->create_window(wid, win_name, pid, opt);
        }
        return resp;
    }

    if (cmd == "select-window" || cmd == "selectw") {
        std::string target;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (target.empty()) {
                target = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        if (!s) {
            resp.exit_code = 1;
            resp.stderr_data = "Session not found\n";
            return resp;
        }
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (win) {
            s->select_window(win->window_id());
        } else {
            resp.exit_code = 1;
            resp.stderr_data = "Window not found\n";
        }
        return resp;
    }

    if (cmd == "next-window" || cmd == "next") {
        auto s = resolve_session(mgr, "", current_session_id);
        if (s) {
            s->next_window();
        }
        return resp;
    }

    if (cmd == "previous-window" || cmd == "prev") {
        auto s = resolve_session(mgr, "", current_session_id);
        if (s) {
            s->previous_window();
        }
        return resp;
    }

    if (cmd == "rename-window") {
        std::string target;
        std::string new_name;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (new_name.empty()) {
                new_name = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (!win || new_name.empty()) {
            resp.exit_code = 1;
            resp.stderr_data = "Usage: rename-window [-t target] new_name\n";
            return resp;
        }
        win->set_name(new_name);
        return resp;
    }

    if (cmd == "split-window" || cmd == "splitw") {
        bool vertical = true; // default in tmux splitw is vertical unless -h
        std::string target;
        std::string cwd;
        std::string command;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-h") {
                vertical = false; // Horizontal split in tmux divides width
            } else if (tokens[i] == "-v") {
                vertical = true;  // Vertical split in tmux divides height
            } else if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (tokens[i] == "-c" && i + 1 < tokens.size()) {
                cwd = tokens[++i];
            } else if (command.empty()) {
                command = tokens[i];
            }
        }

        auto s = resolve_session(mgr, target, current_session_id);
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (!win || !pane) {
            resp.exit_code = 1;
            resp.stderr_data = "Target pane/window not found\n";
            return resp;
        }

        uint32_t new_pid = mgr.allocate_pane_id();
        if (mock_pty) {
            win->split_pane_mock(pane->pane_id(), new_pid, vertical);
        } else {
            pty::PtyOptions opt;
            opt.cwd = cwd;
            opt.command = command;
            win->split_pane(pane->pane_id(), new_pid, vertical, opt);
        }
        return resp;
    }

    if (cmd == "select-pane" || cmd == "selectp") {
        std::string target;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (target.empty()) {
                target = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (win && pane) {
            win->select_pane(pane->pane_id());
        } else {
            resp.exit_code = 1;
            resp.stderr_data = "Pane not found\n";
        }
        return resp;
    }

    if (cmd == "resize-pane" || cmd == "resizep") {
        std::string target;
        bool zoom = false;
        ResizeDirection dir = ResizeDirection::Down;
        int amount = 5;
        bool has_dir = false;

        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-Z") {
                zoom = true;
            } else if (tokens[i] == "-U") {
                dir = ResizeDirection::Up;
                has_dir = true;
                if (i + 1 < tokens.size() && !tokens[i + 1].starts_with("-")) {
                    std::from_chars(tokens[i + 1].data(), tokens[i + 1].data() + tokens[i + 1].size(), amount);
                    ++i;
                }
            } else if (tokens[i] == "-D") {
                dir = ResizeDirection::Down;
                has_dir = true;
                if (i + 1 < tokens.size() && !tokens[i + 1].starts_with("-")) {
                    std::from_chars(tokens[i + 1].data(), tokens[i + 1].data() + tokens[i + 1].size(), amount);
                    ++i;
                }
            } else if (tokens[i] == "-L") {
                dir = ResizeDirection::Left;
                has_dir = true;
                if (i + 1 < tokens.size() && !tokens[i + 1].starts_with("-")) {
                    std::from_chars(tokens[i + 1].data(), tokens[i + 1].data() + tokens[i + 1].size(), amount);
                    ++i;
                }
            } else if (tokens[i] == "-R") {
                dir = ResizeDirection::Right;
                has_dir = true;
                if (i + 1 < tokens.size() && !tokens[i + 1].starts_with("-")) {
                    std::from_chars(tokens[i + 1].data(), tokens[i + 1].data() + tokens[i + 1].size(), amount);
                    ++i;
                }
            } else if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            }
        }

        auto s = resolve_session(mgr, target, current_session_id);
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (!win || !pane) {
            resp.exit_code = 1;
            resp.stderr_data = "Pane not found\n";
            return resp;
        }

        if (zoom) {
            win->toggle_zoom(pane->pane_id());
        } else if (has_dir) {
            win->resize_pane(pane->pane_id(), dir, amount);
        }
        return resp;
    }

    if (cmd == "zoom-pane") {
        std::string target;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (target.empty()) {
                target = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (win && pane) {
            win->toggle_zoom(pane->pane_id());
        } else {
            resp.exit_code = 1;
            resp.stderr_data = "Pane not found\n";
        }
        return resp;
    }

    if (cmd == "kill-pane" || cmd == "killp") {
        std::string target;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (target.empty()) {
                target = tokens[i];
            }
        }
        auto s = resolve_session(mgr, target, current_session_id);
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (!win || !pane) {
            resp.exit_code = 1;
            resp.stderr_data = "Pane not found\n";
            return resp;
        }
        win->kill_pane(pane->pane_id());
        if (win->pane_count() == 0) {
            s->kill_window(win->window_id());
            if (s->window_count() == 0) {
                mgr.kill_session(s->session_id());
            }
        }
        return resp;
    }

    if (cmd == "send-keys" || cmd == "send") {
        std::string target;
        size_t start_idx = 1;
        if (tokens.size() > 2 && tokens[1] == "-t") {
            target = tokens[2];
            start_idx = 3;
        }

        auto s = resolve_session(mgr, target, current_session_id);
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (!pane) {
            resp.exit_code = 1;
            resp.stderr_data = "Pane not found\n";
            return resp;
        }

        for (size_t i = start_idx; i < tokens.size(); ++i) {
            const auto& t = tokens[i];
            if (t == "Enter") {
                pane->write_input("\r");
                if (!pane->has_pty()) {
                    pane->feed_output("\r\n");
                }
            } else if (t == "C-c") {
                pane->write_input("\x03");
                if (!pane->has_pty()) {
                    pane->feed_output("^C\r\n");
                }
            } else if (t == "C-d") {
                pane->write_input("\x04");
            } else {
                pane->write_input(t);
                if (!pane->has_pty()) {
                    pane->feed_output(t);
                }
            }
        }
        if (pane->has_pty()) {
            pane->read_pty();
        }
        return resp;
    }

    if (cmd == "capture-pane" || cmd == "capturep") {
        std::string target;
        int start_line = 0;
        int end_line = -1;
        bool has_start = false;

        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "-t" && i + 1 < tokens.size()) {
                target = tokens[++i];
            } else if (tokens[i] == "-p") {
                // Print to stdout (default behavior here)
            } else if (tokens[i] == "-S" && i + 1 < tokens.size()) {
                has_start = true;
                std::from_chars(tokens[i + 1].data(), tokens[i + 1].data() + tokens[i + 1].size(), start_line);
                ++i;
            } else if (tokens[i] == "-E" && i + 1 < tokens.size()) {
                std::from_chars(tokens[i + 1].data(), tokens[i + 1].data() + tokens[i + 1].size(), end_line);
                ++i;
            }
        }

        auto s = resolve_session(mgr, target, current_session_id);
        auto [win, pane] = resolve_window_and_pane(s, target);
        if (!pane) {
            resp.exit_code = 1;
            resp.stderr_data = "Pane not found\n";
            return resp;
        }

        resp.stdout_data = pane->capture_text(has_start, start_line, end_line);
        return resp;
    }

    resp.exit_code = 1;
    resp.stderr_data = "Unknown command: " + cmd + "\n";
    return resp;
}

} // namespace tmux2::server
