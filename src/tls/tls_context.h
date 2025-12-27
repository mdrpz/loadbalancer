#pragma once

#include <string>
#include <memory>

// Include OpenSSL headers for SSL types
// Using full includes since we need SSL_CTX* and SSL* pointer types
#include <openssl/ssl.h>

namespace lb::tls {

class TlsContext {
public:
    TlsContext();
    ~TlsContext();

    bool load_certificate(const std::string& cert_path, const std::string& key_path);
    SSL* create_ssl(int fd);
    void destroy_ssl(SSL* ssl);

    SSL_CTX* ctx() { return ctx_; }

private:
    SSL_CTX* ctx_;
};

} // namespace lb::tls

