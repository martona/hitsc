#include "aten_network.hpp"

#include "aten_session.hpp"
#include "log.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

std::atomic_bool g_aten_full_framebuffer_refresh_requested{true};

namespace {

using AtenWebSocket = BmcWebSocketStream;

constexpr std::chrono::milliseconds kAtenFramebufferRequestBackoff{33};
// When the host is powered off the BMC streams the same ~20KB 800x600 "I'm off"
// JPEG forever (see handle_ast_rect). Once we've confirmed that state, slow our
// pull-rate from ~30fps to this, cutting the wasted receive + TLS + bandwidth ~6x.
// Reset to the fast rate the instant a non-duplicate frame arrives.
constexpr std::chrono::milliseconds kAtenStaticFramebufferInterval{200};
// A duplicate only counts toward that backoff if it's at least this big -- i.e. a
// full-screen image, not a sub-KB all-SKIP "nothing changed" delta from a live idle
// host. The powered-off image is ~20KB; idle deltas are well under 1KB, so 10KB is a
// wide, safe divider that keeps an idle-but-ALIVE host at full responsiveness.
constexpr std::size_t kAtenStaticFrameMinBytes = 10 * 1024;
// Consecutive large identical frames before we engage the backoff (~165ms of
// confirmation at the fast rate). Resets on any non-duplicate / small / no-op frame.
constexpr int kAtenStaticBackoffAfterFrames = 5;
constexpr std::size_t kMaxAtenMessagesPerReceiveDrain = 8;

struct AtenQueuedWrite {
    std::vector<std::uint8_t> packet;
};

std::uint16_t load_cursor_pattern_word(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    bool big_endian)
{
    if (big_endian) {
        return load_be16(bytes, offset);
    }
    if (offset + 2 > bytes.size()) {
        throw std::runtime_error("truncated little-endian cursor pattern word");
    }
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

int clamped_cursor_coordinate(std::uint32_t value)
{
    return static_cast<int>(std::min<std::uint32_t>(
        value,
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
}

std::vector<std::uint16_t> decode_aten_cursor_pattern(
    const std::vector<std::uint8_t>& bytes,
    bool big_endian)
{
    std::vector<std::uint16_t> pattern(bytes.size() / 2U);
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        pattern[index] = load_cursor_pattern_word(bytes, index * 2U, big_endian);
    }
    return pattern;
}

HardwareCursor make_aten_hardware_cursor(
    const AtenCursorPositionMessage& message,
    const std::vector<std::uint16_t>& cached_pattern,
    int cached_pattern_width,
    int cached_pattern_height,
    int source_width,
    int source_height,
    bool big_endian)
{
    HardwareCursor cursor;
    cursor.type = clamped_cursor_coordinate(message.pattern_type);
    cursor.x = clamped_cursor_coordinate(message.x);
    cursor.y = clamped_cursor_coordinate(message.y);
    cursor.x_offset = 0;
    cursor.y_offset = 0;

    if (!message.pattern.empty()) {
        cursor.pattern = decode_aten_cursor_pattern(message.pattern, big_endian);
        cursor.pattern_width = clamped_cursor_coordinate(message.width);
        cursor.pattern_height = clamped_cursor_coordinate(message.height);
        cursor.pattern_from_packet = true;
    } else if (!cached_pattern.empty()) {
        cursor.pattern = cached_pattern;
        cursor.pattern_width = cached_pattern_width;
        cursor.pattern_height = cached_pattern_height;
    } else {
        cursor.pattern_width = clamped_cursor_coordinate(message.width);
        cursor.pattern_height = clamped_cursor_coordinate(message.height);
    }

    cursor.has_pattern = !cursor.pattern.empty();
    if (source_width <= 0) {
        source_width = cursor.pattern_width;
    }
    if (source_height <= 0) {
        source_height = cursor.pattern_height;
    }
    if (source_width <= 0 || source_height <= 0 ||
        cursor.x >= source_width || cursor.y >= source_height) {
        cursor.visible = false;
        return cursor;
    }

    const int pattern_width = cursor.pattern_width > 0 ? cursor.pattern_width : 15;
    const int pattern_height = cursor.pattern_height > 0 ? cursor.pattern_height : 15;
    cursor.width = std::min(source_width - cursor.x, pattern_width);
    cursor.height = std::min(source_height - cursor.y, pattern_height);
    cursor.visible = cursor.width > 0 && cursor.height > 0;
    return cursor;
}

std::vector<std::uint8_t> buffer_bytes(const beast::flat_buffer& buffer)
{
    std::vector<std::uint8_t> bytes(boost::asio::buffer_size(buffer.data()));
    boost::asio::buffer_copy(boost::asio::buffer(bytes), buffer.data());
    return bytes;
}

void append_buffer_bytes(std::vector<std::uint8_t>& destination, const beast::flat_buffer& source)
{
    const std::size_t size = boost::asio::buffer_size(source.data());
    if (size == 0) {
        return;
    }

    const std::size_t offset = destination.size();
    destination.resize(offset + size);
    boost::asio::buffer_copy(boost::asio::buffer(destination.data() + offset, size), source.data());
}

std::string ascii_from_bytes(const std::vector<std::uint8_t>& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

class RfbHandshakeStream {
public:
    explicit RfbHandshakeStream(AtenWebSocket& ws)
        : ws_(ws)
    {
    }

    std::vector<std::uint8_t> read_exact(std::size_t count)
    {
        while (buffered_bytes() < count) {
            read_message();
        }

        std::vector<std::uint8_t> bytes(
            buffer_.begin() + static_cast<std::ptrdiff_t>(offset_),
            buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += count;
        compact_if_needed();
        return bytes;
    }

    std::uint8_t read_u8()
    {
        return read_exact(1).front();
    }

    std::uint16_t read_be16()
    {
        const std::vector<std::uint8_t> bytes = read_exact(2);
        return load_be16(bytes, 0);
    }

    std::uint32_t read_be32()
    {
        const std::vector<std::uint8_t> bytes = read_exact(4);
        return load_be32(bytes, 0);
    }

    std::string read_string(std::size_t count)
    {
        return ascii_from_bytes(read_exact(count));
    }

    void write(const std::vector<std::uint8_t>& bytes)
    {
        ws_.binary(true);
        ws_.write(asio::buffer(bytes));
    }

    void write_string(std::string_view text)
    {
        ws_.binary(true);
        ws_.write(asio::buffer(text.data(), text.size()));
    }

    std::vector<std::uint8_t> take_queued_bytes()
    {
        std::vector<std::uint8_t> queued(
            buffer_.begin() + static_cast<std::ptrdiff_t>(offset_),
            buffer_.end());
        buffer_.clear();
        offset_ = 0;
        return queued;
    }

private:
    std::size_t buffered_bytes() const
    {
        return offset_ <= buffer_.size() ? buffer_.size() - offset_ : 0;
    }

    void read_message()
    {
        beast::flat_buffer buffer;
        ws_.read(buffer);
        append_buffer_bytes(buffer_, buffer);
    }

    void compact_if_needed()
    {
        if (offset_ == 0) {
            return;
        }
        if (offset_ == buffer_.size()) {
            buffer_.clear();
            offset_ = 0;
            return;
        }
        if (offset_ >= 4096 && offset_ * 2 >= buffer_.size()) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
            offset_ = 0;
        }
    }

    AtenWebSocket& ws_;
    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
};

std::uint8_t negotiate_security(RfbHandshakeStream& rfb, std::string_view client_version)
{
    if (!version_uses_security_types(client_version)) {
        const std::uint32_t auth_scheme = rfb.read_be32();
        if (auth_scheme > 255) {
            throw std::runtime_error("ATEN RFB 3.3 security scheme is too large");
        }
        log_info() << "aten rfb security scheme=" << auth_scheme
                   << " " << security_type_name(static_cast<std::uint8_t>(auth_scheme));
        return static_cast<std::uint8_t>(auth_scheme);
    }

    const std::uint8_t count = rfb.read_u8();
    if (count == 0) {
        const std::uint32_t reason_length = rfb.read_be32();
        const std::string reason = rfb.read_string(reason_length);
        throw std::runtime_error("ATEN RFB security failure: " + reason);
    }

    const std::vector<std::uint8_t> types = rfb.read_exact(count);
    std::uint8_t selected = 0;
    for (const std::uint8_t type : types) {
        if (type > selected && is_supported_js_security_type(type)) {
            selected = type;
        }
    }
    if (selected == 0) {
        throw std::runtime_error("ATEN RFB offered no supported security type");
    }

    {
        LogLine line = log_info();
        line << "aten rfb security types=";
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (i != 0) {
                line << ',';
            }
            line << static_cast<int>(types[i]) << "(" << security_type_name(types[i]) << ")";
        }
        line << " selected=" << static_cast<int>(selected)
             << "(" << security_type_name(selected) << ")";
    }

    rfb.write(std::vector<std::uint8_t>{selected});
    return selected;
}

void authenticate_insyde(RfbHandshakeStream& rfb, std::string_view credential, bool verbose)
{
    const std::vector<std::uint8_t> challenge = rfb.read_exact(24);
    if (verbose) {
        log_info() << "aten rfb insyde challenge bytes=" << challenge.size()
                   << " first-bytes=" << hex_preview(challenge);
    }

    std::vector<std::uint8_t> reply(49, 0);
    const std::size_t copy_size = std::min<std::size_t>(24, credential.size());
    for (std::size_t i = 0; i < copy_size; ++i) {
        reply[i] = static_cast<std::uint8_t>(credential[i]);
    }
    reply[48] = 0;
    rfb.write(reply);

    if (verbose) {
        log_info() << "sent ATEN/Insyde username auth reply"
                   << " credential-bytes=" << copy_size
                   << " client-type=0";
    }
}

void authenticate(RfbHandshakeStream& rfb, std::uint8_t security_type, std::string_view credential, bool verbose)
{
    switch (security_type) {
    case 1:
        return;
    case 15:
    case 16:
        authenticate_insyde(rfb, credential, verbose);
        return;
    case 2:
        throw std::runtime_error("ATEN RFB VNCAuth/DES is not implemented yet");
    case 14:
        throw std::runtime_error("ATEN RFB Tight auth is not implemented yet");
    case 22:
        throw std::runtime_error("ATEN RFB XVP auth is not implemented yet");
    default:
        throw std::runtime_error("unsupported ATEN RFB security type: " + std::to_string(security_type));
    }
}

void read_security_result(RfbHandshakeStream& rfb, bool verbose)
{
    const std::uint32_t result = rfb.read_be32();
    if (result == 0) {
        if (verbose) {
            log_info() << "aten rfb authentication succeeded";
        }
        return;
    }

    if (result == 1) {
        const std::uint32_t reason_length = rfb.read_be32();
        const std::string reason = rfb.read_string(reason_length);
        throw std::runtime_error("ATEN RFB authentication failed: " + reason);
    }
    if (result == 2) {
        throw std::runtime_error("ATEN RFB authentication failed: too many attempts");
    }
    throw std::runtime_error("ATEN RFB authentication failed with result " + std::to_string(result));
}

AtenRfbServerInit read_server_init(RfbHandshakeStream& rfb, bool insyde_extension)
{
    AtenRfbServerInit init;
    init.width = rfb.read_be16();
    init.height = rfb.read_be16();
    init.bits_per_pixel = rfb.read_u8();
    init.depth = rfb.read_u8();
    init.big_endian = rfb.read_u8() != 0;
    init.true_color = rfb.read_u8() != 0;
    init.red_max = rfb.read_be16();
    init.green_max = rfb.read_be16();
    init.blue_max = rfb.read_be16();
    init.red_shift = rfb.read_u8();
    init.green_shift = rfb.read_u8();
    init.blue_shift = rfb.read_u8();
    rfb.read_exact(3);
    const std::uint32_t name_length = rfb.read_be32();
    init.name = rfb.read_string(name_length);

    if (insyde_extension) {
        init.insyde_extension = true;
        rfb.read_exact(4);
        init.session_id = rfb.read_be32();
        init.video_enable = rfb.read_u8();
        init.keyboard_mouse_enable = rfb.read_u8();
        init.kick_user_enable = rfb.read_u8();
        init.virtual_media_enable = rfb.read_u8();
    }

    return init;
}

class AtenNetworkRfbSession : public std::enable_shared_from_this<AtenNetworkRfbSession> {
public:
    AtenNetworkRfbSession(
        asio::io_context& io,
        AtenWebSocket& ws,
        AtenViewState& state,
        AtenViewOptions options,
        AtenRfbServerInit init,
        std::atomic_bool& stop_requested)
        : io_(io)
        , ws_(ws)
        , state_(state)
        , options_(std::move(options))
        , init_(std::move(init))
        , stop_requested_(stop_requested)
        , strand_(asio::make_strand(io))
        , framebuffer_request_timer_(io)
    {
    }

    void start(std::vector<std::uint8_t> initial_bytes)
    {
        auto weak = weak_from_this();
        state_.input.install(
            [weak](std::vector<std::uint8_t> packet) mutable {
                if (auto self = weak.lock()) {
                    self->send_packet(std::move(packet));
                }
            });

        auto self = shared_from_this();
        asio::dispatch(strand_, [self, initial_bytes = std::move(initial_bytes)]() mutable {
            if (self->closed_ || self->stop_requested_.load()) {
                return;
            }
            if (!initial_bytes.empty()) {
                self->parser_.append(std::move(initial_bytes));
            }
            self->queue_framebuffer_update_request();
            if (self->options_.login.vverbose) {
                log_info() << "requested ATEN framebuffer update";
            }
            self->parse_receive_buffer();
            self->start_read();
        });
    }

    void send_packet(std::vector<std::uint8_t> packet)
    {
        auto self = shared_from_this();
        asio::post(strand_, [self, packet = std::move(packet)]() mutable {
            if (self->closed_ || self->stop_requested_.load()) {
                return;
            }
            self->queue_write(std::move(packet));
        });
    }

    void request_stop()
    {
        if (options_.login.verbose) {
            log_debug() << "aten websocket stop requested";
        }
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            self->stop_requested_.store(true);
            self->close_now();
        });
    }

private:
    static bool is_hot_write_packet(const std::vector<std::uint8_t>& packet)
    {
        if (packet.empty()) {
            return false;
        }
        return packet.front() == 3 || packet.front() == 4 || packet.front() == 5;
    }

    void start_read()
    {
        if (closed_) {
            return;
        }

        auto self = shared_from_this();
        ws_.async_read(
            read_buffer_,
            asio::bind_executor(strand_, [self](beast::error_code error, std::size_t bytes_transferred) {
                self->on_read(error, bytes_transferred);
            }));
    }

    void on_read(beast::error_code error, std::size_t bytes_transferred)
    {
        if (closed_) {
            return;
        }
        if (error) {
            handle_read_error(error);
            return;
        }

        state_.view_status.data_received(bytes_transferred);
        std::vector<std::uint8_t> bytes = buffer_bytes(read_buffer_);
        read_buffer_.consume(read_buffer_.size());
        ++total_websocket_messages_;
        stats_rx_bytes_ += bytes.size();
        last_websocket_message_bytes_ = bytes.size();

        if (options_.login.vverbose &&
            (total_websocket_messages_ <= 5 || total_websocket_messages_ % 120 == 0)) {
            LogLine line = log_info();
            line << "aten websocket message"
                 << " bytes=" << bytes.size();
            if (!bytes.empty()) {
                line << " first-bytes=" << hex_preview(bytes);
            }
        }

        parser_.append(std::move(bytes));
        parse_receive_buffer();
        start_read();
    }

    void handle_read_error(beast::error_code error)
    {
        const bool expected_stop =
            stop_requested_.load()
            || error == websocket::error::closed
            || error == asio::error::operation_aborted
            || error == asio::error::bad_descriptor
            || error == ssl::error::stream_truncated;
        if (!expected_stop) {
            state_.set_exception(std::make_exception_ptr(
                beast::system_error(error, "ATEN websocket read failed")));
            log_error() << "aten websocket read callback error=" << error.message();
        } else if (options_.login.verbose) {
            log_debug() << "aten websocket read stopped error=" << error.message();
        }
        close_now();
    }

    void parse_receive_buffer()
    {
        if (closed_ || receive_parse_posted_) {
            return;
        }
        receive_parse_posted_ = true;
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            self->receive_parse_posted_ = false;
            self->do_parse_receive_buffer();
        });
    }

    void do_parse_receive_buffer()
    {
        if (closed_ || stop_requested_.load()) {
            return;
        }

        try {
            std::size_t parsed_messages = 0;
            while (!closed_ && !stop_requested_.load()) {
                std::optional<AtenRfbMessage> message = parser_.next();
                if (!message) {
                    break;
                }
                handle_message(*message);
                ++parsed_messages;
                if (parsed_messages >= kMaxAtenMessagesPerReceiveDrain) {
                    parse_receive_buffer();
                    break;
                }
            }
            log_stats_if_due();
        } catch (...) {
            state_.set_exception(std::current_exception());
            close_now();
        }
    }

    void handle_message(const AtenRfbMessage& message)
    {
        ++total_messages_;
        ++stats_messages_;
        if (options_.login.vverbose && message.type != 0) {
            log_info() << "aten rfb message #" << total_messages_
                       << " type=" << static_cast<int>(message.type)
                       << " " << aten_server_message_name(message.type)
                       << " buffered=" << parser_.buffered_bytes();
        }

        switch (message.kind) {
        case AtenRfbMessageKind::framebuffer_update:
            handle_framebuffer_update(message);
            break;
        case AtenRfbMessageKind::bell:
            if (options_.login.vverbose) {
                log_info() << "aten rfb bell";
            }
            break;
        case AtenRfbMessageKind::cursor_position:
            handle_cursor_position(message.cursor);
            break;
        case AtenRfbMessageKind::message22:
            if (options_.login.vverbose) {
                log_info() << "aten rfb message 22 value=" << static_cast<int>(message.value);
            }
            break;
        case AtenRfbMessageKind::mouse_control:
            if (options_.login.vverbose) {
                log_info() << "aten rfb mouse/control"
                           << " type=" << static_cast<int>(message.type)
                           << " crypto=" << static_cast<int>(message.mouse_crypto)
                           << " mode=" << static_cast<int>(message.mouse_mode)
                           << " status=" << static_cast<int>(message.mouse_status);
            }
            break;
        case AtenRfbMessageKind::control_message:
            if (options_.login.vverbose) {
                log_info() << "aten rfb control-message"
                           << " count=" << message.control_count
                           << " code-digits=" << message.control_code_digits
                           << " first-bytes=" << hex_preview(message.control_message);
            }
            break;
        case AtenRfbMessageKind::service:
            if (options_.login.vverbose) {
                log_info() << "aten rfb service"
                           << " type=" << static_cast<int>(message.type)
                           << " value=" << static_cast<int>(message.value);
            }
            break;
        }
    }

    void handle_framebuffer_update(const AtenRfbMessage& message)
    {
        if (!init_.insyde_extension) {
            throw std::runtime_error("standard RFB framebuffer updates are not implemented yet");
        }

        ++updates_;
        ++stats_framebuffer_updates_;
        if (options_.login.vverbose && (updates_ <= 5 || updates_ % 120 == 0)) {
            log_info() << "aten rfb framebuffer update #" << updates_
                       << " rects=" << message.rects.size();
        }

        for (std::size_t i = 0; i < message.rects.size(); ++i) {
            const AtenFramebufferUpdateRect& update_rect = message.rects[i];
            const AtenFramebufferRect& rect = update_rect.rect;

            if (options_.login.vverbose && (updates_ <= 5 || updates_ % 120 == 0)) {
                LogLine line = log_info();
                line << "aten rfb rect"
                     << " index=" << (i + 1)
                     << " xy=" << rect.x << ',' << rect.y
                     << " size=" << rect.width << 'x' << rect.height
                     << " encoding=" << rect.encoding << '(' << encoding_name(rect.encoding) << ')'
                     << " mode=" << rect.mode
                     << " payload=" << rect.data_length;
                if (!update_rect.payload.empty()) {
                    line << " first-bytes=" << hex_preview(update_rect.payload);
                }
            }

            if (rect.encoding != 87 || update_rect.payload.empty()) {
                if (rect.encoding == 87 && update_rect.payload.empty()) {
                    handle_blank_screen_packet(rect);
                }
                continue;
            }

            handle_ast_rect(rect, update_rect.payload);
        }

        framebuffer_request_pending_ = false;
        schedule_framebuffer_update_request();
    }

    void handle_ast_rect(const AtenFramebufferRect& rect, const std::vector<std::uint8_t>& payload)
    {
        const AtenAstPayloadHeader ast = read_ast_payload_header(payload);
        if (ast_payload_is_frame_end_only(payload)) {
            previous_width_ = rect.width;
            previous_height_ = rect.height;
            static_frame_count_ = 0; // a real no-change frame from a live host: stay responsive
            if (options_.login.vverbose && (updates_ <= 20 || updates_ % 60 == 0)) {
                log_info() << "skipped ATEN no-op frame #" << updates_
                           << " size=" << rect.width << 'x' << rect.height
                           << " payload=" << payload.size()
                           << " mode=" << ast.mode;
            }
            return;
        }

        if (payload.size() <= 4) {
            return;
        }

        // Supermicro/ATEN powered-off "video", a.k.a. why this dedup exists.
        // When the host is OFF, the BMC does NOT send a no-signal packet (its own RFB
        // layer supports exactly that). Instead it BROADCASTS a full-frame 800x600
        // branded "system is powered off" JPEG and re-sends it ~30 times a second,
        // full frames, NOT delta-compressed, indefinitely, to announce that nothing
        // is happening. Their own H5 viewer "handles" this by brute force: it pegs
        // SIX cores to decode a static image at 12 fps. We manage it on one core at
        // ~30 fps, which is less humiliating but still ridiculous.
        //
        // Fix: if the compressed payload is byte-for-byte identical to the previous
        // frame, drop it here -- before it's queued or decoded. The size compare +
        // memcmp costs a few microseconds; the JPEG decode it avoids costs ~a milli-
        // second. Net on a dead host: ~zero CPU, which is what announcing "I'm off"
        // should have cost before SMC decided a 30fps video stream was the way to do
        // it. We compare payload[4:] (past the AST frame-tag word) so a changing tag
        // can't defeat it. Thanks, Supermicro.
        const std::uint8_t* compressed_data = payload.data() + 4;
        const std::size_t compressed_size = payload.size() - 4;
        if (compressed_size == previous_compressed_.size() &&
            std::memcmp(compressed_data, previous_compressed_.data(), compressed_size) == 0) {
            state_.view_status.kvm_display_status(true); // still showing the identical image
            previous_width_ = rect.width;
            previous_height_ = rect.height;
            // Only a LARGE identical repeat (the ~20KB powered-off image) counts toward
            // the request-rate backoff; a tiny all-SKIP "nothing changed" delta from a
            // live idle host does not, so an idle-but-ALIVE screen keeps full latency.
            if (compressed_size >= kAtenStaticFrameMinBytes) {
                if (static_frame_count_ < kAtenStaticBackoffAfterFrames) {
                    ++static_frame_count_;
                }
            } else {
                static_frame_count_ = 0;
            }
            if (options_.login.vverbose && (updates_ <= 20 || updates_ % 120 == 0)) {
                log_info() << "skipped duplicate ATEN frame #" << updates_
                           << " size=" << rect.width << 'x' << rect.height
                           << " compressed=" << compressed_size
                           << " static-streak=" << static_frame_count_;
            }
            return;
        }
        previous_compressed_.assign(compressed_data, compressed_data + compressed_size);
        static_frame_count_ = 0; // a real (different) frame -- resume full-rate polling

        blank_screen_packets_ = 0;
        AspeedCompressedFrame frame;
        frame.width = rect.width;
        frame.height = rect.height;
        frame.decode_options = make_aten_aspeed_decode_options(rect.width, rect.height, ast);
        frame.compressed.assign(payload.begin() + 4, payload.end());
        frame.received_at = std::chrono::steady_clock::now();
        frame.websocket_bytes = last_websocket_message_bytes_;
        frame.published_at = std::chrono::steady_clock::now();
        state_.frames.publish(std::move(frame));
        state_.view_status.kvm_display_status(true);

        previous_width_ = rect.width;
        previous_height_ = rect.height;

        if (options_.login.vverbose && (updates_ <= 20 || updates_ % 60 == 0)) {
            log_info() << "queued ATEN frame #" << updates_
                       << " size=" << rect.width << 'x' << rect.height
                       << " compressed=" << (payload.size() - 4)
                       << " mode=" << ast.mode
                       << " y-sel=" << ast.y_selector
                       << " uv-sel=" << ast.uv_selector;
        }
    }

    void handle_blank_screen_packet(const AtenFramebufferRect& rect)
    {
        ++blank_screen_packets_;
        state_.view_status.kvm_display_status(false);
        g_aten_full_framebuffer_refresh_requested.store(true);
        previous_compressed_.clear(); // never dedup across a blank: force the next real frame to decode
        static_frame_count_ = 0;      // and resume full-rate polling

        if (rect.width > 0) {
            previous_width_ = rect.width;
        }
        if (rect.height > 0) {
            previous_height_ = rect.height;
        }

        if (blank_screen_packets_ == 1) {
            log_warning() << "remote requested ATEN blank screen"
                          << " size=" << rect.width << 'x' << rect.height
                          << " mode=" << rect.mode;
        }
    }

    void handle_cursor_position(const AtenCursorPositionMessage& cursor)
    {
        cursor_position_request_pending_ = false;
        HardwareCursor hardware_cursor = make_aten_hardware_cursor(
            cursor,
            cursor_pattern_,
            cursor_pattern_width_,
            cursor_pattern_height_,
            previous_width_ > 0 ? previous_width_ : init_.width,
            previous_height_ > 0 ? previous_height_ : init_.height,
            init_.big_endian);
        if (hardware_cursor.pattern_from_packet) {
            cursor_pattern_ = hardware_cursor.pattern;
            cursor_pattern_width_ = hardware_cursor.pattern_width;
            cursor_pattern_height_ = hardware_cursor.pattern_height;
        }
        // Log before publishing (reads hardware_cursor in place) so we don't deep-
        // copy the whole struct incl. its pattern on every packet for a line that is
        // off unless vverbose.
        if (options_.login.vverbose) {
            LogLine line = log_info();
            line << "ATEN cursor"
                 << " xy=" << cursor.x << ',' << cursor.y
                 << " size=" << cursor.width << 'x' << cursor.height
                 << " valid=" << cursor.valid
                 << " render-visible=" << hardware_cursor.visible
                 << " render-size=" << hardware_cursor.width << 'x' << hardware_cursor.height;
            if (cursor.valid == 1) {
                line << " pattern-type=" << cursor.pattern_type
                     << " pattern-bytes=" << cursor.pattern_size;
            }
        }

        state_.cursors.publish(std::move(hardware_cursor));
    }

    void log_stats_if_due()
    {
        if (!options_.login.vverbose) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - stats_started_at_ < std::chrono::seconds(1)) {
            return;
        }

        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - stats_started_at_).count();
        log_info() << "aten rfb stats"
                   << " elapsed-ms=" << elapsed_ms
                   << " rx-bytes=" << stats_rx_bytes_
                   << " messages=" << stats_messages_
                   << " framebuffer-updates=" << stats_framebuffer_updates_
                   << " receive-buffer=" << parser_.buffered_bytes();

        stats_started_at_ = now;
        stats_rx_bytes_ = 0;
        stats_messages_ = 0;
        stats_framebuffer_updates_ = 0;
    }

    void schedule_framebuffer_update_request()
    {
        if (closed_ || stop_requested_.load() || framebuffer_request_pending_ ||
            framebuffer_request_timer_active_) {
            return;
        }

        framebuffer_request_timer_active_ = true;
        auto self = shared_from_this();
        // Wind the pull-rate down to kAtenStaticFramebufferInterval once we're sure we're
        // staring at the powered-off image (a streak of large identical frames); otherwise
        // poll at the normal fast rate. static_frame_count_ resets to 0 on any non-dup /
        // small / no-op frame, so recovery latency is <= one slow tick.
        const auto request_interval = static_frame_count_ >= kAtenStaticBackoffAfterFrames
            ? kAtenStaticFramebufferInterval
            : kAtenFramebufferRequestBackoff;
        framebuffer_request_timer_.expires_after(request_interval);
        framebuffer_request_timer_.async_wait(
            asio::bind_executor(strand_, [self](beast::error_code error) {
                self->framebuffer_request_timer_active_ = false;
                if (error == boost::asio::error::operation_aborted || self->closed_ ||
                    self->stop_requested_.load()) {
                    return;
                }
                if (error) {
                    self->state_.set_exception(std::make_exception_ptr(
                        beast::system_error(error, "ATEN framebuffer request timer failed")));
                    self->close_now();
                    return;
                }

                self->queue_framebuffer_update_request();
            }));
    }

    void queue_framebuffer_update_request()
    {
        if (closed_ || stop_requested_.load() || framebuffer_request_pending_) {
            return;
        }

        const auto request_width = static_cast<std::uint16_t>(
            previous_width_ > 0 ? previous_width_ : init_.width);
        const auto request_height = static_cast<std::uint16_t>(
            previous_height_ > 0 ? previous_height_ : init_.height);
        framebuffer_request_pending_ = true;
        queue_write(make_framebuffer_update_request(request_width,
                        request_height, 
                        !g_aten_full_framebuffer_refresh_requested.exchange(false)));
    }

    void queue_write(std::vector<std::uint8_t> packet)
    {
        if (closed_) {
            return;
        }

        write_queue_.push_back(AtenQueuedWrite{std::move(packet)});
        start_write();
    }

    void start_write()
    {
        if (closed_ || write_in_progress_ || write_queue_.empty()) {
            return;
        }

        write_in_progress_ = true;
        active_write_ = std::move(write_queue_.front());
        write_queue_.pop_front();
        ws_.binary(true);
        auto self = shared_from_this();
        ws_.async_write(
            asio::buffer(active_write_.packet),
            asio::bind_executor(strand_, [self](beast::error_code error, std::size_t) {
                self->on_write(error);
            }));
    }

    void on_write(beast::error_code error)
    {
        write_in_progress_ = false;
        if (closed_) {
            return;
        }
        if (error) {
            const bool expected_stop =
                stop_requested_.load()
                || error == websocket::error::closed
                || error == asio::error::operation_aborted
                || error == asio::error::bad_descriptor;
            if (!expected_stop) {
                log_error() << "aten websocket write callback error=" << error.message();
                state_.set_exception(std::make_exception_ptr(
                    beast::system_error(error, "ATEN websocket write failed")));
            }
            close_now();
            return;
        }

        if (options_.login.vverbose && !is_hot_write_packet(active_write_.packet)) {
            log_info() << "sent ATEN packet " << describe_aten_client_packet(active_write_.packet);
        }
        active_write_ = {};
        start_write();
    }

    void close_now()
    {
        if (closed_) {
            return;
        }
        closed_ = true;
        state_.input.clear();
        framebuffer_request_timer_.cancel();
        parser_.clear();

        beast::error_code error;
        beast::get_lowest_layer(ws_).socket().cancel(error);
        if (error && options_.login.verbose) {
            log_error() << "aten websocket stream cancel error=" << error.message();
        }
        error.clear();
        beast::get_lowest_layer(ws_).socket().shutdown(tcp::socket::shutdown_both, error);
        if (error && options_.login.verbose) {
            log_error() << "aten websocket socket shutdown error=" << error.message();
        }
        error.clear();
        beast::get_lowest_layer(ws_).socket().close(error);
        if (error && options_.login.verbose) {
            log_error() << "aten websocket socket close error=" << error.message();
        }
        io_.stop();
    }

    asio::io_context& io_;
    AtenWebSocket& ws_;
    AtenViewState& state_;
    AtenViewOptions options_;
    AtenRfbServerInit init_;
    std::atomic_bool& stop_requested_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer framebuffer_request_timer_;
    beast::flat_buffer read_buffer_;
    AtenRfbMessageBuffer parser_;
    std::deque<AtenQueuedWrite> write_queue_;
    AtenQueuedWrite active_write_;
    std::vector<std::uint16_t> cursor_pattern_;
    std::chrono::steady_clock::time_point stats_started_at_ = std::chrono::steady_clock::now();
    std::uint64_t total_websocket_messages_ = 0;
    std::uint64_t total_messages_ = 0;
    std::uint64_t stats_rx_bytes_ = 0;
    std::uint64_t stats_messages_ = 0;
    std::uint64_t stats_framebuffer_updates_ = 0;
    std::size_t last_websocket_message_bytes_ = 0;
    int previous_width_ = 0;
    int previous_height_ = 0;
    std::vector<std::uint8_t> previous_compressed_; // last decoded frame's payload, for powered-off dedup
    int static_frame_count_ = 0;                    // consecutive large identical frames (powered-off backoff)
    int cursor_pattern_width_ = 0;
    int cursor_pattern_height_ = 0;
    int updates_ = 0;
    int blank_screen_packets_ = 0;
    bool write_in_progress_ = false;
    bool framebuffer_request_pending_ = false;
    bool framebuffer_request_timer_active_ = false;
    bool cursor_position_request_pending_ = false;
    bool receive_parse_posted_ = false;
    bool closed_ = false;
};

} // namespace

void run_aten_network_session(
    const AtenViewOptions& options,
    AtenViewState& state,
    std::atomic_bool& stop_requested)
{
    struct ConnectionStatusGuard {
        AtenViewState& state;
        ~ConnectionStatusGuard()
        {
            state.view_status.kvm_connection(false);
        }
    } connection_status_guard{state};

    AtenSession session = login_aten(options.login);
    AtenLogoutGuard logout_guard(options.login);
    logout_guard.arm(session);
    log_info() << "aten login succeeded";
    if (options.login.verbose) {
        log_info() << "cookies stored: " << session.web.cookie_count();
    }

    std::string rfb_credential = options.login.username;
    const std::string bootstrap_credential =
        fetch_aten_ikvm_bootstrap(options.login, session.web);
    if (!bootstrap_credential.empty()) {
        rfb_credential = bootstrap_credential;
    } else {
        log_warning() << "aten warning: bootstrap did not expose entry_value; "
                         "falling back to login username for RFB auth";
    }

    auto websocket = session.web.open_websocket(BmcWebSocketConnectOptions{
        .role = "aten",
        .log_name = "aten websocket",
        .path = "/",
        .idle_timeout_seconds = options.idle_timeout_seconds,
        .tcp_no_delay = true,
    });
    auto ws_ptr = websocket.connection->stream();
    AtenWebSocket& ws = *ws_ptr;
    asio::io_context& io = websocket.connection->io_context();
    state.set_force_close([&web = session.web] {
        web.force_close_websocket("aten");
    });

    RfbHandshakeStream rfb(ws);
    const std::string server_protocol_line = rfb.read_string(12);
    if (!server_protocol_line.starts_with("RFB ") || server_protocol_line.size() != 12) {
        throw std::runtime_error("ATEN websocket did not start with an RFB protocol banner");
    }
    const std::string server_version = server_protocol_line.substr(4, 7);
    const std::string client_version = client_protocol_version(server_version);
    log_info() << "aten rfb server-version=" << server_version
               << " client-version=" << client_version;
    rfb.write_string("RFB " + client_version + "\n");

    const std::uint8_t security_type = negotiate_security(rfb, client_version);
    authenticate(rfb, security_type, rfb_credential, options.login.verbose);
    if (security_type != 1 || version_uses_security_result_for_none(client_version)) {
        read_security_result(rfb, options.login.verbose);
    }

    rfb.write(std::vector<std::uint8_t>{options.shared ? 1U : 0U});
    const bool insyde_extension = security_type == 15 || security_type == 16;
    const AtenRfbServerInit init = read_server_init(rfb, insyde_extension);
    // Publish the host resolution so pointer input can map (and thus wake a sleeping
    // host) before any video frame decodes. Height first, width last: a reader that
    // sees width > 0 also sees height. Video frame dims override this once they arrive.
    if (init.width > 0 && init.height > 0) {
        state.host_input_height.store(init.height, std::memory_order_relaxed);
        state.host_input_width.store(init.width, std::memory_order_relaxed);
    }
    if (options.login.verbose) {
        log_info() << "aten rfb server-init"
                   << " size=" << init.width << 'x' << init.height
                   << " name=\"" << init.name << "\""
                   << " bpp=" << static_cast<int>(init.bits_per_pixel)
                   << " depth=" << static_cast<int>(init.depth)
                   << " true-color=" << (init.true_color ? "yes" : "no")
                   << " endian=" << (init.big_endian ? "big" : "little");
    }
    if (init.insyde_extension && options.login.verbose) {
        log_info() << "aten rfb insyde extension"
                   << " session-id=" << init.session_id
                   << " video=" << static_cast<int>(init.video_enable)
                   << " kbms=" << static_cast<int>(init.keyboard_mouse_enable)
                   << " kick-user=" << static_cast<int>(init.kick_user_enable)
                   << " vmedia=" << static_cast<int>(init.virtual_media_enable);
    }

    state.view_status.kvm_connection(true);
    if (init.insyde_extension) {
        state.view_status.kvm_display_status(init.video_enable != 0);
    } else {
        state.view_status.kvm_display_status(true);
    }

    auto network_session = std::make_shared<AtenNetworkRfbSession>(
        io,
        ws,
        state,
        options,
        init,
        stop_requested);
    std::weak_ptr<AtenNetworkRfbSession> weak_network = network_session;
    state.set_force_close([weak_network] {
        if (auto network = weak_network.lock()) {
            network->request_stop();
        }
    });

    network_session->start(rfb.take_queued_bytes());
    io.run();

    state.input.clear();
    state.set_force_close({});
}

} // namespace hitsc
