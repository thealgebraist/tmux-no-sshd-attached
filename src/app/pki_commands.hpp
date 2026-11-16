#pragma once

#include "src/client/cli_parser.hpp"

namespace tmux2::app {

int run_pki_init(const client::CliOptions& opts);
int run_pki_export_client(const client::CliOptions& opts);
int run_profile_import(const client::CliOptions& opts);

} // namespace tmux2::app
