#include "tls/tls_context.h"
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <iostream>
#include <stdexcept>

namespace lb::tls {

TlsContext::TlsContext() : ctx_(nullptr) {
    // OpenSSL initialization will be done in initialize()
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

bool TlsContext::initialize() {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                         nullptr) != 1) {
        std::cerr << "TLS: Failed to initialize OpenSSL library" << std::endl;
        return false;
    }
#else
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif

    const SSL_METHOD* method = TLS_server_method();
    if (!method) {
        std::cerr << "TLS: Failed to get TLS server method" << std::endl;
        return false;
    }

    ctx_ = SSL_CTX_new(method);
    if (!ctx_) {
        std::cerr << "TLS: Failed to create SSL context" << std::endl;
        return false;
    }

    if (SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION) != 1) {
        std::cerr << "TLS: Failed to set minimum protocol version" << std::endl;
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return false;
    }

    SSL_CTX_set_options(ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    SSL_CTX_set_options(ctx_, SSL_OP_CIPHER_SERVER_PREFERENCE);

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    SSL_CTX_set_options(ctx_, SSL_OP_NO_RENEGOTIATION);
#endif

    return true;
}

bool TlsContext::load_certificate(const std::string& cert_path, const std::string& key_path) {
    if (!ctx_) {
        std::cerr << "TLS: SSL context not initialized. Call initialize() first." << std::endl;
        return false;
    }

    if (SSL_CTX_use_certificate_file(ctx_, cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        std::cerr << "TLS: Failed to load certificate from " << cert_path << ": " << err_buf
                  << std::endl;
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx_, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        std::cerr << "TLS: Failed to load private key from " << key_path << ": " << err_buf
                  << std::endl;
        return false;
    }

    if (SSL_CTX_check_private_key(ctx_) != 1) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        std::cerr << "TLS: Private key does not match certificate: " << err_buf << std::endl;
        return false;
    }

    return true;
}

SSL* TlsContext::create_ssl(int fd) {
    if (!ctx_) {
        std::cerr << "TLS: SSL context not initialized" << std::endl;
        return nullptr;
    }

    SSL* ssl = SSL_new(ctx_);
    if (!ssl) {
        std::cerr << "TLS: Failed to create SSL object" << std::endl;
        return nullptr;
    }

    if (SSL_set_fd(ssl, fd) != 1) {
        std::cerr << "TLS: Failed to set file descriptor for SSL object" << std::endl;
        SSL_free(ssl);
        return nullptr;
    }

    SSL_set_accept_state(ssl);

    return ssl;
}

void TlsContext::destroy_ssl(SSL* ssl) {
    if (ssl) {
        SSL_free(ssl);
    }
}

} // namespace lb::tls
