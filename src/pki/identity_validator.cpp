#include "identity_validator.hpp"
#include <format>
#include <openssl/x509v3.h>
#include <arpa/inet.h>

namespace tmux2::pki {

std::optional<std::string> ClientIdentity::extract_username() const {
    // 1. Check tmux2://user/<username>
    for (const auto& uri : san_uris) {
        constexpr std::string_view kPrefix = "tmux2://user/";
        if (uri.starts_with(kPrefix)) {
            std::string user = uri.substr(kPrefix.size());
            if (!user.empty()) {
                return user;
            }
        }
    }

    // 2. Check spiffe://<trust-domain>/user/<username>
    for (const auto& uri : san_uris) {
        constexpr std::string_view kSpiffe = "spiffe://";
        if (uri.starts_with(kSpiffe)) {
            auto pos = uri.rfind("/user/");
            if (pos != std::string::npos) {
                std::string user = uri.substr(pos + 6);
                if (!user.empty()) {
                    return user;
                }
            }
        }
    }

    // 3. Fallback to CN
    if (!common_name.empty()) {
        return common_name;
    }

    // 4. Fallback to email
    if (!san_emails.empty()) {
        auto at = san_emails[0].find('@');
        if (at != std::string::npos && at > 0) {
            return san_emails[0].substr(0, at);
        }
        return san_emails[0];
    }

    return std::nullopt;
}

std::expected<ClientIdentity, std::string> extract_client_identity(const X509* cert) {
    if (!cert) {
        return std::unexpected("cert is null");
    }

    ClientIdentity identity;

    // Common Name (CN)
    char cn_buf[256];
    int cn_len = X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn_buf, sizeof(cn_buf));
    if (cn_len > 0) {
        identity.common_name = std::string(cn_buf, static_cast<size_t>(cn_len));
    }

    // Subject Alternative Names (SAN)
    UniqueGeneralNames gens(static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr)
    ));

    if (gens) {
        int count = sk_GENERAL_NAME_num(gens.get());
        for (int i = 0; i < count; ++i) {
            const GENERAL_NAME* gen = sk_GENERAL_NAME_value(gens.get(), i);
            if (!gen) continue;

            if (gen->type == GEN_URI) {
                ASN1_STRING* s = gen->d.uniformResourceIdentifier;
                if (s) {
                    const char* data = reinterpret_cast<const char*>(ASN1_STRING_get0_data(s));
                    int len = ASN1_STRING_length(s);
                    if (data && len > 0) {
                        identity.san_uris.emplace_back(data, static_cast<size_t>(len));
                    }
                }
            } else if (gen->type == GEN_DNS) {
                ASN1_STRING* s = gen->d.dNSName;
                if (s) {
                    const char* data = reinterpret_cast<const char*>(ASN1_STRING_get0_data(s));
                    int len = ASN1_STRING_length(s);
                    if (data && len > 0) {
                        identity.san_dns.emplace_back(data, static_cast<size_t>(len));
                    }
                }
            } else if (gen->type == GEN_EMAIL) {
                ASN1_STRING* s = gen->d.rfc822Name;
                if (s) {
                    const char* data = reinterpret_cast<const char*>(ASN1_STRING_get0_data(s));
                    int len = ASN1_STRING_length(s);
                    if (data && len > 0) {
                        identity.san_emails.emplace_back(data, static_cast<size_t>(len));
                    }
                }
            }
        }
    }

    // Fingerprints
    auto fp = compute_cert_fingerprint(cert);
    if (fp) {
        identity.cert_fingerprint = std::move(*fp);
    }
    auto spki = compute_spki_fingerprint(cert);
    if (spki) {
        identity.spki_fingerprint = std::move(*spki);
    }

    return identity;
}

std::expected<void, std::string> verify_certificate_chain(
    const X509* cert,
    const std::vector<const X509*>& trusted_cas,
    const std::vector<const X509*>& untrusted_intermediates
) {
    if (!cert) {
        return std::unexpected("Certificate to verify is null");
    }
    if (trusted_cas.empty()) {
        return std::unexpected("No trusted CA certificates provided");
    }

    UniqueX509Store store(X509_STORE_new());
    if (!store) {
        return std::unexpected(std::format("Failed to create X509_STORE: {}", get_openssl_error()));
    }

    for (const auto* ca : trusted_cas) {
        if (!ca) continue;
        if (X509_STORE_add_cert(store.get(), const_cast<X509*>(ca)) != 1) {
            // Note: ignore X509_R_CERT_ALREADY_IN_HASH_TABLE if duplicate
            unsigned long err = ERR_peek_last_error();
            if (ERR_GET_REASON(err) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
                return std::unexpected(std::format("Failed to add CA cert to store: {}", get_openssl_error()));
            }
            ERR_clear_error();
        }
    }

    UniqueX509StoreCtx ctx(X509_STORE_CTX_new());
    if (!ctx) {
        return std::unexpected(std::format("Failed to create X509_STORE_CTX: {}", get_openssl_error()));
    }

    // Set untrusted intermediate chain if present
    STACK_OF(X509)* untrusted_stack = nullptr;
    if (!untrusted_intermediates.empty()) {
        untrusted_stack = sk_X509_new_null();
        for (const auto* inter : untrusted_intermediates) {
            sk_X509_push(untrusted_stack, const_cast<X509*>(inter));
        }
    }

    if (X509_STORE_CTX_init(ctx.get(), store.get(), const_cast<X509*>(cert), untrusted_stack) != 1) {
        if (untrusted_stack) sk_X509_free(untrusted_stack);
        return std::unexpected(std::format("Failed to init X509_STORE_CTX: {}", get_openssl_error()));
    }

    int verify_result = X509_verify_cert(ctx.get());
    if (untrusted_stack) {
        sk_X509_free(untrusted_stack);
    }

    if (verify_result != 1) {
        int err_code = X509_STORE_CTX_get_error(ctx.get());
        const char* err_str = X509_verify_cert_error_string(err_code);
        return std::unexpected(std::format("Certificate verification failed (code {}): {}", err_code, err_str));
    }

    return {};
}

bool verify_server_hostname(const X509* cert, std::string_view expected_host) {
    if (!cert || expected_host.empty()) {
        return false;
    }

    std::string host_str(expected_host);

    // Check if expected_host is IP address
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, host_str.c_str(), &a4) == 1 || inet_pton(AF_INET6, host_str.c_str(), &a6) == 1) {
        return X509_check_ip_asc(const_cast<X509*>(cert), host_str.c_str(), 0) == 1;
    }

    // DNS check
    return X509_check_host(
        const_cast<X509*>(cert),
        host_str.data(),
        host_str.size(),
        0,
        nullptr
    ) == 1;
}

} // namespace tmux2::pki
