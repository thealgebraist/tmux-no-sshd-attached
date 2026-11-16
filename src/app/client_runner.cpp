#include "src/app/client_runner.hpp"
#include "src/client/client_app.hpp"
#include "src/client/tty_raw.hpp"
#include "src/config/config_parser.hpp"
#include "src/platform/platform.hpp"
#include "src/transport/transport.hpp"
#include "src/transport/unix_transport.hpp"
#include "src/transport/tls_tcp_transport.hpp"
#include "src/transport/quic_transport.hpp"
#include "src/transport/happy_eyeballs.hpp"
#include "src/transport/tls_utils.hpp"
#include "src/protocol/messages.hpp"
#include "src/protocol/compression.hpp"
#include <tmux2/reactor/reactor.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>

namespace tmux2::app {

namespace {

std::string load_pem_file_or_string(const std::string& input, const std::filesystem::path& base_dir = {}) {
    if (input.empty()) return {};
    if (input.starts_with("-----BEGIN")) return input;
    std::filesystem::path p = config::expand_path(input);
    if (!std::filesystem::exists(p) && !base_dir.empty()) {
        p = base_dir / input;
    }
    if (std::filesystem::exists(p)) {
        std::ifstream f(p, std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return input;
}

std::filesystem::path find_pki_dir(const client::CliOptions& opts, const std::filesystem::path& cfg_dir) {
    if (!opts.pki_dir.empty() && std::filesystem::exists(opts.pki_dir)) {
        return opts.pki_dir;
    }
    if (!cfg_dir.empty() && std::filesystem::exists(cfg_dir / "pki")) {
        return cfg_dir / "pki";
    }
    if (const char* env = std::getenv("TMUX2_CONFIG_DIR")) {
        std::filesystem::path env_pki = std::filesystem::path(env) / "pki";
        if (std::filesystem::exists(env_pki)) return env_pki;
    }
    auto def_pki = config::expand_path("~/.config/tmux2/pki");
    if (std::filesystem::exists(def_pki)) return def_pki;
    return {};
}

std::expected<void, std::error_code> send_stream_message(
    transport::IStream& stream,
    const protocol::ProtocolMessage& msg,
    bool compress = false)
{
    auto msg_type = protocol::get_message_type(msg);
    protocol::BinaryWriter writer;
    protocol::encode_message_payload(msg, writer);
    uint8_t flags = protocol::kFlagNone;
    std::span<const std::byte> payload = writer.span();
    std::vector<std::byte> comp_buf;
    if (compress && payload.size() > 64) {
        auto comp_res = protocol::zstd_compress(payload, 3);
        if (comp_res) {
            comp_buf = std::move(*comp_res);
            payload = comp_buf;
            flags |= protocol::kFlagZstdCompressed;
        }
    }
    return stream.write_frame(static_cast<uint8_t>(msg_type), flags, payload);
}

std::string quote_arg(const std::string& a) {
    if (a.find(' ') != std::string::npos || a.find('\t') != std::string::npos || a.find('"') != std::string::npos) {
        std::string q = "\"";
        for (char c : a) {
            if (c == '"' || c == '\\') q.push_back('\\');
            q.push_back(c);
        }
        q.push_back('"');
        return q;
    }
    return a;
}

std::string build_command_dsl(const client::CliOptions& opts) {
    switch (opts.command) {
        case client::CliCommandType::ListSessions:
            return "list-sessions";
        case client::CliCommandType::KillSession: {
            std::string cmd = "kill-session";
            if (!opts.session_name.empty()) cmd += " -t " + quote_arg(opts.session_name);
            return cmd;
        }
        case client::CliCommandType::SplitWindow: {
            std::string cmd = "split-window";
            if (opts.horizontal_split) cmd += " -h";
            else if (opts.vertical_split) cmd += " -v";
            if (!opts.target_pane.empty()) cmd += " -t " + quote_arg(opts.target_pane);
            if (!opts.cwd.empty()) cmd += " -c " + quote_arg(opts.cwd);
            for (const auto& a : opts.args) cmd += " " + quote_arg(a);
            return cmd;
        }
        case client::CliCommandType::SendKeys: {
            std::string cmd = "send-keys";
            if (!opts.target_pane.empty()) cmd += " -t " + quote_arg(opts.target_pane);
            for (const auto& a : opts.args) cmd += " " + quote_arg(a);
            return cmd;
        }
        case client::CliCommandType::CapturePane: {
            std::string cmd = "capture-pane";
            if (!opts.target_pane.empty()) cmd += " -t " + quote_arg(opts.target_pane);
            if (opts.print_to_stdout) cmd += " -p";
            if (opts.capture_start != 0) cmd += " -S " + std::to_string(opts.capture_start);
            if (opts.capture_end != -1) cmd += " -E " + std::to_string(opts.capture_end);
            return cmd;
        }
        case client::CliCommandType::NewSession: {
            std::string cmd = "new-session";
            if (opts.detached) cmd += " -d";
            if (!opts.session_name.empty()) cmd += " -s " + quote_arg(opts.session_name);
            if (!opts.window_name.empty()) cmd += " -n " + quote_arg(opts.window_name);
            if (!opts.cwd.empty()) cmd += " -c " + quote_arg(opts.cwd);
            for (const auto& a : opts.args) cmd += " " + quote_arg(a);
            return cmd;
        }
        default:
            return "";
    }
}

std::expected<std::shared_ptr<transport::IConnection>, std::string>
connect_transport(const client::CliOptions& opts) {
    // 1. Determine config directory
    std::filesystem::path cfg_dir;
    if (!opts.config_dir.empty()) {
        cfg_dir = opts.config_dir;
    } else if (const char* env = std::getenv("TMUX2_CONFIG_DIR")) {
        cfg_dir = std::filesystem::path(env);
    } else {
        cfg_dir = config::expand_path("~/.config/tmux2");
    }

    // Load client config if available
    config::ClientConfig client_cfg;
    std::filesystem::path cfg_path = !opts.config_path.empty() ? std::filesystem::path(opts.config_path) : (cfg_dir / "config.toml");
    if (std::filesystem::exists(cfg_path)) {
        auto loaded = config::load_client_config(cfg_path);
        if (loaded) {
            client_cfg = std::move(*loaded);
        }
    }

    std::string target_host = opts.host;
    if (target_host.empty() && !opts.server_addr.empty()) {
        target_host = opts.server_addr;
    }
    if (target_host.empty() && opts.unix_socket.empty() && !client_cfg.default_host.empty()) {
        target_host = client_cfg.default_host;
    }

    // 2. Remote Transport
    if (!target_host.empty()) {
        std::string address = target_host;
        std::string transport_type = "auto";
        std::string ca_file, cert_file, key_file;

        auto hit = client_cfg.hosts.find(target_host);
        if (hit == client_cfg.hosts.end()) {
            for (auto it = client_cfg.hosts.begin(); it != client_cfg.hosts.end(); ++it) {
                if (it->second.address == target_host) {
                    hit = it;
                    break;
                }
            }
            if (hit == client_cfg.hosts.end() && client_cfg.hosts.size() == 1) {
                hit = client_cfg.hosts.begin();
            } else if (hit == client_cfg.hosts.end() && !client_cfg.default_host.empty()) {
                auto def_it = client_cfg.hosts.find(client_cfg.default_host);
                if (def_it != client_cfg.hosts.end()) {
                    hit = def_it;
                }
            }
        }
        if (hit != client_cfg.hosts.end()) {
            if (target_host == hit->first && !hit->second.address.empty()) {
                address = hit->second.address;
            }
            if (!hit->second.transport.empty()) transport_type = hit->second.transport;
            ca_file = hit->second.ca_cert;
            cert_file = hit->second.client_cert;
            key_file = hit->second.client_key;
        }

        auto pki_dir = find_pki_dir(opts, cfg_dir);
        if (ca_file.empty()) ca_file = (pki_dir / "ca.crt").string();
        if (cert_file.empty()) cert_file = (pki_dir / "client.crt").string();
        if (key_file.empty()) key_file = (pki_dir / "client.key").string();

        std::string host = address;
        uint16_t port = 8443;
        auto colon = address.rfind(':');
        if (colon != std::string::npos) {
            host = address.substr(0, colon);
            if (host.front() == '[' && host.back() == ']') {
                host = host.substr(1, host.size() - 2);
            }
            port = static_cast<uint16_t>(std::stoi(address.substr(colon + 1)));
        }

        transport::TlsConfig tls_cfg;
        tls_cfg.cert_pem = load_pem_file_or_string(cert_file, cfg_dir);
        tls_cfg.key_pem = load_pem_file_or_string(key_file, cfg_dir);
        tls_cfg.ca_cert_pem = load_pem_file_or_string(ca_file, cfg_dir);
        tls_cfg.expected_server_name = host;
        tls_cfg.alpn = "tmux2/1";
        tls_cfg.verify_peer = true;

        if (transport_type == "quic") {
            auto q_res = transport::QuicConnector::connect(host, port, tls_cfg);
            if (!q_res) return std::unexpected("QUIC connect failed: " + q_res.error());
            return *q_res;
        } else if (transport_type == "tcp") {
            auto t_res = transport::TcpTlsConnector::connect(host, port, tls_cfg);
            if (!t_res) return std::unexpected("TCP TLS connect failed: " + t_res.error());
            return *t_res;
        } else {
            auto h_res = transport::HappyEyeballsConnector::connect(host, port, tls_cfg);
            if (!h_res) return std::unexpected("Happy Eyeballs connect failed: " + h_res.error());
            return *h_res;
        }
    }

    // 3. Local AF_UNIX Transport with Auto-Spawn
    std::filesystem::path sock_path;
    if (!opts.unix_socket.empty()) {
        sock_path = opts.unix_socket;
    } else if (const char* env = std::getenv("TMUX2_SOCKET")) {
        sock_path = env;
    } else {
        auto def_sock = platform::default_socket_path();
        sock_path = def_sock ? *def_sock : std::filesystem::path("/tmp/tmux2-default.sock");
    }

    auto conn_res = transport::UnixConnector::connect(sock_path, std::chrono::milliseconds(200));
    if (conn_res) {
        return *conn_res;
    }

    // Not running: auto-spawn daemon
    auto exe = platform::self_exe_path();
    std::string exe_str = exe ? exe->string() : "tmux2";

    pid_t pid = ::fork();
    if (pid == 0) {
        std::string sock_str = sock_path.string();
        std::string pid_str = sock_path.string() + ".pid";
        char* const argv[] = {
            const_cast<char*>(exe_str.c_str()),
            const_cast<char*>("server"),
            const_cast<char*>("--unix-socket"),
            const_cast<char*>(sock_str.c_str()),
            const_cast<char*>("--pid-file"),
            const_cast<char*>(pid_str.c_str()),
            const_cast<char*>("--daemonize"),
            nullptr
        };
        ::execv(exe_str.c_str(), argv);
        ::_exit(127);
    }
    if (pid > 0) {
        ::waitpid(pid, nullptr, 0);
    }

    // Retry connecting up to 2 seconds
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto retry_res = transport::UnixConnector::connect(sock_path, std::chrono::milliseconds(100));
        if (retry_res) {
            return *retry_res;
        }
    }

    return std::unexpected("Failed to connect to local daemon at " + sock_path.string());
}

} // namespace

int run_client(const client::CliOptions& opts) {
    bool is_one_shot = (opts.command == client::CliCommandType::ListSessions ||
                        opts.command == client::CliCommandType::KillSession ||
                        opts.command == client::CliCommandType::SplitWindow ||
                        opts.command == client::CliCommandType::SendKeys ||
                        opts.command == client::CliCommandType::CapturePane ||
                        (opts.command == client::CliCommandType::NewSession && opts.detached));

    auto conn_res = connect_transport(opts);
    if (!conn_res) {
        std::cerr << "Connection error: " << conn_res.error() << "\n";
        return 1;
    }
    auto conn = *conn_res;

    auto reactor_res = reactor::Reactor::create_default();
    if (!reactor_res) {
        std::cerr << "Failed to create client reactor\n";
        return 1;
    }
    auto reactor_ptr = std::move(*reactor_res);
    auto& reactor = *reactor_ptr;

    conn->register_with_reactor(reactor);

    std::shared_ptr<transport::IStream> stream;
    if (conn->type() == transport::TransportType::Quic) {
        auto s_res = conn->open_stream(0, true);
        if (!s_res) {
            std::cerr << "Failed to open stream on QUIC connection\n";
            return 1;
        }
        stream = *s_res;
    } else {
        stream = conn->get_stream(0);
    }

    // ------------------------------------------------------------------------
    // Mode A: One-Shot Command Execution
    // ------------------------------------------------------------------------
    if (is_one_shot) {
        std::string dsl_cmd = build_command_dsl(opts);
        int exit_code = 0;
        bool got_resp = false;

        stream->set_on_frame([&](const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
            std::vector<std::byte> decomp;
            std::span<const std::byte> eff = payload;
            if (hdr.is_compressed()) {
                auto d = protocol::zstd_decompress(payload);
                if (d) {
                    decomp = std::move(*d);
                    eff = decomp;
                }
            }
            protocol::BinaryReader reader(eff);
            auto msg_res = protocol::decode_message_payload(static_cast<protocol::MsgType>(hdr.msg_type), reader);
            if (!msg_res) return;

            if (std::holds_alternative<protocol::MsgServerHello>(*msg_res)) {
                protocol::MsgCommandRpcReq req;
                req.rpc_id = 1;
                req.command_dsl = dsl_cmd;
                (void)send_stream_message(*stream, req);
            } else if (std::holds_alternative<protocol::MsgCommandRpcResp>(*msg_res)) {
                const auto& resp = std::get<protocol::MsgCommandRpcResp>(*msg_res);
                if (!resp.stdout_data.empty()) {
                    std::cout << resp.stdout_data;
                    std::cout.flush();
                }
                if (!resp.stderr_data.empty()) {
                    std::cerr << resp.stderr_data;
                    std::cerr.flush();
                }
                exit_code = resp.exit_code;
                got_resp = true;
                reactor.stop();
            }
        });

        // Send ClientHello to initiate handshake
        protocol::MsgClientHello hello;
        hello.protocol_version = 1;
        hello.term_caps = 0;
        (void)send_stream_message(*stream, hello);

        reactor.add_repeating_timer(std::chrono::milliseconds(16), [&](reactor::TimerId) {
            conn->poll_io();
        });

        // Timeout fallback
        reactor.add_timer(std::chrono::seconds(5), [&](reactor::TimerId) {
            if (!got_resp) {
                std::cerr << "Timeout waiting for server response\n";
                exit_code = 124;
                reactor.stop();
            }
        });

        reactor.run();
        conn->unregister_from_reactor(reactor);
        conn->close();
        return exit_code;
    }

    // ------------------------------------------------------------------------
    // Mode B: Interactive TUI (attach, new without -d, or bare tmux2)
    // ------------------------------------------------------------------------
    client::TtyRaw tty(0, 1);
    bool is_tty = tty.is_a_tty();

    if (is_tty) {
        tty.enable_raw_mode();
        tty.enter_alt_screen();
        tty.enable_mouse();
        tty.enable_bracketed_paste();
        tty.hide_cursor();
    }

    auto ws = is_tty ? tty.get_window_size() : client::WindowSize{.cols = 80, .rows = 24};
    client::ClientApp app(ws.cols, ws.rows);

    std::string resume_token;
    uint64_t last_srv_seq = 0;
    bool user_detached = false;

    auto setup_stream_callbacks = [&](std::shared_ptr<transport::IStream>& s) {
        s->set_on_frame([&, s](const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
            std::vector<std::byte> decomp;
            std::span<const std::byte> eff = payload;
            if (hdr.is_compressed()) {
                auto d = protocol::zstd_decompress(payload);
                if (d) {
                    decomp = std::move(*d);
                    eff = decomp;
                }
            }
            protocol::BinaryReader reader(eff);
            auto msg_res = protocol::decode_message_payload(static_cast<protocol::MsgType>(hdr.msg_type), reader);
            if (!msg_res) return;

            if (std::holds_alternative<protocol::MsgServerHello>(*msg_res)) {
                const auto& s_hello = std::get<protocol::MsgServerHello>(*msg_res);
                resume_token = s_hello.resume_token;

                auto replay = app.prepare_reconnect_replay(s_hello.last_client_seq_acked);
                for (const auto& rmsg : replay) {
                    (void)send_stream_message(*s, rmsg);
                }

                if (app.attached_session_id() == 0) {
                    if (opts.command == client::CliCommandType::NewSession) {
                        protocol::MsgNewSessionReq new_req;
                        new_req.name = opts.session_name.empty() ? "0" : opts.session_name;
                        new_req.initial_window_name = opts.window_name;
                        new_req.cwd = opts.cwd;
                        new_req.detached = false;
                        (void)send_stream_message(*s, new_req);
                    } else {
                        protocol::MsgAttachSessionReq att;
                        att.session_name = opts.session_name;
                        att.read_only = opts.read_only;
                        (void)send_stream_message(*s, att);
                    }
                }
            } else if (std::holds_alternative<protocol::MsgFrameDiff>(*msg_res)) {
                last_srv_seq = std::get<protocol::MsgFrameDiff>(*msg_res).server_seq;
            } else if (std::holds_alternative<protocol::MsgScreenSnapshot>(*msg_res)) {
                last_srv_seq = std::get<protocol::MsgScreenSnapshot>(*msg_res).server_seq;
            }

            auto s_res = app.handle_server_message(*msg_res);
            if (!s_res.terminal_output.empty()) {
                [[maybe_unused]] auto w = ::write(1, s_res.terminal_output.data(), s_res.terminal_output.size());
            }
            if (s_res.should_redraw) {
                std::string frame = app.render_frame();
                [[maybe_unused]] auto w = ::write(1, frame.data(), frame.size());
            }
            if (s_res.slice_req) {
                (void)send_stream_message(*s, *s_res.slice_req);
            }
        });
    };

    setup_stream_callbacks(stream);

    // Send initial ClientHello
    protocol::MsgClientHello hello;
    hello.protocol_version = 1;
    hello.term_caps = 0x01;
    hello.target = opts.session_name;
    (void)send_stream_message(*stream, hello);

    // Periodic 16ms timer for I/O polling and automatic reconnect
    std::chrono::steady_clock::time_point last_reconnect{};
    reactor.add_repeating_timer(std::chrono::milliseconds(16), [&](reactor::TimerId) {
        if (user_detached) return;

        if (conn && conn->is_connected()) {
            conn->poll_io();
            return;
        }

        // Connection lost: attempt reconnect
        auto now = std::chrono::steady_clock::now();
        if (now - last_reconnect < std::chrono::milliseconds(250)) {
            return;
        }
        last_reconnect = now;

        auto reconn_res = connect_transport(opts);
        if (reconn_res) {
            if (conn) {
                conn->unregister_from_reactor(reactor);
                conn->close();
            }
            conn = *reconn_res;
            conn->register_with_reactor(reactor);

            if (conn->type() == transport::TransportType::Quic) {
                auto s_res = conn->open_stream(0, true);
                if (s_res) {
                    stream = *s_res;
                } else {
                    return;
                }
            } else {
                stream = conn->get_stream(0);
            }

            if (stream) {
                setup_stream_callbacks(stream);
                protocol::MsgClientHello h;
                h.protocol_version = 1;
                h.term_caps = 0x01;
                h.target = opts.session_name;
                h.resume_token = resume_token;
                h.last_srv_seq = last_srv_seq;
                (void)send_stream_message(*stream, h);
            }
        }
    });

    // Stdin handling on reactor
    if (is_tty) {
        (void)reactor.register_fd(0, reactor::EventFlags::Readable, [&](int fd, reactor::EventFlags) {
            uint8_t in_buf[1024];
            ssize_t n = ::read(fd, in_buf, sizeof(in_buf));
            if (n <= 0) {
                user_detached = true;
                reactor.stop();
                return;
            }
            auto pres = app.process_input_bytes(std::span(in_buf, static_cast<size_t>(n)));
            if (!pres.terminal_output.empty()) {
                [[maybe_unused]] auto w = ::write(1, pres.terminal_output.data(), pres.terminal_output.size());
            }
            if (pres.should_redraw) {
                std::string frame = app.render_frame();
                [[maybe_unused]] auto w = ::write(1, frame.data(), frame.size());
            }
            if (stream && conn && conn->is_connected()) {
                for (const auto& out_msg : pres.outbound_messages) {
                    (void)send_stream_message(*stream, out_msg);
                }
            }
            if (pres.should_detach) {
                user_detached = true;
                reactor.stop();
            }
        });
    }

    // Window resize handler
    (void)reactor.register_signal(SIGWINCH, [&](int) {
        auto new_ws = tty.get_window_size();
        protocol::MsgClientResize rsz;
        rsz.cols = new_ws.cols;
        rsz.rows = new_ws.rows;
        if (stream && conn && conn->is_connected()) {
            (void)send_stream_message(*stream, rsz);
        }
    });

    (void)reactor.register_signal(SIGINT, [&](int) {
        user_detached = true;
        reactor.stop();
    });
    (void)reactor.register_signal(SIGTERM, [&](int) {
        user_detached = true;
        reactor.stop();
    });

    reactor.run();

    if (is_tty) {
        tty.show_cursor();
        tty.leave_alt_screen();
        tty.disable_bracketed_paste();
        tty.disable_mouse();
        tty.disable_raw_mode();
    }

    conn->unregister_from_reactor(reactor);
    conn->close();
    return 0;
}

} // namespace tmux2::app
