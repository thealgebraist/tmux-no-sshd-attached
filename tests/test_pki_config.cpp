#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/pki/pki.hpp"
#include "../src/config/config.hpp"

#include <sys/stat.h>
#include <filesystem>
#include <iostream>

using namespace tmux2::pki;
using namespace tmux2::config;

TEST_CASE("Key Generation and PEM Serialization") {
    SUBCASE("Ed25519 Key Generation") {
        auto key_res = generate_ed25519_key();
        REQUIRE(key_res.has_value());
        UniquePKey pkey = std::move(*key_res);
        CHECK(pkey != nullptr);

        auto priv_pem = pkey_to_pem_private(pkey.get());
        REQUIRE(priv_pem.has_value());
        CHECK(priv_pem->find("BEGIN PRIVATE KEY") != std::string::npos);

        auto pub_pem = pkey_to_pem_public(pkey.get());
        REQUIRE(pub_pem.has_value());
        CHECK(pub_pem->find("BEGIN PUBLIC KEY") != std::string::npos);

        auto parsed_priv = pem_to_private_key(*priv_pem);
        REQUIRE(parsed_priv.has_value());
        CHECK(parsed_priv->get() != nullptr);

        auto parsed_pub = pem_to_public_key(*pub_pem);
        REQUIRE(parsed_pub.has_value());
        CHECK(parsed_pub->get() != nullptr);

        auto spki_fp = compute_pkey_spki_fingerprint(pkey.get());
        REQUIRE(spki_fp.has_value());
        CHECK(spki_fp->size() == 95); // 32 bytes hex with colons = 32*2 + 31 = 95
    }

    SUBCASE("P-256 Key Generation") {
        auto p256_res = generate_p256_key();
        REQUIRE(p256_res.has_value());
        CHECK(*p256_res != nullptr);
    }

    SUBCASE("Corrupted and Invalid PEM Inputs") {
        auto invalid_priv = pem_to_private_key("--- INVALID PEM DATA ---");
        CHECK(!invalid_priv.has_value());

        auto empty_priv = pem_to_private_key("");
        CHECK(!empty_priv.has_value());

        auto invalid_pub = pem_to_public_key("NOT A KEY");
        CHECK(!invalid_pub.has_value());

        auto invalid_x509 = pem_to_x509("GARBAGE CERT");
        CHECK(!invalid_x509.has_value());

        auto empty_x509 = pem_to_x509("");
        CHECK(!empty_x509.has_value());

        auto null_priv_res = pkey_to_pem_private(nullptr);
        CHECK(!null_priv_res.has_value());

        auto null_pub_res = pkey_to_pem_public(nullptr);
        CHECK(!null_pub_res.has_value());

        auto null_cert_res = x509_to_pem(nullptr);
        CHECK(!null_cert_res.has_value());
    }
}

TEST_CASE("CA Certificate Generation and Self-Verification") {
    CaOptions opts{
        .common_name = "Test Root CA",
        .organization = "tmux2 Test Org",
        .validity_days = 365
    };

    auto ca_res = generate_ca(opts);
    REQUIRE(ca_res.has_value());
    auto& ca = *ca_res;

    CHECK(ca.certificate != nullptr);
    CHECK(ca.private_key != nullptr);
    CHECK(ca.certificate_pem.find("BEGIN CERTIFICATE") != std::string::npos);
    CHECK(ca.private_key_pem.find("BEGIN PRIVATE KEY") != std::string::npos);
    CHECK(!ca.fingerprint_sha256.empty());
    CHECK(!ca.spki_fingerprint_sha256.empty());

    // Verify self-signed chain
    auto verify_res = verify_certificate_chain(ca.certificate.get(), {ca.certificate.get()});
    CHECK(verify_res.has_value());
}

TEST_CASE("Server Certificate Generation and Hostname Verification") {
    auto ca_res = generate_ca();
    REQUIRE(ca_res.has_value());
    auto& ca = *ca_res;

    ServerCertOptions srv_opts{
        .common_name = "tmux2 Test Server",
        .organization = "tmux2",
        .hosts = {"dev.tmux2.internal", "192.168.1.50", "2001:db8::1"},
        .validity_days = 30
    };

    auto srv_res = generate_server_cert(srv_opts, ca.private_key.get(), ca.certificate.get());
    REQUIRE(srv_res.has_value());
    auto& srv = *srv_res;

    // Verify chain against CA
    auto verify_res = verify_certificate_chain(srv.certificate.get(), {ca.certificate.get()});
    CHECK(verify_res.has_value());

    // Verify failure against empty CA list
    auto fail_res = verify_certificate_chain(srv.certificate.get(), {});
    CHECK(!fail_res.has_value());

    // Hostname / IP SAN validation
    CHECK(verify_server_hostname(srv.certificate.get(), "dev.tmux2.internal"));
    CHECK(verify_server_hostname(srv.certificate.get(), "192.168.1.50"));
    CHECK(verify_server_hostname(srv.certificate.get(), "2001:db8::1"));
    CHECK(verify_server_hostname(srv.certificate.get(), "localhost"));
    CHECK(verify_server_hostname(srv.certificate.get(), "127.0.0.1"));
    CHECK(verify_server_hostname(srv.certificate.get(), "::1"));

    // Negative hostname validation
    CHECK(!verify_server_hostname(srv.certificate.get(), "evil.attacker.com"));
    CHECK(!verify_server_hostname(srv.certificate.get(), "192.168.1.99"));
    CHECK(!verify_server_hostname(nullptr, "localhost"));
    CHECK(!verify_server_hostname(srv.certificate.get(), ""));
}

TEST_CASE("Client Certificate Generation and Identity Extraction") {
    auto ca_res = generate_ca();
    REQUIRE(ca_res.has_value());
    auto& ca = *ca_res;

    ClientCertOptions cli_opts{
        .common_name = "Alice Key",
        .organization = "tmux2",
        .user_name = "alice",
        .custom_san_uris = {"spiffe://cluster.local/user/alice"},
        .validity_days = 60
    };

    auto cli_res = generate_client_cert(cli_opts, ca.private_key.get(), ca.certificate.get());
    REQUIRE(cli_res.has_value());
    auto& cli = *cli_res;

    // Verify chain
    auto chain_res = verify_certificate_chain(cli.certificate.get(), {ca.certificate.get()});
    CHECK(chain_res.has_value());

    // Identity extraction
    auto id_res = extract_client_identity(cli.certificate.get());
    REQUIRE(id_res.has_value());
    const auto& id = *id_res;

    CHECK(id.common_name == "Alice Key");
    CHECK(id.extract_username().value_or("") == "alice");

    bool has_tmux2_uri = false;
    bool has_spiffe_uri = false;
    for (const auto& u : id.san_uris) {
        if (u == "tmux2://user/alice") has_tmux2_uri = true;
        if (u == "spiffe://cluster.local/user/alice") has_spiffe_uri = true;
    }
    CHECK(has_tmux2_uri);
    CHECK(has_spiffe_uri);
    CHECK(!id.cert_fingerprint.empty());
    CHECK(!id.spki_fingerprint.empty());

    SUBCASE("Mismatched CA Chain Verification") {
        auto other_ca_res = generate_ca(CaOptions{.common_name = "Rogue CA"});
        REQUIRE(other_ca_res.has_value());
        // Verifying alice's cert against another CA must fail!
        auto fail_chain = verify_certificate_chain(cli.certificate.get(), {other_ca_res->certificate.get()});
        CHECK(!fail_chain.has_value());
    }
}

TEST_CASE("PKI Init, File Permissions & Profile Export/Import Roundtrip") {
    std::filesystem::path temp_base = std::filesystem::current_path() / "test_pki_env";
    std::error_code ec;
    std::filesystem::remove_all(temp_base, ec);
    std::filesystem::create_directories(temp_base, ec);

    auto pki_dir = temp_base / "server_pki";
    auto init_res = init_pki(pki_dir, "mybox.corp", "bob", "0.0.0.0:9443");
    REQUIRE(init_res.has_value());

    CHECK(std::filesystem::exists(init_res->ca_crt_path));
    CHECK(std::filesystem::exists(init_res->ca_key_path));
    CHECK(std::filesystem::exists(init_res->server_crt_path));
    CHECK(std::filesystem::exists(init_res->server_key_path));
    CHECK(std::filesystem::exists(init_res->client_crt_path));
    CHECK(std::filesystem::exists(init_res->client_key_path));
    CHECK(std::filesystem::exists(init_res->server_toml_path));

    // Verify strict 0600 file permissions on private keys
    struct stat st;
    REQUIRE(::stat(init_res->ca_key_path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    REQUIRE(::stat(init_res->server_key_path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    REQUIRE(::stat(init_res->client_key_path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    // Export client profile
    auto profile_export_res = export_client_profile_from_files(
        "mybox",
        "mybox.corp:9443",
        "quic",
        init_res->ca_crt_path,
        init_res->client_crt_path,
        init_res->client_key_path
    );
    REQUIRE(profile_export_res.has_value());
    std::string profile_toml = *profile_export_res;
    CHECK(profile_toml.find("mybox.corp:9443") != std::string::npos);

    // Save profile to file with 0600 permissions
    auto profile_file = temp_base / "mybox.tmux2-profile";
    auto save_p_res = write_file_secure(profile_file, profile_toml, 0600);
    REQUIRE(save_p_res.has_value());

    REQUIRE(::stat(profile_file.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    // Import client profile into a new client directory
    auto client_config_dir = temp_base / "client_config";
    auto import_res = import_client_profile_file(profile_file, client_config_dir);
    REQUIRE(import_res.has_value());

    CHECK(import_res->default_host == "mybox");
    REQUIRE(import_res->hosts.contains("mybox"));
    const auto& host_entry = import_res->hosts.at("mybox");
    CHECK(host_entry.address == "mybox.corp:9443");
    CHECK(host_entry.transport == "quic");
    CHECK(std::filesystem::exists(host_entry.ca_cert));
    CHECK(std::filesystem::exists(host_entry.client_cert));
    CHECK(std::filesystem::exists(host_entry.client_key));

    // Verify imported client key has 0600 permission
    REQUIRE(::stat(host_entry.client_key.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    // Clean up
    std::filesystem::remove_all(temp_base, ec);
}

TEST_CASE("Client Config TOML Serialization & Parsing Roundtrip") {
    std::string sample_toml = R"(
default_host = "dev"
prefix_key = "C-a"
escape_time_ms = 10
mouse = false
clipboard = "osc52"

[hosts.dev]
address = "dev.example.com:8443"
transport = "auto"
ca_cert = "~/.config/tmux2/pki/dev/ca.crt"
client_cert = "~/.config/tmux2/pki/dev/client.crt"
client_key = "~/.config/tmux2/pki/dev/client.key"
auto_reconnect = true
server_fingerprint = "SHA256:12:34:56:78"

[hosts.prod]
address = "prod.example.com:8443"
transport = "tcp"
ca_cert = "/etc/ssl/certs/ca.pem"
client_cert = "~/.config/tmux2/pki/prod/client.crt"
client_key = "~/.config/tmux2/pki/prod/client.key"
auto_reconnect = false
)";

    auto parse_res = parse_client_config(sample_toml);
    REQUIRE(parse_res.has_value());
    const auto& cfg = *parse_res;

    CHECK(cfg.default_host == "dev");
    CHECK(cfg.prefix_key == "C-a");
    CHECK(cfg.escape_time_ms == 10);
    CHECK(!cfg.mouse);
    CHECK(cfg.clipboard == "osc52");

    REQUIRE(cfg.hosts.contains("dev"));
    const auto& dev_h = cfg.hosts.at("dev");
    CHECK(dev_h.address == "dev.example.com:8443");
    CHECK(dev_h.transport == "auto");
    CHECK(dev_h.auto_reconnect);
    CHECK(dev_h.server_fingerprint == "SHA256:12:34:56:78");

    REQUIRE(cfg.hosts.contains("prod"));
    const auto& prod_h = cfg.hosts.at("prod");
    CHECK(prod_h.address == "prod.example.com:8443");
    CHECK(prod_h.transport == "tcp");
    CHECK(!prod_h.auto_reconnect);

    // Roundtrip serialization
    std::string serialized = serialize_client_config(cfg);
    auto roundtrip_res = parse_client_config(serialized);
    REQUIRE(roundtrip_res.has_value());
    CHECK(roundtrip_res->default_host == cfg.default_host);
    CHECK(roundtrip_res->prefix_key == cfg.prefix_key);
    CHECK(roundtrip_res->hosts.size() == 2);
    CHECK(roundtrip_res->hosts.at("dev").address == dev_h.address);
}

TEST_CASE("Server Config TOML Serialization, Parsing & Authorization") {
    std::string server_toml = R"(
listen_addrs = ["0.0.0.0:8443", "[::]:8443"]
local_socket = "/run/tmux2/1000/default.sock"
mode = "user"
scrollback_lines = 50000
session_idle_timeout = "never"
resume_token_ttl = "24h"

[tls]
server_cert = "~/.config/tmux2/pki/server.crt"
server_key = "~/.config/tmux2/pki/server.key"
client_ca_cert = "~/.config/tmux2/pki/ca.crt"
require_client_cert = true
min_version = "1.3"

[[authorization.allow]]
cert_san_uri = "tmux2://user/alice"
unix_user = "alice"
allowed_sessions = ["*"]
role = "admin"

[[authorization.allow]]
cert_cn = "bob-laptop"
unix_user = "bob"
allowed_sessions = ["dev", "pair"]
role = "dev"

[[authorization.allow]]
cert_fingerprint = "AA:BB:CC:DD:EE:FF"
unix_user = "guest"
allowed_sessions = ["public"]
role = "readonly"
)";

    auto srv_parse_res = parse_server_config(server_toml);
    REQUIRE(srv_parse_res.has_value());
    const auto& srv = *srv_parse_res;

    CHECK(srv.listen_addrs.size() == 2);
    CHECK(srv.mode == "user");
    CHECK(srv.scrollback_lines == 50000);
    CHECK(srv.tls.require_client_cert);
    CHECK(srv.authorization_rules.size() == 3);

    // Test Rule 1: SAN URI matching
    tmux2::pki::ClientIdentity alice_id{
        .san_uris = {"tmux2://user/alice"},
        .common_name = "alice-cert",
        .cert_fingerprint = "11:22:33",
        .spki_fingerprint = "44:55:66"
    };
    auto auth_alice = authorize_client(srv, alice_id, "any_session");
    REQUIRE(auth_alice.has_value());
    CHECK((*auth_alice)->unix_user == "alice");
    CHECK((*auth_alice)->role.value_or("") == "admin");

    // Test Rule 2: CN matching & session restriction
    tmux2::pki::ClientIdentity bob_id{
        .san_uris = {},
        .common_name = "bob-laptop",
        .cert_fingerprint = "77:88:99",
        .spki_fingerprint = "AA:BB:CC"
    };
    // "dev" session allowed
    auto auth_bob_dev = authorize_client(srv, bob_id, "dev");
    REQUIRE(auth_bob_dev.has_value());
    CHECK((*auth_bob_dev)->unix_user == "bob");

    // "secret" session rejected for bob
    auto auth_bob_secret = authorize_client(srv, bob_id, "secret");
    CHECK(!auth_bob_secret.has_value());

    // Test Rule 3: Fingerprint matching (normalized, case insensitive, with or without SHA256: prefix)
    tmux2::pki::ClientIdentity guest_id{
        .san_uris = {},
        .common_name = "unknown",
        .cert_fingerprint = "SHA256:aa:bb:cc:dd:ee:ff",
        .spki_fingerprint = "00:11"
    };
    auto auth_guest = authorize_client(srv, guest_id, "public");
    REQUIRE(auth_guest.has_value());
    CHECK((*auth_guest)->unix_user == "guest");

    // Test Unmatched identity
    tmux2::pki::ClientIdentity stranger{
        .san_uris = {"tmux2://user/eve"},
        .common_name = "eve",
        .cert_fingerprint = "99:99:99",
        .spki_fingerprint = "99:99:99"
    };
    auto auth_stranger = authorize_client(srv, stranger);
    CHECK(!auth_stranger.has_value());

    // Roundtrip server config
    std::string srv_serialized = serialize_server_config(srv);
    auto srv_rt = parse_server_config(srv_serialized);
    REQUIRE(srv_rt.has_value());
    CHECK(srv_rt->authorization_rules.size() == 3);
    CHECK(srv_rt->tls.min_version == "1.3");
}

TEST_CASE("Multi-Certificate CA Bundle parsing & Intermediate CA Validation") {
    auto root_ca_res = generate_ca(CaOptions{.common_name = "Corporate Root CA"});
    REQUIRE(root_ca_res.has_value());
    auto& root_ca = *root_ca_res;

    // Create Intermediate CA
    UniqueX509 inter_cert(X509_new());
    REQUIRE(inter_cert != nullptr);
    X509_set_version(inter_cert.get(), 2);

    UniqueBignum bn(BN_new());
    BN_rand(bn.get(), 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY);
    UniqueAsn1Integer serial(BN_to_ASN1_INTEGER(bn.get(), nullptr));
    X509_set_serialNumber(inter_cert.get(), serial.get());

    X509_gmtime_adj(X509_getm_notBefore(inter_cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(inter_cert.get()), 365 * 86400L);

    X509_NAME* iname = X509_get_subject_name(inter_cert.get());
    X509_NAME_add_entry_by_txt(iname, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("Corporate Intermediate CA"), -1, -1, 0);
    X509_set_issuer_name(inter_cert.get(), X509_get_subject_name(root_ca.certificate.get()));

    auto inter_key_res = generate_ed25519_key();
    REQUIRE(inter_key_res.has_value());
    UniquePKey inter_key = std::move(*inter_key_res);
    X509_set_pubkey(inter_cert.get(), inter_key.get());

    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, root_ca.certificate.get(), inter_cert.get(), nullptr, nullptr, 0);

    UniqueX509Extension ext1(X509V3_EXT_conf_nid(nullptr, &ctx, NID_basic_constraints, const_cast<char*>("critical,CA:TRUE")));
    X509_add_ext(inter_cert.get(), ext1.get(), -1);
    UniqueX509Extension ext2(X509V3_EXT_conf_nid(nullptr, &ctx, NID_key_usage, const_cast<char*>("critical,keyCertSign,cRLSign")));
    X509_add_ext(inter_cert.get(), ext2.get(), -1);

    REQUIRE(X509_sign(inter_cert.get(), root_ca.private_key.get(), nullptr) > 0);

    // Now issue leaf client cert using Intermediate CA
    ClientCertOptions leaf_opts{
        .common_name = "Corporate Worker",
        .organization = "Corporate",
        .user_name = "worker1"
    };
    auto leaf_res = generate_client_cert(leaf_opts, inter_key.get(), inter_cert.get());
    REQUIRE(leaf_res.has_value());

    // Verify leaf with Intermediate in untrusted stack, and Root CA in trust store
    auto valid_chain = verify_certificate_chain(
        leaf_res->certificate.get(),
        {root_ca.certificate.get()},
        {inter_cert.get()}
    );
    CHECK(valid_chain.has_value());

    // Verify leaf fails without intermediate certificate
    auto fail_chain = verify_certificate_chain(
        leaf_res->certificate.get(),
        {root_ca.certificate.get()},
        {}
    );
    CHECK(!fail_chain.has_value());

    // Multi-CA bundle serialization and parsing
    auto inter_pem = x509_to_pem(inter_cert.get());
    REQUIRE(inter_pem.has_value());
    std::string bundle = root_ca.certificate_pem + "\n" + *inter_pem;

    auto parsed_bundle = pem_bundle_to_x509s(bundle);
    REQUIRE(parsed_bundle.has_value());
    CHECK(parsed_bundle->size() == 2);
}

TEST_CASE("Error and Edge Cases in TOML and Profiles") {
    SUBCASE("Invalid TOML strings") {
        auto bad_cli = parse_client_config("invalid [[ toml {{{");
        CHECK(!bad_cli.has_value());

        auto bad_srv = parse_server_config("invalid server toml ][[");
        CHECK(!bad_srv.has_value());

        auto bad_prof = parse_profile_config("invalid profile = [");
        CHECK(!bad_prof.has_value());
    }

    SUBCASE("Profile missing fields") {
        std::string missing_name = R"(
server = "test:8443"
ca_cert = "ca"
client_cert = "cert"
client_key = "key"
)";
        CHECK(!parse_profile_config(missing_name).has_value());

        std::string missing_server = R"(
name = "dev"
ca_cert = "ca"
client_cert = "cert"
client_key = "key"
)";
        CHECK(!parse_profile_config(missing_server).has_value());

        std::string missing_key = R"(
name = "dev"
server = "test:8443"
ca_cert = "ca"
client_cert = "cert"
)";
        CHECK(!parse_profile_config(missing_key).has_value());
    }

    SUBCASE("Export profile with empty fields") {
        auto empty_name = export_client_profile_string("", "s:8443", "auto", "ca", "cert", "key");
        CHECK(!empty_name.has_value());

        auto empty_server = export_client_profile_string("dev", "", "auto", "ca", "cert", "key");
        CHECK(!empty_server.has_value());

        auto empty_ca = export_client_profile_string("dev", "s:8443", "auto", "", "cert", "key");
        CHECK(!empty_ca.has_value());
    }

    SUBCASE("PKI init with empty host") {
        auto res = init_pki("/tmp/ignored_pki", "");
        CHECK(!res.has_value());
    }

    SUBCASE("Authorize client with empty rules") {
        ServerConfig empty_cfg;
        empty_cfg.authorization_rules.clear();
        ClientIdentity id;
        auto auth_res = authorize_client(empty_cfg, id);
        CHECK(!auth_res.has_value());
    }
}
