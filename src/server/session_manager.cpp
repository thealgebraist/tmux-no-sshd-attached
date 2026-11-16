#include "src/server/session_manager.hpp"

#include <algorithm>

namespace tmux2::server {

std::shared_ptr<Session> SessionManager::new_session(
    const protocol::MsgNewSessionReq& req,
    uint16_t default_cols, uint16_t default_rows,
    bool mock_pty) {
    uint32_t sid = allocate_session_id();
    uint32_t wid = allocate_window_id();
    uint32_t pid = allocate_pane_id();

    std::string sname = req.name.empty() ? ("session-" + std::to_string(sid)) : req.name;
    std::string wname = req.initial_window_name.empty() ? "0" : req.initial_window_name;

    pty::PtyOptions pty_opts;
    pty_opts.cols = default_cols;
    pty_opts.rows = default_rows;
    pty_opts.cwd = req.cwd;
    pty_opts.command = req.command;
    for (const auto& [k, v] : req.env) {
        pty_opts.env[k] = v;
    }

    std::shared_ptr<Session> session;
    if (mock_pty) {
        session = Session::create_mock(sid, sname, wid, pid, wname, default_cols, default_rows);
    } else {
        session = Session::create(sid, sname, wid, pid, wname, default_cols, default_rows, pty_opts);
    }

    sessions_[sid] = session;
    return session;
}

bool SessionManager::kill_session(uint32_t session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    sessions_.erase(it);
    return true;
}

bool SessionManager::rename_session(uint32_t session_id, std::string_view new_name) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    it->second->set_name(new_name);
    return true;
}

std::shared_ptr<Session> SessionManager::find_session(uint32_t session_id) const {
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) return it->second;
    return nullptr;
}

std::shared_ptr<Session> SessionManager::find_session_by_name(std::string_view name) const {
    for (const auto& [id, s] : sessions_) {
        if (s->name() == name) return s;
    }
    return nullptr;
}

std::shared_ptr<Session> SessionManager::find_session_or_default(uint32_t session_id, std::string_view name) const {
    if (session_id != 0) {
        auto s = find_session(session_id);
        if (s) return s;
    }
    if (!name.empty()) {
        auto s = find_session_by_name(name);
        if (s) return s;
    }
    if (!sessions_.empty()) {
        return sessions_.begin()->second;
    }
    return nullptr;
}

std::pair<std::shared_ptr<Session>, std::shared_ptr<Pane>> SessionManager::find_pane(uint32_t pane_id) const {
    for (const auto& [sid, session] : sessions_) {
        auto pane = session->find_pane(pane_id);
        if (pane) {
            return {session, pane};
        }
    }
    return {nullptr, nullptr};
}

std::vector<protocol::SessionSummary> SessionManager::list_sessions(std::string_view filter) const {
    std::vector<protocol::SessionSummary> list;
    list.reserve(sessions_.size());

    for (const auto& [id, session] : sessions_) {
        if (filter.empty() || session->name().find(filter) != std::string::npos) {
            list.push_back(session->get_summary());
        }
    }

    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.session_id < b.session_id;
    });

    return list;
}

} // namespace tmux2::server
