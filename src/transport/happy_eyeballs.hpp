#pragma once

#include "src/transport/transport.hpp"
#include "src/transport/quic_transport.hpp"
#include "src/transport/tls_tcp_transport.hpp"
#include <chrono>

namespace tmux2::transport {

struct HappyEyeballsOptions {
    std::chrono::milliseconds fallback_delay{100}; // Delay before starting TCP fallback race
    std::chrono::milliseconds total_timeout{5000};  // Total time allowed for connection
};

class HappyEyeballsConnector {
public:
    static std::expected<std::shared_ptr<IConnection>, std::string> connect(
        const std::string& host,
        uint16_t quic_port,
        uint16_t tcp_port,
        const TlsConfig& config,
        const HappyEyeballsOptions& options = {},
        reactor::Reactor* reactor = nullptr
    );

    // Convenience overload when both QUIC and TCP listen on the same port
    static std::expected<std::shared_ptr<IConnection>, std::string> connect(
        const std::string& host,
        uint16_t port,
        const TlsConfig& config,
        const HappyEyeballsOptions& options = {},
        reactor::Reactor* reactor = nullptr
    ) {
        return connect(host, port, port, config, options, reactor);
    }
};

} // namespace tmux2::transport
