#pragma once

#include "src/protocol/messages.hpp"
#include "src/pty/pty.hpp"
#include "src/server/window.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tmux2::server {

struct ClientAttachment {
    uint64_t client_id{0};
    bool read_only{false};
    uint16_t cols{80};
    uint16_t rows{24};
    std::chrono::steady_clock::time_point last_interaction{std::chrono::steady_clock::now()};
};

class Session {
public:
    Session(uint32_t session_id, std::string_view name,
            uint16_t cols = 80, uint16_t rows = 24);
    ~Session() = default;

    // Factory
    static std::shared_ptr<Session> create(
        uint32_t session_id, std::string_view name,
        uint32_t initial_window_id, uint32_t initial_pane_id,
        std::string_view initial_window_name = "0",
        uint16_t cols = 80, uint16_t rows = 24,
        const pty::PtyOptions& options = {});

    static std::shared_ptr<Session> create_mock(
        uint32_t session_id, std::string_view name,
        uint32_t initial_window_id, uint32_t initial_pane_id,
        std::string_view initial_window_name = "0",
        uint16_t cols = 80, uint16_t rows = 24);

    // Identity
    [[nodiscard]] uint32_t session_id() const noexcept { return session_id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string_view name) { name_ = name; }
    [[nodiscard]] uint64_t created_at() const noexcept { return created_at_; }

    [[nodiscard]] uint16_t cols() const noexcept { return cols_; }
    [[nodiscard]] uint16_t rows() const noexcept { return rows_; }

    [[nodiscard]] const std::string& size_policy() const noexcept { return size_policy_; }
    void set_size_policy(std::string_view policy) { size_policy_ = policy; }

    // Window management
    std::shared_ptr<Window> create_window(
        uint32_t window_id, std::string_view name, uint32_t initial_pane_id,
        const pty::PtyOptions& options = {});

    std::shared_ptr<Window> create_window_mock(
        uint32_t window_id, std::string_view name, uint32_t initial_pane_id);

    bool kill_window(uint32_t window_id);
    bool select_window(uint32_t window_id);
    bool select_window_by_index(uint32_t index);
    bool next_window();
    bool previous_window();

    [[nodiscard]] std::shared_ptr<Window> active_window() const;
    [[nodiscard]] std::shared_ptr<Window> find_window(uint32_t window_id) const;
    [[nodiscard]] std::shared_ptr<Pane> find_pane(uint32_t pane_id) const;

    [[nodiscard]] const std::vector<std::shared_ptr<Window>>& windows() const noexcept { return windows_; }
    [[nodiscard]] size_t window_count() const noexcept { return windows_.size(); }

    // Client attachment
    std::vector<uint64_t> attach_client(
        uint64_t client_id, bool read_only, uint16_t cols, uint16_t rows,
        bool detach_others = false);

    bool detach_client(uint64_t client_id);
    [[nodiscard]] bool has_client(uint64_t client_id) const noexcept;
    [[nodiscard]] const std::vector<ClientAttachment>& attached_clients() const noexcept { return attached_clients_; }
    [[nodiscard]] size_t attached_client_count() const noexcept { return attached_clients_.size(); }

    // Interaction & size policy
    bool client_interacted(uint64_t client_id, uint16_t cols, uint16_t rows);
    void resize(uint16_t cols, uint16_t rows);

    // Protocol summaries
    [[nodiscard]] protocol::SessionSummary get_summary() const;
    [[nodiscard]] std::string preview_summary() const;

private:
    void apply_size_policy(uint64_t interacting_client_id = 0);

    uint32_t session_id_{0};
    std::string name_;
    uint64_t created_at_{0};
    uint16_t cols_{80};
    uint16_t rows_{24};
    std::string size_policy_{"latest"};

    std::vector<std::shared_ptr<Window>> windows_;
    uint32_t active_window_id_{0};
    uint32_t last_window_id_{0};

    std::vector<ClientAttachment> attached_clients_;
};

} // namespace tmux2::server
