#include "src/client/cli_parser.hpp"

#include <charconv>

namespace tmux2::client {

bool CliParser::parse_host_session(std::string_view str, std::string& out_host, std::string& out_session) {
    if (str.starts_with('/') || str.starts_with("./") || str.starts_with("../")) {
        return false;
    }
    auto colon = str.find(':');
    if (colon != std::string_view::npos && colon > 0 && colon + 1 < str.size()) {
        // Check that it's not a port number like localhost:8443 or http://
        std::string_view right = str.substr(colon + 1);
        int dummy = 0;
        auto [ptr, ec] = std::from_chars(right.data(), right.data() + right.size(), dummy);
        if (ec == std::errc() && ptr == right.data() + right.size()) {
            return false; // it's host:port, not host:session
        }
        out_host = std::string(str.substr(0, colon));
        out_session = std::string(right);
        return true;
    }
    return false;
}

CliOptions CliParser::parse(int argc, const char* const* argv, bool is_daemon_default) {
    std::string_view prog = (argc > 0 && argv[0]) ? argv[0] : "";
    if (prog.ends_with("tmux2d")) {
        is_daemon_default = true;
    }

    std::vector<std::string_view> args;
    args.reserve(argc > 1 ? argc - 1 : 0);
    for (int i = 1; i < argc; ++i) {
        if (argv[i]) {
            args.emplace_back(argv[i]);
        }
    }
    return parse(args, is_daemon_default);
}

CliOptions CliParser::parse(std::span<const std::string_view> args, bool is_daemon_default) {
    CliOptions opts;
    size_t i = 0;

    // Check for service unit flags first across all args
    for (std::string_view a : args) {
        if (a == "--print-systemd-unit") {
            opts.command = CliCommandType::PrintSystemdUnit;
            return opts;
        }
        if (a == "--print-launchd-plist") {
            opts.command = CliCommandType::PrintLaunchdPlist;
            return opts;
        }
    }

    // Parse leading global flags
    while (i < args.size()) {
        std::string_view arg = args[i];
        if (arg == "-H" || arg == "--host") {
            if (i + 1 < args.size()) {
                opts.host = std::string(args[++i]);
            } else {
                opts.success = false;
                opts.error_message = "-H requires a host argument";
                return opts;
            }
        } else if (arg == "--server") {
            if (i + 1 < args.size()) {
                opts.server_addr = std::string(args[++i]);
                if (opts.host.empty()) {
                    opts.host = opts.server_addr;
                }
            } else {
                opts.success = false;
                opts.error_message = "--server requires an address argument";
                return opts;
            }
        } else if (arg == "-h" || arg == "--help") {
            opts.command = CliCommandType::Help;
            return opts;
        } else if (arg == "-v" || arg == "--version") {
            opts.command = CliCommandType::Version;
            return opts;
        } else if (arg == "--unix-socket") {
            if (i + 1 < args.size()) {
                opts.unix_socket = std::string(args[++i]);
            } else {
                opts.success = false;
                opts.error_message = "--unix-socket requires a path argument";
                return opts;
            }
        } else if (arg == "--listen") {
            if (i + 1 < args.size()) {
                opts.listen_addr = std::string(args[++i]);
            } else {
                opts.success = false;
                opts.error_message = "--listen requires an address argument";
                return opts;
            }
        } else if (arg == "--tcp-only" || arg == "--no-quic") {
            opts.tcp_only = true;
        } else if (arg == "--pid-file") {
            if (i + 1 < args.size()) {
                opts.pid_file = std::string(args[++i]);
            } else {
                opts.success = false;
                opts.error_message = "--pid-file requires a path argument";
                return opts;
            }
        } else if (arg == "--config-dir") {
            if (i + 1 < args.size()) {
                opts.config_dir = std::string(args[++i]);
            } else {
                opts.success = false;
                opts.error_message = "--config-dir requires a directory argument";
                return opts;
            }
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < args.size()) {
                opts.config_path = std::string(args[++i]);
            } else {
                opts.success = false;
                opts.error_message = "-c requires a config path argument";
                return opts;
            }
        } else if (arg == "--daemonize") {
            opts.daemonize = true;
        } else if (arg.starts_with("-H")) {
            opts.host = std::string(arg.substr(2));
        } else {
            break;
        }
        ++i;
    }

    if (i >= args.size()) {
        if (is_daemon_default || opts.daemonize || !opts.listen_addr.empty()) {
            opts.command = CliCommandType::Server;
        } else {
            opts.command = CliCommandType::AttachSession;
        }
        return opts;
    }

    std::string_view subcmd = args[i++];

    // Check host:session shorthand as first positional (e.g. tmux2 dev:work)
    std::string h, s;
    if (parse_host_session(subcmd, h, s)) {
        opts.host = std::move(h);
        opts.session_name = std::move(s);
        opts.command = CliCommandType::AttachSession;
        return opts;
    }

    if (is_daemon_default && subcmd != "server" && subcmd != "help" && subcmd != "version") {
        // If invoked as tmux2d and subcmd is not recognized, treat all remaining as server flags
        opts.command = CliCommandType::Server;
        --i;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "--daemonize") opts.daemonize = true;
            else if ((a == "-c" || a == "--config") && i + 1 < args.size()) opts.config_path = std::string(args[++i]);
            else if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--listen" && i + 1 < args.size()) opts.listen_addr = std::string(args[++i]);
            else if (a == "--tcp-only" || a == "--no-quic") opts.tcp_only = true;
            else if (a == "--pid-file" && i + 1 < args.size()) opts.pid_file = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            ++i;
        }
        return opts;
    }

    if (subcmd == "help") {
        opts.command = CliCommandType::Help;
    } else if (subcmd == "version") {
        opts.command = CliCommandType::Version;
    } else if (subcmd == "ls" || subcmd == "list-sessions") {
        opts.command = CliCommandType::ListSessions;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            else if ((a == "-H" || a == "--host") && i + 1 < args.size()) opts.host = std::string(args[++i]);
            else if (a == "--server" && i + 1 < args.size()) { opts.server_addr = std::string(args[++i]); opts.host = opts.server_addr; }
            ++i;
        }
    } else if (subcmd == "new" || subcmd == "new-session") {
        opts.command = CliCommandType::NewSession;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "-A") opts.attach_if_exists = true;
            else if (a == "-d") opts.detached = true;
            else if (a == "-s" && i + 1 < args.size()) opts.session_name = std::string(args[++i]);
            else if (a == "-n" && i + 1 < args.size()) opts.window_name = std::string(args[++i]);
            else if (a == "-c" && i + 1 < args.size()) opts.cwd = std::string(args[++i]);
            else if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            else if ((a == "-H" || a == "--host") && i + 1 < args.size()) opts.host = std::string(args[++i]);
            else if (a == "--server" && i + 1 < args.size()) { opts.server_addr = std::string(args[++i]); opts.host = opts.server_addr; }
            else {
                opts.args.emplace_back(a);
            }
            ++i;
        }
    } else if (subcmd == "attach" || subcmd == "attach-session") {
        opts.command = CliCommandType::AttachSession;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "-d") opts.detached = true;
            else if (a == "-r") opts.read_only = true;
            else if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            else if ((a == "-H" || a == "--host") && i + 1 < args.size()) opts.host = std::string(args[++i]);
            else if (a == "--server" && i + 1 < args.size()) { opts.server_addr = std::string(args[++i]); opts.host = opts.server_addr; }
            else if (a == "-t" && i + 1 < args.size()) {
                std::string target(args[++i]);
                if (parse_host_session(target, h, s)) {
                    opts.host = std::move(h);
                    opts.session_name = std::move(s);
                } else {
                    opts.session_name = std::move(target);
                }
            } else if (!a.starts_with("-")) {
                if (parse_host_session(a, h, s)) {
                    opts.host = std::move(h);
                    opts.session_name = std::move(s);
                } else {
                    opts.session_name = std::string(a);
                }
            }
            ++i;
        }
    } else if (subcmd == "kill-session") {
        opts.command = CliCommandType::KillSession;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "-t" && i + 1 < args.size()) opts.session_name = std::string(args[++i]);
            else if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            else if ((a == "-H" || a == "--host") && i + 1 < args.size()) opts.host = std::string(args[++i]);
            else if (a == "--server" && i + 1 < args.size()) { opts.server_addr = std::string(args[++i]); opts.host = opts.server_addr; }
            else if (!a.starts_with("-")) opts.session_name = std::string(a);
            ++i;
        }
    } else if (subcmd == "split-window") {
        opts.command = CliCommandType::SplitWindow;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "-h") opts.horizontal_split = true;
            else if (a == "-v") opts.vertical_split = true;
            else if (a == "-t" && i + 1 < args.size()) opts.target_pane = std::string(args[++i]);
            else if (a == "-c" && i + 1 < args.size()) opts.cwd = std::string(args[++i]);
            else if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            else if ((a == "-H" || a == "--host") && i + 1 < args.size()) opts.host = std::string(args[++i]);
            else if (a == "--server" && i + 1 < args.size()) { opts.server_addr = std::string(args[++i]); opts.host = opts.server_addr; }
            else opts.args.emplace_back(a);
            ++i;
        }
    } else if (subcmd == "send-keys") {
        opts.command = CliCommandType::SendKeys;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "-t" && i + 1 < args.size()) opts.target_pane = std::string(args[++i]);
            else if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            else if ((a == "-H" || a == "--host") && i + 1 < args.size()) opts.host = std::string(args[++i]);
            else if (a == "--server" && i + 1 < args.size()) { opts.server_addr = std::string(args[++i]); opts.host = opts.server_addr; }
            else opts.args.emplace_back(a);
            ++i;
        }
    } else if (subcmd == "capture-pane") {
        opts.command = CliCommandType::CapturePane;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "-p") opts.print_to_stdout = true;
            else if (a == "-t" && i + 1 < args.size()) opts.target_pane = std::string(args[++i]);
            else if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            else if ((a == "-H" || a == "--host") && i + 1 < args.size()) opts.host = std::string(args[++i]);
            else if (a == "--server" && i + 1 < args.size()) { opts.server_addr = std::string(args[++i]); opts.host = opts.server_addr; }
            else if (a == "-S" && i + 1 < args.size()) {
                std::string_view val = args[++i];
                std::from_chars(val.data(), val.data() + val.size(), opts.capture_start);
            } else if (a == "-E" && i + 1 < args.size()) {
                std::string_view val = args[++i];
                std::from_chars(val.data(), val.data() + val.size(), opts.capture_end);
            }
            ++i;
        }
    } else if (subcmd == "server") {
        opts.command = CliCommandType::Server;
        while (i < args.size()) {
            std::string_view a = args[i];
            if (a == "--daemonize") opts.daemonize = true;
            else if ((a == "-c" || a == "--config") && i + 1 < args.size()) opts.config_path = std::string(args[++i]);
            else if (a == "--unix-socket" && i + 1 < args.size()) opts.unix_socket = std::string(args[++i]);
            else if (a == "--listen" && i + 1 < args.size()) opts.listen_addr = std::string(args[++i]);
            else if (a == "--tcp-only" || a == "--no-quic") opts.tcp_only = true;
            else if (a == "--pid-file" && i + 1 < args.size()) opts.pid_file = std::string(args[++i]);
            else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
            ++i;
        }
    } else if (subcmd == "pki") {
        if (i < args.size()) {
            std::string_view pki_sub = args[i++];
            if (pki_sub == "init") {
                opts.command = CliCommandType::PkiInit;
                while (i < args.size()) {
                    std::string_view a = args[i];
                    if (a == "--host" && i + 1 < args.size()) opts.pki_host = std::string(args[++i]);
                    else if (a == "--user" && i + 1 < args.size()) opts.pki_user = std::string(args[++i]);
                    else if (a == "--dir" && i + 1 < args.size()) opts.pki_dir = std::string(args[++i]);
                    else if (a == "--listen" && i + 1 < args.size()) opts.pki_listen = std::string(args[++i]);
                    else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
                    ++i;
                }
            } else if (pki_sub == "export-client") {
                opts.command = CliCommandType::PkiExportClient;
                if (i < args.size() && !args[i].starts_with("-")) {
                    opts.pki_user = std::string(args[i++]);
                }
                while (i < args.size()) {
                    std::string_view a = args[i];
                    if (a == "--server" && i + 1 < args.size()) opts.pki_server = std::string(args[++i]);
                    else if ((a == "-o" || a == "--out") && i + 1 < args.size()) opts.pki_out_file = std::string(args[++i]);
                    else if (a == "--dir" && i + 1 < args.size()) opts.pki_dir = std::string(args[++i]);
                    else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
                    else if (a == "--transport" && i + 1 < args.size()) opts.pki_transport = std::string(args[++i]);
                    ++i;
                }
            } else if (pki_sub == "bootstrap-server") {
                opts.command = CliCommandType::PkiBootstrapServer;
                while (i < args.size()) {
                    std::string_view a = args[i];
                    if (a == "--host" && i + 1 < args.size()) opts.pki_host = std::string(args[++i]);
                    else if (a == "--user" && i + 1 < args.size()) opts.pki_user = std::string(args[++i]);
                    else if (a == "--server" && i + 1 < args.size()) opts.pki_server = std::string(args[++i]);
                    else if (a == "--listen" && i + 1 < args.size()) opts.pki_listen = std::string(args[++i]);
                    else if (a == "--dir" && i + 1 < args.size()) opts.pki_dir = std::string(args[++i]);
                    else if (a == "--config-dir" && i + 1 < args.size()) opts.config_dir = std::string(args[++i]);
                    else if (a == "--transport" && i + 1 < args.size()) opts.pki_transport = std::string(args[++i]);
                    else if (a == "--no-daemon") opts.detached = true;
                    ++i;
                }
            } else {
                opts.success = false;
                opts.error_message = "Unknown pki command: " + std::string(pki_sub);
                return opts;
            }
        } else {
            opts.command = CliCommandType::Help;
        }
    } else if (subcmd == "bootstrap") {
        opts.command = CliCommandType::Bootstrap;
        while (i < args.size()) {
            std::string_view a = args[i];
            if ((a == "--user" || a == "--name") && i + 1 < args.size()) {
                opts.pki_user = std::string(args[++i]);
            } else if (a == "--server" && i + 1 < args.size()) {
                opts.pki_server = std::string(args[++i]);
            } else if (a == "--config-dir" && i + 1 < args.size()) {
                opts.config_dir = std::string(args[++i]);
            } else if (a == "--transport" && i + 1 < args.size()) {
                opts.pki_transport = std::string(args[++i]);
            } else if (a == "--ssh-cmd" && i + 1 < args.size()) {
                opts.ssh_command = std::string(args[++i]);
            } else if (!a.starts_with("-") && opts.bootstrap_target.empty()) {
                opts.bootstrap_target = std::string(a);
            }
            ++i;
        }
        if (opts.bootstrap_target.empty() && opts.ssh_command.empty()) {
            opts.success = false;
            opts.error_message = "bootstrap requires a target ([user@]host[:port])";
            return opts;
        }
    } else if (subcmd == "profile") {
        if (i < args.size() && args[i] == "import") {
            opts.command = CliCommandType::ProfileImport;
            ++i;
            while (i < args.size()) {
                std::string_view a = args[i];
                if (a == "--config-dir" && i + 1 < args.size()) {
                    opts.config_dir = std::string(args[++i]);
                } else if ((a == "-" || !a.starts_with("-")) && opts.profile_file.empty()) {
                    opts.profile_file = std::string(a);
                }
                ++i;
            }
            if (opts.profile_file.empty()) {
                opts.profile_file = "-";
            }
        }
    } else {
        // Unknown command
        opts.success = false;
        opts.error_message = "Unknown command: " + std::string(subcmd);
    }

    return opts;
}

std::string CliParser::format_help() {
    return "tmux2 - mTLS-native persistent multi-session terminal multiplexer\n"
           "Usage:\n"
           "  tmux2 [-H host] [command] [options...]\n"
           "  tmux2 host:session\n"
           "\n"
           "Commands:\n"
           "  new [-A] [-d] [-s name] [-n win] [-c dir] [cmd...]\n"
           "  attach [-d] [-r] [-t target]\n"
           "  ls / list-sessions\n"
           "  kill-session [-t target]\n"
           "  split-window [-h|-v] [-t target] [-c dir] [cmd...]\n"
           "  send-keys [-t target] <keys...>\n"
           "  capture-pane [-p] [-t target] [-S start] [-E end]\n"
           "  pki init [--host <host>] [--user <user>] [--dir <dir>] [--listen <addr>]\n"
           "  pki export-client [<name>] [--server <server>] [-o <file>] [--dir <dir>]\n"
           "  profile import [<file>|-] [--config-dir <dir>]\n"
           "  bootstrap [user@]<host>[:port] [--name <profile>] [--config-dir <dir>]\n"
           "  server [--daemonize] [-c config] [--unix-socket <path>] [--listen <addr>] [--tcp-only] [--pid-file <path>]\n";
}

std::string CliParser::format_version() {
    return "tmux2 version 1.0.0 (C++23 mTLS native)\n";
}

} // namespace tmux2::client
