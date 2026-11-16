#include "src/server/layout.hpp"

#include <algorithm>
#include <sstream>

namespace tmux2::server {

std::string LayoutLeaf::to_json() const {
    std::ostringstream ss;
    ss << "{\"type\":\"leaf\","
       << "\"pane_id\":" << pane_id_ << ","
       << "\"x\":" << rect_.x << ","
       << "\"y\":" << rect_.y << ","
       << "\"width\":" << rect_.width << ","
       << "\"height\":" << rect_.height << "}";
    return ss.str();
}

void LayoutLeaf::calculate(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t zoomed_pane_id) {
    if (zoomed_pane_id != 0) {
        if (pane_id_ == zoomed_pane_id) {
            rect_ = {x, y, w, h};
        } else {
            rect_ = {0, 0, 0, 0};
        }
    } else {
        rect_ = {x, y, w, h};
    }
}

void LayoutLeaf::collect_pane_ids(std::vector<uint32_t>& out) const {
    out.push_back(pane_id_);
}

std::optional<Rect> LayoutLeaf::find_pane_rect(uint32_t pane_id) const {
    if (pane_id_ == pane_id) {
        return rect_;
    }
    return std::nullopt;
}

std::string LayoutSplit::to_json() const {
    std::ostringstream ss;
    ss << "{\"type\":\"split\","
       << "\"direction\":\"" << (type_ == SplitType::Horizontal ? "horizontal" : "vertical") << "\","
       << "\"ratio\":" << ratio_ << ","
       << "\"first\":" << (first_ ? first_->to_json() : "null") << ","
       << "\"second\":" << (second_ ? second_->to_json() : "null") << "}";
    return ss.str();
}

void LayoutSplit::calculate(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t zoomed_pane_id) {
    if (zoomed_pane_id != 0) {
        if (first_) first_->calculate(x, y, w, h, zoomed_pane_id);
        if (second_) second_->calculate(x, y, w, h, zoomed_pane_id);
        return;
    }

    if (type_ == SplitType::Horizontal) {
        uint16_t w1 = static_cast<uint16_t>(w * ratio_);
        if (w1 == 0 && w > 1) w1 = 1;
        if (w1 >= w && w > 1) w1 = w - 1;
        uint16_t w2 = (w > w1) ? (w - w1) : 0;

        if (first_) first_->calculate(x, y, w1, h, 0);
        if (second_) second_->calculate(static_cast<uint16_t>(x + w1), y, w2, h, 0);
    } else {
        uint16_t h1 = static_cast<uint16_t>(h * ratio_);
        if (h1 == 0 && h > 1) h1 = 1;
        if (h1 >= h && h > 1) h1 = h - 1;
        uint16_t h2 = (h > h1) ? (h - h1) : 0;

        if (first_) first_->calculate(x, y, w, h1, 0);
        if (second_) second_->calculate(x, static_cast<uint16_t>(y + h1), w, h2, 0);
    }
}

void LayoutSplit::collect_pane_ids(std::vector<uint32_t>& out) const {
    if (first_) first_->collect_pane_ids(out);
    if (second_) second_->collect_pane_ids(out);
}

std::optional<Rect> LayoutSplit::find_pane_rect(uint32_t pane_id) const {
    if (first_) {
        auto r = first_->find_pane_rect(pane_id);
        if (r) return r;
    }
    if (second_) {
        auto r = second_->find_pane_rect(pane_id);
        if (r) return r;
    }
    return std::nullopt;
}

LayoutTree::LayoutTree(uint32_t initial_pane_id) {
    reset(initial_pane_id);
}

void LayoutTree::reset(uint32_t pane_id) {
    root_ = std::make_unique<LayoutLeaf>(pane_id);
}

void LayoutTree::calculate(uint16_t cols, uint16_t rows, uint32_t zoomed_pane_id) {
    if (root_) {
        root_->calculate(0, 0, cols, rows, zoomed_pane_id);
    }
}

std::optional<Rect> LayoutTree::get_pane_rect(uint32_t pane_id) const {
    if (!root_) return std::nullopt;
    return root_->find_pane_rect(pane_id);
}

std::vector<uint32_t> LayoutTree::all_pane_ids() const {
    std::vector<uint32_t> out;
    if (root_) {
        root_->collect_pane_ids(out);
    }
    return out;
}

std::string LayoutTree::to_json() const {
    if (!root_) return "{}";
    return root_->to_json();
}

static bool split_node_rec(std::unique_ptr<LayoutNode>& node, uint32_t target_pane_id, uint32_t new_pane_id, SplitType type, double ratio) {
    if (!node) return false;
    if (node->is_leaf()) {
        auto* leaf = static_cast<LayoutLeaf*>(node.get());
        if (leaf->pane_id() == target_pane_id) {
            auto first = std::make_unique<LayoutLeaf>(target_pane_id);
            auto second = std::make_unique<LayoutLeaf>(new_pane_id);
            node = std::make_unique<LayoutSplit>(type, std::move(first), std::move(second), ratio);
            return true;
        }
        return false;
    }

    auto* split = static_cast<LayoutSplit*>(node.get());
    if (split_node_rec(split->first_ptr(), target_pane_id, new_pane_id, type, ratio)) {
        return true;
    }
    return split_node_rec(split->second_ptr(), target_pane_id, new_pane_id, type, ratio);
}

bool LayoutTree::split_pane(uint32_t target_pane_id, uint32_t new_pane_id, SplitType type, double ratio) {
    if (!root_) {
        reset(target_pane_id);
    }
    return split_node_rec(root_, target_pane_id, new_pane_id, type, ratio);
}

static bool remove_node_rec(std::unique_ptr<LayoutNode>& node, uint32_t pane_id) {
    if (!node || node->is_leaf()) return false;

    auto* split = static_cast<LayoutSplit*>(node.get());

    // Check if first child is the leaf to remove
    if (split->first() && split->first()->is_leaf()) {
        auto* leaf = static_cast<LayoutLeaf*>(split->first());
        if (leaf->pane_id() == pane_id) {
            node = std::move(split->second_ptr());
            return true;
        }
    }

    // Check if second child is the leaf to remove
    if (split->second() && split->second()->is_leaf()) {
        auto* leaf = static_cast<LayoutLeaf*>(split->second());
        if (leaf->pane_id() == pane_id) {
            node = std::move(split->first_ptr());
            return true;
        }
    }

    if (remove_node_rec(split->first_ptr(), pane_id)) return true;
    return remove_node_rec(split->second_ptr(), pane_id);
}

bool LayoutTree::remove_pane(uint32_t pane_id) {
    if (!root_) return false;
    if (root_->is_leaf()) {
        auto* leaf = static_cast<LayoutLeaf*>(root_.get());
        if (leaf->pane_id() == pane_id) {
            root_.reset();
            return true;
        }
        return false;
    }
    return remove_node_rec(root_, pane_id);
}

static bool contains_pane(const LayoutNode* node, uint32_t pane_id) {
    if (!node) return false;
    if (node->is_leaf()) {
        return static_cast<const LayoutLeaf*>(node)->pane_id() == pane_id;
    }
    const auto* split = static_cast<const LayoutSplit*>(node);
    return contains_pane(split->first(), pane_id) || contains_pane(split->second(), pane_id);
}

static bool resize_node_rec(LayoutNode* node, uint32_t pane_id, ResizeDirection dir, double delta_ratio) {
    if (!node || node->is_leaf()) return false;

    auto* split = static_cast<LayoutSplit*>(node);
    bool in_first = contains_pane(split->first(), pane_id);
    bool in_second = contains_pane(split->second(), pane_id);

    if (!in_first && !in_second) return false;

    // Check if this split matches the direction
    bool is_horizontal_dir = (dir == ResizeDirection::Left || dir == ResizeDirection::Right);
    bool is_split_horizontal = (split->type() == SplitType::Horizontal);

    if (is_horizontal_dir == is_split_horizontal) {
        // Adjust ratio
        double r = split->ratio();
        if (in_first) {
            if (dir == ResizeDirection::Right || dir == ResizeDirection::Down) {
                r += delta_ratio;
            } else {
                r -= delta_ratio;
            }
        } else { // in_second
            if (dir == ResizeDirection::Left || dir == ResizeDirection::Up) {
                r -= delta_ratio;
            } else {
                r += delta_ratio;
            }
        }
        r = std::clamp(r, 0.05, 0.95);
        split->set_ratio(r);
        return true;
    }

    if (in_first && resize_node_rec(split->first(), pane_id, dir, delta_ratio)) return true;
    if (in_second && resize_node_rec(split->second(), pane_id, dir, delta_ratio)) return true;
    return false;
}

bool LayoutTree::resize_pane(uint32_t pane_id, ResizeDirection dir, int amount, uint16_t total_cols, uint16_t total_rows) {
    if (!root_ || root_->is_leaf() || amount <= 0) return false;
    double delta_ratio = 0.05 * amount;
    if (dir == ResizeDirection::Left || dir == ResizeDirection::Right) {
        if (total_cols > 0) delta_ratio = static_cast<double>(amount) / total_cols;
    } else {
        if (total_rows > 0) delta_ratio = static_cast<double>(amount) / total_rows;
    }
    return resize_node_rec(root_.get(), pane_id, dir, delta_ratio);
}

} // namespace tmux2::server
