#include "src/app/pki_commands.hpp"
#include "src/pki/pki_manager.hpp"
#include "src/platform/platform.hpp"
#include "src/config/config_parser.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ifaddrs.h>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace tmux2::app {

namespace {

std::string detect_primary_host_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (::getifaddrs(&ifaddr) == -1 || !ifaddr) {
        return "127.0.0.1";
    }

    std::string best_ip;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            char buf[INET_ADDRSTRLEN]{};
            if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
                std::string ip(buf);
                if (ip != "127.0.0.1" && !ip.starts_with("127.")) {
                    best_ip = ip;
                    // Prefer public IP over RFC1918/docker if available
                    if (!ip.starts_with("10.") && !ip.starts_with("172.") && !ip.starts_with("192.168.")) {
                        break;
                    }
                }
            }
        }
    }
    ::freeifaddrs(ifaddr);
    return best_ip.empty() ? "127.0.0.1" : best_ip;
}

bool is_tcp_port_listening(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bool listening = (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(fd);
    return listening;
}

uint16_t parse_port_from_addr(std::string_view addr, uint16_t default_port = 8443) {
    auto colon = addr.rfind(':');
    if (colon != std::string_view::npos && colon + 1 < addr.size()) {
        try {
            int p = std::stoi(std::string(addr.substr(colon + 1)));
            if (p > 0 && p <= 65535) return static_cast<uint16_t>(p);
        } catch (...) {}
    }
    return default_port;
}

} // namespace

int run_pki_init(const client::CliOptions& opts) {
    std::string host = opts.pki_host.empty() ? detect_primary_host_ip() : opts.pki_host;

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

    auto res = pki::init_pki(target_dir, host, user, listen);
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
    std::string server_addr = opts.pki_server.empty()
        ? (detect_primary_host_ip() + ":8443")
        : opts.pki_server;

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
        server_addr,
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

int run_pki_bootstrap_server(const client::CliOptions& opts) {
    std::string host = opts.pki_host.empty() ? detect_primary_host_ip() : opts.pki_host;
    std::string user = opts.pki_user.empty() ? "alice" : opts.pki_user;
    std::string listen = opts.pki_listen.empty() ? "0.0.0.0:8443" : opts.pki_listen;
    uint16_t port = parse_port_from_addr(listen, 8443);
    std::string server_addr = opts.pki_server.empty()
        ? (host + ":" + std::to_string(port))
        : opts.pki_server;

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

    if (!std::filesystem::exists(pki_dir / "ca.crt") ||
        !std::filesystem::exists(pki_dir / "server.crt") ||
        !std::filesystem::exists(pki_dir / "client.crt")) {
        auto init_res = pki::init_pki(pki_dir, host, user, listen);
        if (!init_res) {
            std::cerr << "Error initializing PKI during bootstrap: " << init_res.error() << "\n";
            return 1;
        }
        (void)platform::set_file_permissions(init_res->ca_key_path, 0600);
        (void)platform::set_file_permissions(init_res->server_key_path, 0600);
        (void)platform::set_file_permissions(init_res->client_key_path, 0600);
        (void)platform::set_file_permissions(init_res->server_toml_path, 0600);
    }

    // Start tmux2d in daemonized background mode unless --no-daemon was requested or port is already listening
    if (!opts.detached && !is_tcp_port_listening(port)) {
        auto exe_res = platform::self_exe_path();
        if (exe_res) {
            std::string cmd = "\"" + exe_res->string() + "\" server --daemonize --config \"" +
                              (pki_dir / "server.toml").string() + "\" >/dev/null 2>&1";
            (void)std::system(cmd.c_str());
        }
    }

    auto export_res = pki::export_client_profile_from_files(
        user,
        server_addr,
        opts.pki_transport.empty() ? "auto" : opts.pki_transport,
        pki_dir / "ca.crt",
        pki_dir / "client.crt",
        pki_dir / "client.key"
    );
    if (!export_res) {
        std::cerr << "Error exporting client profile during bootstrap: " << export_res.error() << "\n";
        return 1;
    }

    std::cout << *export_res;
    return 0;
}

int run_profile_import(const client::CliOptions& opts) {
    std::filesystem::path cfg_dir;
    if (!opts.config_dir.empty()) {
        cfg_dir = opts.config_dir;
    } else if (const char* env = std::getenv("TMUX2_CONFIG_DIR")) {
        cfg_dir = std::filesystem::path(env);
    } else {
        cfg_dir = config::expand_path("~/.config/tmux2");
    }

    std::expected<config::ClientConfig, std::string> res;
    if (opts.profile_file.empty() || opts.profile_file == "-") {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        std::string content = ss.str();
        if (content.empty()) {
            std::cerr << "Error: No profile content received on stdin\n";
            return 1;
        }
        res = pki::import_client_profile(content, cfg_dir);
    } else {
        res = pki::import_client_profile_file(opts.profile_file, cfg_dir);
    }

    if (!res) {
        std::cerr << "Error importing client profile: " << res.error() << "\n";
        return 1;
    }

    (void)platform::set_file_permissions(cfg_dir / "config.toml", 0600);
    std::cout << "Successfully imported profile into " << cfg_dir.string() << "\n";
    return 0;
}

int run_bootstrap(const client::CliOptions& opts) {
    // Parse [user@]<host>[:port]
    std::string target = opts.bootstrap_target;
    std::string ssh_dest = target;
    std::string host_only = target;
    std::string port_str = "8443";

    auto at_pos = target.find('@');
    if (at_pos != std::string::npos) {
        host_only = target.substr(at_pos + 1);
    }

    auto colon_pos = host_only.rfind(':');
    if (colon_pos != std::string::npos && colon_pos + 1 < host_only.size()) {
        port_str = host_only.substr(colon_pos + 1);
        host_only = host_only.substr(0, colon_pos);
        // Strip :port from ssh_dest
        auto dest_colon = ssh_dest.rfind(':');
        if (dest_colon != std::string::npos) {
            ssh_dest = ssh_dest.substr(0, dest_colon);
        }
    }

    std::string server_endpoint = opts.pki_server.empty()
        ? (host_only + ":" + port_str)
        : opts.pki_server;
    std::string profile_name = opts.pki_user.empty() ? "alice" : opts.pki_user;

    std::string full_cmd;
    if (!opts.ssh_command.empty()) {
        full_cmd = opts.ssh_command;
    } else {
        std::string remote_script =
            "PATH=\"$HOME/.local/bin:$HOME/tmux2/build:/tmp/tmux2-build:/usr/local/bin:/opt/homebrew/bin:$PATH\"; "
            "BIN=$(command -v tmux2 || true); "
            "if [ -z \"$BIN\" ] && [ -x \"$HOME/tmux2/build/tmux2\" ]; then BIN=\"$HOME/tmux2/build/tmux2\"; fi; "
            "if [ -z \"$BIN\" ]; then echo 'Error: tmux2 binary not found on remote server' >&2; exit 127; fi; "
            "\"$BIN\" pki bootstrap-server --host \"" + host_only +
            "\" --server \"" + server_endpoint +
            "\" --listen \"0.0.0.0:" + port_str +
            "\" --user \"" + profile_name + "\"";
        full_cmd = "ssh " + ssh_dest + " '" + remote_script + "'";
    }

    FILE* pipe = ::popen(full_cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << "Error: Failed to spawn bootstrap command\n";
        return 1;
    }

    std::ostringstream captured;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        captured << buf;
    }
    int status = ::pclose(pipe);
    if (status != 0) {
        std::cerr << "Error: Remote bootstrap command exited with non-zero status\n";
        return 1;
    }

    std::string profile_toml = captured.str();
    if (profile_toml.empty()) {
        std::cerr << "Error: Remote bootstrap returned empty profile\n";
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

    auto import_res = pki::import_client_profile(profile_toml, cfg_dir);
    if (!import_res) {
        std::cerr << "Error importing bootstrapped profile: " << import_res.error() << "\n";
        return 1;
    }

    (void)platform::set_file_permissions(cfg_dir / "config.toml", 0600);
    std::cout << "Successfully bootstrapped profile '" << profile_name << "' (" << server_endpoint
              << ") into " << cfg_dir.string() << "\n";
    return 0;
}

} // namespace tmux2::app
