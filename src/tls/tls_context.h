#pragma once

#include <memory>
#include <string>

#include <openssl/ssl.h>

namespace lb::tls {

class TlsContext {
public:
    TlsContext();
    ~TlsContext();

    bool initialize();

    bool load_certificate(const std::string& cert_path, const std::string& key_path);

    SSL* create_ssl(int fd);

    static void destroy_ssl(SSL* ssl);

    SSL_CTX* ctx() {
        return ctx_;
    }

    [[nodiscard]] bool is_initialized() const {
        return ctx_ != nullptr;
    }

private:
    SSL_CTX* ctx_;
};

} // namespace lb::tls
