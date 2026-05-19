#pragma once

#include "tls_session_cache.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>

#include <string>

namespace hitsc {

void configure_tls(
    boost::asio::ssl::context& context,
    boost::beast::ssl_stream<boost::beast::tcp_stream>& stream,
    const std::string& host,
    bool insecure);

void set_server_name_indication(
    boost::beast::ssl_stream<boost::beast::tcp_stream>& stream,
    const std::string& host);

void configure_tls_session_cache(
    boost::asio::ssl::context& context,
    TlsSessionCache& cache);

void prepare_tls_session_resumption(
    boost::beast::ssl_stream<boost::beast::tcp_stream>& stream,
    TlsSessionCache& cache,
    bool verbose);

void log_tls_session_handshake_result(
    boost::beast::ssl_stream<boost::beast::tcp_stream>& stream,
    const TlsSessionCache& cache,
    bool verbose);

} // namespace hitsc
