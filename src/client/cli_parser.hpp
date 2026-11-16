#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tmux2::client {

enum class CliCommandType {
    None,
    Help,
    Version,
    PrintSystemdUnit,
    PrintLaunchdPlist,
    PkiInit,
    PkiExportClient,
    PkiBootstrapServer,
    ProfileImport,
    Bootstrap,
    NewSession,
    AttachSession,
    ListSessions,
    KillSession,
    SplitWindow,
    SendKeys,
    CapturePane,
    Server,
};

struct CliOptions {
    CliCommandType command{CliCommandType::None};
    std::string host;
    std::string session_name;
    std::string target_pane;
    std::string window_name;
    std::string cwd;
    std::string command_line;
    std::vector<std::string> args;

    // Flags
    bool attach_if_exists{false}; // -A
    bool detached{false};         // -d
    bool read_only{false};        // -r
    bool vertical_split{false};   // -v
    bool horizontal_split{false}; // -h
    bool daemonize{false};        // --daemonize
    bool print_to_stdout{false};  // -p for capture-pane
    int64_t capture_start{0};     // -S
    int64_t capture_end{-1};      // -E

    // Extended server/client flags
    std::string unix_socket;      // --unix-socket <path>
    std::string listen_addr;      // --listen <addr>
    bool tcp_only{false};         // --tcp-only / --no-quic
    std::string pid_file;         // --pid-file <path>
    std::string config_dir;       // --config-dir <dir>
    std::string server_addr;      // --server <addr>

    // PKI options
    std::string pki_host;
    std::string pki_user{"alice"};
    std::string pki_dir;
    std::string pki_listen{"0.0.0.0:8443"};
    std::string pki_server;
    std::string pki_out_file;
    std::string pki_transport{"auto"};
    std::string profile_file;
    std::string bootstrap_target;
    std::string ssh_command;

    // Config file override
    std::string config_path;

    // Error message if parse failed
    std::string error_message;
    bool success{true};
};

class CliParser {
public:
    static CliOptions parse(int argc, const char* const* argv, bool is_daemon_default = false);
    static CliOptions parse(std::span<const std::string_view> args, bool is_daemon_default = false);

    static std::string format_help();
    static std::string format_version();

private:
    static bool parse_host_session(std::string_view str, std::string& out_host, std::string& out_session);
};

} // namespace tmux2::client
