#pragma once

#include "cert_generator.hpp"
#include "identity_validator.hpp"
#include "../config/config_types.hpp"
#include <expected>
#include <filesystem>
#include <string>

namespace tmux2::pki {

struct PkiInitResult {
    std::filesystem::path pki_dir;
    std::filesystem::path ca_crt_path;
    std::filesystem::path ca_key_path;
    std::filesystem::path server_crt_path;
    std::filesystem::path server_key_path;
    std::filesystem::path client_crt_path;
    std::filesystem::path client_key_path;
    std::filesystem::path server_toml_path;
    std::string ca_crt_pem;
    std::string server_crt_pem;
    std::string client_crt_pem;
    std::string client_key_pem;
};

// Generates Ed25519 CA, Server, and Client certs and writes them to pki_dir with 0600 permissions
std::expected<PkiInitResult, std::string> init_pki(
    const std::filesystem::path& pki_dir,
    const std::string& host,
    const std::string& client_user = "alice",
    const std::string& server_listen = "0.0.0.0:8443"
);

// Exports client profile TOML string from PEM files or strings
std::expected<std::string, std::string> export_client_profile_string(
    const std::string& profile_name,
    const std::string& server_address,
    const std::string& transport,
    const std::string& ca_crt_pem,
    const std::string& client_crt_pem,
    const std::string& client_key_pem
);

std::expected<std::string, std::string> export_client_profile_from_files(
    const std::string& profile_name,
    const std::string& server_address,
    const std::string& transport,
    const std::filesystem::path& ca_crt_path,
    const std::filesystem::path& client_crt_path,
    const std::filesystem::path& client_key_path
);

// Imports client profile and writes 0600 files to ~/.config/tmux2/pki/<name>/ and updates config.toml
std::expected<config::ClientConfig, std::string> import_client_profile(
    std::string_view profile_toml_content,
    const std::filesystem::path& config_dir = {}
);

std::expected<config::ClientConfig, std::string> import_client_profile_file(
    const std::filesystem::path& profile_path,
    const std::filesystem::path& config_dir = {}
);

} // namespace tmux2::pki
