#pragma once

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

} // namespace hitsc
