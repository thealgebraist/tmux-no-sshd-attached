#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tmux2::config {

inline std::filesystem::path expand_path(std::string_view path_str) {
    if (path_str.empty()) {
        return {};
    }
    if (path_str[0] == '~') {
        const char* home = std::getenv("HOME");
        std::filesystem::path base = home ? home : "/tmp";
        if (path_str.size() == 1) {
            return base;
        }
        if (path_str[1] == '/') {
            return base / path_str.substr(2);
        }
    }
    return std::filesystem::path(path_str);
}

struct HostConfig {
    std::string address;
    std::string transport = "auto"; // "auto", "quic", "tcp"
    std::string ca_cert;
    std::string client_cert;
    std::string client_key;
    bool auto_reconnect = true;
    std::optional<std::string> server_fingerprint; // For TOFU or cert pinning
};

struct ClientConfig {
    std::string default_host;
    std::string prefix_key = "C-b";
    uint32_t escape_time_ms = 0;
    bool mouse = true;
    std::string clipboard = "osc52";
    std::map<std::string, HostConfig> hosts;
};

struct ServerTlsConfig {
    std::string server_cert;
    std::string server_key;
    std::string client_ca_cert;
    bool require_client_cert = true;
    std::string min_version = "1.3";
};

struct AuthorizationRule {
    std::optional<std::string> cert_san_uri;     // e.g. "tmux2://user/alice"
    std::optional<std::string> cert_fingerprint; // SHA-256 fingerprint
    std::optional<std::string> cert_cn;          // Common Name
    std::string unix_user;                       // Target UNIX username
    std::vector<std::string> allowed_sessions{"*"};
    std::optional<std::string> role;             // e.g. "admin", "readonly"
};

struct ServerConfig {
    std::vector<std::string> listen_addrs{"0.0.0.0:8443", "[::]:8443"};
    std::string local_socket = "/run/tmux2/1000/default.sock";
    std::string mode = "user"; // "user" or "system"
    size_t scrollback_lines = 50000;
    std::string session_idle_timeout = "never";
    std::string resume_token_ttl = "24h";
    ServerTlsConfig tls;
    std::vector<AuthorizationRule> authorization_rules;
};

struct ProfileConfig {
    int version = 1;
    std::string name;
    std::string server;
    std::string transport = "auto";
    std::string ca_cert;      // Inline PEM string
    std::string client_cert;  // Inline PEM string
    std::string client_key;   // Inline PEM string
};

} // namespace tmux2::config
