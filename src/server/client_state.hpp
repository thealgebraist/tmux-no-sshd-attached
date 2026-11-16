#pragma once

#include "src/protocol/messages.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tmux2::server {

class ClientState {
public:
    static constexpr size_t kDefaultRingBufferCapacity = 256;

    ClientState(uint64_t client_id,
                const std::array<uint8_t, 16>& client_instance_id,
                std::string_view cert_fingerprint,
                size_t ring_buffer_capacity = kDefaultRingBufferCapacity);
    ~ClientState() = default;

    // Identity & credentials
    [[nodiscard]] uint64_t client_id() const noexcept { return client_id_; }
    [[nodiscard]] const std::array<uint8_t, 16>& client_instance_id() const noexcept { return client_instance_id_; }
    [[nodiscard]] const std::string& cert_fingerprint() const noexcept { return cert_fingerprint_; }

    [[nodiscard]] uint32_t term_caps() const noexcept { return term_caps_; }
    void set_term_caps(uint32_t caps) noexcept { term_caps_ = caps; }

    [[nodiscard]] const std::string& resume_token() const noexcept { return resume_token_; }
    void set_resume_token(std::string_view token) { resume_token_ = token; }

    // Session attachment
    [[nodiscard]] uint32_t attached_session_id() const noexcept { return attached_session_id_; }
    void set_attached_session_id(uint32_t sid) noexcept { attached_session_id_ = sid; }

    [[nodiscard]] bool is_read_only() const noexcept { return read_only_; }
    void set_read_only(bool ro) noexcept { read_only_ = ro; }

    [[nodiscard]] uint16_t cols() const noexcept { return cols_; }
    [[nodiscard]] uint16_t rows() const noexcept { return rows_; }
    void set_dimensions(uint16_t cols, uint16_t rows) noexcept { cols_ = cols; rows_ = rows; }

    // Sequence tracking
    [[nodiscard]] uint64_t last_srv_seq() const noexcept { return last_srv_seq_; }
    void set_last_srv_seq(uint64_t seq) noexcept { last_srv_seq_ = seq; }

    [[nodiscard]] uint64_t last_client_seq_acked() const noexcept { return last_client_seq_acked_; }
    void set_last_client_seq_acked(uint64_t seq) noexcept { last_client_seq_acked_ = seq; }

    // Section 5 input deduplication: returns true if client_seq is NEW, false if DUPLICATE
    bool deduplicate_client_seq(uint64_t client_seq);

    // Section 5 FrameDiff ring buffer
    void record_frame_diff(const protocol::MsgFrameDiff& diff);
    [[nodiscard]] bool can_delta_replay(uint64_t last_srv_seq) const;
    [[nodiscard]] std::vector<protocol::MsgFrameDiff> get_replay_diffs(uint64_t last_srv_seq) const;
    void clear_ring_buffer();

    // Section 5 certificate-bound HMAC resume token helpers
    static std::string generate_resume_token(
        uint64_t client_id,
        const std::string& cert_fingerprint,
        std::span<const uint8_t> hmac_secret,
        std::chrono::seconds ttl = std::chrono::hours(24));

    static bool validate_resume_token(
        std::string_view token,
        uint64_t client_id,
        const std::string& cert_fingerprint,
        std::span<const uint8_t> hmac_secret);

private:
    uint64_t client_id_{0};
    std::array<uint8_t, 16> client_instance_id_{};
    std::string cert_fingerprint_;
    uint32_t term_caps_{0};
    std::string resume_token_;

    uint32_t attached_session_id_{0};
    bool read_only_{false};
    uint16_t cols_{80};
    uint16_t rows_{24};

    uint64_t last_srv_seq_{0};
    uint64_t last_client_seq_acked_{0};

    size_t ring_buffer_capacity_{kDefaultRingBufferCapacity};
    std::deque<protocol::MsgFrameDiff> frame_ring_buffer_;
};

} // namespace tmux2::server
