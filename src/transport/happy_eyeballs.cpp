#include "src/transport/happy_eyeballs.hpp"
#include <chrono>
#include <thread>
#include <iostream>

namespace tmux2::transport {

std::expected<std::shared_ptr<IConnection>, std::string> HappyEyeballsConnector::connect(
    const std::string& host,
    uint16_t quic_port,
    uint16_t tcp_port,
    const TlsConfig& config,
    const HappyEyeballsOptions& options,
    reactor::Reactor* reactor
) {
    auto start_time = std::chrono::steady_clock::now();

    // 1. Try launching QUIC first
    // Note: QuicConnector::connect with small timeout or nonblocking driving
    std::shared_ptr<QuicConnection> quic_conn;
    std::string quic_err;

    // Check if QUIC connects fast within fallback_delay
    auto quic_res = QuicConnector::connect(
        host,
        quic_port,
        config,
        options.fallback_delay,
        nullptr
    );

    if (quic_res) {
        // QUIC succeeded fast!
        auto conn = *quic_res;
        if (reactor) {
            conn->register_with_reactor(*reactor);
        }
        return conn;
    }

    // QUIC either timed out or failed with an error
    quic_err = quic_res.error();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time
    );

    // If total timeout already elapsed
    if (elapsed >= options.total_timeout) {
        return std::unexpected("Connection timed out (QUIC attempt failed: " + quic_err + ")");
    }

    // 2. Launch TCP TLS 1.3 fallback
    auto remaining_time = options.total_timeout - elapsed;
    auto tcp_res = TcpTlsConnector::connect(
        host,
        tcp_port,
        config,
        remaining_time,
        nullptr
    );

    if (tcp_res) {
        // TCP TLS fallback won!
        auto conn = *tcp_res;
        if (reactor) {
            conn->register_with_reactor(*reactor);
        }
        return conn;
    }

    // If TCP failed too, combine errors
    return std::unexpected(
        "Happy Eyeballs failed. QUIC: " + quic_err + "; TCP TLS: " + tcp_res.error()
    );
}

} // namespace tmux2::transport
