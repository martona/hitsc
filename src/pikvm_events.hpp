#pragma once

#include "cookie_jar.hpp"
#include "options.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace hitsc {

using PikvmWebSocket = boost::beast::websocket::stream<
    boost::beast::ssl_stream<boost::beast::tcp_stream>>;

std::shared_ptr<PikvmWebSocket> connect_pikvm_websocket(
    boost::asio::io_context& io,
    boost::asio::ssl::context& tls_context,
    const LoginOptions& options,
    const CookieJar& cookies,
    int idle_timeout_seconds,
    std::string_view path);

void force_close_pikvm_websocket(PikvmWebSocket& ws);

class PikvmEventSession;

struct PikvmInputTiming {
    std::chrono::steady_clock::time_point ui_event_at;
    std::chrono::steady_clock::time_point enqueued_at;
};

struct PikvmInputWork {
    std::vector<std::uint8_t> packet;
    bool coalesce_mouse_motion = false;
    PikvmInputTiming timing;
};

std::shared_ptr<PikvmEventSession> start_pikvm_event_session(
    std::shared_ptr<PikvmWebSocket> ws,
    PikvmViewOptions options,
    const std::atomic_bool& stop_requested,
    std::function<void(bool)> on_display_status,
    std::function<void(std::exception_ptr)> on_error);

std::function<void(PikvmInputWork)> make_pikvm_event_input_sink(
    const std::shared_ptr<PikvmEventSession>& session);

void stop_pikvm_event_session(
    const std::shared_ptr<PikvmEventSession>& session,
    std::chrono::milliseconds grace_period);

} // namespace hitsc
