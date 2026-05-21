#include "pikvm_events.hpp"

#include "errors.hpp"
#include "log.hpp"
#include "pikvm_input.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace json = boost::json;
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

struct PikvmJsonEventSummary {
    std::string event_type;
    std::optional<bool> streamer_source_online;
};

PikvmJsonEventSummary parse_json_event(std::string_view text)
{
    PikvmJsonEventSummary result;
    boost::system::error_code error;
    json::value value = json::parse(text, error);
    if (error) {
        return result;
    }

    try {
        const json::object& object = value.as_object();
        result.event_type = std::string(object.at("event_type").as_string());
        if (result.event_type == "streamer") {
            result.streamer_source_online = object.at("event").as_object()
                .at("streamer").as_object()
                .at("source").as_object()
                .at("online").as_bool();
        }
    } catch (const std::exception&) {
    }

    return result;
}

} // namespace

class PikvmEventSession : public std::enable_shared_from_this<PikvmEventSession> {
public:
    PikvmEventSession(
        std::shared_ptr<PikvmWebSocket> ws,
        PikvmViewOptions options,
        const std::atomic_bool& stop_requested,
        std::function<void(bool)> on_display_status,
        std::function<void(std::exception_ptr)> on_error)
        : ws_(std::move(ws))
        , options_(std::move(options))
        , stop_requested_(stop_requested)
        , on_display_status_(std::move(on_display_status))
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

    void enqueue_input(PikvmInputWork work)
    {
        auto self = shared_from_this();
        asio::post(
            ws_->get_executor(),
            [self, work = std::move(work)]() mutable {
                self->do_enqueue_input(std::move(work));
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
                on_error_(std::make_exception_ptr(
                    UserError("PiKVM event websocket read failed: " + error.message())));
            }
            force_close_now();
            return;
        }

        ++messages_seen_;
        std::optional<PikvmJsonEventSummary> json_summary;
        if (ws_->got_text()) {
            const std::string text = buffer_text(read_buffer_);
            json_summary = parse_json_event(text);
            if (json_summary->streamer_source_online && on_display_status_) {
                on_display_status_(*json_summary->streamer_source_online);
            }
        }

        if (options_.login.vverbose && (messages_seen_ <= 8 || messages_seen_ % 120 == 0)) {
            LogLine line = log_info();
            line << "pikvm event websocket message"
                 << " bytes=" << bytes_transferred;
            if (ws_->got_text()) {
                const std::string text = buffer_text(read_buffer_);
                if (json_summary && !json_summary->event_type.empty()) {
                    line << " event_type=" << json_summary->event_type;
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

    void do_enqueue_input(PikvmInputWork work)
    {
        if (closed_ || stopping_ || work.packet.empty()) {
            return;
        }

        write_queue_.push_back(std::move(work));

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
                on_error_(std::make_exception_ptr(
                    UserError("PiKVM event websocket write failed: " + error.message())));
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
    std::function<void(bool)> on_display_status_;
    std::function<void(std::exception_ptr)> on_error_;
    asio::steady_timer force_close_timer_;
    beast::flat_buffer read_buffer_;
    std::deque<PikvmInputWork> write_queue_;
    std::uint64_t messages_seen_ = 0;
    bool write_in_progress_ = false;
    bool stopping_ = false;
    bool closed_ = false;
};

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
    std::function<void(bool)> on_display_status,
    std::function<void(std::exception_ptr)> on_error)
{
    auto session = std::make_shared<PikvmEventSession>(
        std::move(ws),
        std::move(options),
        stop_requested,
        std::move(on_display_status),
        std::move(on_error));
    session->start();
    return session;
}

std::function<void(PikvmInputWork)> make_pikvm_event_input_sink(
    const std::shared_ptr<PikvmEventSession>& session)
{
    // The UI may still queue input while the control worker is shutting down.
    // Capturing a shared_ptr here would keep the session alive past its websocket/io_context.
    // A weak sink makes those late packets harmless no-ops instead.
    std::weak_ptr<PikvmEventSession> weak_session = session;
    return [weak_session](PikvmInputWork work) {
        if (std::shared_ptr<PikvmEventSession> session = weak_session.lock()) {
            session->enqueue_input(std::move(work));
        }
    };
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
