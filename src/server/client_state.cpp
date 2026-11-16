#include "src/server/client_state.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace tmux2::server {

ClientState::ClientState(uint64_t client_id,
                         const std::array<uint8_t, 16>& client_instance_id,
                         std::string_view cert_fingerprint,
                         size_t ring_buffer_capacity)
    : client_id_(client_id),
      client_instance_id_(client_instance_id),
      cert_fingerprint_(cert_fingerprint),
      ring_buffer_capacity_(ring_buffer_capacity > 0 ? ring_buffer_capacity : 1) {
}

bool ClientState::deduplicate_client_seq(uint64_t client_seq) {
    if (client_seq == 0) {
        return true;
    }
    if (client_seq <= last_client_seq_acked_) {
        return false; // Duplicate input event
    }
    last_client_seq_acked_ = client_seq;
    return true;
}

void ClientState::record_frame_diff(const protocol::MsgFrameDiff& diff) {
    if (frame_ring_buffer_.size() >= ring_buffer_capacity_) {
        frame_ring_buffer_.pop_front();
    }
    frame_ring_buffer_.push_back(diff);
    last_srv_seq_ = diff.server_seq;
}

bool ClientState::can_delta_replay(uint64_t last_srv_seq) const {
    if (frame_ring_buffer_.empty()) {
        return false;
    }
    uint64_t oldest = frame_ring_buffer_.front().server_seq;
    uint64_t newest = frame_ring_buffer_.back().server_seq;

    // Client must have seen at least the sequence just before our oldest, or within range
    if (last_srv_seq + 1 < oldest) {
        return false; // Aged out!
    }
    if (last_srv_seq > newest) {
        return false;
    }
    return true;
}

std::vector<protocol::MsgFrameDiff> ClientState::get_replay_diffs(uint64_t last_srv_seq) const {
    if (!can_delta_replay(last_srv_seq)) {
        return {};
    }
    std::vector<protocol::MsgFrameDiff> diffs;
    for (const auto& diff : frame_ring_buffer_) {
        if (diff.server_seq > last_srv_seq) {
            diffs.push_back(diff);
        }
    }
    return diffs;
}

void ClientState::clear_ring_buffer() {
    frame_ring_buffer_.clear();
}

static std::string hmac_sha256_hex(std::span<const uint8_t> key, std::string_view data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;

    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         md, &md_len);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < md_len; ++i) {
        ss << std::setw(2) << static_cast<int>(md[i]);
    }
    return ss.str();
}

std::string ClientState::generate_resume_token(
    uint64_t client_id,
    const std::string& cert_fingerprint,
    std::span<const uint8_t> hmac_secret,
    std::chrono::seconds ttl) {
    auto now = std::chrono::system_clock::now();
    uint64_t expiry = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>((now + ttl).time_since_epoch()).count());

    std::ostringstream payload;
    payload << client_id << ":" << cert_fingerprint << ":" << expiry;
    std::string sig = hmac_sha256_hex(hmac_secret, payload.str());

    std::ostringstream token;
    token << client_id << "." << expiry << "." << sig;
    return token.str();
}

bool ClientState::validate_resume_token(
    std::string_view token,
    uint64_t client_id,
    const std::string& cert_fingerprint,
    std::span<const uint8_t> hmac_secret) {
    size_t dot1 = token.find('.');
    if (dot1 == std::string_view::npos) return false;
    size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string_view::npos) return false;

    std::string_view id_str = token.substr(0, dot1);
    std::string_view exp_str = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string_view sig_str = token.substr(dot2 + 1);

    uint64_t token_client_id = 0;
    try {
        token_client_id = std::stoull(std::string(id_str));
    } catch (...) {
        return false;
    }

    if (token_client_id != client_id) {
        return false;
    }

    uint64_t expiry = 0;
    try {
        expiry = std::stoull(std::string(exp_str));
    } catch (...) {
        return false;
    }

    auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (now >= expiry) {
        return false; // Expired token
    }

    std::ostringstream payload;
    payload << client_id << ":" << cert_fingerprint << ":" << expiry;
    std::string expected_sig = hmac_sha256_hex(hmac_secret, payload.str());

    if (expected_sig.size() != sig_str.size()) {
        return false;
    }

    return CRYPTO_memcmp(expected_sig.data(), sig_str.data(), expected_sig.size()) == 0;
}

} // namespace tmux2::server
