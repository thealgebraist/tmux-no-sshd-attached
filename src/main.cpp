#include "src/client/cli_parser.hpp"
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    std::string_view prog = (argc > 0 && argv[0]) ? argv[0] : "";
    bool is_daemon = prog.ends_with("tmux2d");

    auto opts = tmux2::client::CliParser::parse(argc, argv);
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
    if (is_daemon || opts.command == tmux2::client::CliCommandType::Server) {
        // Daemon / server entrypoint placeholder
        return 0;
    }
    return 0;
}
