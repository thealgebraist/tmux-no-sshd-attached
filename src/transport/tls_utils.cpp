#include "src/transport/tls_utils.hpp"
#include <openssl/err.h>
#include <openssl/pem.h>
#include <cstring>

namespace tmux2::transport {

std::vector<unsigned char> make_wire_alpn(std::string_view alpn) {
    std::vector<unsigned char> out;
    if (alpn.empty() || alpn.size() > 255) return out;
    out.reserve(alpn.size() + 1);
    out.push_back(static_cast<unsigned char>(alpn.size()));
    out.insert(out.end(), alpn.begin(), alpn.end());
    return out;
}

// Stores the ALPN wire bytes associated with the context
struct ServerAlpnContext {
    std::vector<unsigned char> wire_alpn;
};

static int get_alpn_ex_index() {
    static int idx = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr,
        [](void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
            delete static_cast<ServerAlpnContext*>(ptr);
        });
    return idx;
}

static void configure_server_alpn(SSL_CTX* ctx, const std::string& alpn) {
    auto* alpn_ctx = new ServerAlpnContext{make_wire_alpn(alpn)};
    SSL_CTX_set_ex_data(ctx, get_alpn_ex_index(), alpn_ctx);
    SSL_CTX_set_alpn_select_cb(ctx, [](SSL* ssl, const unsigned char **out, unsigned char *out_len,
                                       const unsigned char *in, unsigned int in_len, void*) -> int {
        SSL_CTX* c = SSL_get_SSL_CTX(ssl);
        auto* data = static_cast<ServerAlpnContext*>(SSL_CTX_get_ex_data(c, get_alpn_ex_index()));
        if (!data || data->wire_alpn.empty()) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (SSL_select_next_proto(
                const_cast<unsigned char**>(out),
                out_len,
                data->wire_alpn.data(),
                static_cast<unsigned int>(data->wire_alpn.size()),
                in,
                in_len) == OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_OK;
        }
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }, nullptr);
}

static bool load_certificates(SSL_CTX* ctx, const TlsConfig& config) {
    if (!config.cert_pem.empty()) {
        pki::UniqueBio bio(BIO_new_mem_buf(config.cert_pem.data(), static_cast<int>(config.cert_pem.size())));
        if (!bio) return false;
        pki::UniqueX509 cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
        if (!cert || SSL_CTX_use_certificate(ctx, cert.get()) <= 0) {
            return false;
        }
    }

    if (!config.key_pem.empty()) {
        pki::UniqueBio bio(BIO_new_mem_buf(config.key_pem.data(), static_cast<int>(config.key_pem.size())));
        if (!bio) return false;
        pki::UniquePKey pkey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
        if (!pkey || SSL_CTX_use_PrivateKey(ctx, pkey.get()) <= 0) {
            return false;
        }
    }

    if (!config.ca_cert_pem.empty()) {
        pki::UniqueBio bio(BIO_new_mem_buf(config.ca_cert_pem.data(), static_cast<int>(config.ca_cert_pem.size())));
        if (!bio) return false;
        X509_STORE* store = SSL_CTX_get_cert_store(ctx);
        if (!store) return false;

        // Read potentially multiple CA certs from bundle
        while (true) {
            pki::UniqueX509 ca(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
            if (!ca) break;
            X509_STORE_add_cert(store, ca.get());
            SSL_CTX_add_client_CA(ctx, ca.get());
        }
    }

    return true;
}

std::expected<UniqueSslCtx, std::string> create_tls_server_ctx(const TlsConfig& config) {
    const SSL_METHOD* method = TLS_server_method();
    UniqueSslCtx ctx(SSL_CTX_new(method));
    if (!ctx) return std::unexpected("Failed to allocate TLS server context");

    // Strict TLS 1.3 only
    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION) <= 0 ||
        SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) <= 0) {
        return std::unexpected("Failed to set TLS 1.3 protocol version");
    }

    // Strict TLS 1.3 cipher suites
    const char* ciphers = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
    if (SSL_CTX_set_ciphersuites(ctx.get(), ciphers) <= 0) {
        return std::unexpected("Failed to set TLS 1.3 cipher suites");
    }

    if (!load_certificates(ctx.get(), config)) {
        return std::unexpected("Failed to load certificates or private key into server TLS context");
    }

    // Configure peer verification
    int verify_mode = SSL_VERIFY_NONE;
    if (config.verify_peer) {
        verify_mode = SSL_VERIFY_PEER;
        if (config.require_client_cert) {
            verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
    }
    SSL_CTX_set_verify(ctx.get(), verify_mode, nullptr);

    // ALPN
    if (!config.alpn.empty()) {
        configure_server_alpn(ctx.get(), config.alpn);
    }

    return ctx;
}

std::expected<UniqueSslCtx, std::string> create_tls_client_ctx(const TlsConfig& config) {
    const SSL_METHOD* method = TLS_client_method();
    UniqueSslCtx ctx(SSL_CTX_new(method));
    if (!ctx) return std::unexpected("Failed to allocate TLS client context");

    // Strict TLS 1.3 only
    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION) <= 0 ||
        SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) <= 0) {
        return std::unexpected("Failed to set TLS 1.3 protocol version");
    }

    const char* ciphers = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
    if (SSL_CTX_set_ciphersuites(ctx.get(), ciphers) <= 0) {
        return std::unexpected("Failed to set TLS 1.3 cipher suites");
    }

    if (!load_certificates(ctx.get(), config)) {
        return std::unexpected("Failed to load certificates or private key into client TLS context");
    }

    int verify_mode = config.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(ctx.get(), verify_mode, nullptr);

    return ctx;
}

std::expected<UniqueSslCtx, std::string> create_quic_server_ctx(const TlsConfig& config) {
    const SSL_METHOD* method = OSSL_QUIC_server_method();
    UniqueSslCtx ctx(SSL_CTX_new(method));
    if (!ctx) return std::unexpected("Failed to allocate QUIC server context");

    const char* ciphers = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
    SSL_CTX_set_ciphersuites(ctx.get(), ciphers);
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION);

    if (!load_certificates(ctx.get(), config)) {
        return std::unexpected("Failed to load certificates or private key into QUIC server context");
    }

    int verify_mode = SSL_VERIFY_NONE;
    if (config.verify_peer) {
        verify_mode = SSL_VERIFY_PEER;
        if (config.require_client_cert) {
            verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
    }
    SSL_CTX_set_verify(ctx.get(), verify_mode, nullptr);

    if (!config.alpn.empty()) {
        configure_server_alpn(ctx.get(), config.alpn);
    }

    return ctx;
}

std::expected<UniqueSslCtx, std::string> create_quic_client_ctx(const TlsConfig& config) {
    const SSL_METHOD* method = OSSL_QUIC_client_method();
    UniqueSslCtx ctx(SSL_CTX_new(method));
    if (!ctx) return std::unexpected("Failed to allocate QUIC client context");

    const char* ciphers = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
    SSL_CTX_set_ciphersuites(ctx.get(), ciphers);
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION);

    if (!load_certificates(ctx.get(), config)) {
        return std::unexpected("Failed to load certificates or private key into QUIC client context");
    }

    int verify_mode = config.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(ctx.get(), verify_mode, nullptr);

    return ctx;
}

bool set_client_alpn(SSL* ssl, std::string_view alpn) {
    if (!ssl || alpn.empty()) return false;
    auto wire = make_wire_alpn(alpn);
    return SSL_set_alpn_protos(ssl, wire.data(), static_cast<unsigned int>(wire.size())) == 0;
}

bool set_client_host(SSL* ssl, std::string_view host) {
    if (!ssl || host.empty()) return false;
    std::string h(host);
    if (SSL_set_tlsext_host_name(ssl, h.c_str()) <= 0) {
        return false;
    }
    // Set host verification parameter
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
    if (param) {
        // If IP address, use ip asc
        if (X509_VERIFY_PARAM_set1_ip_asc(param, h.c_str()) <= 0) {
            X509_VERIFY_PARAM_set1_host(param, h.c_str(), h.length());
        }
    }
    return true;
}

} // namespace tmux2::transport
