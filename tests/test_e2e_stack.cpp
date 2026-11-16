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

#include <algorithm>
#include <array>
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
            send_to_client(client_id, msg);
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

    std::shared_ptr<IConnection> wait_for_connection(size_t expected_count = 1, std::chrono::milliseconds timeout = 3000ms) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (m_connections.size() >= expected_count && m_connections[expected_count - 1]->is_connected()) {
                    return m_connections[expected_count - 1];
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        return nullptr;
    }

    void send_to_client(uint64_t client_id, const ProtocolMessage& msg) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_client_streams.find(client_id);
        if (it != m_client_streams.end() && it->second && !it->second->is_closed()) {
            send_proto(*it->second, msg);
        }
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
// TestClientEndpoint: Convenient raw stream 0 protocol test endpoint
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

// ============================================================================
// ConnectedClientApp: Wires real ClientApp to live transport stream & server
// ============================================================================
struct ConnectedClientApp {
    ClientApp app;
    std::shared_ptr<IConnection> conn;
    std::shared_ptr<IStream> stream;
    std::mutex mtx;
    std::vector<ProtocolMessage> inbound_msgs;

    ConnectedClientApp(std::shared_ptr<IConnection> c, uint16_t cols = 80, uint16_t rows = 24)
        : app(cols, rows), conn(c) {
        REQUIRE(conn != nullptr);
        auto s_res = conn->open_stream(0, true);
        REQUIRE(s_res.has_value());
        stream = *s_res;

        stream->set_on_frame([this](const StreamHeader& hdr, std::span<const std::byte> payload) {
            auto dec = decode_proto(hdr, payload);
            if (!dec) return;
            std::lock_guard<std::mutex> lock(mtx);
            inbound_msgs.push_back(*dec);
            // Feed to ClientApp
            auto res = app.handle_server_message(*dec);
            if (res.slice_req) {
                send_proto(*stream, *res.slice_req);
            }
        });
    }

    void poll() {
        if (conn) {
            ERR_clear_error();
            conn->poll_io();
        }
    }

    void send_msg(const ProtocolMessage& msg, bool compress = false) {
        REQUIRE(stream != nullptr);
        send_proto(*stream, msg, compress);
        poll();
    }

    void send_input_event(const InputEvent& ev) {
        auto res = app.process_input_event(ev);
        for (const auto& m : res.outbound_messages) {
            send_proto(*stream, m);
        }
        poll();
    }

    template <typename T>
    std::optional<T> wait_for_inbound(std::chrono::milliseconds timeout = 3000ms) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            poll();
            {
                std::lock_guard<std::mutex> lock(mtx);
                for (auto it = inbound_msgs.begin(); it != inbound_msgs.end(); ++it) {
                    if (std::holds_alternative<T>(*it)) {
                        T found = std::get<T>(*it);
                        inbound_msgs.erase(it);
                        return found;
                    }
                }
            }
            std::this_thread::sleep_for(3ms);
        }
        return std::nullopt;
    }

    template <typename Predicate>
    std::optional<ProtocolMessage> wait_for_matching_inbound(Predicate pred, std::chrono::milliseconds timeout = 3000ms) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            poll();
            {
                std::lock_guard<std::mutex> lock(mtx);
                for (auto it = inbound_msgs.begin(); it != inbound_msgs.end(); ++it) {
                    if (pred(*it)) {
                        ProtocolMessage found = *it;
                        inbound_msgs.erase(it);
                        return found;
                    }
                }
            }
            std::this_thread::sleep_for(3ms);
        }
        return std::nullopt;
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

    auto server_conn = runner.wait_for_connection(1);
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

        auto server_conn = tcp_runner.wait_for_connection(1);
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

    auto server_conn = runner.wait_for_connection(1);
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

    // ClientApp driven against live server
    ClientApp client_app(80, 24);

    // --- Subcase A: Delta Replay on fresh reconnect connection ---
    std::string resume_tok;
    uint64_t last_srv_seq = 0;

    {
        auto conn = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
        auto s_conn = runner.wait_for_connection(1);
        REQUIRE(s_conn != nullptr);
        TestClientEndpoint c1;
        c1.conn = conn;
        c1.setup();

        c1.send(MsgClientHello{.protocol_version = 1});
        auto hello = c1.wait_for_msg<MsgServerHello>();
        REQUIRE(hello.has_value());
        resume_tok = hello->resume_token;
        CHECK_FALSE(resume_tok.empty());
        client_app.handle_server_message(*hello);

        c1.send(MsgNewSessionReq{.name = "recon_sess", .detached = false});
        auto n_resp = c1.wait_for_msg<MsgNewSessionResp>();
        REQUIRE(n_resp.has_value());
        CHECK(n_resp->success);
        client_app.handle_server_message(*n_resp);

        auto layout = c1.wait_for_msg<MsgLayoutUpdate>();
        REQUIRE(layout.has_value());
        last_srv_seq = layout->server_seq;
        client_app.handle_server_message(*layout);

        // Drain any additional initial layout/snapshot messages to capture latest server_seq
        while (auto msg = c1.wait_for_matching([](const ProtocolMessage&) { return true; }, 50ms)) {
            client_app.handle_server_message(*msg);
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
        auto s_conn2 = runner.wait_for_connection(2);
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
        client_app.handle_server_message(*hello2);

        // Verify missed MsgFrameDiffs are replayed!
        auto replayed_diff = c2.wait_for_matching([](const ProtocolMessage& m) {
            return std::holds_alternative<MsgFrameDiff>(m) &&
                   frame_diff_contains(std::get<MsgFrameDiff>(m), "DELTA_CATCHUP_SUCCESS");
        }, 3000ms);
        REQUIRE(replayed_diff.has_value());

        // Apply replayed diff to ClientApp and verify client grid catches up accurately!
        client_app.handle_server_message(*replayed_diff);
        std::string rendered = client_app.render_frame(/*use_decset_2026=*/false);
        CHECK(rendered.find("DELTA_CATCHUP_SUCCESS") != std::string::npos);

        conn2->close();
    }

    // --- Subcase B: Snapshot Fallback on ring buffer age-out ---
    {
        // Reconnect presenting aged-out last_srv_seq = 0
        auto conn3 = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
        auto s_conn3 = runner.wait_for_connection(3);
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
        if (fallback_layout) client_app.handle_server_message(*fallback_layout);

        auto fallback_snapshot = c3.wait_for_msg<MsgScreenSnapshot>();
        CHECK(fallback_snapshot.has_value());
        if (fallback_snapshot) client_app.handle_server_message(*fallback_snapshot);

        conn3->close();
    }

    // --- Subcase C: client_seq Deduplication ---
    {
        // 1. Buffer MsgKeyEvents in ClientApp::reconnect_buffer()
        uint64_t s1 = 10;
        uint64_t s2 = 11;
        client_app.reconnect_buffer().push(s1, MsgKeyEvent{.client_seq = s1, .key_code = 'x'});
        client_app.reconnect_buffer().push(s2, MsgKeyEvent{.client_seq = s2, .key_code = 'y'});
        CHECK(client_app.reconnect_buffer().size() == 2);

        // Server acks s1: verify already-acked event is removed from buffer
        client_app.reconnect_buffer().ack(s1);
        CHECK(client_app.reconnect_buffer().size() == 1);

        // Prepare replay: unacked client_seq > last_client_seq_acked is replayed
        auto replay = client_app.prepare_reconnect_replay(s1);
        REQUIRE(replay.size() == 1);
        CHECK(std::get<MsgKeyEvent>(replay[0]).client_seq == s2);

        // 2. Verify server-side deduplication logic
        {
            std::lock_guard<std::recursive_mutex> lock(runner.mutex());
            auto client_state = runner.engine().get_client(1);
            REQUIRE(client_state != nullptr);

            client_state->set_last_client_seq_acked(s1);
            // Already-acked seq s1 is rejected
            CHECK_FALSE(client_state->deduplicate_client_seq(s1));
            // Unacked seq s2 is accepted
            CHECK(client_state->deduplicate_client_seq(s2));
            // Duplicate seq s2 is rejected
            CHECK_FALSE(client_state->deduplicate_client_seq(s2));
        }

        // 3. Connect live client and verify unacked event executes exactly once on server
        auto conn4 = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
        auto s_conn4 = runner.wait_for_connection(4);
        REQUIRE(s_conn4 != nullptr);
        TestClientEndpoint c4;
        c4.conn = conn4;
        c4.setup();

        c4.send(MsgClientHello{.protocol_version = 1, .resume_token = resume_tok, .last_srv_seq = last_srv_seq});
        REQUIRE(c4.wait_for_msg<MsgServerHello>().has_value());

        // Send unacked seq 20
        c4.send_input_str("echo DEDUP_EXEC_ONCE\n", 20);
        auto exec_diff = c4.wait_for_matching([](const ProtocolMessage& m) {
            return std::holds_alternative<MsgFrameDiff>(m) &&
                   frame_diff_contains(std::get<MsgFrameDiff>(m), "DEDUP_EXEC_ONCE");
        }, 4000ms);
        CHECK(exec_diff.has_value());

        // Send duplicate seq 20 -> server ignores it
        c4.send_input_str("echo DEDUP_EXEC_ONCE\n", 20);
        std::this_thread::sleep_for(100ms);

        conn4->close();
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
    auto s_conn1 = runner.wait_for_connection(1);
    REQUIRE(s_conn1 != nullptr);
    TestClientEndpoint c1;
    c1.conn = conn1;
    c1.setup();

    c1.send(MsgClientHello{.protocol_version = 1});
    auto h1 = c1.wait_for_msg<MsgServerHello>();
    REQUIRE(h1.has_value());
    uint64_t c1_id = h1->assigned_client_id;
    CHECK(c1_id == 1);

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
    auto s_conn2 = runner.wait_for_connection(2);
    REQUIRE(s_conn2 != nullptr);
    TestClientEndpoint c2;
    c2.conn = conn2;
    c2.setup();

    c2.send(MsgClientHello{.protocol_version = 1});
    auto h2 = c2.wait_for_msg<MsgServerHello>();
    REQUIRE(h2.has_value());
    uint64_t c2_id = h2->assigned_client_id;
    CHECK(c2_id == 2);

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

    // 5. Client 3 attaches over live connection with detach_others = true (attach -d)
    auto conn3 = *TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    auto s_conn3 = runner.wait_for_connection(3);
    REQUIRE(s_conn3 != nullptr);
    TestClientEndpoint c3;
    c3.conn = conn3;
    c3.setup();

    c3.send(MsgClientHello{.protocol_version = 1});
    auto h3 = c3.wait_for_msg<MsgServerHello>();
    REQUIRE(h3.has_value());
    uint64_t c3_id = h3->assigned_client_id;
    CHECK(c3_id == 3);

    // Client 3 attaches with detach_others = true
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(1);
        REQUIRE(s != nullptr);
        auto detached = s->attach_client(c3_id, /*read_only=*/false, 80, 24, /*detach_others=*/true);
        CHECK(detached.size() == 2);
        for (uint64_t did : detached) {
            MsgDetachNotice notice;
            notice.reason = "Detached by another client attachment (-d)";
            runner.send_to_client(did, notice);
        }
    }

    // Verify both Client 1 and Client 2 receive MsgDetachNotice over the wire!
    auto notice1 = c1.wait_for_msg<MsgDetachNotice>();
    REQUIRE(notice1.has_value());
    CHECK(notice1->reason.find("Detached") != std::string::npos);

    auto notice2 = c2.wait_for_msg<MsgDetachNotice>();
    REQUIRE(notice2.has_value());
    CHECK(notice2->reason.find("Detached") != std::string::npos);

    // Verify only Client 3 remains attached to session 1
    {
        std::lock_guard<std::recursive_mutex> lock(runner.mutex());
        auto s = runner.engine().session_manager().find_session(1);
        REQUIRE(s != nullptr);
        CHECK(s->attached_clients().size() == 1);
        CHECK(s->attached_clients()[0].client_id == c3_id);
    }

    conn1->close();
    conn2->close();
    conn3->close();
    runner.stop();
    listener->close();
}

// ============================================================================
// Requirement 6: Interactive Daily-Driver Client UX End-to-End
// ============================================================================
TEST_CASE("E2E-6: Interactive Daily-Driver Client UX (Session Chooser overlay, Copy Mode scrollback fetch + OSC 52, Mouse drag resize & DECSET 2026)") {
    std::string sock_path = "/tmp/test_tmux2_ux_e2e_" + std::to_string(::getpid()) + ".sock";
    ::unlink(sock_path.c_str());

    auto listener_res = UnixListener::create(sock_path);
    REQUIRE(listener_res.has_value());
    auto listener = std::move(*listener_res);

    ServerBridgeRunner runner(listener.get(), /*mock_pty=*/false);

    auto client_conn_res = UnixConnector::connect(sock_path, 2000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;

    auto s_conn = runner.wait_for_connection(1);
    REQUIRE(s_conn != nullptr);

    // ClientApp connected across live transport to live ServerEngine
    ConnectedClientApp client(client_conn, 80, 24);

    // Initial handshake
    client.send_msg(MsgClientHello{.protocol_version = 1});
    auto hello_resp = client.wait_for_inbound<MsgServerHello>();
    REQUIRE(hello_resp.has_value());

    // 1. Create sessions "work" (attached) and "infra" (detached)
    client.send_msg(MsgNewSessionReq{.name = "work", .detached = false});
    auto work_resp = client.wait_for_inbound<MsgNewSessionResp>();
    REQUIRE(work_resp.has_value());
    CHECK(work_resp->success);
    uint32_t work_sid = work_resp->session_id;

    auto att_resp = client.wait_for_inbound<MsgAttachSessionResp>();
    REQUIRE(att_resp.has_value());
    CHECK(att_resp->success);

    auto work_layout = client.wait_for_inbound<MsgLayoutUpdate>();
    REQUIRE(work_layout.has_value());

    // Split window in "work" so we have 2 panes for testing split-border drag resize and mouse click focus
    client.send_msg(MsgCommandRpcReq{.rpc_id = 50, .command_dsl = "split-window -h"});
    auto split_resp = client.wait_for_inbound<MsgCommandRpcResp>();
    REQUIRE(split_resp.has_value());
    CHECK(split_resp->exit_code == 0);

    auto split_layout = client.wait_for_inbound<MsgLayoutUpdate>();
    REQUIRE(split_layout.has_value());

    // Create session "infra" (detached)
    client.send_msg(MsgNewSessionReq{.name = "infra", .detached = true});
    auto infra_resp = client.wait_for_inbound<MsgNewSessionResp>();
    REQUIRE(infra_resp.has_value());
    CHECK(infra_resp->success);
    uint32_t infra_sid = infra_resp->session_id;

    // Refresh session list so ClientApp caches both "work" and "infra"
    client.send_msg(MsgListSessionsReq{});
    auto list_resp = client.wait_for_inbound<MsgListSessionsResp>();
    REQUIRE(list_resp.has_value());
    CHECK(list_resp->sessions.size() == 2);

    // 2. Session Chooser overlay (Prefix + s)
    {
        CHECK_FALSE(client.app.session_chooser().is_active());

        // Press Prefix (Ctrl-b)
        client.send_input_event(ClientKeyEvent{
            .key_code = 'b', .modifiers = KeyModifiers::Ctrl, .raw_bytes = {0x02}
        });
        CHECK(client.app.keybindings().is_prefix_active());

        // Press 's'
        client.send_input_event(ClientKeyEvent{.key_code = 's', .raw_bytes = {'s'}});
        CHECK(client.app.session_chooser().is_active());

        // Move to second session 'j' and press Enter
        client.send_input_event(ClientKeyEvent{.key_code = 'j'});
        // Press Enter -> generates MsgSwitchSessionReq to "infra" and transmits across transport
        client.send_input_event(ClientKeyEvent{.key_code = SpecialKey::Enter});
        CHECK_FALSE(client.app.session_chooser().is_active());

        // Verify server executes in-band switch across transport
        auto sw_resp = client.wait_for_inbound<MsgSwitchSessionResp>();
        REQUIRE(sw_resp.has_value());
        CHECK(sw_resp->success);
        CHECK(sw_resp->session_id == infra_sid);
        CHECK(client.app.attached_session_id() == infra_sid);

        // Switch back to "work" session
        client.send_msg(MsgSwitchSessionReq{.session_name = "work"});
        auto back_resp = client.wait_for_inbound<MsgSwitchSessionResp>();
        REQUIRE(back_resp.has_value());
        CHECK(back_resp->success);
        CHECK(client.app.attached_session_id() == work_sid);

        // Drain layout update for "work"
        auto work_layout2 = client.wait_for_inbound<MsgLayoutUpdate>();
        REQUIRE(work_layout2.has_value());
    }

    // 3. Copy Mode with >100 lines real PTY scrollback slice fetch + OSC 52 yank
    {
        // Generate >100 lines of real PTY output
        {
            std::lock_guard<std::recursive_mutex> lock(runner.mutex());
            auto s = runner.engine().session_manager().find_session(work_sid);
            REQUIRE(s != nullptr);
            auto win = s->active_window();
            REQUIRE(win != nullptr);
            auto pane = win->active_pane();
            REQUIRE(pane != nullptr);
            pane->write_input("for i in $(seq 1 150); do echo \"UX_LOG_LINE_$i\"; done\n");
        }

        // Wait for output to be processed by VTE
        auto line_diff = client.wait_for_matching_inbound([](const ProtocolMessage& m) {
            return std::holds_alternative<MsgFrameDiff>(m) &&
                   frame_diff_contains(std::get<MsgFrameDiff>(m), "UX_LOG_LINE_150");
        }, 5000ms);
        REQUIRE(line_diff.has_value());

        // Configure renderer pane rects
        client.app.renderer().set_pane_rects({
            PaneRect{.pane_id = 1, .x = 0, .y = 0, .width = 39, .height = 23},
            PaneRect{.pane_id = 2, .x = 40, .y = 0, .width = 40, .height = 23}
        });

        // Enter copy mode via Prefix + [
        client.send_input_event(ClientKeyEvent{
            .key_code = 'b', .modifiers = KeyModifiers::Ctrl, .raw_bytes = {0x02}
        });
        client.send_input_event(ClientKeyEvent{.key_code = '[', .raw_bytes = {'['}});
        CHECK(client.app.copy_mode().is_active());

        // Press 'g' to jump to top of history (line 0)
        client.send_input_event(ClientKeyEvent{.key_code = 'g'});
        // ClientApp generated MsgScrollbackSliceReq and sent it to server; wait for MsgScrollbackSliceResp
        auto slice_resp = client.wait_for_inbound<MsgScrollbackSliceResp>();
        REQUIRE(slice_resp.has_value());
        CHECK(slice_resp->total_available_lines >= 100);
        CHECK(client.app.copy_mode().total_lines() >= 100);

        // Verify slice_resp contains log lines
        bool has_log_line = false;
        for (const auto& line : slice_resp->lines) {
            std::string s;
            for (const auto& c : line.cells) {
                if (c.codepoint > 0 && c.codepoint < 128) s.push_back((char)c.codepoint);
            }
            if (s.find("UX_LOG_LINE") != std::string::npos) {
                has_log_line = true;
                break;
            }
        }
        CHECK(has_log_line);

        // Navigate down to line with output ('j' 5 times)
        for (int i = 0; i < 5; ++i) {
            client.send_input_event(ClientKeyEvent{.key_code = 'j'});
        }

        // Line selection (V) and yank (y)
        client.send_input_event(ClientKeyEvent{.key_code = 'V'});
        CHECK(client.app.copy_mode().is_selecting());

        auto r_yank = client.app.process_input_event(ClientKeyEvent{.key_code = 'y'});
        CHECK_FALSE(client.app.copy_mode().is_active());
        CHECK_FALSE(r_yank.terminal_output.empty());

        // Verify OSC 52 sequence format: \x1b]52;c;...\x07
        CHECK(r_yank.terminal_output.starts_with("\x1b]52;c;"));
        CHECK(r_yank.terminal_output.ends_with("\x07"));

        // Verify decoded base64 contents contain text from shell output
        size_t b64_start = r_yank.terminal_output.rfind(';') + 1;
        size_t b64_end = r_yank.terminal_output.find('\x07', b64_start);
        std::string b64 = r_yank.terminal_output.substr(b64_start, b64_end - b64_start);
        std::string decoded = Renderer::base64_decode(b64);
        CHECK(decoded.find("UX_LOG_LINE") != std::string::npos);
    }

    // 4. Mouse SGR Click Focus, Split-Border Drag Resize, and Wheel Scroll
    {
        client.app.renderer().set_active_pane(1);
        const auto rects_init = client.app.renderer().pane_rects();
        REQUIRE(rects_init.size() >= 2);
        uint32_t p2_id = rects_init[1].pane_id;

        // Click inside pane 2 to focus it
        ClientMouseEvent click_p2{
            .kind = ClientMouseEvent::Kind::Press,
            .button = ClientMouseEvent::Button::Left,
            .col = static_cast<uint16_t>(rects_init[1].x + 5),
            .row = static_cast<uint16_t>(rects_init[1].y + 5)
        };
        auto r_click = client.app.process_input_event(click_p2);
        CHECK(r_click.should_redraw);
        CHECK(client.app.renderer().active_pane() == p2_id);

        // Drag vertical border
        uint16_t border_col = rects_init[0].x + rects_init[0].width;
        uint16_t border_row = rects_init[0].y + 5;

        client.app.process_input_event(ClientMouseEvent{
            .kind = ClientMouseEvent::Kind::Press,
            .button = ClientMouseEvent::Button::Left,
            .col = border_col,
            .row = border_row
        });
        client.app.process_input_event(ClientMouseEvent{
            .kind = ClientMouseEvent::Kind::Move,
            .button = ClientMouseEvent::Button::Left,
            .col = static_cast<uint16_t>(border_col + 4),
            .row = border_row
        });
        client.app.process_input_event(ClientMouseEvent{
            .kind = ClientMouseEvent::Kind::Release,
            .button = ClientMouseEvent::Button::Left,
            .col = static_cast<uint16_t>(border_col + 4),
            .row = border_row
        });

        const auto& rects_dragged = client.app.renderer().pane_rects();
        REQUIRE(rects_dragged.size() >= 2);
        CHECK(rects_dragged[0].width == rects_init[0].width + 4);

        // Mouse wheel scroll enters copy mode
        ClientMouseEvent wheel{
            .kind = ClientMouseEvent::Kind::WheelUp,
            .button = ClientMouseEvent::Button::None,
            .col = 10,
            .row = 5
        };
        auto r_wheel = client.app.process_input_event(wheel);
        CHECK(r_wheel.should_redraw);
        CHECK(client.app.copy_mode().is_active());
    }

    // 5. DECSET 2026 Synchronized Output
    {
        std::string frame = client.app.render_frame(/*use_decset_2026=*/true);
        CHECK(frame.starts_with("\x1b[?2026h"));
        CHECK(frame.ends_with("\x1b[?2026l"));
    }

    client_conn->close();
    runner.stop();
    listener->close();
    ::unlink(sock_path.c_str());
}
