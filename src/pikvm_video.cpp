#include "pikvm_video.hpp"

#include "errors.hpp"
#include "log.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cctype>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace json = boost::json;
namespace websocket = boost::beast::websocket;

using PikvmClock = std::chrono::steady_clock;

const char* pikvm_video_pixel_format_name(PikvmVideoPixelFormat format)
{
    switch (format) {
    case PikvmVideoPixelFormat::rgba32:
        return "rgba32";
    case PikvmVideoPixelFormat::i420:
        return "i420";
    case PikvmVideoPixelFormat::nv12:
        return "nv12";
    case PikvmVideoPixelFormat::d3d11_nv12:
        return "d3d11_nv12";
    }
    return "unknown";
}

std::size_t pikvm_video_frame_payload_bytes(const PikvmVideoFrame& frame)
{
    const auto plane_bytes = [](const std::uint8_t* data, int pitch, int rows) -> std::size_t {
        if (data == nullptr || pitch <= 0 || rows <= 0) {
            return 0;
        }
        return static_cast<std::size_t>(pitch) * static_cast<std::size_t>(rows);
    };

    const int chroma_height = (frame.height + 1) / 2;
    switch (frame.format) {
    case PikvmVideoPixelFormat::rgba32:
        return frame.rgba.size();
    case PikvmVideoPixelFormat::i420:
        return plane_bytes(frame.planes[0], frame.pitches[0], frame.height)
             + plane_bytes(frame.planes[1], frame.pitches[1], chroma_height)
             + plane_bytes(frame.planes[2], frame.pitches[2], chroma_height);
    case PikvmVideoPixelFormat::nv12:
        return plane_bytes(frame.planes[0], frame.pitches[0], frame.height)
             + plane_bytes(frame.planes[1], frame.pitches[1], chroma_height);
    case PikvmVideoPixelFormat::d3d11_nv12:
        return static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height)
             + static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(chroma_height);
    }
    return 0;
}

namespace {

std::string ffmpeg_error_string(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::runtime_error ffmpeg_error(int code, const char* context)
{
    return std::runtime_error(std::string(context) + ": " + ffmpeg_error_string(code));
}

std::vector<std::uint8_t> buffer_bytes(const beast::flat_buffer& buffer)
{
    std::vector<std::uint8_t> bytes(boost::asio::buffer_size(buffer.data()));
    boost::asio::buffer_copy(boost::asio::buffer(bytes), buffer.data());
    return bytes;
}

std::string buffer_text(const beast::flat_buffer& buffer)
{
    std::string text(boost::asio::buffer_size(buffer.data()), '\0');
    boost::asio::buffer_copy(boost::asio::buffer(text), buffer.data());
    return text;
}

std::string json_string_field(const json::object& object, std::string_view name)
{
    const json::value* value = object.if_contains(name);
    if (value == nullptr) {
        return {};
    }

    if (const json::string* string = value->if_string()) {
        return std::string(*string);
    }
    if (const auto* number = value->if_int64()) {
        return std::to_string(*number);
    }
    if (const auto* number = value->if_uint64()) {
        return std::to_string(*number);
    }
    return {};
}

std::string media_event_type(std::string_view text)
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

std::string media_h264_profile_level_id(std::string_view text)
{
    boost::system::error_code error;
    json::value value = json::parse(text, error);
    if (error) {
        return {};
    }

    const json::object* object = value.if_object();
    if (object == nullptr || json_string_field(*object, "event_type") != "media") {
        return {};
    }

    const json::value* event_value = object->if_contains("event");
    const json::object* event = event_value != nullptr ? event_value->if_object() : nullptr;
    const json::value* video_value = event != nullptr ? event->if_contains("video") : nullptr;
    const json::object* video = video_value != nullptr ? video_value->if_object() : nullptr;
    const json::value* h264_value = video != nullptr ? video->if_contains("h264") : nullptr;
    const json::object* h264 = h264_value != nullptr ? h264_value->if_object() : nullptr;
    return h264 != nullptr ? json_string_field(*h264, "profile_level_id") : std::string{};
}

std::string printable_preview(std::string_view value, std::size_t limit = 160)
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

std::vector<std::uint8_t> pikvm_media_start_message()
{
    static constexpr std::string_view message =
        R"({"event_type":"start","event":{"type":"video","format":"h264"}})";
    return std::vector<std::uint8_t>(message.begin(), message.end());
}

void d3d11_lock_callback(void* lock_context)
{
    static_cast<std::recursive_mutex*>(lock_context)->lock();
}

void d3d11_unlock_callback(void* lock_context)
{
    static_cast<std::recursive_mutex*>(lock_context)->unlock();
}

bool codec_supports_d3d11(const AVCodec* codec)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (config == nullptr) {
            return false;
        }
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA
            && config->pix_fmt == AV_PIX_FMT_D3D11
            && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            return true;
        }
    }
}

} // namespace

struct PikvmH264Decoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecParserContext* parser = nullptr;
    AVCodecContext* context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    AVBufferRef* d3d11_device_ref = nullptr;
    PikvmVideoDecodeMode requested_decode_mode = PikvmVideoDecodeMode::auto_select;
    std::shared_ptr<PikvmD3D11Context> d3d11_context;
    bool d3d11_decode = false;

    Impl(
        PikvmVideoDecodeMode decode_mode,
        std::shared_ptr<PikvmD3D11Context> d3d11_context_in)
        : requested_decode_mode(decode_mode)
        , d3d11_context(std::move(d3d11_context_in))
    {
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec == nullptr) {
            throw UserError("FFmpeg H.264 decoder is not available");
        }

        parser = av_parser_init(AV_CODEC_ID_H264);
        if (parser == nullptr) {
            throw UserError("FFmpeg H.264 parser is not available");
        }

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            throw std::runtime_error("failed to allocate FFmpeg packet/frame");
        }

        open_best_decoder();
    }

    ~Impl()
    {
        if (sws != nullptr) {
            sws_freeContext(sws);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
        if (d3d11_device_ref != nullptr) {
            av_buffer_unref(&d3d11_device_ref);
        }
        if (parser != nullptr) {
            av_parser_close(parser);
        }
    }

    void open_best_decoder()
    {
        if (requested_decode_mode != PikvmVideoDecodeMode::software) {
            try {
                open_decoder(true);
                d3d11_decode = true;
                log_info() << "ffmpeg H.264 decoder opened"
                           << " mode=d3d11";
                return;
            } catch (const std::exception& error) {
                close_decoder_context();
                if (requested_decode_mode == PikvmVideoDecodeMode::d3d11) {
                    throw UserError("D3D11 H.264 decode unavailable: " + std::string(error.what()));
                }
                log_warning() << "D3D11 H.264 decode unavailable; using software decode: "
                              << error.what();
            }
        }

        open_decoder(false);
        log_info() << "ffmpeg H.264 decoder opened"
                   << " mode=software";
    }

    void close_decoder_context()
    {
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
        if (d3d11_device_ref != nullptr) {
            av_buffer_unref(&d3d11_device_ref);
        }
        d3d11_decode = false;
    }

    void open_decoder(bool use_d3d11)
    {
        if (context != nullptr || d3d11_device_ref != nullptr) {
            close_decoder_context();
        }

        context = avcodec_alloc_context3(codec);
        if (context == nullptr) {
            throw std::runtime_error("failed to allocate FFmpeg H.264 decoder context");
        }
        context->flags2 |= AV_CODEC_FLAG2_FAST;
        context->opaque = this;

        if (use_d3d11) {
            if (!codec_supports_d3d11(codec)) {
                throw std::runtime_error("FFmpeg H.264 decoder does not advertise D3D11VA");
            }
            if (!d3d11_context || d3d11_context->device == nullptr || !d3d11_context->lock) {
                throw std::runtime_error("SDL D3D11 renderer device is not available");
            }

            context->thread_count = 1;
            context->thread_type = 0;
            context->get_format = &Impl::choose_pixel_format;
            d3d11_device_ref = create_d3d11_device_context();
            context->hw_device_ctx = av_buffer_ref(d3d11_device_ref);
            if (context->hw_device_ctx == nullptr) {
                throw std::runtime_error("failed to reference FFmpeg D3D11 device context");
            }
        } else {
            context->thread_count = 0;
            context->thread_type = FF_THREAD_FRAME;
        }

        const int result = avcodec_open2(context, codec, nullptr);
        if (result < 0) {
            throw ffmpeg_error(
                result,
                use_d3d11
                    ? "failed to open FFmpeg D3D11 H.264 decoder"
                    : "failed to open FFmpeg H.264 decoder");
        }
    }

    AVBufferRef* create_d3d11_device_context()
    {
        AVBufferRef* device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (device_ref == nullptr) {
            throw std::runtime_error("failed to allocate FFmpeg D3D11 device context");
        }

        AVHWDeviceContext* device_context =
            reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
        auto* d3d11 =
            reinterpret_cast<AVD3D11VADeviceContext*>(device_context->hwctx);
        d3d11->device = d3d11_context->device;
        d3d11->device->AddRef();
        d3d11->lock = &d3d11_lock_callback;
        d3d11->unlock = &d3d11_unlock_callback;
        d3d11->lock_ctx = d3d11_context->lock.get();
        d3d11->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

        const int result = av_hwdevice_ctx_init(device_ref);
        if (result < 0) {
            av_buffer_unref(&device_ref);
            throw ffmpeg_error(result, "failed to initialize FFmpeg D3D11 device context");
        }

        return device_ref;
    }

    static AVPixelFormat choose_pixel_format(
        AVCodecContext* decoder_context,
        const AVPixelFormat* formats)
    {
        auto* self = static_cast<Impl*>(decoder_context->opaque);
        for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == AV_PIX_FMT_D3D11) {
                if (self != nullptr && self->verbose) {
                    log_info() << "selected FFmpeg D3D11 pixel format";
                }
                return *format;
            }
        }

        if (self != nullptr) {
            log_warning() << "FFmpeg did not offer D3D11 frames for H.264 decode";
        }
        return formats[0];
    }

    std::vector<PikvmVideoFrame> ingest(std::span<const std::uint8_t> bytes)
    {
        std::vector<PikvmVideoFrame> frames;
        const std::uint8_t* data = bytes.data();
        int remaining = static_cast<int>(std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(INT_MAX)));

        while (remaining > 0) {
            std::uint8_t* packet_data = nullptr;
            int packet_size = 0;
            const int consumed = av_parser_parse2(
                parser,
                context,
                &packet_data,
                &packet_size,
                data,
                remaining,
                AV_NOPTS_VALUE,
                AV_NOPTS_VALUE,
                0);
            if (consumed < 0) {
                throw ffmpeg_error(consumed, "failed to parse H.264 stream");
            }

            data += consumed;
            remaining -= consumed;

            if (packet_size > 0) {
                ++packets_parsed;
                decode_packet(packet_data, packet_size, frames);
            }
        }

        return frames;
    }

    void decode_packet(const std::uint8_t* packet_data, int packet_size, std::vector<PikvmVideoFrame>& frames)
    {
        av_packet_unref(packet);
        int result = av_new_packet(packet, packet_size);
        if (result < 0) {
            throw ffmpeg_error(result, "failed to allocate FFmpeg H.264 packet");
        }
        std::memcpy(packet->data, packet_data, static_cast<std::size_t>(packet_size));

        result = avcodec_send_packet(context, packet);
        av_packet_unref(packet);
        if (result < 0) {
            throw ffmpeg_error(result, "failed to send H.264 packet to decoder");
        }

        receive_frames(frames);
    }

    void receive_frames(std::vector<PikvmVideoFrame>& frames)
    {
        while (true) {
            const int result = avcodec_receive_frame(context, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            if (result < 0) {
                throw ffmpeg_error(result, "failed to receive decoded H.264 frame");
            }

            frames.push_back(make_output_frame());
            ++frames_decoded;
            av_frame_unref(frame);
        }
    }

    PikvmVideoFrame make_output_frame()
    {
        if (frame->width <= 0 || frame->height <= 0) {
            throw std::runtime_error("decoded H.264 frame has invalid dimensions");
        }

        const auto source_format = static_cast<AVPixelFormat>(frame->format);
        switch (source_format) {
        case AV_PIX_FMT_D3D11:
            return reference_d3d11_frame();
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            if (has_positive_planes(3)) {
                return reference_decoded_frame(PikvmVideoPixelFormat::i420);
            }
            break;
        case AV_PIX_FMT_NV12:
            if (has_positive_planes(2)) {
                return reference_decoded_frame(PikvmVideoPixelFormat::nv12);
            }
            break;
        default:
            break;
        }

        return convert_frame_to_rgba(source_format);
    }

    PikvmVideoFrame reference_d3d11_frame()
    {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        if (texture == nullptr) {
            throw std::runtime_error("decoded D3D11 H.264 frame is missing its texture");
        }

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12) {
            throw std::runtime_error("decoded D3D11 H.264 frame is not NV12");
        }

        AVFrame* cloned = av_frame_clone(frame);
        if (cloned == nullptr) {
            throw std::runtime_error("failed to reference decoded D3D11 FFmpeg frame");
        }

        PikvmVideoFrame output;
        output.width = cloned->width;
        output.height = cloned->height;
        output.format = PikvmVideoPixelFormat::d3d11_nv12;
        output.d3d11_texture = reinterpret_cast<ID3D11Texture2D*>(cloned->data[0]);
        output.d3d11_array_slice = static_cast<int>(reinterpret_cast<intptr_t>(cloned->data[1]));
        if (d3d11_context) {
            output.d3d11_lock = d3d11_context->lock;
        }
        output.owner = std::shared_ptr<void>(cloned, [](void* pointer) {
            AVFrame* frame = static_cast<AVFrame*>(pointer);
            av_frame_free(&frame);
        });
        return output;
    }

    bool has_positive_planes(int count) const
    {
        for (int i = 0; i < count; ++i) {
            if (frame->data[i] == nullptr || frame->linesize[i] <= 0) {
                return false;
            }
        }
        return true;
    }

    PikvmVideoFrame reference_decoded_frame(PikvmVideoPixelFormat format)
    {
        AVFrame* cloned = av_frame_clone(frame);
        if (cloned == nullptr) {
            throw std::runtime_error("failed to reference decoded FFmpeg frame");
        }

        PikvmVideoFrame output;
        output.width = cloned->width;
        output.height = cloned->height;
        output.format = format;
        for (std::size_t i = 0; i < output.planes.size(); ++i) {
            output.planes[i] = cloned->data[i];
            output.pitches[i] = cloned->linesize[i];
        }
        output.owner = std::shared_ptr<void>(cloned, [](void* pointer) {
            AVFrame* frame = static_cast<AVFrame*>(pointer);
            av_frame_free(&frame);
        });
        return output;
    }

    PikvmVideoFrame convert_frame_to_rgba(AVPixelFormat source_format)
    {
        sws = sws_getCachedContext(
            sws,
            frame->width,
            frame->height,
            source_format,
            frame->width,
            frame->height,
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (sws == nullptr) {
            throw std::runtime_error("failed to create FFmpeg RGBA scaler");
        }

        PikvmVideoFrame output;
        output.width = frame->width;
        output.height = frame->height;
        output.format = PikvmVideoPixelFormat::rgba32;
        output.rgba.assign(
            static_cast<std::size_t>(output.width) * static_cast<std::size_t>(output.height) * 4U,
            0);
        output.planes[0] = output.rgba.data();
        output.pitches[0] = output.width * 4;

        std::uint8_t* destination_data[4] = {output.rgba.data(), nullptr, nullptr, nullptr};
        int destination_linesize[4] = {output.width * 4, 0, 0, 0};
        const int scaled = sws_scale(
            sws,
            frame->data,
            frame->linesize,
            0,
            frame->height,
            destination_data,
            destination_linesize);
        if (scaled != frame->height) {
            throw std::runtime_error("FFmpeg RGBA scaler returned a partial frame");
        }

        return output;
    }

    bool verbose = false;
    std::uint64_t packets_parsed = 0;
    std::uint64_t frames_decoded = 0;
};

PikvmH264Decoder::PikvmH264Decoder()
    : PikvmH264Decoder(PikvmVideoDecodeMode::software, {})
{
}

PikvmH264Decoder::PikvmH264Decoder(
    PikvmVideoDecodeMode decode_mode,
    std::shared_ptr<PikvmD3D11Context> d3d11_context)
    : impl_(std::make_unique<Impl>(decode_mode, std::move(d3d11_context)))
{
}

PikvmH264Decoder::~PikvmH264Decoder() = default;

std::vector<PikvmVideoFrame> PikvmH264Decoder::ingest(std::span<const std::uint8_t> bytes)
{
    return impl_->ingest(bytes);
}

void PikvmH264Decoder::set_verbose(bool verbose)
{
    impl_->verbose = verbose;
}

namespace {

class PikvmVideoStream : public std::enable_shared_from_this<PikvmVideoStream> {
public:
    PikvmVideoStream(
        std::shared_ptr<PikvmWebSocket> ws,
        PikvmViewOptions options,
        std::shared_ptr<PikvmD3D11Context> d3d11_context,
        const std::atomic_bool& stop_requested,
        std::function<void(std::size_t)> on_data,
        std::function<void(PikvmVideoFrame)> on_frame,
        std::function<void(std::exception_ptr)> on_error)
        : ws_(std::move(ws))
        , options_(std::move(options))
        , stop_requested_(stop_requested)
        , on_data_(std::move(on_data))
        , on_frame_(std::move(on_frame))
        , on_error_(std::move(on_error))
        , decoder_(options_.video_decode, std::move(d3d11_context))
    {
    }

    void start()
    {
        decoder_.set_verbose(options_.login.verbose);
        auto self = shared_from_this();
        asio::dispatch(ws_->get_executor(), [self] {
            self->start_read();
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
                || error == websocket::error::closed
                || error == asio::error::operation_aborted
                || error == asio::error::bad_descriptor;
            if (!expected_stop) {
                on_error_(std::make_exception_ptr(
                    UserError("PiKVM video websocket read failed: " + error.message())));
            }
            closed_ = true;
            return;
        }

        try {
            if (ws_->got_text()) {
                handle_text_message(bytes_transferred);
            } else {
                handle_binary_message(bytes_transferred);
            }
        } catch (...) {
            on_error_(std::current_exception());
            closed_ = true;
            return;
        }

        start_read();
    }

    void handle_text_message(std::size_t bytes_transferred)
    {
        const std::string text = buffer_text(read_buffer_);
        read_buffer_.consume(read_buffer_.size());
        const std::string event_type = media_event_type(text);
        ++text_messages_seen_;
        if (options_.login.verbose) {
            LogLine line = log_info();
            line << "pikvm video websocket text"
                 << " bytes=" << bytes_transferred;
            if (!event_type.empty()) {
                line << " event_type=" << event_type;
            }
            line << " preview=\"" << printable_preview(text) << "\"";
        }

        if (event_type != "media") {
            return;
        }

        const std::string profile_level_id = media_h264_profile_level_id(text);
        if (profile_level_id.empty()) {
            throw UserError("PiKVM media websocket did not advertise H.264");
        }

        log_info() << "pikvm media H.264 available"
                   << " profile-level-id=" << profile_level_id;
        send_start_message();
    }

    void send_start_message()
    {
        if (start_sent_) {
            return;
        }

        start_message_ = pikvm_media_start_message();
        start_sent_ = true;
        ws_->text(true);
        auto self = shared_from_this();
        ws_->async_write(
            boost::asio::buffer(start_message_),
            [self](beast::error_code error, std::size_t bytes_transferred) {
                self->on_start_write(error, bytes_transferred);
            });
    }

    void on_start_write(beast::error_code error, std::size_t bytes_transferred)
    {
        if (closed_) {
            return;
        }
        if (error) {
            on_error_(std::make_exception_ptr(
                UserError("PiKVM media start write failed: " + error.message())));
            closed_ = true;
            return;
        }
        if (options_.login.verbose) {
            log_info() << "sent PiKVM media start"
                       << " bytes=" << bytes_transferred;
        }
    }

    void handle_binary_message(std::size_t bytes_transferred)
    {
        const auto media_received_at = PikvmClock::now();
        std::vector<std::uint8_t> bytes = buffer_bytes(read_buffer_);
        read_buffer_.consume(read_buffer_.size());

        if (bytes.empty()) {
            return;
        }

        if (on_data_) {
            on_data_(bytes_transferred);
        }

        const std::uint8_t op = bytes[0];
        if (op == 255) {
            missed_heartbeats_ = 0;
            if (options_.login.verbose) {
                log_info() << "pikvm video websocket pong"
                           << " bytes=" << bytes_transferred;
            }
            return;
        }

        if (op != 1) {
            if (options_.login.verbose) {
                log_warning() << "ignored PiKVM video websocket binary op"
                              << " op=" << static_cast<int>(op)
                              << " bytes=" << bytes_transferred;
            }
            return;
        }

        if (bytes.size() < 2) {
            if (options_.login.verbose) {
                log_warning() << "ignored truncated PiKVM video frame header";
            }
            return;
        }

        const bool key = bytes[1] != 0;
        (void)key;
        const std::span<const std::uint8_t> payload(bytes.data() + 2, bytes.size() - 2);
        ++messages_seen_;

        std::vector<PikvmVideoFrame> frames = decoder_.ingest(payload);
        const auto decoded_at = PikvmClock::now();
        for (PikvmVideoFrame& frame : frames) {
            frame.timing.media_received_at = media_received_at;
            frame.timing.decoded_at = decoded_at;
            on_frame_(std::move(frame));
            ++frames_decoded_;
        }
    }

    std::shared_ptr<PikvmWebSocket> ws_;
    PikvmViewOptions options_;
    const std::atomic_bool& stop_requested_;
    std::function<void(std::size_t)> on_data_;
    std::function<void(PikvmVideoFrame)> on_frame_;
    std::function<void(std::exception_ptr)> on_error_;
    beast::flat_buffer read_buffer_;
    PikvmH264Decoder decoder_;
    std::vector<std::uint8_t> start_message_;
    std::uint64_t messages_seen_ = 0;
    std::uint64_t text_messages_seen_ = 0;
    std::uint64_t frames_decoded_ = 0;
    int missed_heartbeats_ = 0;
    bool start_sent_ = false;
    bool closed_ = false;
};

} // namespace

void start_pikvm_video_stream(
    std::shared_ptr<PikvmWebSocket> ws,
    PikvmViewOptions options,
    std::shared_ptr<PikvmD3D11Context> d3d11_context,
    const std::atomic_bool& stop_requested,
    std::function<void(std::size_t)> on_data,
    std::function<void(PikvmVideoFrame)> on_frame,
    std::function<void(std::exception_ptr)> on_error)
{
    std::make_shared<PikvmVideoStream>(
        std::move(ws),
        std::move(options),
        std::move(d3d11_context),
        stop_requested,
        std::move(on_data),
        std::move(on_frame),
        std::move(on_error))
        ->start();
}

} // namespace hitsc
