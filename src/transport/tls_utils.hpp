#pragma once

#include "src/transport/transport.hpp"
#include "src/pki/raii.hpp"
#include <openssl/ssl.h>
#include <openssl/quic.h>
#include <openssl/x509v3.h>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace tmux2::transport {

struct SslCtxDeleter {
    void operator()(SSL_CTX* ctx) const noexcept {
        if (ctx) SSL_CTX_free(ctx);
    }
};
using UniqueSslCtx = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept {
        if (ssl) SSL_free(ssl);
    }
};
using UniqueSsl = std::unique_ptr<SSL, SslDeleter>;

// Helper to convert an ALPN string to TLS wire format (e.g. "tmux2/1" -> {7, 't', 'm', ...})
std::vector<unsigned char> make_wire_alpn(std::string_view alpn);

// Context builders
std::expected<UniqueSslCtx, std::string> create_tls_server_ctx(const TlsConfig& config);
std::expected<UniqueSslCtx, std::string> create_tls_client_ctx(const TlsConfig& config);
std::expected<UniqueSslCtx, std::string> create_quic_server_ctx(const TlsConfig& config);
std::expected<UniqueSslCtx, std::string> create_quic_client_ctx(const TlsConfig& config);

// Configure ALPN on a specific client SSL object
bool set_client_alpn(SSL* ssl, std::string_view alpn);

// Configure SNI and hostname verification on a client SSL object
bool set_client_host(SSL* ssl, std::string_view host);

} // namespace tmux2::transport
