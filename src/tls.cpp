#include "tls.hpp"

#include "cert_trust.hpp"
#include "log.hpp"

#include <boost/beast/core/error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;

namespace {

int tls_session_cache_ex_data_index()
{
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int cache_new_tls_session(SSL* ssl_handle, SSL_SESSION* session)
{
    const int index = tls_session_cache_ex_data_index();
    if (index < 0) {
        return 0;
    }

    auto* cache = static_cast<TlsSessionCache*>(SSL_get_ex_data(ssl_handle, index));
    if (cache == nullptr) {
        return 0;
    }

    return cache->push(session) ? 1 : 0;
}

} // namespace

void configure_tls(
    ssl::context& context,
    beast::ssl_stream<beast::tcp_stream>& stream,
    const std::string& host,
    bool insecure)
{
    if (insecure) {
        stream.set_verify_mode(ssl::verify_none);
        return;
    }

    context.set_default_verify_paths();
#ifdef _WIN32
    if (SSL_CTX_load_verify_store(context.native_handle(), "org.openssl.winstore:") != 1) {
        const auto ssl_error = static_cast<int>(::ERR_get_error());
        throw beast::system_error(
            beast::error_code(ssl_error, asio::error::get_ssl_category()),
            "failed to load Windows certificate store");
    }
#endif
    stream.set_verify_mode(ssl::verify_peer);

    // Walk the chain with the system trust store, but make the final accept/
    // reject decision ourselves at the leaf: an OS-trusted cert connects
    // silently, a pinned one connects silently, anything else is handed to the
    // trust broker (which may prompt and pin). Accumulate whether every depth
    // preverified, plus the first failure code, so the leaf has the full
    // picture. The callback runs on the network thread; blocking it on a modal
    // prompt is exactly the pause we want.
    auto chain_state = std::make_shared<std::pair<bool, long>>(true, static_cast<long>(X509_V_OK));
    const std::string verify_host = host;
    stream.set_verify_callback(
        [chain_state, verify_host](bool preverified, ssl::verify_context& ctx) -> bool {
            X509_STORE_CTX* store_ctx = ctx.native_handle();
            if (!preverified && chain_state->first) {
                chain_state->first = false;
                chain_state->second = X509_STORE_CTX_get_error(store_ctx);
            }
            if (X509_STORE_CTX_get_error_depth(store_ctx) != 0) {
                return true; // keep walking down to the leaf
            }
            X509* leaf = X509_STORE_CTX_get_current_cert(store_ctx);
            return cert_trust_evaluate(
                verify_host, leaf, chain_state->first, chain_state->second);
        });
}

void set_server_name_indication(beast::ssl_stream<beast::tcp_stream>& stream, const std::string& host)
{
    if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) != 1) {
        const auto ssl_error = static_cast<int>(::ERR_get_error());
        throw beast::system_error(
            beast::error_code(ssl_error, asio::error::get_ssl_category()),
            "failed to set TLS server name indication");
    }
}

void configure_tls_session_cache(ssl::context& context, TlsSessionCache& cache)
{
    (void)cache;

    const int index = tls_session_cache_ex_data_index();
    if (index < 0) {
        throw std::runtime_error("failed to allocate TLS session cache context index");
    }

    SSL_CTX_set_session_cache_mode(
        context.native_handle(),
        SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(context.native_handle(), &cache_new_tls_session);
}

void prepare_tls_session_resumption(
    beast::ssl_stream<beast::tcp_stream>& stream,
    TlsSessionCache& cache,
    bool verbose)
{
    SSL* ssl_handle = stream.native_handle();
    if (SSL_set_ex_data(ssl_handle, tls_session_cache_ex_data_index(), &cache) != 1) {
        throw std::runtime_error("failed to attach TLS session cache to connection");
    }

    SslSessionPtr session = cache.pop();
    if (session == nullptr) {
        return;
    }

    if (SSL_set_session(ssl_handle, session.get()) != 1) {
        ERR_clear_error();
        if (verbose) {
            log_debug() << "tls session resumption ticket rejected before handshake";
        }
        return;
    }

    if (verbose) {
        log_debug() << "tls session resumption ticket offered";
    }
}

void log_tls_session_handshake_result(
    beast::ssl_stream<beast::tcp_stream>& stream,
    const TlsSessionCache& cache,
    bool verbose)
{
    if (!verbose) {
        return;
    }

    log_debug() << "tls session handshake"
                << " reused=" << (SSL_session_reused(stream.native_handle()) == 1 ? "yes" : "no")
                << " cached-sessions=" << cache.size();
}

} // namespace hitsc
