#pragma once

#include "config_types.hpp"
#include "../pki/identity_validator.hpp"
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace tmux2::config {

// Client Config
std::expected<ClientConfig, std::string> parse_client_config(std::string_view toml_str);
std::string serialize_client_config(const ClientConfig& config);
std::expected<ClientConfig, std::string> load_client_config(const std::filesystem::path& path);
std::expected<void, std::string> save_client_config(const std::filesystem::path& path, const ClientConfig& config);

// Server Config
std::expected<ServerConfig, std::string> parse_server_config(std::string_view toml_str);
std::string serialize_server_config(const ServerConfig& config);
std::expected<ServerConfig, std::string> load_server_config(const std::filesystem::path& path);
std::expected<void, std::string> save_server_config(const std::filesystem::path& path, const ServerConfig& config);

// Profile Config (.tmux2-profile)
std::expected<ProfileConfig, std::string> parse_profile_config(std::string_view toml_str);
std::string serialize_profile_config(const ProfileConfig& config);
std::expected<ProfileConfig, std::string> load_profile_config(const std::filesystem::path& path);
std::expected<void, std::string> save_profile_config(const std::filesystem::path& path, const ProfileConfig& config);

// Authorization & Identity resolution against ServerConfig rules
// Priority:
// 1. Explicit cert_san_uri match
// 2. cert_cn match
// 3. cert_fingerprint match
// Returns pointer to matched rule if allowed, or error message if denied
std::expected<const AuthorizationRule*, std::string> authorize_client(
    const ServerConfig& config,
    const tmux2::pki::ClientIdentity& identity,
    std::string_view requested_session = ""
);

} // namespace tmux2::config
