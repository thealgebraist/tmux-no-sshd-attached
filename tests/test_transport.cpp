#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "src/transport/transport.hpp"
#include "src/transport/tls_utils.hpp"
#include "src/transport/quic_transport.hpp"
#include "src/transport/tls_tcp_transport.hpp"
#include "src/transport/unix_transport.hpp"
#include "src/transport/happy_eyeballs.hpp"
#include "src/pki/cert_generator.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace tmux2;
using namespace tmux2::transport;
using namespace tmux2::pki;
using namespace std::chrono_literals;

namespace {

struct TestPki {
    GeneratedCertificate ca;
    GeneratedCertificate server;
    GeneratedCertificate client;
    TlsConfig server_cfg;
    TlsConfig client_cfg;

    static TestPki create(const std::string& host = "127.0.0.1", const std::string& user = "alice") {
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

        return p;
    }
};

class ServerRunner {
public:
    explicit ServerRunner(IListener* listener)
        : m_listener(listener), m_stop(false) {
        m_thread = std::thread([this]() {
            while (!m_stop.load()) {
                if (m_listener) {
                    auto acc = m_listener->accept_connection();
                    if (acc && *acc) {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_connections.push_back(*acc);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (auto& conn : m_connections) {
                        conn->poll_io();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    ~ServerRunner() {
        stop();
    }

    void stop() {
        if (!m_stop.exchange(true)) {
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }
    }

    std::shared_ptr<IConnection> wait_for_connection(std::chrono::milliseconds timeout = 2000ms) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_connections.empty() && m_connections.front()->is_connected()) {
                    return m_connections.front();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return nullptr;
    }

private:
    IListener* m_listener;
    std::atomic<bool> m_stop;
    std::mutex m_mutex;
    std::vector<std::shared_ptr<IConnection>> m_connections;
    std::thread m_thread;
};

} // namespace

TEST_CASE("QUIC Transport: Server listener, client connection, and mTLS handshake") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    auto listener_res = QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    REQUIRE(listener_res.has_value());
    auto listener = std::move(*listener_res);
    uint16_t port = listener->bound_port();
    CHECK(port > 0);

    ServerRunner runner(listener.get());

    auto client_conn_res = QuicConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;
    CHECK(client_conn->is_connected());
    CHECK(client_conn->type() == TransportType::Quic);

    auto server_conn = runner.wait_for_connection();
    REQUIRE(server_conn != nullptr);
    CHECK(server_conn->is_connected());
    CHECK(server_conn->type() == TransportType::Quic);

    // Verify mTLS identity extraction
    auto peer_id = server_conn->peer_identity();
    REQUIRE(peer_id.has_value());
    CHECK_FALSE(peer_id->cert_fingerprint.empty());
    auto username = peer_id->extract_username();
    REQUIRE(username.has_value());
    CHECK(*username == "alice");

    runner.stop();
    client_conn->close();
    server_conn->close();
    listener->close();
}

TEST_CASE("QUIC Transport: Multi-stream (control/input, render, scrollback)") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    auto listener = *QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    auto client_conn_res = QuicConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;

    auto server_conn = runner.wait_for_connection();
    REQUIRE(server_conn != nullptr);
    runner.stop();

    // 1. Open Stream 0 (Control)
    auto c_ctrl = *client_conn->open_stream(0, true);
    CHECK(c_ctrl->id() == 0);

    // Write framed message on control stream
    std::string msg = "rpc_list_sessions";
    auto write_res = c_ctrl->write_frame(1, protocol::kFlagNone, std::as_bytes(std::span(msg)));
    CHECK(write_res.has_value());

    // Server receives stream
    std::shared_ptr<IStream> s_ctrl;
    server_conn->set_on_stream([&](std::shared_ptr<IStream> s) {
        if (!s_ctrl) s_ctrl = s;
    });

    for (int i = 0; i < 50 && !s_ctrl; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(s_ctrl != nullptr);
    CHECK(s_ctrl->id() == 0);

    // Server reads frame
    bool frame_received = false;
    std::string received_payload;
    s_ctrl->set_on_frame([&](const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
        frame_received = true;
        CHECK(hdr.msg_type == 1);
        CHECK_FALSE(hdr.is_urgent());
        received_payload = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
    });

    for (int i = 0; i < 50 && !frame_received; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }
    CHECK(frame_received);
    CHECK(received_payload == "rpc_list_sessions");

    // 2. Open Stream 2 (Render)
    auto c_render = *client_conn->open_stream(2, true);
    std::string render_diff = "diff_row_12";
    CHECK(c_render->write_frame(2, protocol::kFlagPriorityUrgent, std::as_bytes(std::span(render_diff))).has_value());

    std::shared_ptr<IStream> s_render;
    server_conn->set_on_stream([&](std::shared_ptr<IStream> s) {
        s_render = s;
    });

    for (int i = 0; i < 50 && !s_render; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(s_render != nullptr);

    bool render_frame_received = false;
    s_render->set_on_frame([&](const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
        render_frame_received = true;
        CHECK(hdr.msg_type == 2);
        CHECK(hdr.is_urgent());
        std::string s(reinterpret_cast<const char*>(payload.data()), payload.size());
        CHECK(s == "diff_row_12");
    });

    for (int i = 0; i < 50 && !render_frame_received; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }
    CHECK(render_frame_received);

    // 3. Open Stream 4 (Scrollback)
    auto c_scroll = *client_conn->open_stream(4, true);
    std::string scroll_data = "scrollback_slice_lines_0_100";
    CHECK(c_scroll->write_frame(3, protocol::kFlagZstdCompressed, std::as_bytes(std::span(scroll_data))).has_value());

    std::shared_ptr<IStream> s_scroll;
    server_conn->set_on_stream([&](std::shared_ptr<IStream> s) {
        s_scroll = s;
    });

    for (int i = 0; i < 50 && !s_scroll; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(s_scroll != nullptr);

    bool scroll_received = false;
    s_scroll->set_on_frame([&](const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
        scroll_received = true;
        CHECK(hdr.msg_type == 3);
        CHECK(hdr.is_compressed());
        std::string s(reinterpret_cast<const char*>(payload.data()), payload.size());
        CHECK(s == "scrollback_slice_lines_0_100");
    });

    for (int i = 0; i < 50 && !scroll_received; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }
    CHECK(scroll_received);

    client_conn->close();
    server_conn->close();
    listener->close();
}

TEST_CASE("QUIC Transport: Reactor event loop integration") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    auto reactor_res = reactor::Reactor::create_default();
    REQUIRE(reactor_res.has_value());
    auto reactor = std::move(*reactor_res);

    auto listener = *QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    std::atomic<bool> accepted{false};
    std::shared_ptr<IConnection> accepted_conn;
    listener->set_on_accept([&](std::shared_ptr<IConnection> conn) {
        accepted_conn = conn;
        accepted = true;
    });
    listener->register_with_reactor(*reactor);

    std::atomic<bool> stop_reactor{false};
    std::thread reactor_th([&]() {
        while (!stop_reactor.load()) {
            reactor->run_once(2ms);
        }
    });

    auto client_conn_res = QuicConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;

    for (int i = 0; i < 50 && !accepted.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    stop_reactor = true;
    reactor_th.join();

    REQUIRE(accepted_conn != nullptr);
    CHECK(accepted_conn->is_connected());

    client_conn->close();
    accepted_conn->close();
    listener->close();
}

TEST_CASE("TCP TLS 1.3 Transport: mTLS handshake, strict version & cipher verification") {
    auto pki = TestPki::create("127.0.0.1", "bob");

    auto listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    auto client_conn_res = TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;
    CHECK(client_conn->is_connected());
    CHECK(client_conn->type() == TransportType::TcpTls);

    auto server_conn = runner.wait_for_connection();
    REQUIRE(server_conn != nullptr);
    CHECK(server_conn->is_connected());
    runner.stop();

    // Verify TLS version and ciphers
    auto* tcp_server = dynamic_cast<TcpTlsConnection*>(server_conn.get());
    REQUIRE(tcp_server != nullptr);
    CHECK(std::string(SSL_get_version(tcp_server->ssl())) == "TLSv1.3");

    const char* cipher = SSL_get_cipher_name(tcp_server->ssl());
    std::string cipher_str = cipher ? cipher : "";
    CHECK((cipher_str == "TLS_AES_256_GCM_SHA384" ||
           cipher_str == "TLS_CHACHA20_POLY1305_SHA256" ||
           cipher_str == "TLS_AES_128_GCM_SHA256"));

    // Verify peer identity
    auto peer_id = server_conn->peer_identity();
    REQUIRE(peer_id.has_value());
    CHECK(*peer_id->extract_username() == "bob");

    client_conn->close();
    server_conn->close();
    listener->close();
}

TEST_CASE("TCP TLS 1.3 Transport: 8-byte FrameHeader stream multiplexing") {
    auto pki = TestPki::create("127.0.0.1", "bob");

    auto listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    auto client_conn_res = TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;

    auto server_conn = runner.wait_for_connection();
    REQUIRE(server_conn != nullptr);
    runner.stop();

    // Open Stream 0 (Control) and Stream 2 (Render) concurrently on client
    auto c_ctrl = *client_conn->open_stream(0);
    auto c_render = *client_conn->open_stream(2);

    std::string ctrl_msg = "hello control";
    std::string render_msg = "hello render";

    CHECK(c_ctrl->write_frame(1, protocol::kFlagNone, std::as_bytes(std::span(ctrl_msg))).has_value());
    CHECK(c_render->write_frame(2, protocol::kFlagPriorityUrgent, std::as_bytes(std::span(render_msg))).has_value());

    // Flush and read on server
    for (int i = 0; i < 20; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }

    auto s_ctrl = server_conn->get_stream(0);
    REQUIRE(s_ctrl != nullptr);
    auto s_render = server_conn->get_stream(2);
    REQUIRE(s_render != nullptr);

    // Verify raw read
    std::byte buf[64];
    auto r1 = s_ctrl->read(buf);
    REQUIRE(r1.has_value());
    CHECK(*r1 == ctrl_msg.size());
    CHECK(std::string(reinterpret_cast<char*>(buf), *r1) == ctrl_msg);

    auto r2 = s_render->read(buf);
    REQUIRE(r2.has_value());
    CHECK(*r2 == render_msg.size());
    CHECK(std::string(reinterpret_cast<char*>(buf), *r2) == render_msg);

    // Test FIN stream flag
    c_ctrl->close();
    for (int i = 0; i < 20; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }
    CHECK(s_ctrl->is_closed());

    client_conn->close();
    server_conn->close();
    listener->close();
}

TEST_CASE("Strict mTLS Handshake Rejection: Missing client certificate") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    auto listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    // Client config has NO certificate/key
    TlsConfig bad_client_cfg = pki.client_cfg;
    bad_client_cfg.cert_pem.clear();
    bad_client_cfg.key_pem.clear();

    auto conn_res = TcpTlsConnector::connect("127.0.0.1", port, bad_client_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("Strict mTLS Handshake Rejection: Untrusted / Wrong CA") {
    auto pki1 = TestPki::create("127.0.0.1", "alice");
    auto pki2 = TestPki::create("127.0.0.1", "mallory"); // CA 2

    auto listener = *TcpTlsListener::create("127.0.0.1", 0, pki1.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    // Client presents cert signed by CA 2 to Server trusting CA 1
    TlsConfig attacker_cfg = pki2.client_cfg;
    attacker_cfg.ca_cert_pem = pki1.ca.certificate_pem; // trusts server, but presents cert from CA2

    auto conn_res = TcpTlsConnector::connect("127.0.0.1", port, attacker_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("Strict mTLS Handshake Rejection: Untrusted Server Certificate") {
    auto pki1 = TestPki::create("127.0.0.1", "alice");
    auto pki2 = TestPki::create("127.0.0.1", "rogue_server"); // CA 2

    // Server has cert signed by CA 2
    auto listener = *TcpTlsListener::create("127.0.0.1", 0, pki2.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    // Client only trusts CA 1
    TlsConfig strict_client_cfg = pki1.client_cfg;

    auto conn_res = TcpTlsConnector::connect("127.0.0.1", port, strict_client_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("Strict mTLS Handshake Rejection: Hostname mismatch") {
    // Server cert is generated for "dev.tmux2.internal"
    auto pki = TestPki::create("dev.tmux2.internal", "alice");

    auto listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    // Client expects "prod.tmux2.internal"
    TlsConfig mismatch_client_cfg = pki.client_cfg;
    mismatch_client_cfg.expected_server_name = "prod.tmux2.internal";

    auto conn_res = TcpTlsConnector::connect("127.0.0.1", port, mismatch_client_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("Strict mTLS Handshake Rejection: ALPN mismatch") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    auto listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    TlsConfig bad_alpn_cfg = pki.client_cfg;
    bad_alpn_cfg.alpn = "http/1.1"; // Mismatched ALPN

    auto conn_res = TcpTlsConnector::connect("127.0.0.1", port, bad_alpn_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("QUIC Strict mTLS Rejection: Missing client certificate") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    auto listener = *QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    TlsConfig bad_client_cfg = pki.client_cfg;
    bad_client_cfg.cert_pem.clear();
    bad_client_cfg.key_pem.clear();

    auto conn_res = QuicConnector::connect("127.0.0.1", port, bad_client_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("QUIC Strict mTLS Rejection: Untrusted / Wrong CA") {
    auto pki1 = TestPki::create("127.0.0.1", "alice");
    auto pki2 = TestPki::create("127.0.0.1", "mallory");

    auto listener = *QuicListener::create("127.0.0.1", 0, pki1.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    TlsConfig attacker_cfg = pki2.client_cfg;
    attacker_cfg.ca_cert_pem = pki1.ca.certificate_pem;

    auto conn_res = QuicConnector::connect("127.0.0.1", port, attacker_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("QUIC Strict mTLS Rejection: Untrusted Server Certificate") {
    auto pki1 = TestPki::create("127.0.0.1", "alice");
    auto pki2 = TestPki::create("127.0.0.1", "rogue_server");

    auto listener = *QuicListener::create("127.0.0.1", 0, pki2.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    TlsConfig strict_client_cfg = pki1.client_cfg;

    auto conn_res = QuicConnector::connect("127.0.0.1", port, strict_client_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("QUIC Strict mTLS Rejection: Hostname mismatch") {
    auto pki = TestPki::create("dev.tmux2.internal", "alice");

    auto listener = *QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    TlsConfig mismatch_client_cfg = pki.client_cfg;
    mismatch_client_cfg.expected_server_name = "prod.tmux2.internal";

    auto conn_res = QuicConnector::connect("127.0.0.1", port, mismatch_client_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("QUIC Strict mTLS Rejection: ALPN mismatch") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    auto listener = *QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    TlsConfig bad_alpn_cfg = pki.client_cfg;
    bad_alpn_cfg.alpn = "http/1.1";

    auto conn_res = QuicConnector::connect("127.0.0.1", port, bad_alpn_cfg, 1000ms);
    CHECK_FALSE(conn_res.has_value());

    runner.stop();
    listener->close();
}

TEST_CASE("AF_UNIX Domain Socket: 0600 permissions, peer UID, and FrameHeader multiplexing") {
    std::string sock_path = "/tmp/test_tmux2_unix_" + std::to_string(::getpid()) + ".sock";

    auto listener_res = UnixListener::create(sock_path);
    REQUIRE(listener_res.has_value());
    auto listener = std::move(*listener_res);

    // Verify 0600 file permissions
    struct stat st{};
    REQUIRE(::stat(sock_path.c_str(), &st) == 0);
    mode_t perms = st.st_mode & 0777;
    CHECK(perms == 0600);

    // Connect client
    auto client_conn_res = UnixConnector::connect(sock_path, 1000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;
    CHECK(client_conn->is_connected());
    CHECK(client_conn->type() == TransportType::Unix);

    // Accept on server
    auto server_conn_res = listener->accept_connection();
    REQUIRE(server_conn_res.has_value());
    auto server_conn = *server_conn_res;
    CHECK(server_conn->is_connected());
    CHECK(server_conn->type() == TransportType::Unix);

    // Verify peer UID extraction via SO_PEERCRED
    auto uid = server_conn->peer_uid();
    REQUIRE(uid.has_value());
    CHECK(*uid == ::getuid());

    // Test FrameHeader multiplexing over Unix domain socket
    auto c_ctrl = *client_conn->open_stream(0);
    auto c_render = *client_conn->open_stream(2);

    std::string ctrl_msg = "unix_ctrl_message";
    std::string render_msg = "unix_render_snapshot";

    CHECK(c_ctrl->write_frame(1, protocol::kFlagNone, std::as_bytes(std::span(ctrl_msg))).has_value());
    CHECK(c_render->write_frame(2, protocol::kFlagPriorityUrgent, std::as_bytes(std::span(render_msg))).has_value());

    for (int i = 0; i < 20; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }

    auto s_ctrl = server_conn->get_stream(0);
    REQUIRE(s_ctrl != nullptr);
    auto s_render = server_conn->get_stream(2);
    REQUIRE(s_render != nullptr);

    std::byte buf[64];
    auto r1 = s_ctrl->read(buf);
    REQUIRE(r1.has_value());
    CHECK(std::string(reinterpret_cast<char*>(buf), *r1) == ctrl_msg);

    auto r2 = s_render->read(buf);
    REQUIRE(r2.has_value());
    CHECK(std::string(reinterpret_cast<char*>(buf), *r2) == render_msg);

    client_conn->close();
    server_conn->close();
    listener->close();

    // Verify socket file was unlinked
    CHECK(::access(sock_path.c_str(), F_OK) != 0);
}

TEST_CASE("Happy Eyeballs Connector: QUIC fast path ('auto' mode)") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    // Start QUIC listener
    auto quic_listener = *QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t quic_port = quic_listener->bound_port();

    // Start TCP listener
    auto tcp_listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t tcp_port = tcp_listener->bound_port();

    ServerRunner quic_runner(quic_listener.get());
    ServerRunner tcp_runner(tcp_listener.get());

    HappyEyeballsOptions opts;
    opts.fallback_delay = 100ms;
    opts.total_timeout = 2000ms;

    auto conn_res = HappyEyeballsConnector::connect(
        "127.0.0.1",
        quic_port,
        tcp_port,
        pki.client_cfg,
        opts
    );
    REQUIRE(conn_res.has_value());
    auto conn = *conn_res;
    CHECK(conn->is_connected());
    // QUIC should win because it is reachable and fast
    CHECK(conn->type() == TransportType::Quic);

    conn->close();
    quic_runner.stop();
    tcp_runner.stop();
    quic_listener->close();
    tcp_listener->close();
}

TEST_CASE("Happy Eyeballs Connector: TCP TLS 1.3 fallback when UDP/QUIC is unreachable/blocked") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    // TCP listener is started
    auto tcp_listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t tcp_port = tcp_listener->bound_port();

    ServerRunner tcp_runner(tcp_listener.get());

    // For QUIC, we use an unreachable/unbound UDP port (e.g. 59999 or closed port)
    uint16_t unreachable_quic_port = 59999;

    HappyEyeballsOptions opts;
    opts.fallback_delay = 50ms;
    opts.total_timeout = 3000ms;

    auto conn_res = HappyEyeballsConnector::connect(
        "127.0.0.1",
        unreachable_quic_port,
        tcp_port,
        pki.client_cfg,
        opts
    );
    REQUIRE(conn_res.has_value());
    auto conn = *conn_res;
    CHECK(conn->is_connected());
    // TCP TLS 1.3 fallback MUST win!
    CHECK(conn->type() == TransportType::TcpTls);

    auto server_conn = tcp_runner.wait_for_connection();
    REQUIRE(server_conn != nullptr);
    tcp_runner.stop();

    // Verify stream exchange on fallback connection
    auto client_stream = *conn->open_stream(0);
    std::string msg = "fallback_stream_hello";
    CHECK(client_stream->write_frame(1, protocol::kFlagNone, std::as_bytes(std::span(msg))).has_value());

    for (int i = 0; i < 20; ++i) {
        conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(5ms);
    }

    auto server_stream = server_conn->get_stream(0);
    REQUIRE(server_stream != nullptr);
    std::byte buf[64];
    auto r = server_stream->read(buf);
    REQUIRE(r.has_value());
    CHECK(std::string(reinterpret_cast<char*>(buf), *r) == msg);

    conn->close();
    server_conn->close();
    tcp_listener->close();
}

TEST_CASE("Happy Eyeballs Connector: Concurrent race when QUIC exceeds fallback delay but wins") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    // Start QUIC listener
    auto quic_listener = *QuicListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t quic_port = quic_listener->bound_port();

    ServerRunner quic_runner(quic_listener.get());

    // TCP port is closed/unreachable so TCP fallback fails or stalls
    uint16_t unreachable_tcp_port = 59998;

    // Set fallback_delay very short (2ms) so fallback TCP thread is guaranteed to start
    // while QUIC is still handshaking. In the old sequential code, QUIC would timeout at 2ms
    // and fail completely. In concurrent Happy Eyeballs, QUIC keeps racing and succeeds!
    HappyEyeballsOptions opts;
    opts.fallback_delay = 2ms;
    opts.total_timeout = 3000ms;

    auto conn_res = HappyEyeballsConnector::connect(
        "127.0.0.1",
        quic_port,
        unreachable_tcp_port,
        pki.client_cfg,
        opts
    );
    REQUIRE(conn_res.has_value());
    auto conn = *conn_res;
    CHECK(conn->is_connected());
    CHECK(conn->type() == TransportType::Quic);

    conn->close();
    quic_runner.stop();
    quic_listener->close();
}

TEST_CASE("Transport Frame Fragmentation & Reassembly") {
    auto pki = TestPki::create("127.0.0.1", "alice");

    auto listener = *TcpTlsListener::create("127.0.0.1", 0, pki.server_cfg);
    uint16_t port = listener->bound_port();

    ServerRunner runner(listener.get());

    auto client_conn_res = TcpTlsConnector::connect("127.0.0.1", port, pki.client_cfg, 2000ms);
    REQUIRE(client_conn_res.has_value());
    auto client_conn = *client_conn_res;

    auto server_conn = runner.wait_for_connection();
    REQUIRE(server_conn != nullptr);
    runner.stop();

    // Send a 128 KB payload spanning multiple frames / packets
    std::vector<std::byte> large_payload(128 * 1024);
    for (size_t i = 0; i < large_payload.size(); ++i) {
        large_payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    auto c_stream = *client_conn->open_stream(0);
    CHECK(c_stream->write_frame(42, protocol::kFlagNone, large_payload).has_value());

    std::vector<std::byte> received_data;
    bool frame_delivered = false;

    // Use on_frame callback
    auto s_stream = server_conn->get_stream(0);
    s_stream->set_on_frame([&](const protocol::StreamHeader& hdr, std::span<const std::byte> payload) {
        frame_delivered = true;
        CHECK(hdr.msg_type == 42);
        CHECK(hdr.payload_len == large_payload.size());
        received_data.assign(payload.begin(), payload.end());
    });

    for (int i = 0; i < 100 && !frame_delivered; ++i) {
        client_conn->poll_io();
        server_conn->poll_io();
        std::this_thread::sleep_for(10ms);
    }

    CHECK(frame_delivered);
    CHECK(received_data == large_payload);

    client_conn->close();
    server_conn->close();
    listener->close();
}
