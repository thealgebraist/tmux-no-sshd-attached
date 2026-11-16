#include "src/server/window.hpp"

#include <algorithm>

namespace tmux2::server {

Window::Window(uint32_t window_id, uint32_t index, std::string_view name,
               uint16_t cols, uint16_t rows)
    : window_id_(window_id), index_(index), name_(name),
      cols_(cols > 0 ? cols : 80), rows_(rows > 0 ? rows : 24) {
}

std::shared_ptr<Window> Window::create(
    uint32_t window_id, uint32_t index, std::string_view name,
    uint32_t initial_pane_id, uint16_t cols, uint16_t rows,
    const pty::PtyOptions& options) {
    auto win = std::make_shared<Window>(window_id, index, name, cols, rows);
    win->layout_.reset(initial_pane_id);

    auto pane = Pane::create(initial_pane_id, cols, rows, options);
    win->panes_[initial_pane_id] = pane;
    win->active_pane_id_ = initial_pane_id;
    win->apply_layout();
    return win;
}

std::shared_ptr<Window> Window::create_mock(
    uint32_t window_id, uint32_t index, std::string_view name,
    uint32_t initial_pane_id, uint16_t cols, uint16_t rows) {
    auto win = std::make_shared<Window>(window_id, index, name, cols, rows);
    win->layout_.reset(initial_pane_id);

    auto pane = Pane::create_mock(initial_pane_id, cols, rows);
    win->panes_[initial_pane_id] = pane;
    win->active_pane_id_ = initial_pane_id;
    win->apply_layout();
    return win;
}

std::shared_ptr<Pane> Window::active_pane() const {
    auto it = panes_.find(active_pane_id_);
    if (it != panes_.end()) {
        return it->second;
    }
    if (!panes_.empty()) {
        return panes_.begin()->second;
    }
    return nullptr;
}

std::shared_ptr<Pane> Window::find_pane(uint32_t pane_id) const {
    auto it = panes_.find(pane_id);
    if (it != panes_.end()) {
        return it->second;
    }
    return nullptr;
}

void Window::apply_layout() {
    layout_.calculate(cols_, rows_, zoomed_pane_id_);
    for (auto& [pid, pane] : panes_) {
        auto r = layout_.get_pane_rect(pid);
        if (r) {
            pane->set_rect(*r);
        }
    }
}

std::shared_ptr<Pane> Window::split_pane(
    uint32_t target_pane_id, uint32_t new_pane_id, bool vertical,
    const pty::PtyOptions& options) {
    if (panes_.find(target_pane_id) == panes_.end()) {
        if (!panes_.empty()) {
            target_pane_id = active_pane_id_;
        } else {
            return nullptr;
        }
    }

    SplitType st = vertical ? SplitType::Vertical : SplitType::Horizontal;
    if (!layout_.split_pane(target_pane_id, new_pane_id, st, 0.5)) {
        return nullptr;
    }

    // Unzoom if zoomed
    zoomed_pane_id_ = 0;

    auto new_pane = Pane::create(new_pane_id, cols_ / 2, rows_ / 2, options);
    panes_[new_pane_id] = new_pane;
    active_pane_id_ = new_pane_id;

    apply_layout();
    return new_pane;
}

std::shared_ptr<Pane> Window::split_pane_mock(
    uint32_t target_pane_id, uint32_t new_pane_id, bool vertical) {
    if (panes_.find(target_pane_id) == panes_.end()) {
        if (!panes_.empty()) {
            target_pane_id = active_pane_id_;
        } else {
            return nullptr;
        }
    }

    SplitType st = vertical ? SplitType::Vertical : SplitType::Horizontal;
    if (!layout_.split_pane(target_pane_id, new_pane_id, st, 0.5)) {
        return nullptr;
    }

    zoomed_pane_id_ = 0;

    auto new_pane = Pane::create_mock(new_pane_id, cols_ / 2, rows_ / 2);
    panes_[new_pane_id] = new_pane;
    active_pane_id_ = new_pane_id;

    apply_layout();
    return new_pane;
}

bool Window::kill_pane(uint32_t pane_id) {
    auto it = panes_.find(pane_id);
    if (it == panes_.end()) return false;

    panes_.erase(it);
    layout_.remove_pane(pane_id);

    if (zoomed_pane_id_ == pane_id) {
        zoomed_pane_id_ = 0;
    }

    if (active_pane_id_ == pane_id) {
        if (!panes_.empty()) {
            active_pane_id_ = panes_.begin()->first;
        } else {
            active_pane_id_ = 0;
        }
    }

    apply_layout();
    return true;
}

bool Window::select_pane(uint32_t pane_id) {
    if (panes_.find(pane_id) != panes_.end()) {
        active_pane_id_ = pane_id;
        return true;
    }
    return false;
}

bool Window::select_next_pane() {
    auto pids = layout_.all_pane_ids();
    if (pids.empty()) {
        if (panes_.empty()) return false;
        active_pane_id_ = panes_.begin()->first;
        return true;
    }
    auto it = std::find(pids.begin(), pids.end(), active_pane_id_);
    if (it == pids.end() || std::next(it) == pids.end()) {
        active_pane_id_ = pids.front();
    } else {
        active_pane_id_ = *std::next(it);
    }
    return true;
}

bool Window::select_previous_pane() {
    auto pids = layout_.all_pane_ids();
    if (pids.empty()) {
        if (panes_.empty()) return false;
        active_pane_id_ = panes_.begin()->first;
        return true;
    }
    auto it = std::find(pids.begin(), pids.end(), active_pane_id_);
    if (it == pids.end() || it == pids.begin()) {
        active_pane_id_ = pids.back();
    } else {
        active_pane_id_ = *std::prev(it);
    }
    return true;
}

bool Window::resize_pane(uint32_t pane_id, ResizeDirection dir, int amount) {
    if (panes_.find(pane_id) == panes_.end()) return false;
    bool ok = layout_.resize_pane(pane_id, dir, amount, cols_, rows_);
    if (ok) {
        apply_layout();
    }
    return ok;
}

bool Window::toggle_zoom(uint32_t pane_id) {
    if (zoomed_pane_id_ != 0) {
        zoomed_pane_id_ = 0;
    } else {
        uint32_t target = (pane_id != 0) ? pane_id : active_pane_id_;
        if (panes_.find(target) == panes_.end()) return false;
        zoomed_pane_id_ = target;
    }
    apply_layout();
    return true;
}

void Window::resize(uint16_t cols, uint16_t rows) {
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;
    cols_ = cols;
    rows_ = rows;
    apply_layout();
}

protocol::MsgLayoutUpdate Window::get_layout_update(
    uint64_t server_seq, uint32_t session_id) const {
    protocol::MsgLayoutUpdate update;
    update.server_seq = server_seq;
    update.session_id = session_id;
    update.window_id = window_id_;
    update.layout_tree_json = layout_.to_json();
    update.active_pane_id = active_pane_id_;
    update.zoomed_pane_id = zoomed_pane_id_;
    return update;
}

} // namespace tmux2::server
