#include "pki_manager.hpp"
#include "../config/config_parser.hpp"
#include <format>
#include <filesystem>

namespace tmux2::pki {

std::expected<PkiInitResult, std::string> init_pki(
    const std::filesystem::path& pki_dir,
    const std::string& host,
    const std::string& client_user,
    const std::string& server_listen
) {
    if (host.empty()) {
        return std::unexpected("Host parameter cannot be empty for PKI init");
    }

    std::error_code ec;
    std::filesystem::create_directories(pki_dir, ec);
    if (ec) {
        return std::unexpected(std::format("Failed to create PKI directory {}: {}", pki_dir.string(), ec.message()));
    }

    // 1. Generate Root CA
    CaOptions ca_opts{
        .common_name = "tmux2 Root CA (" + host + ")",
        .organization = "tmux2",
        .validity_days = 3650
    };
    auto ca_res = generate_ca(ca_opts);
    if (!ca_res) {
        return std::unexpected(std::format("Failed to generate Root CA: {}", ca_res.error()));
    }
    auto& ca = *ca_res;

    // 2. Generate Server Certificate
    ServerCertOptions srv_opts{
        .common_name = "tmux2 Server (" + host + ")",
        .organization = "tmux2",
        .hosts = {host},
        .validity_days = 365
    };
    auto srv_res = generate_server_cert(srv_opts, ca.private_key.get(), ca.certificate.get());
    if (!srv_res) {
        return std::unexpected(std::format("Failed to generate server certificate: {}", srv_res.error()));
    }
    auto& srv = *srv_res;

    // 3. Generate Client Certificate
    ClientCertOptions cli_opts{
        .common_name = "tmux2 Client (" + client_user + ")",
        .organization = "tmux2",
        .user_name = client_user,
        .custom_san_uris = {},
        .validity_days = 365
    };
    auto cli_res = generate_client_cert(cli_opts, ca.private_key.get(), ca.certificate.get());
    if (!cli_res) {
        return std::unexpected(std::format("Failed to generate client certificate: {}", cli_res.error()));
    }
    auto& cli = *cli_res;

    // Write CA files
    auto ca_crt_path = pki_dir / "ca.crt";
    auto ca_key_path = pki_dir / "ca.key";
    if (auto res = write_file_secure(ca_crt_path, ca.certificate_pem, 0644); !res) return std::unexpected(res.error());
    if (auto res = write_file_secure(ca_key_path, ca.private_key_pem, 0600); !res) return std::unexpected(res.error());

    // Write Server files
    auto srv_crt_path = pki_dir / "server.crt";
    auto srv_key_path = pki_dir / "server.key";
    if (auto res = write_file_secure(srv_crt_path, srv.certificate_pem, 0644); !res) return std::unexpected(res.error());
    if (auto res = write_file_secure(srv_key_path, srv.private_key_pem, 0600); !res) return std::unexpected(res.error());

    // Write Client files
    auto cli_crt_path = pki_dir / "client.crt";
    auto cli_key_path = pki_dir / "client.key";
    if (auto res = write_file_secure(cli_crt_path, cli.certificate_pem, 0644); !res) return std::unexpected(res.error());
    if (auto res = write_file_secure(cli_key_path, cli.private_key_pem, 0600); !res) return std::unexpected(res.error());

    // Generate default server.toml in parent or pki_dir
    auto server_toml_path = pki_dir / "server.toml";
    config::ServerConfig srv_config;
    srv_config.listen_addrs = {server_listen, "[::]:8443"};
    srv_config.tls.server_cert = srv_crt_path.string();
    srv_config.tls.server_key = srv_key_path.string();
    srv_config.tls.client_ca_cert = ca_crt_path.string();
    srv_config.tls.require_client_cert = true;
    srv_config.tls.min_version = "1.3";

    config::AuthorizationRule rule;
    rule.cert_san_uri = "tmux2://user/" + client_user;
    rule.unix_user = client_user;
    rule.allowed_sessions = {"*"};
    srv_config.authorization_rules = {std::move(rule)};

    if (auto res = config::save_server_config(server_toml_path, srv_config); !res) {
        return std::unexpected(std::format("Failed to save default server.toml: {}", res.error()));
    }

    return PkiInitResult{
        .pki_dir = pki_dir,
        .ca_crt_path = ca_crt_path,
        .ca_key_path = ca_key_path,
        .server_crt_path = srv_crt_path,
        .server_key_path = srv_key_path,
        .client_crt_path = cli_crt_path,
        .client_key_path = cli_key_path,
        .server_toml_path = server_toml_path,
        .ca_crt_pem = ca.certificate_pem,
        .server_crt_pem = srv.certificate_pem,
        .client_crt_pem = cli.certificate_pem,
        .client_key_pem = cli.private_key_pem
    };
}

std::expected<std::string, std::string> export_client_profile_string(
    const std::string& profile_name,
    const std::string& server_address,
    const std::string& transport,
    const std::string& ca_crt_pem,
    const std::string& client_crt_pem,
    const std::string& client_key_pem
) {
    if (profile_name.empty()) {
        return std::unexpected("Profile name cannot be empty");
    }
    if (server_address.empty()) {
        return std::unexpected("Server address cannot be empty");
    }
    if (ca_crt_pem.empty()) {
        return std::unexpected("CA certificate cannot be empty");
    }
    if (client_crt_pem.empty()) {
        return std::unexpected("Client certificate cannot be empty");
    }
    if (client_key_pem.empty()) {
        return std::unexpected("Client key cannot be empty");
    }

    config::ProfileConfig profile{
        .version = 1,
        .name = profile_name,
        .server = server_address,
        .transport = transport.empty() ? "auto" : transport,
        .ca_cert = ca_crt_pem,
        .client_cert = client_crt_pem,
        .client_key = client_key_pem
    };

    return config::serialize_profile_config(profile);
}

std::expected<std::string, std::string> export_client_profile_from_files(
    const std::string& profile_name,
    const std::string& server_address,
    const std::string& transport,
    const std::filesystem::path& ca_crt_path,
    const std::filesystem::path& client_crt_path,
    const std::filesystem::path& client_key_path
) {
    auto ca_pem = read_file_to_string(ca_crt_path);
    if (!ca_pem) return std::unexpected(ca_pem.error());

    auto cli_cert_pem = read_file_to_string(client_crt_path);
    if (!cli_cert_pem) return std::unexpected(cli_cert_pem.error());

    auto cli_key_pem = read_file_to_string(client_key_path);
    if (!cli_key_pem) return std::unexpected(cli_key_pem.error());

    return export_client_profile_string(
        profile_name,
        server_address,
        transport,
        *ca_pem,
        *cli_cert_pem,
        *cli_key_pem
    );
}

std::expected<config::ClientConfig, std::string> import_client_profile(
    std::string_view profile_toml_content,
    const std::filesystem::path& config_dir
) {
    auto profile_res = config::parse_profile_config(profile_toml_content);
    if (!profile_res) {
        return std::unexpected(profile_res.error());
    }
    const auto& profile = *profile_res;

    std::filesystem::path base_dir = config_dir.empty() ? config::expand_path("~/.config/tmux2") : config_dir;
    std::filesystem::path host_pki_dir = base_dir / "pki" / profile.name;

    std::error_code ec;
    std::filesystem::create_directories(host_pki_dir, ec);
    if (ec) {
        return std::unexpected(std::format("Failed to create profile PKI directory {}: {}", host_pki_dir.string(), ec.message()));
    }

    auto ca_path = host_pki_dir / "ca.crt";
    auto client_crt_path = host_pki_dir / "client.crt";
    auto client_key_path = host_pki_dir / "client.key";

    if (auto res = write_file_secure(ca_path, profile.ca_cert, 0644); !res) return std::unexpected(res.error());
    if (auto res = write_file_secure(client_crt_path, profile.client_cert, 0644); !res) return std::unexpected(res.error());
    if (auto res = write_file_secure(client_key_path, profile.client_key, 0600); !res) return std::unexpected(res.error());

    // Update config.toml
    auto config_path = base_dir / "config.toml";
    config::ClientConfig client_cfg;
    if (std::filesystem::exists(config_path, ec)) {
        auto load_res = config::load_client_config(config_path);
        if (load_res) {
            client_cfg = std::move(*load_res);
        }
    }

    if (client_cfg.default_host.empty()) {
        client_cfg.default_host = profile.name;
    }

    config::HostConfig host_entry;
    host_entry.address = profile.server;
    host_entry.transport = profile.transport;
    host_entry.ca_cert = ca_path.string();
    host_entry.client_cert = client_crt_path.string();
    host_entry.client_key = client_key_path.string();
    host_entry.auto_reconnect = true;

    client_cfg.hosts[profile.name] = std::move(host_entry);

    if (auto res = config::save_client_config(config_path, client_cfg); !res) {
        return std::unexpected(res.error());
    }

    return client_cfg;
}

std::expected<config::ClientConfig, std::string> import_client_profile_file(
    const std::filesystem::path& profile_path,
    const std::filesystem::path& config_dir
) {
    auto content = read_file_to_string(profile_path);
    if (!content) {
        return std::unexpected(content.error());
    }
    return import_client_profile(*content, config_dir);
}

} // namespace tmux2::pki
