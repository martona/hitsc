#include "tls.hpp"

#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core/error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;

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
    stream.set_verify_callback(ssl::host_name_verification(host));
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

} // namespace hitsc
