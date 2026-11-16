#pragma once

#include "raii.hpp"
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

namespace tmux2::pki {

// Key generation
std::expected<UniquePKey, std::string> generate_ed25519_key();
std::expected<UniquePKey, std::string> generate_p256_key();

// PEM conversion for Keys
std::expected<std::string, std::string> pkey_to_pem_private(const EVP_PKEY* pkey);
std::expected<std::string, std::string> pkey_to_pem_public(const EVP_PKEY* pkey);
std::expected<UniquePKey, std::string> pem_to_private_key(std::string_view pem);
std::expected<UniquePKey, std::string> pem_to_public_key(std::string_view pem);

// PEM conversion for X509
std::expected<std::string, std::string> x509_to_pem(const X509* cert);
std::expected<UniqueX509, std::string> pem_to_x509(std::string_view pem);
std::expected<std::vector<UniqueX509>, std::string> pem_bundle_to_x509s(std::string_view pem_bundle);

// Fingerprint calculation (SHA-256)
std::expected<std::string, std::string> compute_cert_fingerprint(const X509* cert, bool colon_format = true);
std::expected<std::string, std::string> compute_spki_fingerprint(const X509* cert, bool colon_format = true);
std::expected<std::string, std::string> compute_pkey_spki_fingerprint(const EVP_PKEY* pkey, bool colon_format = true);

// File helper with strict POSIX permissions (e.g. 0600 for keys/profiles, 0700 for dirs)
std::expected<void, std::string> write_file_secure(const std::filesystem::path& path, std::string_view content, mode_t mode = 0600);
std::expected<std::string, std::string> read_file_to_string(const std::filesystem::path& path);

} // namespace tmux2::pki
