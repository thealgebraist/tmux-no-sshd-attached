#pragma once

#include "crypto_utils.hpp"
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace tmux2::pki {

struct ClientIdentity {
    std::vector<std::string> san_uris{};
    std::vector<std::string> san_dns{};
    std::vector<std::string> san_emails{};
    std::string common_name{};
    std::string cert_fingerprint{};
    std::string spki_fingerprint{};

    // Resolves username following spec priority:
    // 1. tmux2://user/<username> or spiffe://<trust-domain>/user/<username>
    // 2. san_emails or common_name
    std::optional<std::string> extract_username() const;
};

// Extracts identity attributes (SAN URIs, DNS, CN, Fingerprints) from client X509
std::expected<ClientIdentity, std::string> extract_client_identity(const X509* cert);

// Verifies X.509 certificate chain against trusted CA certificates
std::expected<void, std::string> verify_certificate_chain(
    const X509* cert,
    const std::vector<const X509*>& trusted_cas,
    const std::vector<const X509*>& untrusted_intermediates = {}
);

// Validates server certificate against expected hostname or IP address (RFC 6125)
bool verify_server_hostname(const X509* cert, std::string_view expected_host);

} // namespace tmux2::pki
