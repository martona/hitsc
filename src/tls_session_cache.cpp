#include "tls_session_cache.hpp"

#include <openssl/ssl.h>

#include <ctime>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace hitsc {
namespace {

std::time_t session_expires_at(const SSL_SESSION* session)
{
    const std::time_t created_at = SSL_SESSION_get_time_ex(session);
    const long timeout = SSL_SESSION_get_timeout(session);
    if (created_at <= 0 || timeout <= 0) {
        return 0;
    }

    const auto max_time = std::numeric_limits<std::time_t>::max();
    const auto lifetime = static_cast<std::time_t>(timeout);
    if (created_at > max_time - lifetime) {
        return max_time;
    }

    return created_at + lifetime;
}

bool is_session_valid(const SSL_SESSION* session, std::time_t now)
{
    if (session == nullptr) {
        return false;
    }

    return now < session_expires_at(session);
}

} // namespace

void SslSessionDeleter::operator()(SSL_SESSION* session) const noexcept
{
    SSL_SESSION_free(session);
}

struct TlsSessionCache::Impl {
    explicit Impl(std::size_t max_size)
        : max_size(max_size)
    {
    }

    void prune_expired_locked(std::time_t now)
    {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!is_session_valid(it->get(), now)) {
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t max_size = 16;
    mutable std::mutex mutex;
    std::deque<SslSessionPtr> sessions;
};

TlsSessionCache::TlsSessionCache(std::size_t max_size)
    : impl_(std::make_unique<Impl>(max_size))
{
}

TlsSessionCache::~TlsSessionCache() = default;

TlsSessionCache::TlsSessionCache(TlsSessionCache&&) noexcept = default;

TlsSessionCache& TlsSessionCache::operator=(TlsSessionCache&&) noexcept = default;

bool TlsSessionCache::push(SSL_SESSION* session)
{
    const std::time_t now = std::time(nullptr);
    if (impl_->max_size == 0 || !is_session_valid(session, now)) {
        return false;
    }

    SslSessionPtr owned_session(session);

    std::lock_guard lock(impl_->mutex);
    impl_->prune_expired_locked(now);
    impl_->sessions.push_back(std::move(owned_session));
    while (impl_->sessions.size() > impl_->max_size) {
        impl_->sessions.pop_front();
    }

    return true;
}

SslSessionPtr TlsSessionCache::pop()
{
    const std::time_t now = std::time(nullptr);

    std::lock_guard lock(impl_->mutex);
    impl_->prune_expired_locked(now);
    if (impl_->sessions.empty()) {
        return {};
    }

    SslSessionPtr session = std::move(impl_->sessions.front());
    impl_->sessions.pop_front();
    return session;
}

std::size_t TlsSessionCache::size() const
{
    const std::time_t now = std::time(nullptr);

    std::lock_guard lock(impl_->mutex);
    impl_->prune_expired_locked(now);
    return impl_->sessions.size();
}

} // namespace hitsc
