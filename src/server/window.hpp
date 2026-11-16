#pragma once

#include "src/protocol/messages.hpp"
#include "src/pty/pty.hpp"
#include "src/server/layout.hpp"
#include "src/server/pane.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tmux2::server {

class Window {
public:
    Window(uint32_t window_id, uint32_t index, std::string_view name,
           uint16_t cols = 80, uint16_t rows = 24);
    ~Window() = default;

    // Factory with initial pane
    static std::shared_ptr<Window> create(
        uint32_t window_id, uint32_t index, std::string_view name,
        uint32_t initial_pane_id, uint16_t cols = 80, uint16_t rows = 24,
        const pty::PtyOptions& options = {});

    static std::shared_ptr<Window> create_mock(
        uint32_t window_id, uint32_t index, std::string_view name,
        uint32_t initial_pane_id, uint16_t cols = 80, uint16_t rows = 24);

    // Identity & dimensions
    [[nodiscard]] uint32_t window_id() const noexcept { return window_id_; }
    [[nodiscard]] uint32_t index() const noexcept { return index_; }
    void set_index(uint32_t idx) noexcept { index_ = idx; }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string_view name) { name_ = name; }

    [[nodiscard]] uint16_t cols() const noexcept { return cols_; }
    [[nodiscard]] uint16_t rows() const noexcept { return rows_; }

    [[nodiscard]] uint32_t active_pane_id() const noexcept { return active_pane_id_; }
    [[nodiscard]] uint32_t zoomed_pane_id() const noexcept { return zoomed_pane_id_; }
    [[nodiscard]] bool is_zoomed() const noexcept { return zoomed_pane_id_ != 0; }

    [[nodiscard]] const std::string& size_policy() const noexcept { return size_policy_; }
    void set_size_policy(std::string_view policy) { size_policy_ = policy; }

    // Pane access
    [[nodiscard]] std::shared_ptr<Pane> active_pane() const;
    [[nodiscard]] std::shared_ptr<Pane> find_pane(uint32_t pane_id) const;
    [[nodiscard]] const std::unordered_map<uint32_t, std::shared_ptr<Pane>>& panes() const noexcept { return panes_; }
    [[nodiscard]] size_t pane_count() const noexcept { return panes_.size(); }

    // Layout tree
    [[nodiscard]] const LayoutTree& layout() const noexcept { return layout_; }
    [[nodiscard]] LayoutTree& layout() noexcept { return layout_; }

    // Pane split & kill
    std::shared_ptr<Pane> split_pane(
        uint32_t target_pane_id, uint32_t new_pane_id, bool vertical,
        const pty::PtyOptions& options = {});

    std::shared_ptr<Pane> split_pane_mock(
        uint32_t target_pane_id, uint32_t new_pane_id, bool vertical);

    bool kill_pane(uint32_t pane_id);

    // Pane navigation & manipulation
    bool select_pane(uint32_t pane_id);
    bool select_next_pane();
    bool select_previous_pane();
    bool resize_pane(uint32_t pane_id, ResizeDirection dir, int amount);
    bool toggle_zoom(uint32_t pane_id = 0);

    // Window resize
    void resize(uint16_t cols, uint16_t rows);

    // Protocol messages
    [[nodiscard]] protocol::MsgLayoutUpdate get_layout_update(
        uint64_t server_seq, uint32_t session_id) const;

private:
    void apply_layout();

    uint32_t window_id_{0};
    uint32_t index_{0};
    std::string name_;
    uint16_t cols_{80};
    uint16_t rows_{24};

    uint32_t active_pane_id_{0};
    uint32_t zoomed_pane_id_{0};
    std::string size_policy_{"latest"};

    LayoutTree layout_;
    std::unordered_map<uint32_t, std::shared_ptr<Pane>> panes_;
};

} // namespace tmux2::server
