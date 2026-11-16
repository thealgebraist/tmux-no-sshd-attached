#include "src/app/daemon_runner.hpp"
#include "src/server/server_engine.hpp"
#include "src/config/config_parser.hpp"
#include "src/platform/platform.hpp"
#include "src/transport/transport.hpp"
#include "src/transport/unix_transport.hpp"
#include "src/transport/tls_tcp_transport.hpp"
#include "src/transport/quic_transport.hpp"
#include "src/transport/tls_utils.hpp"
#include "src/protocol/messages.hpp"
#include "src/protocol/compression.hpp"
#include <tmux2/reactor/reactor.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <csignal>
#include <unistd.h>
#include <unordered_map>
#include <memory>

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

} // namespace

int run_daemon(const client::CliOptions& opts) {
    // 1. Daemonize if requested
    if (opts.daemonize) {
        platform::DaemonizeOptions dopts;
        if (!opts.pid_file.empty()) {
            dopts.pid_file = opts.pid_file;
        }
        auto d_res = platform::daemonize(dopts);
        if (!d_res) {
            std::cerr << "Failed to daemonize: " << d_res.error() << "\n";
            return 1;
        }
    } else if (!opts.pid_file.empty()) {
        std::ofstream pf(opts.pid_file);
        if (pf) {
            pf << ::getpid() << "\n";
        }
    }

    // 2. Load ServerConfig
    std::filesystem::path cfg_dir;
    if (!opts.config_dir.empty()) {
        cfg_dir = opts.config_dir;
    } else if (const char* env = std::getenv("TMUX2_CONFIG_DIR")) {
        cfg_dir = std::filesystem::path(env);
    } else {
        cfg_dir = config::expand_path("~/.config/tmux2");
    }

    config::ServerConfig server_cfg;
    std::filesystem::path loaded_cfg_path;
    if (!opts.config_path.empty()) {
        loaded_cfg_path = opts.config_path;
    } else if (std::filesystem::exists(cfg_dir / "server.toml")) {
        loaded_cfg_path = cfg_dir / "server.toml";
    } else if (std::filesystem::exists(cfg_dir / "pki" / "server.toml")) {
        loaded_cfg_path = cfg_dir / "pki" / "server.toml";
    }

    if (!loaded_cfg_path.empty()) {
        auto loaded = config::load_server_config(loaded_cfg_path);
        if (loaded) {
            server_cfg = std::move(*loaded);
        } else {
            std::cerr << "Warning: Failed to load config " << loaded_cfg_path.string()
                      << ": " << loaded.error() << "\n";
        }
    }

    // 3. Create Reactor & ServerEngine (real PTY)
    auto reactor_res = reactor::Reactor::create_default();
    if (!reactor_res) {
        std::cerr << "Failed to initialize reactor\n";
        return 1;
    }
    auto reactor_ptr = std::move(*reactor_res);
    auto& reactor = *reactor_ptr;

    server::ServerEngine engine(nullptr, /*mock_pty=*/false);

    // Track active client connections & streams
    struct ClientSession {
        std::shared_ptr<transport::IConnection> conn;
        std::chrono::steady_clock::time_point accepted_at{std::chrono::steady_clock::now()};
        bool was_connected{false};
        bool registered{false};
    };
    std::unordered_map<uint64_t, ClientSession> client_sessions;
    std::unordered_map<uint64_t, std::shared_ptr<transport::IStream>> client_streams;
    uint64_t next_client_id = 1;

    // Stream frame callback & message sender setup
    auto setup_stream = [&](uint64_t client_id, const std::shared_ptr<transport::IStream>& stream) {
        if (!stream) return;
        client_streams[client_id] = stream;

        stream->set_on_frame([&engine, client_id](const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
            std::vector<std::byte> decompressed;
            std::span<const std::byte> effective = payload;
            if (hdr.is_compressed()) {
                auto dec = protocol::zstd_decompress(payload);
                if (dec) {
                    decompressed = std::move(*dec);
                    effective = decompressed;
                }
            }
            protocol::BinaryReader reader(effective);
            auto msg_res = protocol::decode_message_payload(static_cast<protocol::MsgType>(hdr.msg_type), reader);
            if (msg_res) {
                engine.handle_message(client_id, *msg_res);
            }
        });
    };

    auto on_accept = [&](std::shared_ptr<transport::IConnection> conn) {
        if (!conn) return;
        conn->register_with_reactor(reactor);

        uint64_t client_id = next_client_id++;
        bool is_conn = conn->is_connected();
        client_sessions[client_id] = ClientSession{
            .conn = conn,
            .accepted_at = std::chrono::steady_clock::now(),
            .was_connected = is_conn,
            .registered = false
        };

        auto register_engine = [client_id, conn, &engine, &client_sessions]() {
            auto cs_it = client_sessions.find(client_id);
            if (cs_it == client_sessions.end() || cs_it->second.registered) return;

            std::string cert_fp = "local-uid";
            if (conn->type() != transport::TransportType::Unix) {
                auto peer_id = conn->peer_identity();
                if (peer_id && !peer_id->cert_fingerprint.empty()) {
                    cert_fp = peer_id->cert_fingerprint;
                } else {
                    cert_fp = "tls-authenticated-peer";
                }
            }
            engine.register_client(client_id, {}, cert_fp);
            cs_it->second.registered = true;
        };

        if (is_conn) {
            register_engine();
        }

        conn->set_on_stream([client_id, setup_stream, register_engine](std::shared_ptr<transport::IStream> s) {
            register_engine();
            setup_stream(client_id, s);
        });

        if (conn->type() != transport::TransportType::Quic) {
            setup_stream(client_id, conn->get_stream(0));
        }
    };

    engine.set_message_sender([&](uint64_t client_id, const protocol::ProtocolMessage& msg) {
        auto sit = client_streams.find(client_id);
        if (sit == client_streams.end() || !sit->second || sit->second->is_closed()) {
            auto cit = client_sessions.find(client_id);
            if (cit != client_sessions.end() && cit->second.conn && cit->second.conn->is_connected()) {
                if (cit->second.conn->type() != transport::TransportType::Quic) {
                    sit = client_streams.emplace(client_id, cit->second.conn->get_stream(0)).first;
                }
            }
        }
        if (sit != client_streams.end() && sit->second && !sit->second->is_closed()) {
            protocol::MsgType type = protocol::get_message_type(msg);
            protocol::BinaryWriter writer;
            protocol::encode_message_payload(msg, writer);
            uint8_t flags = protocol::kFlagNone;
            std::span<const std::byte> payload = writer.span();
            std::vector<std::byte> comp_buf;
            if (payload.size() > 128) {
                auto comp = protocol::zstd_compress(payload, 3);
                if (comp) {
                    comp_buf = std::move(*comp);
                    payload = comp_buf;
                    flags |= protocol::kFlagZstdCompressed;
                }
            }
            (void)sit->second->write_frame(static_cast<uint8_t>(type), flags, payload);
            auto cit = client_sessions.find(client_id);
            if (cit != client_sessions.end() && cit->second.conn) {
                cit->second.conn->poll_io();
            }
        }
    });

    // 4. Setup UnixListener
    std::filesystem::path unix_sock_path;
    std::unique_ptr<transport::UnixListener> unix_listener;

    if (!opts.unix_socket.empty()) {
        unix_sock_path = opts.unix_socket;
    } else if (opts.listen_addr.empty()) {
        if (!server_cfg.local_socket.empty()) {
            unix_sock_path = server_cfg.local_socket;
        } else {
            auto def_sock = platform::default_socket_path();
            if (def_sock) {
                unix_sock_path = *def_sock;
            } else {
                unix_sock_path = "/tmp/tmux2-default.sock";
            }
        }
    }

    if (!unix_sock_path.empty()) {
        (void)platform::ensure_runtime_dir(unix_sock_path.parent_path(), 0700);
        std::error_code ec;
        std::filesystem::remove(unix_sock_path, ec);

        auto ures = transport::UnixListener::create(unix_sock_path);
        if (ures) {
            unix_listener = std::move(*ures);
            (void)platform::set_file_permissions(unix_sock_path, 0600);
            unix_listener->set_on_accept(on_accept);
            unix_listener->register_with_reactor(reactor);
        } else {
            std::cerr << "Failed to bind unix socket " << unix_sock_path << ": " << ures.error() << "\n";
        }
    }

    // 5. Setup Network Listeners (QUIC & TCP/TLS)
    std::unique_ptr<transport::QuicListener> quic_listener;
    std::unique_ptr<transport::TcpTlsListener> tcp_listener;

    std::string net_listen = opts.listen_addr;
    if (net_listen.empty() && !server_cfg.listen_addrs.empty() && !loaded_cfg_path.empty()) {
        net_listen = server_cfg.listen_addrs[0];
    }

    if (!net_listen.empty()) {
        std::string bind_ip = "0.0.0.0";
        uint16_t port = 8443;
        auto colon = net_listen.rfind(':');
        if (colon != std::string::npos) {
            bind_ip = net_listen.substr(0, colon);
            if (bind_ip.empty() || bind_ip == "*") bind_ip = "0.0.0.0";
            if (bind_ip.front() == '[' && bind_ip.back() == ']') {
                bind_ip = bind_ip.substr(1, bind_ip.size() - 2);
            }
            port = static_cast<uint16_t>(std::stoi(net_listen.substr(colon + 1)));
        } else {
            bind_ip = net_listen;
        }

        auto pki_dir = find_pki_dir(opts, cfg_dir);
        std::filesystem::path base_pki = loaded_cfg_path.empty() ? pki_dir : loaded_cfg_path.parent_path();

        std::string cert_src = server_cfg.tls.server_cert.empty() ? (pki_dir / "server.crt").string() : server_cfg.tls.server_cert;
        std::string key_src = server_cfg.tls.server_key.empty() ? (pki_dir / "server.key").string() : server_cfg.tls.server_key;
        std::string ca_src = server_cfg.tls.client_ca_cert.empty() ? (pki_dir / "ca.crt").string() : server_cfg.tls.client_ca_cert;

        transport::TlsConfig tls_cfg;
        tls_cfg.cert_pem = load_pem_file_or_string(cert_src, base_pki);
        tls_cfg.key_pem = load_pem_file_or_string(key_src, base_pki);
        tls_cfg.ca_cert_pem = load_pem_file_or_string(ca_src, base_pki);
        tls_cfg.require_client_cert = server_cfg.tls.require_client_cert;
        tls_cfg.verify_peer = true;
        tls_cfg.alpn = "tmux2/1";

        if (!tls_cfg.cert_pem.empty() && !tls_cfg.key_pem.empty()) {
            if (!opts.tcp_only) {
                auto qres = transport::QuicListener::create(bind_ip, port, tls_cfg);
                if (qres) {
                    quic_listener = std::move(*qres);
                    quic_listener->set_on_accept(on_accept);
                    quic_listener->register_with_reactor(reactor);
                } else {
                    std::cerr << "Warning: Failed to create QUIC listener on " << bind_ip << ":" << port
                              << ": " << qres.error() << "\n";
                }
            }

            auto tcres = transport::TcpTlsListener::create(bind_ip, port, tls_cfg);
            if (tcres) {
                tcp_listener = std::move(*tcres);
                tcp_listener->set_on_accept(on_accept);
                tcp_listener->register_with_reactor(reactor);
            } else {
                std::cerr << "Warning: Failed to create TCP TLS listener on " << bind_ip << ":" << port
                          << ": " << tcres.error() << "\n";
            }
        } else {
            std::cerr << "Warning: No server TLS certificates found; network listeners skipped.\n";
        }
    }

    if (!unix_listener && !quic_listener && !tcp_listener) {
        std::cerr << "Error: No listeners could be started\n";
        return 1;
    }

    // 6. 16ms periodic reactor timer for PTY polling and connection cleanup
    reactor.add_repeating_timer(std::chrono::milliseconds(16), [&](reactor::TimerId) {
        engine.poll_panes();
        engine.attach_to_reactor(reactor);

        auto now = std::chrono::steady_clock::now();
        for (auto it = client_sessions.begin(); it != client_sessions.end();) {
            auto& session = it->second;
            auto& conn = session.conn;

            conn->poll_io();

            if (conn->is_connected()) {
                if (!session.was_connected) {
                    session.was_connected = true;
                }
                if (!session.registered) {
                    std::string cert_fp = "local-uid";
                    if (conn->type() != transport::TransportType::Unix) {
                        auto peer_id = conn->peer_identity();
                        if (peer_id && !peer_id->cert_fingerprint.empty()) {
                            cert_fp = peer_id->cert_fingerprint;
                        } else {
                            cert_fp = "tls-authenticated-peer";
                        }
                    }
                    engine.register_client(it->first, {}, cert_fp);
                    session.registered = true;
                }
                ++it;
            } else {
                bool should_evict = false;
                if (session.was_connected) {
                    should_evict = true;
                } else if (now - session.accepted_at > std::chrono::seconds(10)) {
                    should_evict = true;
                }

                if (should_evict) {
                    if (session.registered) {
                        engine.unregister_client(it->first);
                    }
                    conn->unregister_from_reactor(reactor);
                    client_streams.erase(it->first);
                    it = client_sessions.erase(it);
                } else {
                    ++it;
                }
            }
        }
    });

    // 7. Signal handlers for clean shutdown
    (void)reactor.register_signal(SIGTERM, [&](int) {
        reactor.stop();
    });
    (void)reactor.register_signal(SIGINT, [&](int) {
        reactor.stop();
    });

    // Run the event loop until SIGTERM/SIGINT
    reactor.run();

    // 8. Clean up
    if (unix_listener) {
        unix_listener->unregister_from_reactor(reactor);
        unix_listener->close();
        if (!unix_sock_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(unix_sock_path, ec);
        }
    }
    if (quic_listener) {
        quic_listener->unregister_from_reactor(reactor);
        quic_listener->close();
    }
    if (tcp_listener) {
        tcp_listener->unregister_from_reactor(reactor);
        tcp_listener->close();
    }
    if (!opts.pid_file.empty()) {
        std::error_code ec;
        std::filesystem::remove(opts.pid_file, ec);
    }

    return 0;
}

} // namespace tmux2::app
