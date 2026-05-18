#include "pikvm_events.hpp"

#include "app_info.hpp"
#include "log.hpp"
#include "pikvm_input.hpp"
#include "text.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

namespace {

std::string buffer_text(const beast::flat_buffer& buffer)
{
    std::string text(boost::asio::buffer_size(buffer.data()), '\0');
    boost::asio::buffer_copy(boost::asio::buffer(text), buffer.data());
    return text;
}

std::string printable_preview(std::string_view value, std::size_t limit = 120)
{
    std::string preview;
    const std::size_t clipped = std::min(value.size(), limit);
    for (std::size_t i = 0; i < clipped; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch == '\n') {
            preview += "\\n";
        } else if (ch == '\r') {
            preview += "\\r";
        } else if (ch == '\t') {
            preview += "\\t";
        } else if (ch == '"') {
            preview += "\\\"";
        } else if (ch == '\\') {
            preview += "\\\\";
        } else if (std::isprint(ch)) {
            preview.push_back(static_cast<char>(ch));
        } else {
            preview.push_back('.');
        }
    }
    if (value.size() > clipped) {
        preview += "...";
    }
    return preview;
}

std::string json_event_type(std::string_view text)
{
    boost::system::error_code error;
    json::value value = json::parse(text, error);
    if (error) {
        return {};
    }

    const json::object* object = value.if_object();
    if (object == nullptr) {
        return {};
    }

    const json::value* event_type = object->if_contains("event_type");
    if (event_type == nullptr) {
        return {};
    }

    const json::string* event_type_string = event_type->if_string();
    if (event_type_string == nullptr) {
        return {};
    }

    return std::string(*event_type_string);
}

struct QueuedPikvmWrite {
    std::vector<std::uint8_t> packet;
};

} // namespace

class PikvmEventSession : public std::enable_shared_from_this<PikvmEventSession> {
public:
    PikvmEventSession(
        std::shared_ptr<PikvmWebSocket> ws,
        PikvmViewOptions options,
        const std::atomic_bool& stop_requested,
        std::function<void(std::exception_ptr)> on_error)
        : ws_(std::move(ws))
        , options_(std::move(options))
        , stop_requested_(stop_requested)
        , on_error_(std::move(on_error))
        , force_close_timer_(ws_->get_executor())
    {
    }

    void start()
    {
        auto self = shared_from_this();
        asio::dispatch(ws_->get_executor(), [self] {
            self->start_read();
        });
    }

    void enqueue_input(std::vector<std::uint8_t> packet, bool coalesce_mouse_motion)
    {
        auto self = shared_from_this();
        asio::post(
            ws_->get_executor(),
            [self, packet = std::move(packet), coalesce_mouse_motion]() mutable {
                self->do_enqueue_input(std::move(packet), coalesce_mouse_motion);
            });
    }

    void request_stop(std::chrono::milliseconds grace_period)
    {
        auto self = shared_from_this();
        asio::post(ws_->get_executor(), [self, grace_period] {
            if (self->closed_) {
                return;
            }
            self->stopping_ = true;
            if (!self->write_in_progress_ && !self->write_queue_.empty()) {
                self->start_write();
            }
            if (self->write_in_progress_ || !self->write_queue_.empty()) {
                self->arm_force_close_timer(grace_period);
                return;
            }
            self->force_close_now();
        });
    }

private:
    void start_read()
    {
        if (closed_) {
            return;
        }

        auto self = shared_from_this();
        ws_->async_read(
            read_buffer_,
            [self](beast::error_code error, std::size_t bytes_transferred) {
                self->on_read(error, bytes_transferred);
            });
    }

    void on_read(beast::error_code error, std::size_t bytes_transferred)
    {
        if (closed_) {
            return;
        }

        if (error) {
            const bool expected_stop =
                stop_requested_.load()
                || stopping_
                || error == websocket::error::closed
                || error == asio::error::operation_aborted
                || error == asio::error::bad_descriptor;
            if (!expected_stop) {
                log_error() << "pikvm event websocket read error: " << error.message();
                on_error_(std::make_exception_ptr(
                    beast::system_error(error, "PiKVM event websocket read failed")));
            }
            force_close_now();
            return;
        }

        ++messages_seen_;
        if (options_.login.verbose && (messages_seen_ <= 8 || messages_seen_ % 120 == 0)) {
            LogLine line = log_info();
            line << "pikvm event websocket message"
                 << " bytes=" << bytes_transferred;
            if (ws_->got_text()) {
                const std::string text = buffer_text(read_buffer_);
                const std::string event_type = json_event_type(text);
                if (!event_type.empty()) {
                    line << " event_type=" << event_type;
                }
                line << " preview=\"" << printable_preview(text) << "\"";
            } else {
                line << " binary=yes";
            }
        }

        read_buffer_.consume(read_buffer_.size());
        if (!stopping_) {
            start_read();
        }
    }

    void do_enqueue_input(std::vector<std::uint8_t> packet, bool coalesce_mouse_motion)
    {
        if (closed_ || stopping_ || packet.empty()) {
            return;
        }

        if (coalesce_mouse_motion
            && !write_queue_.empty()
            && is_pikvm_mouse_move_packet(write_queue_.back().packet)) {
            write_queue_.back().packet = std::move(packet);
        } else {
            write_queue_.push_back(QueuedPikvmWrite{std::move(packet)});
        }

        if (!write_in_progress_) {
            start_write();
        }
    }

    void start_write()
    {
        if (closed_ || write_queue_.empty()) {
            write_in_progress_ = false;
            if (stopping_) {
                force_close_now();
            }
            return;
        }

        write_in_progress_ = true;
        ws_->binary(true);
        auto self = shared_from_this();
        ws_->async_write(
            boost::asio::buffer(write_queue_.front().packet),
            [self](beast::error_code error, std::size_t bytes_transferred) {
                self->on_write(error, bytes_transferred);
            });
    }

    void on_write(beast::error_code error, std::size_t bytes_transferred)
    {
        (void)bytes_transferred;
        if (closed_) {
            return;
        }

        if (error) {
            const bool expected_stop =
                stop_requested_.load()
                || stopping_
                || error == websocket::error::closed
                || error == asio::error::operation_aborted
                || error == asio::error::bad_descriptor;
            if (!expected_stop) {
                log_error() << "pikvm event websocket write error: " << error.message();
                on_error_(std::make_exception_ptr(
                    beast::system_error(error, "PiKVM event websocket write failed")));
            }
            force_close_now();
            return;
        }

        if (!write_queue_.empty()) {
            write_queue_.pop_front();
        }

        if (!write_queue_.empty()) {
            start_write();
            return;
        }

        write_in_progress_ = false;
        if (stopping_) {
            force_close_now();
        }
    }

    void arm_force_close_timer(std::chrono::milliseconds grace_period)
    {
        force_close_timer_.expires_after(grace_period);
        auto self = shared_from_this();
        force_close_timer_.async_wait([self](beast::error_code error) {
            if (!error) {
                self->force_close_now();
            }
        });
    }

    void force_close_now()
    {
        if (closed_) {
            return;
        }

        closed_ = true;
        force_close_timer_.cancel();
        force_close_pikvm_websocket(*ws_);
    }

    std::shared_ptr<PikvmWebSocket> ws_;
    PikvmViewOptions options_;
    const std::atomic_bool& stop_requested_;
    std::function<void(std::exception_ptr)> on_error_;
    asio::steady_timer force_close_timer_;
    beast::flat_buffer read_buffer_;
    std::deque<QueuedPikvmWrite> write_queue_;
    std::uint64_t messages_seen_ = 0;
    bool write_in_progress_ = false;
    bool stopping_ = false;
    bool closed_ = false;
};

std::shared_ptr<PikvmWebSocket> connect_pikvm_websocket(
    asio::io_context& io,
    ssl::context& tls_context,
    const LoginOptions& options,
    const CookieJar& cookies,
    int idle_timeout_seconds,
    std::string_view path)
{
    auto ws = std::make_shared<PikvmWebSocket>(io, tls_context);
    tcp::resolver resolver(io);

    configure_tls(tls_context, ws->next_layer(), options.base_url.host, options.insecure);
    set_server_name_indication(ws->next_layer(), options.base_url.host);

    const auto endpoints = resolver.resolve(options.base_url.host, options.base_url.port);
    beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*ws).connect(endpoints);
    beast::get_lowest_layer(*ws).socket().set_option(tcp::no_delay(true));
    ws->next_layer().handshake(ssl::stream_base::client);
    beast::get_lowest_layer(*ws).expires_never();

    websocket::stream_base::timeout timeout;
    timeout.handshake_timeout = std::chrono::seconds(30);
    if (idle_timeout_seconds > 0) {
        timeout.idle_timeout = std::chrono::seconds(idle_timeout_seconds);
        timeout.keep_alive_pings = true;
    } else {
        timeout.idle_timeout = websocket::stream_base::none();
        timeout.keep_alive_pings = false;
    }
    ws->set_option(timeout);

    const std::string host = make_host_header(options.base_url);
    ws->set_option(websocket::stream_base::decorator([&](websocket::request_type& request) {
        request.set(http::field::user_agent, std::string(kName) + "/" + std::string(BOOST_LIB_VERSION));
        request.set(http::field::origin, make_origin(options.base_url));

        const std::string cookie_header = cookies.header();
        if (!cookie_header.empty()) {
            request.set(http::field::cookie, cookie_header);
        }
    }));

    websocket::response_type response;
    ws->handshake(response, host, std::string(path));
    log_info() << "pikvm websocket connected"
               << " path=" << path
               << " idle-timeout=" << (idle_timeout_seconds > 0 ? std::to_string(idle_timeout_seconds) + "s" : "disabled");
    return ws;
}

void force_close_pikvm_websocket(PikvmWebSocket& ws)
{
    beast::error_code error;
    beast::get_lowest_layer(ws).socket().cancel(error);
    error.clear();
    beast::get_lowest_layer(ws).socket().shutdown(tcp::socket::shutdown_both, error);
    error.clear();
    beast::get_lowest_layer(ws).socket().close(error);
}

std::shared_ptr<PikvmEventSession> start_pikvm_event_session(
    std::shared_ptr<PikvmWebSocket> ws,
    PikvmViewOptions options,
    const std::atomic_bool& stop_requested,
    std::function<void(std::exception_ptr)> on_error)
{
    auto session = std::make_shared<PikvmEventSession>(
        std::move(ws),
        std::move(options),
        stop_requested,
        std::move(on_error));
    session->start();
    return session;
}

void queue_pikvm_event_input(
    const std::shared_ptr<PikvmEventSession>& session,
    std::vector<std::uint8_t> packet,
    bool coalesce_mouse_motion)
{
    if (session) {
        session->enqueue_input(std::move(packet), coalesce_mouse_motion);
    }
}

void stop_pikvm_event_session(
    const std::shared_ptr<PikvmEventSession>& session,
    std::chrono::milliseconds grace_period)
{
    if (session) {
        session->request_stop(grace_period);
    }
}

} // namespace hitsc
