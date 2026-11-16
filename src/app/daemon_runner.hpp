#pragma once

#include "src/client/cli_parser.hpp"

namespace tmux2::app {

int run_daemon(const client::CliOptions& opts);

} // namespace tmux2::app
