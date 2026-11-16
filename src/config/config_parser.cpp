#include "config_parser.hpp"
#include <toml++/toml.hpp>
#include <format>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace tmux2::config {

namespace {

std::string normalize_fp(std::string_view fp) {
    std::string s;
    s.reserve(fp.size());
    for (char c : fp) {
        if (c != ':' && c != ' ') {
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (s.starts_with("sha256")) {
        s = s.substr(6);
    }
    return s;
}

bool session_allowed(const std::vector<std::string>& allowed, std::string_view req) {
    if (req.empty()) {
        return true;
    }
    for (const auto& a : allowed) {
        if (a == "*" || a == req) {
            return true;
        }
    }
    return false;
}

} // namespace

std::expected<ClientConfig, std::string> parse_client_config(std::string_view toml_str) {
    try {
        auto tbl = toml::parse(toml_str);
        ClientConfig config;

        config.default_host = tbl["default_host"].value_or("");
        config.prefix_key = tbl["prefix_key"].value_or("C-b");
        config.escape_time_ms = tbl["escape_time_ms"].value_or<uint32_t>(0);
        config.mouse = tbl["mouse"].value_or(true);
        config.clipboard = tbl["clipboard"].value_or("osc52");

        if (const auto* hosts_node = tbl["hosts"].as_table()) {
            for (const auto& [key, node] : *hosts_node) {
                if (const auto* host_tbl = node.as_table()) {
                    HostConfig host;
                    host.address = (*host_tbl)["address"].value_or("");
                    host.transport = (*host_tbl)["transport"].value_or("auto");
                    host.ca_cert = (*host_tbl)["ca_cert"].value_or("");
                    host.client_cert = (*host_tbl)["client_cert"].value_or("");
                    host.client_key = (*host_tbl)["client_key"].value_or("");
                    host.auto_reconnect = (*host_tbl)["auto_reconnect"].value_or(true);
                    if (auto fp = (*host_tbl)["server_fingerprint"].value<std::string>()) {
                        host.server_fingerprint = *fp;
                    }
                    config.hosts[std::string(key.str())] = std::move(host);
                }
            }
        }

        return config;
    } catch (const toml::parse_error& err) {
        return std::unexpected(std::format("TOML client parse error: {}", err.description()));
    }
}

std::string serialize_client_config(const ClientConfig& config) {
    toml::table tbl;
    tbl.insert_or_assign("default_host", config.default_host);
    tbl.insert_or_assign("prefix_key", config.prefix_key);
    tbl.insert_or_assign("escape_time_ms", static_cast<int64_t>(config.escape_time_ms));
    tbl.insert_or_assign("mouse", config.mouse);
    tbl.insert_or_assign("clipboard", config.clipboard);

    toml::table hosts_tbl;
    for (const auto& [name, host] : config.hosts) {
        toml::table h;
        h.insert_or_assign("address", host.address);
        h.insert_or_assign("transport", host.transport);
        h.insert_or_assign("ca_cert", host.ca_cert);
        h.insert_or_assign("client_cert", host.client_cert);
        h.insert_or_assign("client_key", host.client_key);
        h.insert_or_assign("auto_reconnect", host.auto_reconnect);
        if (host.server_fingerprint) {
            h.insert_or_assign("server_fingerprint", *host.server_fingerprint);
        }
        hosts_tbl.insert_or_assign(name, std::move(h));
    }
    tbl.insert_or_assign("hosts", std::move(hosts_tbl));

    std::ostringstream ss;
    ss << tbl;
    return ss.str();
}

std::expected<ClientConfig, std::string> load_client_config(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return std::unexpected(std::format("Could not open config file: {}", path.string()));
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return parse_client_config(ss.str());
}

std::expected<void, std::string> save_client_config(const std::filesystem::path& path, const ClientConfig& config) {
    auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file(path);
    if (!file) {
        return std::unexpected(std::format("Could not open file for writing: {}", path.string()));
    }
    file << serialize_client_config(config);
    return {};
}

std::expected<ServerConfig, std::string> parse_server_config(std::string_view toml_str) {
    try {
        auto tbl = toml::parse(toml_str);
        ServerConfig config;

        if (const auto* addrs = tbl["listen_addrs"].as_array()) {
            config.listen_addrs.clear();
            for (const auto& el : *addrs) {
                if (auto str = el.value<std::string>()) {
                    config.listen_addrs.push_back(*str);
                }
            }
        }
        config.local_socket = tbl["local_socket"].value_or("/run/tmux2/1000/default.sock");
        config.mode = tbl["mode"].value_or("user");
        config.scrollback_lines = tbl["scrollback_lines"].value_or<size_t>(50000);
        config.session_idle_timeout = tbl["session_idle_timeout"].value_or("never");
        config.resume_token_ttl = tbl["resume_token_ttl"].value_or("24h");

        if (const auto* tls_node = tbl["tls"].as_table()) {
            config.tls.server_cert = (*tls_node)["server_cert"].value_or("");
            config.tls.server_key = (*tls_node)["server_key"].value_or("");
            config.tls.client_ca_cert = (*tls_node)["client_ca_cert"].value_or("");
            config.tls.require_client_cert = (*tls_node)["require_client_cert"].value_or(true);
            config.tls.min_version = (*tls_node)["min_version"].value_or("1.3");
        }

        // Support [[authorization.allow]] or [[authorization]]
        const toml::array* auth_arr = nullptr;
        if (const auto* auth_tbl = tbl["authorization"].as_table()) {
            auth_arr = (*auth_tbl)["allow"].as_array();
        } else if (const auto* arr = tbl["authorization"].as_array()) {
            auth_arr = arr;
        }

        if (auth_arr) {
            config.authorization_rules.clear();
            for (const auto& node : *auth_arr) {
                if (const auto* rule_tbl = node.as_table()) {
                    AuthorizationRule rule;
                    if (auto uri = (*rule_tbl)["cert_san_uri"].value<std::string>()) {
                        rule.cert_san_uri = *uri;
                    }
                    if (auto fp = (*rule_tbl)["cert_fingerprint"].value<std::string>()) {
                        rule.cert_fingerprint = *fp;
                    }
                    if (auto cn = (*rule_tbl)["cert_cn"].value<std::string>()) {
                        rule.cert_cn = *cn;
                    }
                    rule.unix_user = (*rule_tbl)["unix_user"].value_or("");
                    if (auto sessions = (*rule_tbl)["allowed_sessions"].as_array()) {
                        rule.allowed_sessions.clear();
                        for (const auto& s : *sessions) {
                            if (auto s_str = s.value<std::string>()) {
                                rule.allowed_sessions.push_back(*s_str);
                            }
                        }
                    }
                    if (auto role = (*rule_tbl)["role"].value<std::string>()) {
                        rule.role = *role;
                    }
                    config.authorization_rules.push_back(std::move(rule));
                }
            }
        }

        return config;
    } catch (const toml::parse_error& err) {
        return std::unexpected(std::format("TOML server parse error: {}", err.description()));
    }
}

std::string serialize_server_config(const ServerConfig& config) {
    toml::table tbl;

    toml::array addrs;
    for (const auto& a : config.listen_addrs) {
        addrs.push_back(a);
    }
    tbl.insert_or_assign("listen_addrs", std::move(addrs));
    tbl.insert_or_assign("local_socket", config.local_socket);
    tbl.insert_or_assign("mode", config.mode);
    tbl.insert_or_assign("scrollback_lines", static_cast<int64_t>(config.scrollback_lines));
    tbl.insert_or_assign("session_idle_timeout", config.session_idle_timeout);
    tbl.insert_or_assign("resume_token_ttl", config.resume_token_ttl);

    toml::table tls;
    tls.insert_or_assign("server_cert", config.tls.server_cert);
    tls.insert_or_assign("server_key", config.tls.server_key);
    tls.insert_or_assign("client_ca_cert", config.tls.client_ca_cert);
    tls.insert_or_assign("require_client_cert", config.tls.require_client_cert);
    tls.insert_or_assign("min_version", config.tls.min_version);
    tbl.insert_or_assign("tls", std::move(tls));

    if (!config.authorization_rules.empty()) {
        toml::array allow_arr;
        for (const auto& rule : config.authorization_rules) {
            toml::table r;
            if (rule.cert_san_uri) {
                r.insert_or_assign("cert_san_uri", *rule.cert_san_uri);
            }
            if (rule.cert_fingerprint) {
                r.insert_or_assign("cert_fingerprint", *rule.cert_fingerprint);
            }
            if (rule.cert_cn) {
                r.insert_or_assign("cert_cn", *rule.cert_cn);
            }
            r.insert_or_assign("unix_user", rule.unix_user);

            toml::array sessions;
            for (const auto& s : rule.allowed_sessions) {
                sessions.push_back(s);
            }
            r.insert_or_assign("allowed_sessions", std::move(sessions));

            if (rule.role) {
                r.insert_or_assign("role", *rule.role);
            }
            allow_arr.push_back(std::move(r));
        }

        toml::table auth_tbl;
        auth_tbl.insert_or_assign("allow", std::move(allow_arr));
        tbl.insert_or_assign("authorization", std::move(auth_tbl));
    }

    std::ostringstream ss;
    ss << tbl;
    return ss.str();
}

std::expected<ServerConfig, std::string> load_server_config(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return std::unexpected(std::format("Could not open server config file: {}", path.string()));
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return parse_server_config(ss.str());
}

std::expected<void, std::string> save_server_config(const std::filesystem::path& path, const ServerConfig& config) {
    auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file(path);
    if (!file) {
        return std::unexpected(std::format("Could not open file for writing: {}", path.string()));
    }
    file << serialize_server_config(config);
    return {};
}

std::expected<ProfileConfig, std::string> parse_profile_config(std::string_view toml_str) {
    try {
        auto tbl = toml::parse(toml_str);
        ProfileConfig config;
        config.version = tbl["version"].value_or(1);
        config.name = tbl["name"].value_or("");
        config.server = tbl["server"].value_or("");
        config.transport = tbl["transport"].value_or("auto");
        config.ca_cert = tbl["ca_cert"].value_or("");
        config.client_cert = tbl["client_cert"].value_or("");
        config.client_key = tbl["client_key"].value_or("");

        if (config.name.empty()) {
            return std::unexpected("Profile config missing 'name'");
        }
        if (config.server.empty()) {
            return std::unexpected("Profile config missing 'server'");
        }
        if (config.ca_cert.empty()) {
            return std::unexpected("Profile config missing 'ca_cert'");
        }
        if (config.client_cert.empty()) {
            return std::unexpected("Profile config missing 'client_cert'");
        }
        if (config.client_key.empty()) {
            return std::unexpected("Profile config missing 'client_key'");
        }

        return config;
    } catch (const toml::parse_error& err) {
        return std::unexpected(std::format("TOML profile parse error: {}", err.description()));
    }
}

std::string serialize_profile_config(const ProfileConfig& config) {
    toml::table tbl;
    tbl.insert_or_assign("version", config.version);
    tbl.insert_or_assign("name", config.name);
    tbl.insert_or_assign("server", config.server);
    tbl.insert_or_assign("transport", config.transport);
    tbl.insert_or_assign("ca_cert", config.ca_cert);
    tbl.insert_or_assign("client_cert", config.client_cert);
    tbl.insert_or_assign("client_key", config.client_key);

    std::ostringstream ss;
    ss << tbl;
    return ss.str();
}

std::expected<ProfileConfig, std::string> load_profile_config(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return std::unexpected(std::format("Could not open profile file: {}", path.string()));
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return parse_profile_config(ss.str());
}

std::expected<void, std::string> save_profile_config(const std::filesystem::path& path, const ProfileConfig& config) {
    auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file(path);
    if (!file) {
        return std::unexpected(std::format("Could not open profile file for writing: {}", path.string()));
    }
    file << serialize_profile_config(config);
    return {};
}

std::expected<const AuthorizationRule*, std::string> authorize_client(
    const ServerConfig& config,
    const tmux2::pki::ClientIdentity& identity,
    std::string_view requested_session
) {
    if (config.authorization_rules.empty()) {
        return std::unexpected("No authorization rules configured on server");
    }

    // Spec mapping order:
    // 1. cert_san_uri matching spiffe:// or tmux2://
    // 2. cert_cn
    // 3. cert_fingerprint
    for (const auto& rule : config.authorization_rules) {
        bool match = false;

        // 1. Match cert_san_uri
        if (rule.cert_san_uri) {
            for (const auto& uri : identity.san_uris) {
                if (uri == *rule.cert_san_uri) {
                    match = true;
                    break;
                }
            }
        }

        // 2. Match cert_cn
        if (!match && rule.cert_cn) {
            if (!identity.common_name.empty() && identity.common_name == *rule.cert_cn) {
                match = true;
            }
        }

        // 3. Match cert_fingerprint
        if (!match && rule.cert_fingerprint) {
            std::string expected_fp = normalize_fp(*rule.cert_fingerprint);
            std::string cert_fp = normalize_fp(identity.cert_fingerprint);
            std::string spki_fp = normalize_fp(identity.spki_fingerprint);
            if (expected_fp == cert_fp || expected_fp == spki_fp) {
                match = true;
            }
        }

        if (match) {
            if (!session_allowed(rule.allowed_sessions, requested_session)) {
                return std::unexpected(std::format(
                    "User '{}' not authorized for session '{}'",
                    rule.unix_user, requested_session
                ));
            }
            return &rule;
        }
    }

    return std::unexpected("Client identity did not match any authorization rule");
}

} // namespace tmux2::config
