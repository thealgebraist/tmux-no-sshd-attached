#include "cert_generator.hpp"
#include <arpa/inet.h>
#include <format>
#include <sstream>

namespace tmux2::pki {

namespace {

bool is_ip_address(const std::string& host) {
    struct in_addr addr4;
    if (inet_pton(AF_INET, host.c_str(), &addr4) == 1) {
        return true;
    }
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, host.c_str(), &addr6) == 1) {
        return true;
    }
    return false;
}

std::expected<void, std::string> add_extension(X509* cert, X509V3_CTX* ctx, int nid, const char* value) {
    UniqueX509Extension ext(X509V3_EXT_conf_nid(nullptr, ctx, nid, const_cast<char*>(value)));
    if (!ext) {
        return std::unexpected(std::format("Failed to create X509 extension (nid {}): {}", nid, get_openssl_error()));
    }
    if (X509_add_ext(cert, ext.get(), -1) != 1) {
        return std::unexpected(std::format("Failed to add X509 extension (nid {}): {}", nid, get_openssl_error()));
    }
    return {};
}

std::expected<void, std::string> set_random_serial(X509* cert) {
    UniqueBignum bn(BN_new());
    if (!bn) {
        return std::unexpected(std::format("Failed to allocate BIGNUM: {}", get_openssl_error()));
    }
    if (BN_rand(bn.get(), 128, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1) {
        return std::unexpected(std::format("Failed to generate random serial: {}", get_openssl_error()));
    }
    UniqueAsn1Integer asn1_serial(BN_to_ASN1_INTEGER(bn.get(), nullptr));
    if (!asn1_serial) {
        return std::unexpected(std::format("Failed to convert BIGNUM to ASN1_INTEGER: {}", get_openssl_error()));
    }
    if (X509_set_serialNumber(cert, asn1_serial.get()) != 1) {
        return std::unexpected(std::format("Failed to set serial number: {}", get_openssl_error()));
    }
    return {};
}

const EVP_MD* get_signing_md(const EVP_PKEY* pkey) {
    if (EVP_PKEY_is_a(pkey, "ED25519") || EVP_PKEY_base_id(pkey) == EVP_PKEY_ED25519) {
        return nullptr; // Ed25519 does not take an external digest
    }
    return EVP_sha256();
}

std::expected<GeneratedCertificate, std::string> finalize_cert(
    UniquePKey pkey,
    UniqueX509 cert,
    const EVP_PKEY* signing_key
) {
    const EVP_MD* md = get_signing_md(signing_key);
    if (X509_sign(cert.get(), const_cast<EVP_PKEY*>(signing_key), md) <= 0) {
        return std::unexpected(std::format("Failed to sign certificate: {}", get_openssl_error()));
    }

    auto pkey_pem_res = pkey_to_pem_private(pkey.get());
    if (!pkey_pem_res) {
        return std::unexpected(pkey_pem_res.error());
    }

    auto cert_pem_res = x509_to_pem(cert.get());
    if (!cert_pem_res) {
        return std::unexpected(cert_pem_res.error());
    }

    auto fp_res = compute_cert_fingerprint(cert.get());
    if (!fp_res) {
        return std::unexpected(fp_res.error());
    }

    auto spki_res = compute_spki_fingerprint(cert.get());
    if (!spki_res) {
        return std::unexpected(spki_res.error());
    }

    return GeneratedCertificate{
        .private_key = std::move(pkey),
        .certificate = std::move(cert),
        .private_key_pem = std::move(*pkey_pem_res),
        .certificate_pem = std::move(*cert_pem_res),
        .fingerprint_sha256 = std::move(*fp_res),
        .spki_fingerprint_sha256 = std::move(*spki_res)
    };
}

} // namespace

std::expected<GeneratedCertificate, std::string> generate_ca(const CaOptions& options) {
    auto key_res = generate_ed25519_key();
    if (!key_res) {
        return std::unexpected(key_res.error());
    }
    UniquePKey ca_key = std::move(*key_res);

    UniqueX509 cert(X509_new());
    if (!cert) {
        return std::unexpected(std::format("Failed to create X509: {}", get_openssl_error()));
    }

    // Set X.509 version 3 (version number 2 in zero-based indexing)
    if (X509_set_version(cert.get(), 2) != 1) {
        return std::unexpected(std::format("Failed to set X509 version: {}", get_openssl_error()));
    }

    if (auto res = set_random_serial(cert.get()); !res) {
        return std::unexpected(res.error());
    }

    // Validity
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), static_cast<long>(options.validity_days) * 86400L);

    // Subject & Issuer (Self-signed)
    X509_NAME* name = X509_get_subject_name(cert.get());
    if (!name) {
        return std::unexpected("Failed to get subject name from cert");
    }
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(options.common_name.c_str()), -1, -1, 0) != 1) {
        return std::unexpected(std::format("Failed to set CN: {}", get_openssl_error()));
    }
    if (!options.organization.empty()) {
        if (X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(options.organization.c_str()), -1, -1, 0) != 1) {
            return std::unexpected(std::format("Failed to set O: {}", get_openssl_error()));
        }
    }
    if (X509_set_issuer_name(cert.get(), name) != 1) {
        return std::unexpected(std::format("Failed to set issuer name: {}", get_openssl_error()));
    }

    if (X509_set_pubkey(cert.get(), ca_key.get()) != 1) {
        return std::unexpected(std::format("Failed to set pubkey: {}", get_openssl_error()));
    }

    // Extensions
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert.get(), cert.get(), nullptr, nullptr, 0);

    if (auto res = add_extension(cert.get(), &ctx, NID_basic_constraints, "critical,CA:TRUE"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_key_usage, "critical,keyCertSign,cRLSign"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_subject_key_identifier, "hash"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_authority_key_identifier, "keyid:always,issuer"); !res) return std::unexpected(res.error());

    return finalize_cert(std::move(ca_key), std::move(cert), ca_key.get());
}

std::expected<GeneratedCertificate, std::string> generate_server_cert(
    const ServerCertOptions& options,
    const EVP_PKEY* ca_key,
    const X509* ca_cert
) {
    if (!ca_key || !ca_cert) {
        return std::unexpected("CA key or certificate is null");
    }

    auto key_res = generate_ed25519_key();
    if (!key_res) {
        return std::unexpected(key_res.error());
    }
    UniquePKey server_key = std::move(*key_res);

    UniqueX509 cert(X509_new());
    if (!cert) {
        return std::unexpected(std::format("Failed to create X509: {}", get_openssl_error()));
    }

    if (X509_set_version(cert.get(), 2) != 1) {
        return std::unexpected(std::format("Failed to set X509 version: {}", get_openssl_error()));
    }

    if (auto res = set_random_serial(cert.get()); !res) {
        return std::unexpected(res.error());
    }

    // Validity
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), static_cast<long>(options.validity_days) * 86400L);

    // Subject
    X509_NAME* name = X509_get_subject_name(cert.get());
    if (!name) {
        return std::unexpected("Failed to get subject name from cert");
    }
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(options.common_name.c_str()), -1, -1, 0) != 1) {
        return std::unexpected(std::format("Failed to set CN: {}", get_openssl_error()));
    }
    if (!options.organization.empty()) {
        if (X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(options.organization.c_str()), -1, -1, 0) != 1) {
            return std::unexpected(std::format("Failed to set O: {}", get_openssl_error()));
        }
    }

    // Issuer from CA
    X509_NAME* ca_name = X509_get_subject_name(ca_cert);
    if (!ca_name) {
        return std::unexpected("Failed to get subject name from CA cert");
    }
    if (X509_set_issuer_name(cert.get(), ca_name) != 1) {
        return std::unexpected(std::format("Failed to set issuer name: {}", get_openssl_error()));
    }

    if (X509_set_pubkey(cert.get(), server_key.get()) != 1) {
        return std::unexpected(std::format("Failed to set pubkey: {}", get_openssl_error()));
    }

    // Build SAN list
    std::string san_str;
    auto add_san = [&](const std::string& host) {
        if (host.empty()) return;
        std::string entry = is_ip_address(host) ? ("IP:" + host) : ("DNS:" + host);
        if (!san_str.empty()) {
            san_str += ",";
        }
        san_str += entry;
    };

    bool has_localhost = false;
    bool has_loopback4 = false;
    bool has_loopback6 = false;

    for (const auto& h : options.hosts) {
        add_san(h);
        if (h == "localhost") has_localhost = true;
        if (h == "127.0.0.1") has_loopback4 = true;
        if (h == "::1") has_loopback6 = true;
    }

    // Always ensure localhost / loopback SANs are present for local access
    if (!has_localhost) add_san("localhost");
    if (!has_loopback4) add_san("127.0.0.1");
    if (!has_loopback6) add_san("::1");

    // Extensions
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, const_cast<X509*>(ca_cert), cert.get(), nullptr, nullptr, 0);

    if (auto res = add_extension(cert.get(), &ctx, NID_basic_constraints, "critical,CA:FALSE"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_key_usage, "critical,digitalSignature"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_ext_key_usage, "serverAuth"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_subject_key_identifier, "hash"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_authority_key_identifier, "keyid,issuer"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_subject_alt_name, san_str.c_str()); !res) return std::unexpected(res.error());

    return finalize_cert(std::move(server_key), std::move(cert), ca_key);
}

std::expected<GeneratedCertificate, std::string> generate_client_cert(
    const ClientCertOptions& options,
    const EVP_PKEY* ca_key,
    const X509* ca_cert
) {
    if (!ca_key || !ca_cert) {
        return std::unexpected("CA key or certificate is null");
    }

    auto key_res = generate_ed25519_key();
    if (!key_res) {
        return std::unexpected(key_res.error());
    }
    UniquePKey client_key = std::move(*key_res);

    UniqueX509 cert(X509_new());
    if (!cert) {
        return std::unexpected(std::format("Failed to create X509: {}", get_openssl_error()));
    }

    if (X509_set_version(cert.get(), 2) != 1) {
        return std::unexpected(std::format("Failed to set X509 version: {}", get_openssl_error()));
    }

    if (auto res = set_random_serial(cert.get()); !res) {
        return std::unexpected(res.error());
    }

    // Validity
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), static_cast<long>(options.validity_days) * 86400L);

    // Subject
    X509_NAME* name = X509_get_subject_name(cert.get());
    if (!name) {
        return std::unexpected("Failed to get subject name from cert");
    }
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(options.common_name.c_str()), -1, -1, 0) != 1) {
        return std::unexpected(std::format("Failed to set CN: {}", get_openssl_error()));
    }
    if (!options.organization.empty()) {
        if (X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(options.organization.c_str()), -1, -1, 0) != 1) {
            return std::unexpected(std::format("Failed to set O: {}", get_openssl_error()));
        }
    }

    // Issuer from CA
    X509_NAME* ca_name = X509_get_subject_name(ca_cert);
    if (!ca_name) {
        return std::unexpected("Failed to get subject name from CA cert");
    }
    if (X509_set_issuer_name(cert.get(), ca_name) != 1) {
        return std::unexpected(std::format("Failed to set issuer name: {}", get_openssl_error()));
    }

    if (X509_set_pubkey(cert.get(), client_key.get()) != 1) {
        return std::unexpected(std::format("Failed to set pubkey: {}", get_openssl_error()));
    }

    // Build SAN URI list
    std::string san_str;
    auto add_uri = [&](const std::string& uri) {
        if (uri.empty()) return;
        std::string entry = uri.starts_with("URI:") ? uri : ("URI:" + uri);
        if (!san_str.empty()) {
            san_str += ",";
        }
        san_str += entry;
    };

    if (!options.user_name.empty()) {
        add_uri("tmux2://user/" + options.user_name);
    }
    for (const auto& uri : options.custom_san_uris) {
        add_uri(uri);
    }
    if (san_str.empty()) {
        // Fallback default SAN
        add_uri("tmux2://user/default");
    }

    // Extensions
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, const_cast<X509*>(ca_cert), cert.get(), nullptr, nullptr, 0);

    if (auto res = add_extension(cert.get(), &ctx, NID_basic_constraints, "critical,CA:FALSE"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_key_usage, "critical,digitalSignature"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_ext_key_usage, "clientAuth"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_subject_key_identifier, "hash"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_authority_key_identifier, "keyid,issuer"); !res) return std::unexpected(res.error());
    if (auto res = add_extension(cert.get(), &ctx, NID_subject_alt_name, san_str.c_str()); !res) return std::unexpected(res.error());

    return finalize_cert(std::move(client_key), std::move(cert), ca_key);
}

} // namespace tmux2::pki
