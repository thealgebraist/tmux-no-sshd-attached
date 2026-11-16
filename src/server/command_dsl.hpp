#pragma once

#include "src/protocol/messages.hpp"
#include "src/server/session_manager.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tmux2::server {

class ClientState;

class CommandDsl {
public:
    static protocol::MsgCommandRpcResp execute(
        SessionManager& mgr,
        std::string_view command_str,
        uint64_t rpc_id = 0,
        uint32_t current_session_id = 0,
        bool mock_pty = false,
        ClientState* client = nullptr);

    // Helpers
    static std::vector<std::string> tokenize(std::string_view command_str);
};

} // namespace tmux2::server
