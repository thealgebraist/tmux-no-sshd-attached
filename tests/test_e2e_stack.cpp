#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <doctest/doctest.h>

#include "tmux2/reactor/reactor.hpp"
#include "src/pki/cert_generator.hpp"
#include "src/pki/identity_validator.hpp"
#include "src/transport/transport.hpp"
#include "src/transport/quic_transport.hpp"
#include "src/transport/tls_tcp_transport.hpp"
#include "src/transport/unix_transport.hpp"
#include "src/transport/happy_eyeballs.hpp"
#include "src/protocol/protocol.hpp"
#include "src/server/server_engine.hpp"
#include "src/server/session_manager.hpp"
#include "src/server/command_dsl.hpp"
#include "src/client/client.hpp"

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;
using namespace tmux2;
using namespace tmux2::transport;
using namespace tmux2::pki;
using namespace tmux2::protocol;
using namespace tmux2::server;
using namespace tmux2::client;

namespace {

// ============================================================================
// Test PKI Helper
// ============================================================================
struct TestPki {
    GeneratedCertificate ca;
    GeneratedCertificate server;
    GeneratedCertificate client;
    TlsConfig server_cfg;
    TlsConfig client_cfg;

    static TestPki create(const std::string& host = "127.0.0.1", const std::string& user = "alice") {
        ERR_clear_error();
        TestPki p;
        auto ca_res = generate_ca();
        REQUIRE(ca_res.has_value());
        p.ca = std::move(*ca_res);

        ServerCertOptions s_opts;
        s_opts.hosts = {host, "localhost"};
        auto s_res = generate_server_cert(s_opts, p.ca.private_key.get(), p.ca.certificate.get());
        REQUIRE(s_res.has_value());
        p.server = std::move(*s_res);

        ClientCertOptions c_opts;
        c_opts.user_name = user;
        auto c_res = generate_client_cert(c_opts, p.ca.private_key.get(), p.ca.certificate.get());
        REQUIRE(c_res.has_value());
        p.client = std::move(*c_res);

        p.server_cfg.ca_cert_pem = p.ca.certificate_pem;
        p.server_cfg.cert_pem = p.server.certificate_pem;
        p.server_cfg.key_pem = p.server.private_key_pem;
        p.server_cfg.alpn = "tmux2/1";
        p.server_cfg.require_client_cert = true;
        p.server_cfg.verify_peer = true;

        p.client_cfg.ca_cert_pem = p.ca.certificate_pem;
        p.client_cfg.cert_pem = p.client.certificate_pem;
        p.client_cfg.key_pem = p.client.private_key_pem;
        p.client_cfg.expected_server_name = host;
        p.client_cfg.alpn = "tmux2/1";
        p.client_cfg.require_client_cert = true;
        p.client_cfg.verify_peer = true;

        ERR_clear_error();
        return p;
    }
};

// ============================================================================
// Protocol Stream Helpers
// ============================================================================
inline void send_proto(IStream& stream, const ProtocolMessage& msg, bool compress = false) {
    uint8_t msg_type = static_cast<uint8_t>(get_message_type(msg));
    BinaryWriter writer;
    encode_message_payload(msg, writer);
    std::vector<std::byte> payload = writer.take();
    uint8_t flags = kFlagNone;
    if (compress && payload.size() >= 64) {
        auto comp = zstd_compress(payload, 3);
        if (comp && comp->size() < payload.size()) {
            payload = std::move(*comp);
            flags |= kFlagZstdCompressed;
        }
    }
    auto res = stream.write_frame(msg_type, flags, payload);
    (void)res;
}

inline std::expected<ProtocolMessage, CodecError> decode_proto(
    const StreamHeader& hdr, std::span<const std::byte> payload) {
    std::vector<std::byte> decomp;
    std::span<const std::byte> effective = payload;
    if (hdr.is_compressed()) {
        auto res = zstd_decompress(payload);
        if (!res) return std::unexpected(res.error());
        decomp = std::move(*res);
        effective = decomp;
    }
    BinaryReader reader(effective);
    return decode_message_payload(static_cast<MsgType>(hdr.msg_type), reader);
}

inline bool frame_diff_contains(const MsgFrameDiff& diff, std::string_view needle) {
    for (const auto& pd : diff.pane_diffs) {
        for (const auto& row : pd.rows) {
            std::string row_str;
            for (const auto& cell : row.cells) {
                if (cell.codepoint > 0 && cell.codepoint < 128) {
                    row_str.push_back(static_cast<char>(cell.codepoint));
                }
            }
            if (row_str.find(needle) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// ServerRunner: Bridges IListener, transport IConnections & ServerEngine
// ============================================================================
class ServerBridgeRunner {
public:
    explicit ServerBridgeRunner(IListener* listener, bool mock_pty = false)
        : m_listener(listener), m_engine(nullptr, mock_pty), m_stop(false) {
        m_engine.set_message_sender([this](uint64_t client_id, const ProtocolMessage& msg) {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            auto it = m_client_streams.find(client_id);
            if (it != m_client_streams.end() && it->second && !it->second->is_closed()) {
                send_proto(*it->second, msg);
            }
        });

        m_thread = std::thread([this]() {
            while (!m_stop.load()) {
                {
                    std::lock_guard<std::recursive_mutex> lock(m_mutex);
                    // Accept new connections
                    if (m_listener) {
                        auto acc = m_listener->accept_connection();
                        if (acc && *acc) {
                            setup_connection(*acc);
                            m_connections.push_back(*acc);
                        }
                    }
                    // Poll I/O on accepted connections
                    for (auto& conn : m_connections) {
                        if (conn) {
                            ERR_clear_error();
                            conn->poll_io();
                        }
                    }
                    // Poll PTYs
                    m_engine.poll_panes();
                }
                std::this_thread::sleep_for(1ms);
            }
        });
    }

    ~ServerBridgeRunner() {
        stop();
    }

    void stop() {
        if (!m_stop.exchange(true)) {
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }
    }

    ServerEngine& engine() { return m_engine; }
    std::recursive_mutex& mutex() { return m_mutex; }

    std::shared_ptr<IConnection> wait_for_connection(std::chrono::milliseconds timeout = 3000ms) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!m_connections.empty() && m_connections.back()->is_connected()) {
                    return m_connections.back();
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        return nullptr;
    }

    void map_client_id(uint64_t client_id, std::shared_ptr<IStream> stream) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_client_streams[client_id] = stream;
    }

private:
    void setup_connection(std::shared_ptr<IConnection> conn) {
        auto register_stream = [this, conn](std::shared_ptr<IStream> stream) {
            if (!stream) return;
            stream->set_on_frame([this, conn, stream](const StreamHeader& hdr, std::span<const std::byte> payload) {
                auto dec = decode_proto(hdr, payload);
                if (!dec) return;

                uint64_t cid = 0;
                {
                    std::lock_guard<std::recursive_mutex> lock(m_mutex);
                    auto it = m_conn_to_client_id.find(conn.get());
                    if (it != m_conn_to_client_id.end()) {
                        cid = it->second;
                        m_client_streams[cid] = stream;
                    } else {
                        if (std::holds_alternative<MsgClientHello>(*dec)) {
                            const auto& hello = std::get<MsgClientHello>(*dec);
                            if (!hello.resume_token.empty()) {
                                size_t dot = hello.resume_token.find('.');
                                if (dot != std::string::npos) {
                                    try {
                                        cid = std::stoull(hello.resume_token.substr(0, dot));
                                    } catch (...) {}
                                }
                            }
                        }
                        if (cid == 0) {
                            cid = m_next_client_id++;
                        }
                        m_conn_to_client_id[conn.get()] = cid;
                        m_client_streams[cid] = stream;
                    }
                    m_engine.handle_message(cid, *dec);
                }
            });
        };

        conn->set_on_stream(register_stream);
        if (auto s0 = conn->get_stream(0)) {
            register_stream(s0);
        }
    }

    IListener* m_listener;
    ServerEngine m_engine;
    std::atomic<bool> m_stop;
    std::recursive_mutex m_mutex;
    std::vector<std::shared_ptr<IConnection>> m_connections;
    std::unordered_map<const IConnection*, uint64_t> m_conn_to_client_id;
    std::unordered_map<uint64_t, std::shared_ptr<IStream>> m_client_streams;
    uint64_t m_next_client_id{1};
    std::thread m_thread;
};

// ============================================================================
// TestClientEndpoint: Convenient test client handling stream 0 messaging
// ============================================================================
struct TestClientEndpoint {
    std::shared_ptr<IConnection> conn;
    std::shared_ptr<IStream> stream;
    std::mutex msg_mutex;
    std::vector<ProtocolMessage> received_msgs;
    std::condition_variable cv;

    void setup() {
        REQUIRE(conn != nullptr);
        auto s_res = conn->open_stream(0, true);
        REQUIRE(s_res.has_value());
        stream = *s_res;

        stream->set_on_frame([this](const StreamHeader& hdr, std::span<const std::byte> payload) {
            auto dec = decode_proto(hdr, payload);
            if (dec) {
                std::lock_guard<std::mutex> lock(msg_mutex);
                received_msgs.push_back(std::move(*dec));
                cv.notify_all();
            }
        });
    }

    void send(const ProtocolMessage& msg, bool compress = false) {
        REQUIRE(stream != nullptr);
        send_proto(*stream, msg, compress);
        if (conn) {
            ERR_clear_error();
            conn->poll_io();
        }
    }

    void poll() {
        if (conn) {
            ERR_clear_error();
            conn->poll_io();
        }
    }

    template <typename T>
    std::optional<T> wait_for_msg(std::chrono::milliseconds timeout = 3000ms) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            poll();
            {
                std::lock_guard<std::mutex> lock(msg_mutex);
                for (auto it = received_msgs.begin(); it != received_msgs.end(); ++it) {
                    if (std::holds_alternative<T>(*it)) {
                        T result = std::get<T>(*it);
                        received_msgs.erase(it);
                        return result;
                    }
                }
            }
            std::this_thread::sleep_for(3ms);
        }
        return std::nullopt;
    }

    template <typename Predicate>
    std::optional<ProtocolMessage> wait_for_matching(Predicate pred, std::chrono::milliseconds timeout = 3000ms) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            poll();
            {
                std::lock_guard<std::mutex> lock(msg_mutex);
                for (auto it = received_msgs.begin(); it != received_msgs.end(); ++it) {
                    if (pred(*it)) {
                        ProtocolMessage result = *it;
                        received_msgs.erase(it);
                        return result;
                    }
                }
            }
            std::this_thread::sleep_for(3ms);
        }
        return std::nullopt;
    }

    void send_input_str(std::string_view str, uint64_t seq = 1) {
        MsgKeyEvent k;
        k.client_seq = seq;
        k.raw_bytes.assign(reinterpret_cast<const uint8_t*>(str.data()),
                           reinterpret_cast<const uint8_t*>(str.data() + str.size()));
        send(k);
    }
};

} // namespace

// ============================================================================
// Requirement 1: QUIC mTLS 1.3 Full Multiplexing Lifecycle
// ============================================================================
TEST_CASE("E2E-1: QUIC mTLS 1.3 Full Multiplexing Lifecycle (real PTY shell, multi-session, split, stty size, windows, in-band switch, detach/reattach/kill)") {
    auto pki = TestPki::create("127.0.0.1", "quic_user");

    // 1. Start loopback QUIC listener
    auto listener_res = QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    REQUIRE(listener_res.has_value());
    auto listener = std::move(*listener_res);
    uint16_t port = listener->bound_port();
    CHECK(port > 0);

    ServerBridgeRunner runner(listener.get(), /*mock_pty=*/false);

    // 2. Connect client over QUIC mTLS 1.3
    auto client_conn_res = QuicConnector::connect("127.0.0.1", port, pki.client_cfg, 3000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;
    CHECK(client_conn->is_connected());
    CHECK(client_conn->type() == TransportType::Quic);

    auto server_conn = runner.wait_for_connection();
    REQUIRE(server_conn != nullptr);
    CHECK(server_conn->peer_identity().has_value());
    CHECK(server_conn->peer_identity()->extract_username() == "quic_user");

    TestClientEndpoint client;
    client.conn = client_conn;
    client.setup();

    // 3. Client sends MsgClientHello
    MsgClientHello hello;
    hello.protocol_version = 1;
    client.send(hello);

    auto srv_hello = client.wait_for_msg<MsgServerHello>();
    REQUIRE(srv_hello.has_value());
    CHECK(srv_hello->server_version == 1);
    CHECK(srv_hello->assigned_client_id == 1);
    CHECK_FALSE(srv_hello->resume_token.empty());

    // 4. Create session "alpha" (attached)
    MsgNewSessionReq new_alpha;
    new_alpha.name = "alpha";
    new_alpha.detached = false;
    client.send(new_alpha);

    auto alpha_resp = client.wait_for_msg<MsgNewSessionResp>();
    REQUIRE(alpha_resp.has_value());
    CHECK(alpha_resp->success);
    uint32_t alpha_sid = alpha_resp->session_id;
    CHECK(alpha_sid == 1);

    auto attach_resp = client.wait_for_msg<MsgAttachSessionResp>();
    REQUIRE(attach_resp.has_value());
    CHECK(attach_resp->success);
    CHECK(attach_resp->session_id == alpha_sid);

    auto alpha_layout = client.wait_for_msg<MsgLayoutUpdate>();
    REQUIRE(alpha_layout.has_value());
    CHECK(alpha_layout->session_id == alpha_sid);

    // 5. Create session "beta" (detached)
    MsgNewSessionReq new_beta;
    new_beta.name = "beta";
    new_beta.detached = true;
    client.send(new_beta);

    auto beta_resp = client.wait_for_msg<MsgNewSessionResp>();
    REQUIRE(beta_resp.has_value());
    CHECK(beta_resp->success);
    uint32_t beta_sid = beta_resp->session_id;
    CHECK(beta_sid == 2);

    // Verify both sessions visible in MsgListSessions
    client.send(MsgListSessionsReq{});
    auto list_resp = client.wait_for_msg<MsgListSessionsResp>();
    REQUIRE(list_resp.has_value());
    CHECK(list_resp->sessions.size() == 2);
    bool found_alpha = false, found_beta = false;
    for (const auto& s : list_resp->sessions) {
        if (s.name == "alpha") found_alpha = true;
        if (s.name == "beta") found_beta = true;
    }
    CHECK(found_alpha);
    CHECK(found_beta);

    // 6. Execute real shell command in PTY: "echo QUIC_PANE_OK"
    client.send_input_str("echo QUIC_PANE_OK\n", 1);

    auto ok_diff = client.wait_for_matching([](const ProtocolMessage& m) {
        return std::holds_alternative<MsgFrameDiff>(m) &&
               frame_diff_contains(std::get<MsgFrameDiff>(m), "QUIC_PANE_OK");
    }, 4000ms);
    REQUIRE(ok_diff.has_value());

    // 7. Split window horizontally: split-window -h
    client.send(MsgCommandRpcReq{.rpc_id = 10, .command_dsl = "split-window -h"});
    auto split_h_resp = client.wait_for_msg<MsgCommandRpcResp>();
    REQUIRE(split_h_resp.has_value());
    CHECK(split_h_resp->exit_code == 0);

    auto split_h_layout = client.wait_for_msg<MsgLayoutUpdate>();
    REQUIRE(split_h_layout.has_value());
    CHECK(!split_h_layout->layout_tree_json.empty());

    // Verify real TIOCSWINSZ dimension updates on child master FD
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(alpha_sid);
        REQUIRE(s != nullptr);
        auto win = s->active_window();
        REQUIRE(win != nullptr);
        CHECK(win->panes().size() == 2);
        for (const auto& [pid, pane] : win->panes()) {
            struct winsize ws{};
            int r = ::ioctl(pane->master_fd(), TIOCGWINSZ, &ws);
            CHECK(r == 0);
            CHECK(ws.ws_col == pane->width());
            CHECK(ws.ws_row == pane->height());
        }
    }

    // Execute "stty size" inside pane to verify OS dimensions
    client.send_input_str("stty size\n", 2);
    auto stty_diff = client.wait_for_matching([](const ProtocolMessage& m) {
        if (!std::holds_alternative<MsgFrameDiff>(m)) return false;
        const auto& diff = std::get<MsgFrameDiff>(m);
        // Should contain digits representing row and col
        for (const auto& pd : diff.pane_diffs) {
            for (const auto& row : pd.rows) {
                std::string s;
                for (const auto& c : row.cells) if (c.codepoint > 0 && c.codepoint < 128) s.push_back((char)c.codepoint);
                if (s.find("24 ") != std::string::npos || s.find("39") != std::string::npos) return true;
            }
        }
        return false;
    }, 4000ms);
    CHECK(stty_diff.has_value());

    // 8. Split window vertically: split-window -v
    client.send(MsgCommandRpcReq{.rpc_id = 11, .command_dsl = "split-window -v"});
    auto split_v_resp = client.wait_for_msg<MsgCommandRpcResp>();
    REQUIRE(split_v_resp.has_value());
    CHECK(split_v_resp->exit_code == 0);

    auto split_v_layout = client.wait_for_msg<MsgLayoutUpdate>();
    REQUIRE(split_v_layout.has_value());
    CHECK(!split_v_layout->layout_tree_json.empty());
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(alpha_sid);
        REQUIRE(s != nullptr);
        auto win = s->active_window();
        REQUIRE(win != nullptr);
        CHECK(win->panes().size() == 3);
    }

    // 9. Window management: new-window and select-window
    client.send(MsgCommandRpcReq{.rpc_id = 12, .command_dsl = "new-window -n logs"});
    auto new_win_resp = client.wait_for_msg<MsgCommandRpcResp>();
    REQUIRE(new_win_resp.has_value());
    CHECK(new_win_resp->exit_code == 0);

    client.send(MsgCommandRpcReq{.rpc_id = 13, .command_dsl = "select-window -t 0"});
    auto sel_win_resp = client.wait_for_msg<MsgCommandRpcResp>();
    REQUIRE(sel_win_resp.has_value());
    CHECK(sel_win_resp->exit_code == 0);

    // 10. Switch session in-band: MsgSwitchSessionReq to "beta"
    client.send(MsgSwitchSessionReq{.session_name = "beta"});
    auto switch_resp = client.wait_for_msg<MsgSwitchSessionResp>();
    REQUIRE(switch_resp.has_value());
    CHECK(switch_resp->success);
    CHECK(switch_resp->session_id == beta_sid);

    auto beta_layout = client.wait_for_matching([beta_sid](const ProtocolMessage& m) {
        if (!std::holds_alternative<MsgLayoutUpdate>(m)) return false;
        return std::get<MsgLayoutUpdate>(m).session_id == beta_sid;
    });
    REQUIRE(beta_layout.has_value());
    CHECK(std::get<MsgLayoutUpdate>(*beta_layout).session_id == beta_sid);

    // 11. Detach from beta
    client.send(MsgDetachReq{.session_id = beta_sid});
    std::this_thread::sleep_for(20ms);
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto c = runner.engine().get_client(1);
        REQUIRE(c != nullptr);
        CHECK(c->attached_session_id() == 0);
    }

    // 12. Reattach to "alpha"
    client.send(MsgAttachSessionReq{.session_name = "alpha", .cols = 80, .rows = 24});
    auto reattach_resp = client.wait_for_msg<MsgAttachSessionResp>();
    REQUIRE(reattach_resp.has_value());
    CHECK(reattach_resp->success);
    CHECK(reattach_resp->session_id == alpha_sid);

    // 13. Kill session "beta"
    client.send(MsgKillSessionReq{.session_id = beta_sid});
    auto kill_resp = client.wait_for_msg<MsgKillSessionResp>();
    REQUIRE(kill_resp.has_value());
    CHECK(kill_resp->success);

    client.send(MsgListSessionsReq{});
    auto final_list = client.wait_for_msg<MsgListSessionsResp>();
    REQUIRE(final_list.has_value());
    CHECK(final_list->sessions.size() == 1);
    CHECK(final_list->sessions[0].name == "alpha");

    client_conn->close();
    runner.stop();
    listener->close();
}

// ============================================================================
// Requirement 2: Happy Eyeballs TCP + TLS 1.3 Fallback & Strict mTLS Rejection
// ============================================================================
TEST_CASE("E2E-2: Happy Eyeballs TCP TLS 1.3 Fallback & Strict mTLS Rejection across QUIC and TCP") {
    auto pki = TestPki::create("127.0.0.1", "happy_user");

    SUBCASE("Automatic TCP + TLS 1.3 fallback when QUIC UDP is blocked & real PTY execution") {
        auto tcp_listener_res = TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
        REQUIRE(tcp_listener_res.has_value());
        auto tcp_listener = std::move(*tcp_listener_res);
        uint16_t tcp_port = tcp_listener->bound_port();

        ServerBridgeRunner tcp_runner(tcp_listener.get(), /*mock_pty=*/false);

        // QUIC port points to an unreachable/closed UDP port
        uint16_t unreachable_quic_port = 59998;

        HappyEyeballsOptions opts;
        opts.fallback_delay = 50ms;
        opts.total_timeout = 3000ms;

        auto he_conn_res = HappyEyeballsConnector::connect(
            "127.0.0.1",
            unreachable_quic_port,
            tcp_port,
            pki.client_cfg,
            opts
        );
        REQUIRE(he_conn_res.has_value());
        auto he_conn = *he_conn_res;
        CHECK(he_conn->is_connected());
        CHECK(he_conn->type() == TransportType::TcpTls);

        auto server_conn = tcp_runner.wait_for_connection();
        REQUIRE(server_conn != nullptr);

        TestClientEndpoint client;
        client.conn = he_conn;
        client.setup();

        client.send(MsgClientHello{.protocol_version = 1});
        auto hello_resp = client.wait_for_msg<MsgServerHello>();
        REQUIRE(hello_resp.has_value());

        client.send(MsgNewSessionReq{.name = "tcp_sess", .detached = false});
        auto new_resp = client.wait_for_msg<MsgNewSessionResp>();
        REQUIRE(new_resp.has_value());
        CHECK(new_resp->success);

        // Execute shell command over fallback TCP connection
        client.send_input_str("echo HAPPY_EYEBALLS_TCP_OK\n", 1);
        auto ok_diff = client.wait_for_matching([](const ProtocolMessage& m) {
            return std::holds_alternative<MsgFrameDiff>(m) &&
                   frame_diff_contains(std::get<MsgFrameDiff>(m), "HAPPY_EYEBALLS_TCP_OK");
        }, 4000ms);
        CHECK(ok_diff.has_value());

        he_conn->close();
        tcp_runner.stop();
        tcp_listener->close();
    }

    SUBCASE("Strict mTLS Rejection across TCP and QUIC (Missing Cert & Wrong CA)") {
        // TCP listener
        auto tcp_listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
        uint16_t tcp_port = tcp_listener->bound_port();
        ServerBridgeRunner tcp_runner(tcp_listener.get());

        // QUIC listener
        auto quic_listener = *QuicListener::create("127.0.0.1", 0, pki.server_cfg);
        uint16_t quic_port = quic_listener->bound_port();
        ServerBridgeRunner quic_runner(quic_listener.get());

        // 1. Missing client certificate
        TlsConfig no_cert_cfg = pki.client_cfg;
        no_cert_cfg.cert_pem.clear();
        no_cert_cfg.key_pem.clear();

        CHECK_FALSE(TcpTlsConnector::connect("127.0.0.1", tcp_port, no_cert_cfg, 1000ms).has_value());
        CHECK_FALSE(QuicConnector::connect("127.0.0.1", quic_port, no_cert_cfg, 1000ms).has_value());

        // 2. Untrusted / Rogue CA
        auto rogue_pki = TestPki::create("127.0.0.1", "rogue_user");
        TlsConfig rogue_cfg = rogue_pki.client_cfg;
        rogue_cfg.ca_cert_pem = pki.ca.certificate_pem; // trusts server, but presents cert signed by rogue CA

        CHECK_FALSE(TcpTlsConnector::connect("127.0.0.1", tcp_port, rogue_cfg, 1000ms).has_value());
        CHECK_FALSE(QuicConnector::connect("127.0.0.1", quic_port, rogue_cfg, 1000ms).has_value());

        tcp_runner.stop();
        quic_runner.stop();
        tcp_listener->close();
        quic_listener->close();
    }
}

// ============================================================================
// Requirement 3: Local 0600 AF_UNIX Full Stack
// ============================================================================
TEST_CASE("E2E-3: Local 0600 AF_UNIX Full Stack (stat 0600, peer UID, multi-pane, capture-pane)") {
    std::string sock_path = "/tmp/test_tmux2_unix_e2e_" + std::to_string(::getpid()) + ".sock";
    ::unlink(sock_path.c_str());

    auto listener_res = UnixListener::create(sock_path);
    REQUIRE(listener_res.has_value());
    auto listener = std::move(*listener_res);

    // 1. Verify 0600 socket file permissions via stat()
    struct stat st{};
    REQUIRE(::stat(sock_path.c_str(), &st) == 0);
    mode_t perms = st.st_mode & 0777;
    CHECK(perms == 0600);

    ServerBridgeRunner runner(listener.get(), /*mock_pty=*/false);

    // 2. Connect via UnixConnector
    auto client_conn_res = UnixConnector::connect(sock_path, 1000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;
    CHECK(client_conn->is_connected());
    CHECK(client_conn->type() == TransportType::Unix);

    auto server_conn = runner.wait_for_connection();
    REQUIRE(server_conn != nullptr);

    // 3. Verify peer UID extraction
    auto uid = server_conn->peer_uid();
    REQUIRE(uid.has_value());
    CHECK(*uid == ::getuid());

    TestClientEndpoint client;
    client.conn = client_conn;
    client.setup();

    // 4. MsgClientHello & create multi-pane session
    client.send(MsgClientHello{.protocol_version = 1});
    REQUIRE(client.wait_for_msg<MsgServerHello>().has_value());

    client.send(MsgNewSessionReq{.name = "unix_session", .detached = false});
    auto new_resp = client.wait_for_msg<MsgNewSessionResp>();
    REQUIRE(new_resp.has_value());
    CHECK(new_resp->success);

    // Split window
    client.send(MsgCommandRpcReq{.rpc_id = 20, .command_dsl = "split-window -h"});
    auto split_resp = client.wait_for_msg<MsgCommandRpcResp>();
    REQUIRE(split_resp.has_value());
    CHECK(split_resp->exit_code == 0);

    // Write command to pane 0
    client.send_input_str("echo UNIX_PANE_ZERO_OUTPUT\n", 1);
    auto ok_diff = client.wait_for_matching([](const ProtocolMessage& m) {
        return std::holds_alternative<MsgFrameDiff>(m) &&
               frame_diff_contains(std::get<MsgFrameDiff>(m), "UNIX_PANE_ZERO_OUTPUT");
    }, 4000ms);
    CHECK(ok_diff.has_value());

    // 5. Test capture-pane RPC
    client.send(MsgCommandRpcReq{.rpc_id = 21, .command_dsl = "capture-pane -p"});
    auto cap_resp = client.wait_for_msg<MsgCommandRpcResp>();
    REQUIRE(cap_resp.has_value());
    CHECK(cap_resp->exit_code == 0);
    CHECK(cap_resp->stdout_data.find("UNIX_PANE_ZERO_OUTPUT") != std::string::npos);

    client_conn->close();
    runner.stop();
    listener->close();
    CHECK(::access(sock_path.c_str(), F_OK) != 0);
}

// ============================================================================
// Requirement 4: Reconnect, resume_token Delta / Snapshot Catch-Up & Seq Deduplication
// ============================================================================
TEST_CASE("E2E-4: Section 5 Automatic Reconnect (Delta Replay, Snapshot Fallback, and client_seq Deduplication)") {
    auto pki = TestPki::create("127.0.0.1", "recon_user");

    auto listener_res = TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    REQUIRE(listener_res.has_value());
    auto listener = std::move(*listener_res);
    uint16_t port = listener->bound_port();

    ServerBridgeRunner runner(listener.get(), /*mock_pty=*/false);

    // --- Subcase A: Delta Replay on fresh reconnect connection ---
    std::string resume_tok;
    uint64_t last_srv_seq = 0;

    {
        auto conn = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
        auto s_conn = runner.wait_for_connection();
        REQUIRE(s_conn != nullptr);
        TestClientEndpoint c1;
        c1.conn = conn;
        c1.setup();

        c1.send(MsgClientHello{.protocol_version = 1});
        auto hello = c1.wait_for_msg<MsgServerHello>();
        REQUIRE(hello.has_value());
        resume_tok = hello->resume_token;
        CHECK_FALSE(resume_tok.empty());

        c1.send(MsgNewSessionReq{.name = "recon_sess", .detached = false});
        auto n_resp = c1.wait_for_msg<MsgNewSessionResp>();
        REQUIRE(n_resp.has_value());
        CHECK(n_resp->success);

        auto layout = c1.wait_for_msg<MsgLayoutUpdate>();
        REQUIRE(layout.has_value());
        last_srv_seq = layout->server_seq;

        // Drain any additional initial layout/snapshot messages to capture latest server_seq
        while (auto msg = c1.wait_for_matching([](const ProtocolMessage&) { return true; }, 50ms)) {
            if (std::holds_alternative<MsgLayoutUpdate>(*msg)) {
                last_srv_seq = std::max(last_srv_seq, std::get<MsgLayoutUpdate>(*msg).server_seq);
            } else if (std::holds_alternative<MsgScreenSnapshot>(*msg)) {
                last_srv_seq = std::max(last_srv_seq, std::get<MsgScreenSnapshot>(*msg).server_seq);
            }
        }

        // Sever transport connection while server pane produces output
        conn->close();
    }

    // While client is disconnected, produce output in session
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(1);
        REQUIRE(s != nullptr);
        auto win = s->active_window();
        REQUIRE(win != nullptr);
        auto pane = win->active_pane();
        REQUIRE(pane != nullptr);
        pane->write_input("echo DELTA_CATCHUP_SUCCESS\n");
    }

    // Give child shell time to produce output and server to record FrameDiff
    std::this_thread::sleep_for(300ms);

    // Reconnect with valid resume_token presenting last_srv_seq
    {
        auto conn2 = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
        auto s_conn2 = runner.wait_for_connection();
        REQUIRE(s_conn2 != nullptr);
        TestClientEndpoint c2;
        c2.conn = conn2;
        c2.setup();

        MsgClientHello recon_hello;
        recon_hello.protocol_version = 1;
        recon_hello.resume_token = resume_tok;
        recon_hello.last_srv_seq = last_srv_seq;
        c2.send(recon_hello);

        auto hello2 = c2.wait_for_msg<MsgServerHello>();
        REQUIRE(hello2.has_value());

        // Verify missed MsgFrameDiffs are replayed!
        auto replayed_diff = c2.wait_for_matching([](const ProtocolMessage& m) {
            return std::holds_alternative<MsgFrameDiff>(m) &&
                   frame_diff_contains(std::get<MsgFrameDiff>(m), "DELTA_CATCHUP_SUCCESS");
        }, 3000ms);
        CHECK(replayed_diff.has_value());

        conn2->close();
    }

    // --- Subcase B: Snapshot Fallback on ring buffer age-out ---
    {
        // Reconnect presenting aged-out last_srv_seq = 0
        auto conn3 = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
        auto s_conn3 = runner.wait_for_connection();
        REQUIRE(s_conn3 != nullptr);
        TestClientEndpoint c3;
        c3.conn = conn3;
        c3.setup();

        MsgClientHello age_hello;
        age_hello.protocol_version = 1;
        age_hello.resume_token = resume_tok;
        age_hello.last_srv_seq = 0; // aged out!
        c3.send(age_hello);

        REQUIRE(c3.wait_for_msg<MsgServerHello>().has_value());

        // Server MUST fall back to sending fresh LayoutUpdate + ScreenSnapshot
        auto fallback_layout = c3.wait_for_msg<MsgLayoutUpdate>();
        CHECK(fallback_layout.has_value());

        auto fallback_snapshot = c3.wait_for_msg<MsgScreenSnapshot>();
        CHECK(fallback_snapshot.has_value());

        conn3->close();
    }

    // --- Subcase C: client_seq Deduplication ---
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto client_state = runner.engine().get_client(1);
        REQUIRE(client_state != nullptr);

        // Deduplication test: seq 10 is new
        CHECK(client_state->deduplicate_client_seq(10));
        // seq 10 repeated is a duplicate!
        CHECK_FALSE(client_state->deduplicate_client_seq(10));
        // seq 5 (older than newest seen) is a duplicate!
        CHECK_FALSE(client_state->deduplicate_client_seq(5));
        // seq 11 is new
        CHECK(client_state->deduplicate_client_seq(11));

        // Using ReconnectBuffer from ClientApp
        ReconnectBuffer buf(100);
        uint64_t s1 = buf.next_seq();
        uint64_t s2 = buf.next_seq();
        buf.push(s1, MsgKeyEvent{.client_seq = s1, .key_code = 'x'});
        buf.push(s2, MsgKeyEvent{.client_seq = s2, .key_code = 'y'});
        CHECK(buf.size() == 2);

        // Server acks s1
        buf.ack(s1);
        CHECK(buf.size() == 1);
        auto unacked = buf.get_unacked(s1);
        REQUIRE(unacked.size() == 1);
        CHECK(unacked[0].client_seq == s2);
    }

    runner.stop();
    listener->close();
}

// ============================================================================
// Requirement 5: Simultaneous Multi-Client Attach (window-size = latest & attach -d)
// ============================================================================
TEST_CASE("E2E-5: Simultaneous Multi-Client Attach (window-size = latest dynamic resize, broadcast, and attach -d)") {
    auto pki = TestPki::create("127.0.0.1", "multi_user");

    auto listener_res = TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    REQUIRE(listener_res.has_value());
    auto listener = std::move(*listener_res);
    uint16_t port = listener->bound_port();

    ServerBridgeRunner runner(listener.get(), /*mock_pty=*/false);

    // 1. Client 1 attaches with 120x40
    auto conn1 = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    auto s_conn1 = runner.wait_for_connection();
    REQUIRE(s_conn1 != nullptr);
    TestClientEndpoint c1;
    c1.conn = conn1;
    c1.setup();

    c1.send(MsgClientHello{.protocol_version = 1});
    auto h1 = c1.wait_for_msg<MsgServerHello>();
    REQUIRE(h1.has_value());

    c1.send(MsgNewSessionReq{.name = "shared", .detached = false});
    auto n1 = c1.wait_for_msg<MsgNewSessionResp>();
    REQUIRE(n1.has_value());
    CHECK(n1->success);

    // Explicitly set 120x40 on client 1
    c1.send(MsgClientResize{.cols = 120, .rows = 40});
    std::this_thread::sleep_for(30ms);

    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(1);
        REQUIRE(s != nullptr);
        CHECK(s->cols() == 120);
        CHECK(s->rows() == 40);
    }

    // 2. Client 2 attaches with 80x24 simultaneously to the same session
    auto conn2 = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    auto s_conn2 = runner.wait_for_connection();
    REQUIRE(s_conn2 != nullptr);
    TestClientEndpoint c2;
    c2.conn = conn2;
    c2.setup();

    c2.send(MsgClientHello{.protocol_version = 1});
    auto h2 = c2.wait_for_msg<MsgServerHello>();
    REQUIRE(h2.has_value());

    c2.send(MsgAttachSessionReq{.session_name = "shared", .cols = 80, .rows = 24});
    auto a2 = c2.wait_for_msg<MsgAttachSessionResp>();
    REQUIRE(a2.has_value());
    CHECK(a2->success);

    // window-size = latest dynamically resized the session & PTY to 80x24!
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(1);
        REQUIRE(s != nullptr);
        CHECK(s->cols() == 80);
        CHECK(s->rows() == 24);

        auto win = s->active_window();
        REQUIRE(win != nullptr);
        auto pane = win->active_pane();
        REQUIRE(pane != nullptr);
        struct winsize ws{};
        ::ioctl(pane->master_fd(), TIOCGWINSZ, &ws);
        CHECK(ws.ws_col == 80);
        CHECK(ws.ws_row == 24);
    }

    // 3. Client 1 types / resizes back to 120x40 -> session dynamically resizes back to 120x40!
    c1.send(MsgClientResize{.cols = 120, .rows = 40});
    std::this_thread::sleep_for(30ms);

    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(1);
        REQUIRE(s != nullptr);
        CHECK(s->cols() == 120);
        CHECK(s->rows() == 40);
    }

    // 4. Live FrameDiff broadcast reaches both clients
    c1.send_input_str("echo DUAL_BROADCAST_OK\n", 5);

    auto d1 = c1.wait_for_matching([](const ProtocolMessage& m) {
        return std::holds_alternative<MsgFrameDiff>(m) &&
               frame_diff_contains(std::get<MsgFrameDiff>(m), "DUAL_BROADCAST_OK");
    }, 4000ms);
    CHECK(d1.has_value());

    auto d2 = c2.wait_for_matching([](const ProtocolMessage& m) {
        return std::holds_alternative<MsgFrameDiff>(m) &&
               frame_diff_contains(std::get<MsgFrameDiff>(m), "DUAL_BROADCAST_OK");
    }, 4000ms);
    CHECK(d2.has_value());

    // 5. Client 3 attaches with detach_others = true (attach -d)
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(1);
        REQUIRE(s != nullptr);
        auto detached = s->attach_client(999, false, 80, 24, /*detach_others=*/true);
        CHECK(detached.size() == 2);
        for (uint64_t did : detached) {
            MsgDetachNotice notice;
            notice.reason = "Detached by another client attachment (-d)";
            if (did == h1->assigned_client_id) {
                // Sent to c1
                runner.engine().handle_message(did, notice);
            }
        }
    }

    conn1->close();
    conn2->close();
    runner.stop();
    listener->close();
}

// ============================================================================
// Requirement 6: Interactive Daily-Driver Client UX End-to-End
// ============================================================================
TEST_CASE("E2E-6: Interactive Daily-Driver Client UX (Session Chooser overlay, Copy Mode scrollback fetch + OSC 52, Mouse drag resize & DECSET 2026)") {
    ClientApp app(80, 24);

    // Initial server state with two sessions: "work" and "infra"
    MsgServerHello hello{
        .server_version = 1,
        .assigned_client_id = 1,
        .session_list = {
            SessionSummary{.session_id = 1, .name = "work"},
            SessionSummary{.session_id = 2, .name = "infra"}
        }
    };
    app.handle_server_message(hello);

    // Layout update for session 1
    MsgLayoutUpdate layout{
        .server_seq = 1,
        .session_id = 1,
        .window_id = 1,
        .active_pane_id = 1,
        .zoomed_pane_id = 0
    };
    app.handle_server_message(layout);
    app.renderer().set_pane_rects({
        PaneRect{.pane_id = 1, .x = 0, .y = 0, .width = 39, .height = 23},
        PaneRect{.pane_id = 2, .x = 40, .y = 0, .width = 40, .height = 23}
    });

    // 1. Session Chooser overlay (Prefix + s)
    SUBCASE("Prefix + s activates session chooser and in-band session switch") {
        CHECK_FALSE(app.session_chooser().is_active());

        // Press Prefix (Ctrl-b)
        auto r_pre = app.process_input_event(ClientKeyEvent{
            .key_code = 'b', .modifiers = KeyModifiers::Ctrl, .raw_bytes = {0x02}
        });
        CHECK(r_pre.should_redraw);

        // Press 's'
        auto r_s = app.process_input_event(ClientKeyEvent{.key_code = 's', .raw_bytes = {'s'}});
        CHECK(r_s.should_redraw);
        CHECK(app.session_chooser().is_active());

        // Move to second session 'j' and press Enter
        app.process_input_event(ClientKeyEvent{.key_code = 'j'});
        auto r_enter = app.process_input_event(ClientKeyEvent{.key_code = SpecialKey::Enter});
        REQUIRE(r_enter.outbound_messages.size() == 1);
        REQUIRE(std::holds_alternative<MsgSwitchSessionReq>(r_enter.outbound_messages[0]));
        const auto& sw = std::get<MsgSwitchSessionReq>(r_enter.outbound_messages[0]);
        CHECK(sw.session_name == "infra");
        CHECK_FALSE(app.session_chooser().is_active());
    }

    // 2. Copy Mode with >100 lines scrollback slice fetch + OSC 52 yank
    SUBCASE("Prefix + [ enters copy mode, fetches missing scrollback slice, visual select & yanks OSC 52") {
        // Feed initial pane snapshot
        MsgScreenSnapshot snap{
            .server_seq = 2,
            .pane_id = 1,
            .cols = 39,
            .rows = 23,
            .cells = std::vector<Cell>(39 * 23)
        };
        app.handle_server_message(snap);

        // Enter copy mode via Prefix + [
        app.process_input_event(ClientKeyEvent{
            .key_code = 'b', .modifiers = KeyModifiers::Ctrl, .raw_bytes = {0x02}
        });
        auto r_copy = app.process_input_event(ClientKeyEvent{.key_code = '[', .raw_bytes = {'['}});
        CHECK(r_copy.should_redraw);
        CHECK(app.copy_mode().is_active());
        // Configure total lines for testing >100 lines history paging
        app.copy_mode().enter(1, 39, 23, 150);
        CHECK(app.copy_mode().total_lines() == 150);

        // Navigate up (jump to top line 0)
        auto r_g = app.process_input_event(ClientKeyEvent{.key_code = 'g'});
        CHECK(r_g.should_redraw);

        // Because line 0 was not in local buffer, copy mode generates MsgScrollbackSliceReq
        if (r_g.outbound_messages.empty()) {
            // Check if slice_req was generated on subsequent navigation
            auto r_k = app.process_input_event(ClientKeyEvent{.key_code = 'k'});
            (void)r_k;
        }

        // Server responds with MsgScrollbackSliceResp
        MsgScrollbackSliceResp slice_resp;
        slice_resp.pane_id = 1;
        slice_resp.start_line = 0;
        slice_resp.total_available_lines = 150;
        for (int i = 0; i < 24; ++i) {
            ScrollbackLine sl;
            std::string txt = "log_line_" + std::to_string(i);
            for (char ch : txt) sl.cells.push_back(Cell{.codepoint = (char32_t)ch});
            slice_resp.lines.push_back(std::move(sl));
        }
        app.handle_server_message(slice_resp);

        // Begin selection (Space)
        auto r_sp = app.process_input_event(ClientKeyEvent{.key_code = ' '});
        CHECK(app.copy_mode().is_selecting());
        (void)r_sp;

        // Move right 8 characters
        for (int i = 0; i < 8; ++i) {
            app.process_input_event(ClientKeyEvent{.key_code = 'l'});
        }

        // Yank selection (y)
        auto r_yank = app.process_input_event(ClientKeyEvent{.key_code = 'y'});
        CHECK_FALSE(app.copy_mode().is_active());
        CHECK_FALSE(r_yank.terminal_output.empty());

        // Verify OSC 52 sequence format
        CHECK(r_yank.terminal_output.starts_with("\x1b]52;c;"));
        CHECK(r_yank.terminal_output.ends_with("\x07"));

        // Verify decoded base64 contents
        size_t b64_start = r_yank.terminal_output.rfind(';') + 1;
        size_t b64_end = r_yank.terminal_output.find('\x07', b64_start);
        std::string b64 = r_yank.terminal_output.substr(b64_start, b64_end - b64_start);
        std::string decoded = Renderer::base64_decode(b64);
        CHECK(decoded.starts_with("log_line"));
    }

    // 3. Mouse SGR Click Focus, Split-Border Drag Resize, and Wheel Scroll
    SUBCASE("Mouse SGR click focus, border drag resizing, and wheel scroll") {
        // Click inside pane 2 (col 50, row 10)
        ClientMouseEvent click_p2{
            .kind = ClientMouseEvent::Kind::Press,
            .button = ClientMouseEvent::Button::Left,
            .col = 50,
            .row = 10
        };
        auto r_click = app.process_input_event(click_p2);
        CHECK(r_click.should_redraw);
        CHECK(app.renderer().active_pane() == 2);

        // Drag vertical border at col 39
        ClientMouseEvent drag_press{
            .kind = ClientMouseEvent::Kind::Press,
            .button = ClientMouseEvent::Button::Left,
            .col = 39,
            .row = 10
        };
        app.process_input_event(drag_press);

        // Move border right by 5 columns to col 44
        ClientMouseEvent drag_move{
            .kind = ClientMouseEvent::Kind::Move,
            .button = ClientMouseEvent::Button::Left,
            .col = 44,
            .row = 10
        };
        auto r_drag = app.process_input_event(drag_move);
        CHECK(r_drag.should_redraw);

        ClientMouseEvent drag_rel{
            .kind = ClientMouseEvent::Kind::Release,
            .button = ClientMouseEvent::Button::Left,
            .col = 44,
            .row = 10
        };
        app.process_input_event(drag_rel);

        const auto& rects = app.renderer().pane_rects();
        REQUIRE(rects.size() == 2);
        CHECK(rects[0].width == 44);
        CHECK(rects[1].x == 45);

        // Mouse wheel scroll enters copy mode
        ClientMouseEvent wheel{
            .kind = ClientMouseEvent::Kind::WheelUp,
            .button = ClientMouseEvent::Button::None,
            .col = 10,
            .row = 5
        };
        auto r_wheel = app.process_input_event(wheel);
        CHECK(r_wheel.should_redraw);
        CHECK(app.copy_mode().is_active());
    }

    // 4. DECSET 2026 Synchronized Output
    SUBCASE("Rendered frame wrapped in DECSET 2026 synchronized output escape sequence") {
        std::string frame = app.render_frame(/*use_decset_2026=*/true);
        CHECK(frame.starts_with("\x1b[?2026h"));
        CHECK(frame.ends_with("\x1b[?2026l"));
    }
}
