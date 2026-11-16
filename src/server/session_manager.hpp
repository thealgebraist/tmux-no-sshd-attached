#pragma once

#include "src/protocol/messages.hpp"
#include "src/server/session.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tmux2::server {

class SessionManager {
public:
    SessionManager() = default;
    ~SessionManager() = default;

    // Session creation & lifecycle
    std::shared_ptr<Session> new_session(
        const protocol::MsgNewSessionReq& req,
        uint16_t default_cols = 80, uint16_t default_rows = 24,
        bool mock_pty = false);

    bool kill_session(uint32_t session_id);
    bool rename_session(uint32_t session_id, std::string_view new_name);

    // Queries
    [[nodiscard]] std::shared_ptr<Session> find_session(uint32_t session_id) const;
    [[nodiscard]] std::shared_ptr<Session> find_session_by_name(std::string_view name) const;
    [[nodiscard]] std::shared_ptr<Session> find_session_or_default(uint32_t session_id, std::string_view name) const;
    [[nodiscard]] std::pair<std::shared_ptr<Session>, std::shared_ptr<Pane>> find_pane(uint32_t pane_id) const;

    [[nodiscard]] std::vector<protocol::SessionSummary> list_sessions(std::string_view filter = "") const;
    [[nodiscard]] size_t session_count() const noexcept { return sessions_.size(); }
    [[nodiscard]] const std::unordered_map<uint32_t, std::shared_ptr<Session>>& sessions() const noexcept { return sessions_; }

    // ID allocation
    uint32_t allocate_session_id() noexcept { return next_session_id_++; }
    uint32_t allocate_window_id() noexcept { return next_window_id_++; }
    uint32_t allocate_pane_id() noexcept { return next_pane_id_++; }

private:
    uint32_t next_session_id_{1};
    uint32_t next_window_id_{1};
    uint32_t next_pane_id_{1};

    std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions_;
};

} // namespace tmux2::server
