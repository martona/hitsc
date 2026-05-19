#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <memory>

namespace hitsc {

struct SslSessionDeleter {
    void operator()(SSL_SESSION* session) const noexcept;
};

using SslSessionPtr = std::unique_ptr<SSL_SESSION, SslSessionDeleter>;

class TlsSessionCache {
public:
    explicit TlsSessionCache(std::size_t max_size = 16);
    ~TlsSessionCache();

    TlsSessionCache(TlsSessionCache&&) noexcept;
    TlsSessionCache& operator=(TlsSessionCache&&) noexcept;
    TlsSessionCache(const TlsSessionCache&) = delete;
    TlsSessionCache& operator=(const TlsSessionCache&) = delete;

    bool push(SSL_SESSION* session);
    SslSessionPtr pop();
    std::size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hitsc
