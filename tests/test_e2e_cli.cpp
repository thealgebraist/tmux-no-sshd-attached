#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "src/pty/pty.hpp"
#include "src/platform/platform.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

#ifndef TMUX2_BIN_PATH
#define TMUX2_BIN_PATH "/tmp/tmux2-cmake-build/tmux2"
#endif

namespace {

struct CommandResult {
    int exit_code{-1};
    std::string stdout_str;
    std::string stderr_str;
};

CommandResult run_cmd(
    const std::string& bin_path,
    const std::vector<std::string>& args,
    const std::vector<std::pair<std::string, std::string>>& env_vars = {},
    std::chrono::milliseconds timeout = 10000ms
) {
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        return {-1, "", "pipe failed"};
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        return {-1, "", "fork failed"};
    }

    if (pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);

        for (const auto& [k, v] : env_vars) {
            ::setenv(k.c_str(), v.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin_path.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        ::execv(bin_path.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    (void)fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    CommandResult result;
    auto start = std::chrono::steady_clock::now();
    bool child_exited = false;

    while (std::chrono::steady_clock::now() - start < timeout) {
        char buf[1024];
        ssize_t n1 = ::read(stdout_pipe[0], buf, sizeof(buf));
        if (n1 > 0) {
            result.stdout_str.append(buf, n1);
        }
        ssize_t n2 = ::read(stderr_pipe[0], buf, sizeof(buf));
        if (n2 > 0) {
            result.stderr_str.append(buf, n2);
        }

        int status = 0;
        pid_t wp = ::waitpid(pid, &status, WNOHANG);
        if (wp == pid) {
            child_exited = true;
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exit_code = 128 + WTERMSIG(status);
            }
            break;
        }

        std::this_thread::sleep_for(15ms);
    }

    if (!child_exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        result.exit_code = 124;
    }

    char buf[1024];
    while (true) {
        ssize_t n = ::read(stdout_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        result.stdout_str.append(buf, n);
    }
    while (true) {
        ssize_t n = ::read(stderr_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        result.stderr_str.append(buf, n);
    }

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    return result;
}

std::string get_tmux2_bin() {
    return TMUX2_BIN_PATH;
}

std::string get_tmux2d_bin() {
    std::filesystem::path p = TMUX2_BIN_PATH;
    return (p.parent_path() / "tmux2d").string();
}

bool check_permissions(const std::filesystem::path& p, mode_t expected_mode) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    return (st.st_mode & 0777) == expected_mode;
}

struct TempDir {
    std::filesystem::path path;

    TempDir(std::string_view prefix = "tmux2_e2e_") {
        std::string pattern = "/tmp/" + std::string(prefix) + "XXXXXX";
        char* d = ::mkdtemp(pattern.data());
        if (d) {
            path = d;
        }
    }

    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

void kill_process(pid_t pid) {
    if (pid > 0) {
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            if (::kill(pid, 0) != 0) return;
            std::this_thread::sleep_for(50ms);
        }
        ::kill(pid, SIGKILL);
    }
}

} // namespace

// ============================================================================
// Test 1: Service Unit Generators
// ============================================================================
TEST_CASE("E2E CLI: Service unit generators --print-systemd-unit and --print-launchd-plist") {
    std::string bin = get_tmux2_bin();

    // 1. systemd unit
    auto res_sysd = run_cmd(bin, {"--print-systemd-unit"});
    CHECK(res_sysd.exit_code == 0);
    CHECK(res_sysd.stdout_str.find("[Unit]") != std::string::npos);
    CHECK(res_sysd.stdout_str.find("[Service]") != std::string::npos);
    CHECK(res_sysd.stdout_str.find("ExecStart=") != std::string::npos);

    // 2. launchd plist
    auto res_plist = run_cmd(bin, {"--print-launchd-plist"});
    CHECK(res_plist.exit_code == 0);
    CHECK(res_plist.stdout_str.find("<?xml") != std::string::npos);
    CHECK(res_plist.stdout_str.find("<plist") != std::string::npos);
    CHECK(res_plist.stdout_str.find("ProgramArguments") != std::string::npos);
}

// ============================================================================
// Test 2: PKI Init, Export-Client, Profile Import with 0600 Permissions
// ============================================================================
TEST_CASE("E2E CLI: PKI init, export-client, and profile import with 0600 permissions") {
    std::string bin = get_tmux2_bin();
    TempDir pki_dir("tmux2_pki_");
    TempDir client_cfg_dir("tmux2_cli_");

    // 1. tmux2 pki init
    auto res_init = run_cmd(bin, {
        "pki", "init",
        "--host", "127.0.0.1",
        "--user", "alice",
        "--dir", pki_dir.path.string(),
        "--listen", "127.0.0.1:19443"
    });
    REQUIRE(res_init.exit_code == 0);

    // Verify generated files and 0600 permissions
    CHECK(std::filesystem::exists(pki_dir.path / "ca.crt"));
    CHECK(std::filesystem::exists(pki_dir.path / "ca.key"));
    CHECK(std::filesystem::exists(pki_dir.path / "server.crt"));
    CHECK(std::filesystem::exists(pki_dir.path / "server.key"));
    CHECK(std::filesystem::exists(pki_dir.path / "client.crt"));
    CHECK(std::filesystem::exists(pki_dir.path / "client.key"));
    CHECK(std::filesystem::exists(pki_dir.path / "server.toml"));

    CHECK(check_permissions(pki_dir.path / "ca.key", 0600));
    CHECK(check_permissions(pki_dir.path / "server.key", 0600));
    CHECK(check_permissions(pki_dir.path / "client.key", 0600));
    CHECK(check_permissions(pki_dir.path / "server.toml", 0600));

    // 2. tmux2 pki export-client
    std::filesystem::path profile_file = pki_dir.path / "alice.tmux2-profile";
    auto res_export = run_cmd(bin, {
        "pki", "export-client", "alice",
        "--server", "127.0.0.1:19443",
        "--dir", pki_dir.path.string(),
        "-o", profile_file.string()
    });
    REQUIRE(res_export.exit_code == 0);
    CHECK(std::filesystem::exists(profile_file));
    CHECK(check_permissions(profile_file, 0600));

    // 3. tmux2 profile import
    auto res_import = run_cmd(bin, {
        "profile", "import", profile_file.string(),
        "--config-dir", client_cfg_dir.path.string()
    });
    REQUIRE(res_import.exit_code == 0);

    std::filesystem::path imported_cfg = client_cfg_dir.path / "config.toml";
    std::filesystem::path imported_key = client_cfg_dir.path / "pki" / "alice" / "client.key";
    CHECK(std::filesystem::exists(imported_cfg));
    CHECK(std::filesystem::exists(imported_key));
    CHECK(check_permissions(imported_cfg, 0600));
    CHECK(check_permissions(imported_key, 0600));
}

// ============================================================================
// Test 3: Local AF_UNIX Auto-Spawn & CLI Lifecycle
// ============================================================================
TEST_CASE("E2E CLI: Local 0600 AF_UNIX auto-spawn and command lifecycle") {
    std::string bin = get_tmux2_bin();
    TempDir test_dir("tmux2_unix_");
    std::filesystem::path sock = test_dir.path / "test.sock";

    // Ensure daemon is not running yet
    CHECK_FALSE(std::filesystem::exists(sock));

    // 1. auto-spawn via new -d -s localtest
    auto res_new = run_cmd(bin, {
        "new", "-d", "-s", "localtest",
        "--unix-socket", sock.string()
    });
    REQUIRE(res_new.exit_code == 0);
    CHECK(res_new.stdout_str.find("localtest") != std::string::npos);

    // Verify socket was created with 0600 permissions
    CHECK(std::filesystem::exists(sock));
    CHECK(check_permissions(sock, 0600));

    // 2. ls
    auto res_ls = run_cmd(bin, {
        "ls",
        "--unix-socket", sock.string()
    });
    CHECK(res_ls.exit_code == 0);
    CHECK(res_ls.stdout_str.find("localtest") != std::string::npos);

    // 3. send-keys
    auto res_send = run_cmd(bin, {
        "send-keys", "-t", "localtest",
        "--unix-socket", sock.string(),
        "echo E2E_UNIX_OK", "Enter"
    });
    CHECK(res_send.exit_code == 0);

    // Wait for shell to execute command
    std::this_thread::sleep_for(400ms);

    // 4. capture-pane
    auto res_cap = run_cmd(bin, {
        "capture-pane", "-t", "localtest", "-p",
        "--unix-socket", sock.string()
    });
    CHECK(res_cap.exit_code == 0);
    CHECK(res_cap.stdout_str.find("E2E_UNIX_OK") != std::string::npos);

    // 5. split-window
    auto res_split = run_cmd(bin, {
        "split-window", "-h", "-t", "localtest",
        "--unix-socket", sock.string()
    });
    CHECK(res_split.exit_code == 0);

    // 6. kill-session
    auto res_kill = run_cmd(bin, {
        "kill-session", "-t", "localtest",
        "--unix-socket", sock.string()
    });
    CHECK(res_kill.exit_code == 0);

    // 7. ls should now be empty
    auto res_ls2 = run_cmd(bin, {
        "ls",
        "--unix-socket", sock.string()
    });
    CHECK(res_ls2.exit_code == 0);
    CHECK(res_ls2.stdout_str.find("localtest") == std::string::npos);

    // Stop auto-spawned daemon to prevent lingering background processes
    std::filesystem::path pid_file = test_dir.path / "test.sock.pid";
    if (std::filesystem::exists(pid_file)) {
        pid_t pid = 0;
        std::ifstream pf(pid_file);
        pf >> pid;
        if (pid > 0) kill_process(pid);
    }
}

// ============================================================================
// Test 4: Remote QUIC mTLS
// ============================================================================
TEST_CASE("E2E CLI: Remote QUIC mTLS daemon and client operations") {
    std::string bin = get_tmux2_bin();
    std::string dbin = get_tmux2d_bin();
    TempDir pki_dir("tmux2_quic_pki_");
    TempDir client_dir("tmux2_quic_cli_");
    std::filesystem::path pid_file = pki_dir.path / "tmux2d.pid";
    std::string listen_port = "19450";
    std::string listen_addr = "127.0.0.1:" + listen_port;

    // 1. Initialize PKI
    auto res_init = run_cmd(bin, {
        "pki", "init",
        "--host", "127.0.0.1",
        "--user", "alice",
        "--dir", pki_dir.path.string(),
        "--listen", listen_addr
    });
    REQUIRE(res_init.exit_code == 0);

    // Export client profile with quic transport
    std::filesystem::path profile_file = pki_dir.path / "alice.tmux2-profile";
    auto res_exp = run_cmd(bin, {
        "pki", "export-client", "alice",
        "--server", listen_addr,
        "--transport", "quic",
        "--dir", pki_dir.path.string(),
        "-o", profile_file.string()
    });
    REQUIRE(res_exp.exit_code == 0);

    // Import profile into client dir
    auto res_imp = run_cmd(bin, {
        "profile", "import", profile_file.string(),
        "--config-dir", client_dir.path.string()
    });
    REQUIRE(res_imp.exit_code == 0);

    // 2. Start daemon (using tmux2d symlink)
    auto res_d = run_cmd(dbin, {
        "--config", (pki_dir.path / "server.toml").string(),
        "--listen", listen_addr,
        "--pid-file", pid_file.string(),
        "--daemonize"
    });
    REQUIRE(res_d.exit_code == 0);

    // Wait for daemon to be ready
    std::this_thread::sleep_for(300ms);
    REQUIRE(std::filesystem::exists(pid_file));

    pid_t dpid = 0;
    {
        std::ifstream pf(pid_file);
        pf >> dpid;
    }
    REQUIRE(dpid > 0);

    // 3. Client operations over QUIC mTLS
    auto res_new = run_cmd(bin, {
        "-H", "alice",
        "--config-dir", client_dir.path.string(),
        "new", "-d", "-s", "quicsess"
    });
    CHECK(res_new.exit_code == 0);
    CHECK(res_new.stdout_str.find("quicsess") != std::string::npos);

    auto res_ls = run_cmd(bin, {
        "-H", "alice",
        "--config-dir", client_dir.path.string(),
        "ls"
    });
    CHECK(res_ls.exit_code == 0);
    CHECK(res_ls.stdout_str.find("quicsess") != std::string::npos);

    auto res_send = run_cmd(bin, {
        "-H", "alice",
        "--config-dir", client_dir.path.string(),
        "send-keys", "-t", "quicsess",
        "echo QUIC_REMOTE_SUCCESS", "Enter"
    });
    CHECK(res_send.exit_code == 0);

    std::this_thread::sleep_for(400ms);

    auto res_cap = run_cmd(bin, {
        "-H", "alice",
        "--config-dir", client_dir.path.string(),
        "capture-pane", "-t", "quicsess", "-p"
    });
    CHECK(res_cap.exit_code == 0);
    CHECK(res_cap.stdout_str.find("QUIC_REMOTE_SUCCESS") != std::string::npos);

    auto res_kill = run_cmd(bin, {
        "-H", "alice",
        "--config-dir", client_dir.path.string(),
        "kill-session", "-t", "quicsess"
    });
    CHECK(res_kill.exit_code == 0);

    // Stop daemon
    kill_process(dpid);
}

// ============================================================================
// Test 5: Remote Happy Eyeballs Fallback to TCP + TLS 1.3
// ============================================================================
TEST_CASE("E2E CLI: Remote Happy Eyeballs fallback to TCP + TLS 1.3 mTLS") {
    std::string bin = get_tmux2_bin();
    std::string dbin = get_tmux2d_bin();
    TempDir pki_dir("tmux2_he_pki_");
    TempDir client_dir("tmux2_he_cli_");
    std::filesystem::path pid_file = pki_dir.path / "tmux2d.pid";
    std::string listen_port = "19460";
    std::string listen_addr = "127.0.0.1:" + listen_port;

    // 1. Initialize PKI
    auto res_init = run_cmd(bin, {
        "pki", "init",
        "--host", "127.0.0.1",
        "--user", "alice",
        "--dir", pki_dir.path.string(),
        "--listen", listen_addr
    });
    REQUIRE(res_init.exit_code == 0);

    // Export client profile with auto transport (Happy Eyeballs default)
    std::filesystem::path profile_file = pki_dir.path / "alice.tmux2-profile";
    auto res_exp = run_cmd(bin, {
        "pki", "export-client", "alice",
        "--server", listen_addr,
        "--transport", "auto",
        "--dir", pki_dir.path.string(),
        "-o", profile_file.string()
    });
    REQUIRE(res_exp.exit_code == 0);

    auto res_imp = run_cmd(bin, {
        "profile", "import", profile_file.string(),
        "--config-dir", client_dir.path.string()
    });
    REQUIRE(res_imp.exit_code == 0);

    // 2. Start daemon with --tcp-only (QUIC disabled -> forces Happy Eyeballs fallback to TCP)
    auto res_d = run_cmd(dbin, {
        "--tcp-only",
        "--config", (pki_dir.path / "server.toml").string(),
        "--listen", listen_addr,
        "--pid-file", pid_file.string(),
        "--daemonize"
    });
    REQUIRE(res_d.exit_code == 0);

    std::this_thread::sleep_for(300ms);
    REQUIRE(std::filesystem::exists(pid_file));

    pid_t dpid = 0;
    {
        std::ifstream pf(pid_file);
        pf >> dpid;
    }
    REQUIRE(dpid > 0);

    // 3. Connect client using auto transport; Happy Eyeballs falls back to TCP TLS 1.3
    auto res_new = run_cmd(bin, {
        "-H", "alice",
        "--config-dir", client_dir.path.string(),
        "new", "-d", "-s", "fallback_sess"
    });
    CHECK(res_new.exit_code == 0);
    CHECK(res_new.stdout_str.find("fallback_sess") != std::string::npos);

    auto res_ls = run_cmd(bin, {
        "-H", "alice",
        "--config-dir", client_dir.path.string(),
        "ls"
    });
    CHECK(res_ls.exit_code == 0);
    CHECK(res_ls.stdout_str.find("fallback_sess") != std::string::npos);

    auto res_kill = run_cmd(bin, {
        "-H", "alice",
        "--config-dir", client_dir.path.string(),
        "kill-session", "-t", "fallback_sess"
    });
    CHECK(res_kill.exit_code == 0);

    kill_process(dpid);
}

// ============================================================================
// Test 6: Interactive PTY Attach & Clean Ctrl-b d Detach
// ============================================================================
TEST_CASE("E2E CLI: Interactive PTY attach and clean Ctrl-b d detach") {
    std::string bin = get_tmux2_bin();
    TempDir test_dir("tmux2_attach_");
    std::filesystem::path sock = test_dir.path / "attach.sock";

    // 1. Create a detached session
    auto res_new = run_cmd(bin, {
        "new", "-d", "-s", "attachtest",
        "--unix-socket", sock.string()
    });
    REQUIRE(res_new.exit_code == 0);

    // 2. Launch interactive attach inside a real pseudo-terminal
    std::string attach_cmd = bin + " attach -t attachtest --unix-socket " + sock.string();
    tmux2::pty::PtyOptions pty_opts;
    pty_opts.cols = 80;
    pty_opts.rows = 24;
    pty_opts.command = attach_cmd;

    auto pty_proc_res = tmux2::pty::PtyProcess::spawn(pty_opts);
    REQUIRE(pty_proc_res.has_value());
    auto proc = std::move(*pty_proc_res);

    // Read initial attach output from master
    std::string attach_output;
    std::array<char, 512> buf{};
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 3s) {
        auto r = proc.read_master(buf);
        if (r.has_value() && *r > 0) {
            attach_output.append(buf.data(), *r);
            if (attach_output.size() > 20) break;
        }
        std::this_thread::sleep_for(20ms);
    }

    // 3. Send Prefix (Ctrl-b -> '\x02') followed by 'd' to detach
    std::string detach_keys = "\x02" "d";
    auto wr = proc.write_master(detach_keys);
    REQUIRE(wr.has_value());

    // 4. Poll exit: process should detach cleanly and exit with code 0
    bool exited = false;
    for (int i = 0; i < 50; ++i) {
        if (proc.poll_exit()) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    CHECK(exited);
    CHECK(proc.exit_code().has_value());
    if (proc.exit_code().has_value()) {
        CHECK(*proc.exit_code() == 0);
    }

    // 5. Verify the session is STILL ALIVE on the server after detach
    auto res_ls = run_cmd(bin, {
        "ls",
        "--unix-socket", sock.string()
    });
    CHECK(res_ls.exit_code == 0);
    CHECK(res_ls.stdout_str.find("attachtest") != std::string::npos);

    // Clean up
    (void)run_cmd(bin, {"kill-session", "-t", "attachtest", "--unix-socket", sock.string()});

    // Stop auto-spawned daemon
    std::filesystem::path pid_file = test_dir.path / "attach.sock.pid";
    if (std::filesystem::exists(pid_file)) {
        pid_t pid = 0;
        std::ifstream pf(pid_file);
        pf >> pid;
        if (pid > 0) kill_process(pid);
    }
}

// ============================================================================
// Test 7: Section 5 Resumption & Reconnection
// ============================================================================
TEST_CASE("E2E CLI: Section 5 session resumption across re-attachments") {
    std::string bin = get_tmux2_bin();
    std::string dbin = get_tmux2d_bin();
    TempDir test_dir("tmux2_recon_");
    std::filesystem::path sock = test_dir.path / "recon.sock";
    std::filesystem::path pid_file = test_dir.path / "recon.pid";

    // 1. Start daemon explicitly
    auto res_d = run_cmd(dbin, {
        "--unix-socket", sock.string(),
        "--pid-file", pid_file.string(),
        "--daemonize"
    });
    REQUIRE(res_d.exit_code == 0);
    std::this_thread::sleep_for(200ms);

    REQUIRE(std::filesystem::exists(pid_file));
    pid_t dpid = 0;
    {
        std::ifstream pf(pid_file);
        pf >> dpid;
    }
    REQUIRE(dpid > 0);

    // 2. Create session and emit content
    auto res_new = run_cmd(bin, {
        "new", "-d", "-s", "recontest",
        "--unix-socket", sock.string()
    });
    REQUIRE(res_new.exit_code == 0);

    auto res_send = run_cmd(bin, {
        "send-keys", "-t", "recontest",
        "--unix-socket", sock.string(),
        "echo SECTION5_RESUME_OK", "Enter"
    });
    CHECK(res_send.exit_code == 0);
    std::this_thread::sleep_for(400ms);

    // 3. Attach #1 and detach cleanly
    std::string attach_cmd = bin + " attach -t recontest --unix-socket " + sock.string();
    {
        tmux2::pty::PtyOptions pty_opts1;
        pty_opts1.cols = 80;
        pty_opts1.rows = 24;
        pty_opts1.command = attach_cmd;
        auto proc_res = tmux2::pty::PtyProcess::spawn(pty_opts1);
        REQUIRE(proc_res.has_value());
        auto proc = std::move(*proc_res);

        std::array<char, 512> buf{};
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 2s) {
            auto r = proc.read_master(buf);
            if (r.has_value() && *r > 0) {
                if (std::string(buf.data(), *r).find("SECTION5_RESUME_OK") != std::string::npos) break;
            }
            std::this_thread::sleep_for(20ms);
        }

        (void)proc.write_master("\x02" "d");
        for (int i = 0; i < 40; ++i) {
            if (proc.poll_exit()) break;
            std::this_thread::sleep_for(50ms);
        }
        CHECK(proc.poll_exit());
    }

    // 4. Attach #2 (re-attachment resuming session state)
    {
        tmux2::pty::PtyOptions pty_opts2;
        pty_opts2.cols = 80;
        pty_opts2.rows = 24;
        pty_opts2.command = attach_cmd;
        auto proc_res2 = tmux2::pty::PtyProcess::spawn(pty_opts2);
        REQUIRE(proc_res2.has_value());
        auto proc2 = std::move(*proc_res2);

        std::string out2;
        std::array<char, 512> buf{};
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 2s) {
            auto r = proc2.read_master(buf);
            if (r.has_value() && *r > 0) {
                out2.append(buf.data(), *r);
                if (out2.find("SECTION5_RESUME_OK") != std::string::npos) break;
            }
            std::this_thread::sleep_for(20ms);
        }
        CHECK(out2.find("SECTION5_RESUME_OK") != std::string::npos);

        (void)proc2.write_master("\x02" "d");
        for (int i = 0; i < 40; ++i) {
            if (proc2.poll_exit()) break;
            std::this_thread::sleep_for(50ms);
        }
        CHECK(proc2.poll_exit());
    }

    // Cleanup session and daemon
    (void)run_cmd(bin, {"kill-session", "-t", "recontest", "--unix-socket", sock.string()});
    kill_process(dpid);
}
