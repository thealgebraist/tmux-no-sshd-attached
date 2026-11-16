#include "src/pty/terminfo.hpp"

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace tmux2::pty {

namespace {

bool check_terminfo_file(const fs::path& base_dir, std::string_view term) {
    if (term.empty()) return false;
    std::error_code ec;
    if (!fs::exists(base_dir, ec) || !fs::is_directory(base_dir, ec)) {
        return false;
    }

    char first_char = term.front();
    // Standard char subdir (e.g. 't' -> base/t/tmux-256color)
    fs::path p1 = base_dir / std::string(1, first_char) / std::string(term);
    if (fs::exists(p1, ec) && fs::is_regular_file(p1, ec)) {
        return true;
    }

    // Darwin / macOS hex-named subdir (e.g. 't' is 0x74 -> base/74/tmux-256color)
    std::string hex_subdir = std::format("{:02x}", static_cast<unsigned char>(first_char));
    fs::path p2 = base_dir / hex_subdir / std::string(term);
    if (fs::exists(p2, ec) && fs::is_regular_file(p2, ec)) {
        return true;
    }

    return false;
}

bool check_terminfo_path_env(const char* env_val, std::string_view term) {
    if (!env_val || *env_val == '\0') return false;
    std::istringstream stream(env_val);
    std::string dir;
    while (std::getline(stream, dir, ':')) {
        if (!dir.empty() && check_terminfo_file(fs::path(dir), term)) {
            return true;
        }
    }
    return false;
}

bool check_infocmp(std::string_view term) {
    // Attempt fast fork-exec of infocmp -1 <term>
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // Child: suppress stderr & stdout
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::string term_str(term);
        execlp("infocmp", "infocmp", "-1", term_str.c_str(), static_cast<char*>(nullptr));
        _exit(1);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

bool has_terminfo(std::string_view term_name) {
    if (term_name.empty()) return false;

    // 1. Check $TERMINFO
    if (const char* terminfo_env = std::getenv("TERMINFO")) {
        if (check_terminfo_file(fs::path(terminfo_env), term_name)) {
            return true;
        }
    }

    // 2. Check $TERMINFO_DIRS
    if (const char* dirs_env = std::getenv("TERMINFO_DIRS")) {
        if (check_terminfo_path_env(dirs_env, term_name)) {
            return true;
        }
    }

    // 3. User home ~/.terminfo
    if (const char* home_env = std::getenv("HOME")) {
        fs::path user_terminfo = fs::path(home_env) / ".terminfo";
        if (check_terminfo_file(user_terminfo, term_name)) {
            return true;
        }
    }

    // 4. Common system directories on Linux and macOS
    static constexpr std::array<const char*, 6> kSystemPaths = {
        "/usr/share/terminfo",
        "/lib/terminfo",
        "/etc/terminfo",
        "/usr/local/share/terminfo",
        "/opt/homebrew/share/terminfo",
        "/usr/share/lib/terminfo"
    };

    for (const char* sys_path : kSystemPaths) {
        if (check_terminfo_file(fs::path(sys_path), term_name)) {
            return true;
        }
    }

    // 5. Fall back to infocmp(1)
    return check_infocmp(term_name);
}

std::string detect_default_term() {
    if (has_terminfo("tmux-256color")) {
        return "tmux-256color";
    }
    if (has_terminfo("xterm-256color")) {
        return "xterm-256color";
    }
    return "xterm";
}

std::unordered_map<std::string, std::string> build_default_env(std::string_view term) {
    std::unordered_map<std::string, std::string> env;
    std::string term_name = term.empty() ? detect_default_term() : std::string(term);
    env["TERM"] = term_name;
    env["COLORTERM"] = "truecolor";

    // Retain LC_ALL / LANG if available; default to C.UTF-8 / en_US.UTF-8 for UTF-8 compatibility
    const char* lang = std::getenv("LANG");
    if (lang && *lang != '\0') {
        env["LANG"] = lang;
    } else {
        env["LANG"] = "C.UTF-8";
    }

    const char* lc_all = std::getenv("LC_ALL");
    if (lc_all && *lc_all != '\0') {
        env["LC_ALL"] = lc_all;
    }

    return env;
}

} // namespace tmux2::pty
