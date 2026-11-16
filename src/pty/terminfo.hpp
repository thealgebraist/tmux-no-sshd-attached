#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace tmux2::pty {

/**
 * Checks whether a terminfo entry exists for the given terminal name.
 * Searches $TERMINFO, $TERMINFO_DIRS, standard terminfo system locations
 * on Linux and macOS (including Darwin hex-named subdirectories), and falls
 * back to infocmp(1) if available.
 */
bool has_terminfo(std::string_view term_name);

/**
 * Detects the most capable terminal identifier supported by the host system.
 * Prefers "tmux-256color", falls back to "xterm-256color", and finally "xterm".
 */
std::string detect_default_term();

/**
 * Builds the default environment variable map for a child PTY process,
 * ensuring TERM is set (defaulting to detect_default_term()), COLORTERM is
 * set to "truecolor", and UTF-8 locale environment variables are established.
 */
std::unordered_map<std::string, std::string> build_default_env(std::string_view term = "");

} // namespace tmux2::pty
