#include "src/platform/platform.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace tmux2::platform {

std::expected<std::filesystem::path, std::string> self_exe_path() {
#if defined(__APPLE__)
    uint32_t bufsize = 0;
    _NSGetExecutablePath(nullptr, &bufsize);
    if (bufsize == 0) {
        return std::unexpected("Failed to query executable path buffer size");
    }
    std::string buffer(bufsize, '\0');
    if (_NSGetExecutablePath(buffer.data(), &bufsize) == 0) {
        std::error_code ec;
        auto canonical_path = std::filesystem::canonical(buffer.c_str(), ec);
        if (!ec) {
            return canonical_path;
        }
        return std::filesystem::path(buffer.c_str());
    }
    return std::unexpected("Failed to retrieve executable path on macOS");
#elif defined(__linux__)
    std::string buffer(1024, '\0');
    while (true) {
        ssize_t count = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (count < 0) {
            return std::unexpected("readlink(/proc/self/exe) failed: " + std::string(std::strerror(errno)));
        }
        if (static_cast<size_t>(count) < buffer.size()) {
            buffer.resize(count);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    return std::unexpected("self_exe_path() not implemented for current OS");
#endif
}

std::expected<std::filesystem::path, std::string> ensure_runtime_dir(
    const std::filesystem::path& dir, mode_t mode) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        if (!std::filesystem::create_directories(dir, ec) && ec) {
            return std::unexpected("Failed to create runtime directory " + dir.string() + ": " + ec.message());
        }
    }

    if (::chmod(dir.c_str(), mode) != 0) {
        return std::unexpected("Failed to chmod runtime directory " + dir.string() + " to " +
                               std::to_string(mode) + ": " + std::strerror(errno));
    }

    return dir;
}

std::expected<std::filesystem::path, std::string> default_runtime_dir() {
    std::filesystem::path base_path;
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime && *xdg_runtime != '\0') {
        base_path = std::filesystem::path(xdg_runtime) / "tmux2";
    } else {
        uid_t uid = ::getuid();
        base_path = std::filesystem::path("/tmp") / ("tmux2-" + std::to_string(uid));
    }

    return ensure_runtime_dir(base_path, 0700);
}

std::expected<std::filesystem::path, std::string> default_socket_path(const std::string& name) {
    auto dir_res = default_runtime_dir();
    if (!dir_res) {
        return std::unexpected(dir_res.error());
    }
    return (*dir_res) / (name + ".sock");
}

std::expected<void, std::string> set_file_permissions(
    const std::filesystem::path& path, mode_t mode) {
    if (::chmod(path.c_str(), mode) != 0) {
        return std::unexpected("Failed to chmod " + path.string() + ": " + std::strerror(errno));
    }
    return {};
}

std::expected<void, std::string> daemonize(const DaemonizeOptions& options) {
    // Flush stdio before fork
    std::fflush(stdout);
    std::fflush(stderr);

    // 1. First fork: orphan child from current process group
    pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected("First fork failed: " + std::string(std::strerror(errno)));
    }
    if (pid > 0) {
        // Parent terminates
        ::_exit(0);
    }

    // 2. setsid to become session leader
    if (::setsid() < 0) {
        return std::unexpected("setsid failed: " + std::string(std::strerror(errno)));
    }

    // Ignore SIGHUP so child doesn't die when session group leader exits
    ::signal(SIGHUP, SIG_IGN);

    // 3. Second fork: ensure daemon cannot acquire a controlling terminal
    pid = ::fork();
    if (pid < 0) {
        return std::unexpected("Second fork failed: " + std::string(std::strerror(errno)));
    }
    if (pid > 0) {
        ::_exit(0);
    }

    // Set restrictive umask
    ::umask(options.umask_value);

    // Change directory
    if (!options.chdir_path.empty()) {
        std::error_code ec;
        if (::chdir(options.chdir_path.c_str()) != 0) {
            // Ignore error or continue
        }
    }

    // Write PID file if requested
    if (!options.pid_file.empty()) {
        std::ofstream pid_stream(options.pid_file);
        if (pid_stream.is_open()) {
            pid_stream << ::getpid() << "\n";
            pid_stream.close();
            ::chmod(options.pid_file.c_str(), 0644);
        }
    }

    // 4. Redirect standard file descriptors if requested
    if (options.close_fds) {
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                ::close(devnull);
            }
        }

        // Close any higher open file descriptors
        struct rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            rlim_t max_fd = rl.rlim_cur;
            if (max_fd == RLIM_INFINITY || max_fd > 1024) {
                max_fd = 1024;
            }
            for (int fd = 3; fd < static_cast<int>(max_fd); ++fd) {
                ::close(fd);
            }
        }
    }

    return {};
}

std::string generate_systemd_service_unit(const SystemdServiceOptions& options) {
    std::string exec_cmd = options.exec_start;
    if (exec_cmd.empty()) {
        auto exe_res = self_exe_path();
        if (exe_res) {
            exec_cmd = exe_res->string();
        } else {
            exec_cmd = "/usr/bin/tmux2d";
        }
    }

    std::ostringstream ss;
    ss << "[Unit]\n"
       << "Description=" << options.description << "\n"
       << "Documentation=https://github.com/thealgebraist/tmux-no-sshd-attached\n"
       << "After=network.target\n"
       << "\n"
       << "[Service]\n"
       << "Type=simple\n"
       << "ExecStart=" << exec_cmd << "\n"
       << "Restart=" << options.restart << "\n"
       << "RestartSec=" << options.restart_sec << "\n"
       << "StandardOutput=" << options.standard_output << "\n"
       << "StandardError=" << options.standard_error << "\n"
       << "\n"
       << "[Install]\n"
       << "WantedBy=" << options.wanted_by << "\n";
    return ss.str();
}

std::string generate_launchagent_plist(const LaunchAgentOptions& options) {
    std::vector<std::string> args = options.program_arguments;
    if (args.empty()) {
        auto exe_res = self_exe_path();
        if (exe_res) {
            args.push_back(exe_res->string());
        } else {
            args.push_back("/usr/local/bin/tmux2d");
        }
    }

    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       << "<plist version=\"1.0\">\n"
       << "<dict>\n"
       << "    <key>Label</key>\n"
       << "    <string>" << options.label << "</string>\n"
       << "    <key>ProgramArguments</key>\n"
       << "    <array>\n";
    for (const auto& arg : args) {
        ss << "        <string>" << arg << "</string>\n";
    }
    ss << "    </array>\n"
       << "    <key>RunAtLoad</key>\n"
       << "    <" << (options.run_at_load ? "true" : "false") << "/>\n"
       << "    <key>KeepAlive</key>\n"
       << "    <" << (options.keep_alive ? "true" : "false") << "/>\n";

    if (!options.standard_out_path.empty()) {
        ss << "    <key>StandardOutPath</key>\n"
           << "    <string>" << options.standard_out_path << "</string>\n";
    }
    if (!options.standard_error_path.empty()) {
        ss << "    <key>StandardErrorPath</key>\n"
           << "    <string>" << options.standard_error_path << "</string>\n";
    }

    ss << "</dict>\n"
       << "</plist>\n";
    return ss.str();
}

} // namespace tmux2::platform
