#pragma once

#include <memory>
#include <string>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/bn.h>

namespace tmux2::pki {

template <typename T, void (*Deleter)(T*)>
struct SslDeleter {
    void operator()(T* p) const noexcept {
        if (p) Deleter(p);
    }
};

using UniquePKey = std::unique_ptr<EVP_PKEY, SslDeleter<EVP_PKEY, EVP_PKEY_free>>;
using UniquePKeyCtx = std::unique_ptr<EVP_PKEY_CTX, SslDeleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using UniqueX509 = std::unique_ptr<X509, SslDeleter<X509, X509_free>>;
using UniqueX509Name = std::unique_ptr<X509_NAME, SslDeleter<X509_NAME, X509_NAME_free>>;
using UniqueX509Extension = std::unique_ptr<X509_EXTENSION, SslDeleter<X509_EXTENSION, X509_EXTENSION_free>>;
using UniqueX509Store = std::unique_ptr<X509_STORE, SslDeleter<X509_STORE, X509_STORE_free>>;
using UniqueX509StoreCtx = std::unique_ptr<X509_STORE_CTX, SslDeleter<X509_STORE_CTX, X509_STORE_CTX_free>>;
using UniqueBio = std::unique_ptr<BIO, SslDeleter<BIO, BIO_free_all>>;
using UniqueBignum = std::unique_ptr<BIGNUM, SslDeleter<BIGNUM, BN_free>>;
using UniqueAsn1Integer = std::unique_ptr<ASN1_INTEGER, SslDeleter<ASN1_INTEGER, ASN1_INTEGER_free>>;
using UniqueAsn1Time = std::unique_ptr<ASN1_TIME, SslDeleter<ASN1_TIME, ASN1_TIME_free>>;
using UniqueGeneralNames = std::unique_ptr<GENERAL_NAMES, SslDeleter<GENERAL_NAMES, GENERAL_NAMES_free>>;

inline std::string get_openssl_error() {
    char buf[512];
    unsigned long err = ERR_get_error();
    if (err == 0) {
        return "Unknown OpenSSL error";
    }
    ERR_error_string_n(err, buf, sizeof(buf));
    std::string result = buf;
    while ((err = ERR_get_error()) != 0) {
        ERR_error_string_n(err, buf, sizeof(buf));
        result += "; ";
        result += buf;
    }
    return result;
}

} // namespace tmux2::pki
