#include "src/server/session.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace tmux2::server {

Session::Session(uint32_t session_id, std::string_view name,
                 uint16_t cols, uint16_t rows)
    : session_id_(session_id), name_(name),
      cols_(cols > 0 ? cols : 80), rows_(rows > 0 ? rows : 24) {
    created_at_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::shared_ptr<Session> Session::create(
    uint32_t session_id, std::string_view name,
    uint32_t initial_window_id, uint32_t initial_pane_id,
    std::string_view initial_window_name,
    uint16_t cols, uint16_t rows,
    const pty::PtyOptions& options) {
    auto session = std::make_shared<Session>(session_id, name, cols, rows);
    auto win = session->create_window(initial_window_id, initial_window_name, initial_pane_id, options);
    session->active_window_id_ = initial_window_id;
    return session;
}

std::shared_ptr<Session> Session::create_mock(
    uint32_t session_id, std::string_view name,
    uint32_t initial_window_id, uint32_t initial_pane_id,
    std::string_view initial_window_name,
    uint16_t cols, uint16_t rows) {
    auto session = std::make_shared<Session>(session_id, name, cols, rows);
    auto win = session->create_window_mock(initial_window_id, initial_window_name, initial_pane_id);
    session->active_window_id_ = initial_window_id;
    return session;
}

std::shared_ptr<Window> Session::create_window(
    uint32_t window_id, std::string_view name, uint32_t initial_pane_id,
    const pty::PtyOptions& options) {
    uint32_t next_idx = static_cast<uint32_t>(windows_.size());
    auto win = Window::create(window_id, next_idx, name, initial_pane_id, cols_, rows_, options);
    windows_.push_back(win);
    active_window_id_ = window_id;
    return win;
}

std::shared_ptr<Window> Session::create_window_mock(
    uint32_t window_id, std::string_view name, uint32_t initial_pane_id) {
    uint32_t next_idx = static_cast<uint32_t>(windows_.size());
    auto win = Window::create_mock(window_id, next_idx, name, initial_pane_id, cols_, rows_);
    windows_.push_back(win);
    active_window_id_ = window_id;
    return win;
}

bool Session::kill_window(uint32_t window_id) {
    auto it = std::find_if(windows_.begin(), windows_.end(),
        [window_id](const auto& w) { return w->window_id() == window_id; });
    if (it == windows_.end()) return false;

    windows_.erase(it);

    // Re-index remaining windows
    for (size_t i = 0; i < windows_.size(); ++i) {
        windows_[i]->set_index(static_cast<uint32_t>(i));
    }

    if (active_window_id_ == window_id) {
        if (!windows_.empty()) {
            active_window_id_ = windows_.front()->window_id();
        } else {
            active_window_id_ = 0;
        }
    }

    return true;
}

bool Session::select_window(uint32_t window_id) {
    auto win = find_window(window_id);
    if (!win) return false;
    last_window_id_ = active_window_id_;
    active_window_id_ = window_id;
    return true;
}

bool Session::select_window_by_index(uint32_t index) {
    if (index < windows_.size()) {
        last_window_id_ = active_window_id_;
        active_window_id_ = windows_[index]->window_id();
        return true;
    }
    return false;
}

bool Session::next_window() {
    if (windows_.size() <= 1) return false;
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (windows_[i]->window_id() == active_window_id_) {
            size_t next_idx = (i + 1) % windows_.size();
            last_window_id_ = active_window_id_;
            active_window_id_ = windows_[next_idx]->window_id();
            return true;
        }
    }
    return false;
}

bool Session::previous_window() {
    if (windows_.size() <= 1) return false;
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (windows_[i]->window_id() == active_window_id_) {
            size_t prev_idx = (i == 0) ? (windows_.size() - 1) : (i - 1);
            last_window_id_ = active_window_id_;
            active_window_id_ = windows_[prev_idx]->window_id();
            return true;
        }
    }
    return false;
}

std::shared_ptr<Window> Session::active_window() const {
    auto w = find_window(active_window_id_);
    if (!w && !windows_.empty()) {
        return windows_.front();
    }
    return w;
}

std::shared_ptr<Window> Session::find_window(uint32_t window_id) const {
    for (const auto& w : windows_) {
        if (w->window_id() == window_id) return w;
    }
    return nullptr;
}

std::shared_ptr<Pane> Session::find_pane(uint32_t pane_id) const {
    for (const auto& w : windows_) {
        auto p = w->find_pane(pane_id);
        if (p) return p;
    }
    return nullptr;
}

std::vector<uint64_t> Session::attach_client(
    uint64_t client_id, bool read_only, uint16_t cols, uint16_t rows,
    bool detach_others) {
    std::vector<uint64_t> detached;

    if (detach_others) {
        for (const auto& att : attached_clients_) {
            if (att.client_id != client_id) {
                detached.push_back(att.client_id);
            }
        }
        attached_clients_.erase(
            std::remove_if(attached_clients_.begin(), attached_clients_.end(),
                [client_id](const auto& att) { return att.client_id != client_id; }),
            attached_clients_.end());
    }

    auto it = std::find_if(attached_clients_.begin(), attached_clients_.end(),
        [client_id](const auto& att) { return att.client_id == client_id; });

    if (it != attached_clients_.end()) {
        it->read_only = read_only;
        it->cols = cols;
        it->rows = rows;
        it->last_interaction = std::chrono::steady_clock::now();
    } else {
        attached_clients_.push_back(ClientAttachment{
            .client_id = client_id,
            .read_only = read_only,
            .cols = cols,
            .rows = rows,
            .last_interaction = std::chrono::steady_clock::now()
        });
    }

    if (size_policy_ == "latest" && cols > 0 && rows > 0) {
        resize(cols, rows);
    } else {
        apply_size_policy(client_id);
    }

    return detached;
}

bool Session::detach_client(uint64_t client_id) {
    auto it = std::find_if(attached_clients_.begin(), attached_clients_.end(),
        [client_id](const auto& att) { return att.client_id == client_id; });
    if (it != attached_clients_.end()) {
        attached_clients_.erase(it);
        apply_size_policy();
        return true;
    }
    return false;
}

bool Session::has_client(uint64_t client_id) const noexcept {
    return std::any_of(attached_clients_.begin(), attached_clients_.end(),
        [client_id](const auto& att) { return att.client_id == client_id; });
}

bool Session::client_interacted(uint64_t client_id, uint16_t cols, uint16_t rows) {
    auto it = std::find_if(attached_clients_.begin(), attached_clients_.end(),
        [client_id](const auto& att) { return att.client_id == client_id; });
    if (it != attached_clients_.end()) {
        it->last_interaction = std::chrono::steady_clock::now();
        it->cols = cols;
        it->rows = rows;

        if (size_policy_ == "latest" && !it->read_only && cols > 0 && rows > 0) {
            if (cols != cols_ || rows != rows_) {
                resize(cols, rows);
                return true;
            }
        }
    }
    return false;
}

void Session::apply_size_policy(uint64_t interacting_client_id) {
    if (attached_clients_.empty()) return;

    if (size_policy_ == "latest") {
        if (interacting_client_id != 0) {
            auto it = std::find_if(attached_clients_.begin(), attached_clients_.end(),
                [interacting_client_id](const auto& att) { return att.client_id == interacting_client_id; });
            if (it != attached_clients_.end() && it->cols > 0 && it->rows > 0) {
                resize(it->cols, it->rows);
                return;
            }
        }
        // Fallback to most recently interacted client
        auto latest_it = std::max_element(attached_clients_.begin(), attached_clients_.end(),
            [](const auto& a, const auto& b) { return a.last_interaction < b.last_interaction; });
        if (latest_it != attached_clients_.end() && latest_it->cols > 0 && latest_it->rows > 0) {
            resize(latest_it->cols, latest_it->rows);
        }
    } else if (size_policy_ == "smallest") {
        uint16_t min_c = 10000;
        uint16_t min_r = 10000;
        for (const auto& att : attached_clients_) {
            if (att.cols > 0 && att.cols < min_c) min_c = att.cols;
            if (att.rows > 0 && att.rows < min_r) min_r = att.rows;
        }
        if (min_c < 10000 && min_r < 10000) resize(min_c, min_r);
    } else if (size_policy_ == "largest") {
        uint16_t max_c = 0;
        uint16_t max_r = 0;
        for (const auto& att : attached_clients_) {
            if (att.cols > max_c) max_c = att.cols;
            if (att.rows > max_r) max_r = att.rows;
        }
        if (max_c > 0 && max_r > 0) resize(max_c, max_r);
    }
}

void Session::resize(uint16_t cols, uint16_t rows) {
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;
    cols_ = cols;
    rows_ = rows;
    for (auto& w : windows_) {
        w->resize(cols, rows);
    }
}

protocol::SessionSummary Session::get_summary() const {
    protocol::SessionSummary summary;
    summary.session_id = session_id_;
    summary.name = name_;
    summary.created_at = created_at_;
    summary.window_count = static_cast<uint32_t>(windows_.size());
    summary.attached_clients = static_cast<uint32_t>(attached_clients_.size());
    summary.preview_summary = preview_summary();
    return summary;
}

std::string Session::preview_summary() const {
    std::ostringstream ss;
    auto win = active_window();
    if (win) {
        ss << win->name() << " [" << win->pane_count() << " pane"
           << (win->pane_count() == 1 ? "" : "s") << "]";
    } else {
        ss << "(empty)";
    }
    return ss.str();
}

} // namespace tmux2::server
