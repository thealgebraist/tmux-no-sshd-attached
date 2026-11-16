#pragma once

#include "crypto_utils.hpp"
#include <string>
#include <vector>
#include <expected>

namespace tmux2::pki {

struct CaOptions {
    std::string common_name = "tmux2 Root CA";
    std::string organization = "tmux2";
    int validity_days = 3650; // ~10 years
};

struct ServerCertOptions {
    std::string common_name = "tmux2 Server";
    std::string organization = "tmux2";
    std::vector<std::string> hosts{}; // e.g. "dev.example.com", "127.0.0.1", "::1", "localhost"
    int validity_days = 365;        // ~1 year
};

struct ClientCertOptions {
    std::string common_name = "tmux2 Client";
    std::string organization = "tmux2";
    std::string user_name{};          // e.g. "alice" -> SAN URI tmux2://user/alice
    std::vector<std::string> custom_san_uris{}; // e.g. "spiffe://domain/user/alice"
    int validity_days = 365;        // ~1 year
};

struct GeneratedCertificate {
    UniquePKey private_key;
    UniqueX509 certificate;
    std::string private_key_pem;
    std::string certificate_pem;
    std::string fingerprint_sha256;      // X.509 SHA-256 (colon separated)
    std::string spki_fingerprint_sha256; // SPKI SHA-256 (colon separated)
};

// Generates an Ed25519 self-signed Root CA certificate
std::expected<GeneratedCertificate, std::string> generate_ca(const CaOptions& options = {});

// Generates an Ed25519 server certificate signed by CA
std::expected<GeneratedCertificate, std::string> generate_server_cert(
    const ServerCertOptions& options,
    const EVP_PKEY* ca_key,
    const X509* ca_cert
);

// Generates an Ed25519 client certificate signed by CA
std::expected<GeneratedCertificate, std::string> generate_client_cert(
    const ClientCertOptions& options,
    const EVP_PKEY* ca_key,
    const X509* ca_cert
);

} // namespace tmux2::pki
