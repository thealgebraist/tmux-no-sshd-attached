#include "src/app/pki_commands.hpp"
#include "src/pki/pki_manager.hpp"
#include "src/platform/platform.hpp"
#include "src/config/config_parser.hpp"

#include <iostream>
#include <filesystem>
#include <cstdlib>

namespace tmux2::app {

int run_pki_init(const client::CliOptions& opts) {
    if (opts.pki_host.empty()) {
        std::cerr << "Error: --host is required for pki init\n";
        return 1;
    }

    std::filesystem::path target_dir;
    if (!opts.pki_dir.empty()) {
        target_dir = opts.pki_dir;
    } else if (!opts.config_dir.empty()) {
        target_dir = std::filesystem::path(opts.config_dir) / "pki";
    } else if (const char* env = std::getenv("TMUX2_CONFIG_DIR")) {
        target_dir = std::filesystem::path(env) / "pki";
    } else {
        target_dir = config::expand_path("~/.config/tmux2/pki");
    }

    std::string user = opts.pki_user.empty() ? "alice" : opts.pki_user;
    std::string listen = opts.pki_listen.empty() ? "0.0.0.0:8443" : opts.pki_listen;

    auto res = pki::init_pki(target_dir, opts.pki_host, user, listen);
    if (!res) {
        std::cerr << "Error initializing PKI: " << res.error() << "\n";
        return 1;
    }

    // Ensure 0600 on keys and server.toml
    (void)platform::set_file_permissions(res->ca_key_path, 0600);
    (void)platform::set_file_permissions(res->server_key_path, 0600);
    (void)platform::set_file_permissions(res->client_key_path, 0600);
    (void)platform::set_file_permissions(res->server_toml_path, 0600);

    std::cout << "Successfully initialized PKI in " << target_dir.string() << "\n";
    std::cout << "  Root CA: " << res->ca_crt_path.string() << "\n";
    std::cout << "  Server Certificate: " << res->server_crt_path.string() << "\n";
    std::cout << "  Client Certificate: " << res->client_crt_path.string() << "\n";
    std::cout << "  Sample Server Config: " << res->server_toml_path.string() << "\n";
    return 0;
}

int run_pki_export_client(const client::CliOptions& opts) {
    if (opts.pki_server.empty()) {
        std::cerr << "Error: --server <addr> is required for pki export-client\n";
        return 1;
    }

    std::filesystem::path pki_dir;
    if (!opts.pki_dir.empty()) {
        pki_dir = opts.pki_dir;
    } else if (!opts.config_dir.empty()) {
        pki_dir = std::filesystem::path(opts.config_dir) / "pki";
    } else if (const char* env = std::getenv("TMUX2_CONFIG_DIR")) {
        pki_dir = std::filesystem::path(env) / "pki";
    } else {
        pki_dir = config::expand_path("~/.config/tmux2/pki");
    }

    std::string user = opts.pki_user.empty() ? "alice" : opts.pki_user;
    auto ca_path = pki_dir / "ca.crt";
    auto client_crt_path = pki_dir / "client.crt";
    auto client_key_path = pki_dir / "client.key";

    auto res = pki::export_client_profile_from_files(
        user,
        opts.pki_server,
        opts.pki_transport.empty() ? "auto" : opts.pki_transport,
        ca_path,
        client_crt_path,
        client_key_path
    );

    if (!res) {
        std::cerr << "Error exporting client profile: " << res.error() << "\n";
        return 1;
    }

    if (!opts.pki_out_file.empty()) {
        auto save_res = pki::write_file_secure(opts.pki_out_file, *res, 0600);
        if (!save_res) {
            std::cerr << "Error saving profile to " << opts.pki_out_file << ": " << save_res.error() << "\n";
            return 1;
        }
        (void)platform::set_file_permissions(opts.pki_out_file, 0600);
        std::cout << "Exported client profile for " << user << " to " << opts.pki_out_file << "\n";
    } else {
        std::cout << *res;
    }

    return 0;
}

int run_profile_import(const client::CliOptions& opts) {
    if (opts.profile_file.empty()) {
        std::cerr << "Error: Profile file path is required for profile import\n";
        return 1;
    }

    std::filesystem::path cfg_dir;
    if (!opts.config_dir.empty()) {
        cfg_dir = opts.config_dir;
    } else if (const char* env = std::getenv("TMUX2_CONFIG_DIR")) {
        cfg_dir = std::filesystem::path(env);
    } else {
        cfg_dir = config::expand_path("~/.config/tmux2");
    }

    auto res = pki::import_client_profile_file(opts.profile_file, cfg_dir);
    if (!res) {
        std::cerr << "Error importing client profile: " << res.error() << "\n";
        return 1;
    }

    (void)platform::set_file_permissions(cfg_dir / "config.toml", 0600);
    std::cout << "Successfully imported profile into " << cfg_dir.string() << "\n";
    return 0;
}

} // namespace tmux2::app
