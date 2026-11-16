#include "src/client/cli_parser.hpp"
#include "src/app/pki_commands.hpp"
#include "src/app/daemon_runner.hpp"
#include "src/app/client_runner.hpp"
#include "src/platform/platform.hpp"

#include <csignal>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
#if defined(SIGPIPE)
    ::signal(SIGPIPE, SIG_IGN);
#endif
    std::string_view prog = (argc > 0 && argv[0]) ? argv[0] : "";
    bool is_daemon = prog.ends_with("tmux2d");

    auto opts = tmux2::client::CliParser::parse(argc, argv, is_daemon);
    if (!opts.success) {
        std::cerr << "Error: " << opts.error_message << "\n";
        return 1;
    }

    if (opts.command == tmux2::client::CliCommandType::Help) {
        std::cout << tmux2::client::CliParser::format_help();
        return 0;
    }

    if (opts.command == tmux2::client::CliCommandType::Version) {
        std::cout << tmux2::client::CliParser::format_version();
        return 0;
    }

    if (opts.command == tmux2::client::CliCommandType::PrintSystemdUnit) {
        std::cout << tmux2::platform::generate_systemd_service_unit();
        return 0;
    }

    if (opts.command == tmux2::client::CliCommandType::PrintLaunchdPlist) {
        std::cout << tmux2::platform::generate_launchagent_plist();
        return 0;
    }

    if (opts.command == tmux2::client::CliCommandType::PkiInit) {
        return tmux2::app::run_pki_init(opts);
    }

    if (opts.command == tmux2::client::CliCommandType::PkiExportClient) {
        return tmux2::app::run_pki_export_client(opts);
    }

    if (opts.command == tmux2::client::CliCommandType::PkiBootstrapServer) {
        return tmux2::app::run_pki_bootstrap_server(opts);
    }

    if (opts.command == tmux2::client::CliCommandType::ProfileImport) {
        return tmux2::app::run_profile_import(opts);
    }

    if (opts.command == tmux2::client::CliCommandType::Bootstrap) {
        return tmux2::app::run_bootstrap(opts);
    }

    if (is_daemon || opts.command == tmux2::client::CliCommandType::Server) {
        return tmux2::app::run_daemon(opts);
    }

    return tmux2::app::run_client(opts);
}
