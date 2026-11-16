#include "crypto_utils.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace tmux2::pki {

namespace {

std::string bytes_to_hex(const unsigned char* data, size_t len, bool colon_format) {
    std::string result;
    result.reserve(colon_format ? len * 3 : len * 2);
    for (size_t i = 0; i < len; ++i) {
        if (colon_format && i > 0) {
            result += ':';
        }
        result += std::format("{:02X}", data[i]);
    }
    return result;
}

} // namespace

std::expected<UniquePKey, std::string> generate_ed25519_key() {
    EVP_PKEY* pkey = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    if (!pkey) {
        return std::unexpected(std::format("Failed to generate Ed25519 key: {}", get_openssl_error()));
    }
    return UniquePKey(pkey);
}

std::expected<UniquePKey, std::string> generate_p256_key() {
    EVP_PKEY* pkey = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    if (!pkey) {
        return std::unexpected(std::format("Failed to generate P-256 key: {}", get_openssl_error()));
    }
    return UniquePKey(pkey);
}

std::expected<std::string, std::string> pkey_to_pem_private(const EVP_PKEY* pkey) {
    if (!pkey) {
        return std::unexpected("pkey is null");
    }
    UniqueBio bio(BIO_new(BIO_s_mem()));
    if (!bio) {
        return std::unexpected(std::format("Failed to allocate BIO: {}", get_openssl_error()));
    }
    if (PEM_write_bio_PKCS8PrivateKey(bio.get(), const_cast<EVP_PKEY*>(pkey), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        return std::unexpected(std::format("Failed to write private key to PEM: {}", get_openssl_error()));
    }
    char* data = nullptr;
    long len = BIO_get_mem_data(bio.get(), &data);
    if (len <= 0 || !data) {
        return std::unexpected("Failed to extract PEM data from BIO");
    }
    return std::string(data, static_cast<size_t>(len));
}

std::expected<std::string, std::string> pkey_to_pem_public(const EVP_PKEY* pkey) {
    if (!pkey) {
        return std::unexpected("pkey is null");
    }
    UniqueBio bio(BIO_new(BIO_s_mem()));
    if (!bio) {
        return std::unexpected(std::format("Failed to allocate BIO: {}", get_openssl_error()));
    }
    if (PEM_write_bio_PUBKEY(bio.get(), const_cast<EVP_PKEY*>(pkey)) != 1) {
        return std::unexpected(std::format("Failed to write public key to PEM: {}", get_openssl_error()));
    }
    char* data = nullptr;
    long len = BIO_get_mem_data(bio.get(), &data);
    if (len <= 0 || !data) {
        return std::unexpected("Failed to extract PEM data from BIO");
    }
    return std::string(data, static_cast<size_t>(len));
}

std::expected<UniquePKey, std::string> pem_to_private_key(std::string_view pem) {
    if (pem.empty()) {
        return std::unexpected("PEM data is empty");
    }
    UniqueBio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        return std::unexpected(std::format("Failed to create memory BIO: {}", get_openssl_error()));
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
    if (!pkey) {
        return std::unexpected(std::format("Failed to parse private key PEM: {}", get_openssl_error()));
    }
    return UniquePKey(pkey);
}

std::expected<UniquePKey, std::string> pem_to_public_key(std::string_view pem) {
    if (pem.empty()) {
        return std::unexpected("PEM data is empty");
    }
    UniqueBio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        return std::unexpected(std::format("Failed to create memory BIO: {}", get_openssl_error()));
    }
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
    if (!pkey) {
        return std::unexpected(std::format("Failed to parse public key PEM: {}", get_openssl_error()));
    }
    return UniquePKey(pkey);
}

std::expected<std::string, std::string> x509_to_pem(const X509* cert) {
    if (!cert) {
        return std::unexpected("cert is null");
    }
    UniqueBio bio(BIO_new(BIO_s_mem()));
    if (!bio) {
        return std::unexpected(std::format("Failed to allocate BIO: {}", get_openssl_error()));
    }
    if (PEM_write_bio_X509(bio.get(), const_cast<X509*>(cert)) != 1) {
        return std::unexpected(std::format("Failed to write X509 to PEM: {}", get_openssl_error()));
    }
    char* data = nullptr;
    long len = BIO_get_mem_data(bio.get(), &data);
    if (len <= 0 || !data) {
        return std::unexpected("Failed to extract PEM data from BIO");
    }
    return std::string(data, static_cast<size_t>(len));
}

std::expected<UniqueX509, std::string> pem_to_x509(std::string_view pem) {
    if (pem.empty()) {
        return std::unexpected("PEM data is empty");
    }
    UniqueBio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        return std::unexpected(std::format("Failed to create memory BIO: {}", get_openssl_error()));
    }
    X509* cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
    if (!cert) {
        return std::unexpected(std::format("Failed to parse X509 PEM: {}", get_openssl_error()));
    }
    return UniqueX509(cert);
}

std::expected<std::vector<UniqueX509>, std::string> pem_bundle_to_x509s(std::string_view pem_bundle) {
    if (pem_bundle.empty()) {
        return std::unexpected("PEM bundle is empty");
    }
    UniqueBio bio(BIO_new_mem_buf(pem_bundle.data(), static_cast<int>(pem_bundle.size())));
    if (!bio) {
        return std::unexpected(std::format("Failed to create memory BIO: {}", get_openssl_error()));
    }
    std::vector<UniqueX509> certs;
    while (true) {
        X509* cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
        if (!cert) {
            // Check if end of file
            unsigned long err = ERR_peek_last_error();
            if (ERR_GET_REASON(err) == PEM_R_NO_START_LINE) {
                ERR_clear_error();
                break;
            }
            if (certs.empty()) {
                return std::unexpected(std::format("Failed to read any certificates from bundle: {}", get_openssl_error()));
            }
            // Clears any remaining errors if at least one cert was parsed
            ERR_clear_error();
            break;
        }
        certs.emplace_back(cert);
    }
    if (certs.empty()) {
        return std::unexpected("No valid X509 certificates found in bundle");
    }
    return certs;
}

std::expected<std::string, std::string> compute_cert_fingerprint(const X509* cert, bool colon_format) {
    if (!cert) {
        return std::unexpected("cert is null");
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (X509_digest(cert, EVP_sha256(), md, &len) != 1) {
        return std::unexpected(std::format("Failed to compute cert SHA-256 digest: {}", get_openssl_error()));
    }
    return bytes_to_hex(md, len, colon_format);
}

std::expected<std::string, std::string> compute_spki_fingerprint(const X509* cert, bool colon_format) {
    if (!cert) {
        return std::unexpected("cert is null");
    }
    X509_PUBKEY* pubkey = X509_get_X509_PUBKEY(cert);
    if (!pubkey) {
        return std::unexpected("Failed to get X509_PUBKEY from cert");
    }
    int len = i2d_X509_PUBKEY(pubkey, nullptr);
    if (len <= 0) {
        return std::unexpected("Failed to get SPKI DER length");
    }
    std::vector<unsigned char> der(static_cast<size_t>(len));
    unsigned char* p = der.data();
    if (i2d_X509_PUBKEY(pubkey, &p) <= 0) {
        return std::unexpected("Failed to encode SPKI to DER");
    }

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (EVP_Digest(der.data(), der.size(), md, &md_len, EVP_sha256(), nullptr) != 1) {
        return std::unexpected(std::format("Failed to compute SPKI SHA-256 digest: {}", get_openssl_error()));
    }
    return bytes_to_hex(md, md_len, colon_format);
}

std::expected<std::string, std::string> compute_pkey_spki_fingerprint(const EVP_PKEY* pkey, bool colon_format) {
    if (!pkey) {
        return std::unexpected("pkey is null");
    }
    int len = i2d_PUBKEY(const_cast<EVP_PKEY*>(pkey), nullptr);
    if (len <= 0) {
        return std::unexpected("Failed to get PUBKEY DER length");
    }
    std::vector<unsigned char> der(static_cast<size_t>(len));
    unsigned char* p = der.data();
    if (i2d_PUBKEY(const_cast<EVP_PKEY*>(pkey), &p) <= 0) {
        return std::unexpected("Failed to encode PUBKEY to DER");
    }

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (EVP_Digest(der.data(), der.size(), md, &md_len, EVP_sha256(), nullptr) != 1) {
        return std::unexpected(std::format("Failed to compute PKEY SPKI SHA-256 digest: {}", get_openssl_error()));
    }
    return bytes_to_hex(md, md_len, colon_format);
}

std::expected<void, std::string> write_file_secure(const std::filesystem::path& path, std::string_view content, mode_t mode) {
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        if (!std::filesystem::create_directories(parent, ec)) {
            return std::unexpected(std::format("Failed to create directory {}: {}", parent.string(), ec.message()));
        }
        ::chmod(parent.c_str(), 0700);
    }

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        return std::unexpected(std::format("Failed to open file {} for writing: errno {}", path.string(), errno));
    }

    ::fchmod(fd, mode);

    const char* ptr = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written < 0) {
            int err = errno;
            ::close(fd);
            return std::unexpected(std::format("Failed to write to file {}: errno {}", path.string(), err));
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }

    if (::close(fd) != 0) {
        return std::unexpected(std::format("Failed to close file {}: errno {}", path.string(), errno));
    }

    return {};
}

std::expected<std::string, std::string> read_file_to_string(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("Failed to open file for reading: {}", path.string()));
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // namespace tmux2::pki
