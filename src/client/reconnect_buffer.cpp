#include "src/client/reconnect_buffer.hpp"

namespace tmux2::client {

ReconnectBuffer::ReconnectBuffer(size_t max_capacity)
    : max_capacity_(max_capacity > 0 ? max_capacity : 2048) {}

uint64_t ReconnectBuffer::next_seq() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_client_seq_++;
}

uint64_t ReconnectBuffer::current_seq() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_client_seq_;
}

void ReconnectBuffer::push(uint64_t client_seq, protocol::ProtocolMessage msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.size() >= max_capacity_) {
        ring_.pop_front(); // Evict oldest
    }
    ring_.push_back(BufferedOutboundMessage{
        .client_seq = client_seq,
        .message = std::move(msg)
    });
}

void ReconnectBuffer::ack(uint64_t acked_seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!ring_.empty() && ring_.front().client_seq <= acked_seq) {
        ring_.pop_front();
    }
}

std::vector<BufferedOutboundMessage> ReconnectBuffer::get_unacked(uint64_t last_server_acked_seq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BufferedOutboundMessage> res;
    for (const auto& entry : ring_) {
        if (entry.client_seq > last_server_acked_seq) {
            res.push_back(entry);
        }
    }
    return res;
}

void ReconnectBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.clear();
}

size_t ReconnectBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_.size();
}

} // namespace tmux2::client
