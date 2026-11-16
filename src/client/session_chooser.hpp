#pragma once

#include "src/client/input_parser.hpp"
#include "src/protocol/cell.hpp"
#include "src/protocol/messages.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tmux2::client {

class SessionChooser {
public:
    SessionChooser() = default;

    void open(std::vector<protocol::SessionSummary> sessions);
    void close();
    [[nodiscard]] bool is_active() const noexcept { return active_; }

    [[nodiscard]] const std::vector<protocol::SessionSummary>& all_sessions() const noexcept { return sessions_; }
    [[nodiscard]] std::vector<protocol::SessionSummary> filtered_sessions() const;

    [[nodiscard]] size_t selected_index() const noexcept { return selected_index_; }
    [[nodiscard]] std::string_view filter() const noexcept { return filter_; }

    // Handle key event in overlay mode.
    // Returns MsgSwitchSessionReq if Enter pressed on valid session, or nullopt if continuing/cancelled.
    std::optional<protocol::MsgSwitchSessionReq> handle_key(const ClientKeyEvent& key);

    // Render modal overlay onto composite cells (width x height screen)
    void render_overlay(std::vector<protocol::Cell>& screen, uint16_t term_cols, uint16_t term_rows) const;

private:
    bool active_{false};
    std::vector<protocol::SessionSummary> sessions_;
    size_t selected_index_{0};
    std::string filter_;

    void clamp_selected_index();
};

} // namespace tmux2::client
