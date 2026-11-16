#pragma once

#include "src/protocol/messages.hpp"
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace tmux2::client {

struct BufferedOutboundMessage {
    uint64_t client_seq{0};
    protocol::ProtocolMessage message;
};

class ReconnectBuffer {
public:
    explicit ReconnectBuffer(size_t max_capacity = 2048);

    // Next client_seq allocator
    uint64_t next_seq() noexcept;
    [[nodiscard]] uint64_t current_seq() const noexcept;

    // Buffer an outbound message
    void push(uint64_t client_seq, protocol::ProtocolMessage msg);

    // Prune acknowledged messages
    void ack(uint64_t acked_seq);

    // Retrieve unacknowledged messages for replay
    [[nodiscard]] std::vector<BufferedOutboundMessage> get_unacked(uint64_t last_server_acked_seq) const;

    // Clear all entries
    void clear();

    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t max_capacity() const noexcept { return max_capacity_; }

private:
    size_t max_capacity_{2048};
    uint64_t next_client_seq_{1};
    mutable std::mutex mutex_;
    std::deque<BufferedOutboundMessage> ring_;
};

} // namespace tmux2::client
