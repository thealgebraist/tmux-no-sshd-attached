#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

namespace tmux2::platform {

// 1. Cross-platform executable path resolution
std::expected<std::filesystem::path, std::string> self_exe_path();

// 2. Runtime directory and socket path helpers with POSIX 0700/0600 permissions
std::expected<std::filesystem::path, std::string> ensure_runtime_dir(
    const std::filesystem::path& dir, mode_t mode = 0700);

std::expected<std::filesystem::path, std::string> default_runtime_dir();

std::expected<std::filesystem::path, std::string> default_socket_path(
    const std::string& name = "default");

std::expected<void, std::string> set_file_permissions(
    const std::filesystem::path& path, mode_t mode = 0600);

// 3. POSIX daemonization with double-fork, setsid, umask, and optional pid file
struct DaemonizeOptions {
    bool close_fds{true};
    std::filesystem::path pid_file{};
    std::filesystem::path chdir_path{"/"};
    mode_t umask_value{0027};
};

std::expected<void, std::string> daemonize(const DaemonizeOptions& options = {});

// 4. systemd --user service unit generator (Linux)
struct SystemdServiceOptions {
    std::string description{"tmux2 terminal multiplexer daemon"};
    std::string exec_start{}; // If empty, auto-populated with self_exe_path()
    std::string restart{"always"};
    std::string restart_sec{"2s"};
    std::string standard_output{"journal"};
    std::string standard_error{"journal"};
    std::string wanted_by{"default.target"};
};

std::string generate_systemd_service_unit(const SystemdServiceOptions& options = {});

// 5. macOS LaunchAgent plist generator
struct LaunchAgentOptions {
    std::string label{"com.tmux2.tmux2d"};
    std::vector<std::string> program_arguments{}; // If empty, auto-populated with self_exe_path()
    bool keep_alive{true};
    bool run_at_load{true};
    std::string standard_out_path{};
    std::string standard_error_path{};
};

std::string generate_launchagent_plist(const LaunchAgentOptions& options = {});

} // namespace tmux2::platform
