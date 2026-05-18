#include "aten_view.hpp"

#include "app_info.hpp"
#include "aspeed_decoder.hpp"
#include "aten_session.hpp"
#include "diagnostics.hpp"
#include "log.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <SDL3/SDL.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hitsc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

namespace {

using AtenWebSocket = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

struct AtenRfbServerInit {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t bits_per_pixel = 0;
    std::uint8_t depth = 0;
    bool big_endian = false;
    bool true_color = false;
    std::uint16_t red_max = 0;
    std::uint16_t green_max = 0;
    std::uint16_t blue_max = 0;
    std::uint8_t red_shift = 0;
    std::uint8_t green_shift = 0;
    std::uint8_t blue_shift = 0;
    std::string name;
    bool insyde_extension = false;
    std::uint32_t session_id = 0;
    std::uint8_t video_enable = 0;
    std::uint8_t keyboard_mouse_enable = 0;
    std::uint8_t kick_user_enable = 0;
    std::uint8_t virtual_media_enable = 0;
};

struct AtenFramebufferRect {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int32_t encoding = 0;
    std::int32_t mode = 0;
    std::uint32_t data_length = 0;
};

struct AtenViewFrame {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> rgba;
};

struct AtenCursorImage {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool visible = false;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> rgba;
};

struct AtenRemoteMousePosition {
    int x = 0;
    int y = 0;
};

struct PendingAtenInputPacket {
    std::vector<std::uint8_t> packet;
    bool coalesce_mouse_motion = false;
};

struct AtenQueuedWrite {
    std::vector<std::uint8_t> packet;
};

struct AtenViewState {
    std::mutex frame_mutex;
    std::mutex cursor_mutex;
    std::mutex control_mutex;
    std::shared_ptr<const AtenViewFrame> frame;
    std::shared_ptr<const AtenCursorImage> cursor;
    std::uint64_t frame_sequence = 0;
    std::uint64_t cursor_sequence = 0;
    std::string status = "starting";
    std::exception_ptr exception;
    std::function<void()> force_close;
    std::function<void(std::vector<std::uint8_t>, bool)> input_sink;
    std::deque<PendingAtenInputPacket> pending_input;
};

struct AtenAstPayloadHeader {
    unsigned y_selector = 0;
    unsigned uv_selector = 0;
    unsigned mode = 0;
    unsigned mode420 = 0;
};

constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 10;
constexpr std::chrono::milliseconds kAtenFramebufferRequestBackoff{66};
constexpr std::size_t kMaxQueuedInputPackets = 512;
constexpr std::size_t kMaxAtenMessagesPerReceiveDrain = 8;
constexpr std::uint32_t kAtenAstFrameEndWord = 0x90000000U;
using AtenKeyDownState = std::array<bool, 256>;

void append_be16(std::vector<std::uint8_t>& bytes, std::uint16_t value);
void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value);
std::uint16_t load_be16(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::uint32_t load_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::uint32_t load_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset);

void throw_sdl_error(std::string_view context)
{
    throw std::runtime_error(std::string(context) + ": " + SDL_GetError());
}

SDL_FRect centered_target_rect(int window_width, int window_height, int frame_width, int frame_height)
{
    const float width_scale = static_cast<float>(window_width) / static_cast<float>(frame_width);
    const float height_scale = static_cast<float>(window_height) / static_cast<float>(frame_height);
    const float scale = std::min(width_scale, height_scale);
    SDL_FRect rect{};
    rect.w = std::floor(static_cast<float>(frame_width) * scale);
    rect.h = std::floor(static_cast<float>(frame_height) * scale);
    rect.x = std::floor((static_cast<float>(window_width) - rect.w) / 2.0f);
    rect.y = std::floor((static_cast<float>(window_height) - rect.h) / 2.0f);
    return rect;
}

SDL_FRect current_target_rect(SDL_Window* window, int frame_width, int frame_height)
{
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &window_width, &window_height)) {
        SDL_GetWindowSize(window, &window_width, &window_height);
    }
    return centered_target_rect(window_width, window_height, frame_width, frame_height);
}

std::optional<AtenRemoteMousePosition> remote_mouse_position(
    float window_x,
    float window_y,
    const SDL_FRect& target,
    int frame_width,
    int frame_height)
{
    if (frame_width <= 0 || frame_height <= 0 || target.w <= 0.0f || target.h <= 0.0f) {
        return std::nullopt;
    }
    if (window_x < target.x || window_y < target.y ||
        window_x > target.x + target.w || window_y > target.y + target.h) {
        return std::nullopt;
    }

    const double normalized_x =
        (static_cast<double>(window_x) - static_cast<double>(target.x)) / static_cast<double>(target.w);
    const double normalized_y =
        (static_cast<double>(window_y) - static_cast<double>(target.y)) / static_cast<double>(target.h);
    return AtenRemoteMousePosition{
        std::clamp(static_cast<int>(std::floor(normalized_x * frame_width + 0.5)), 0, frame_width),
        std::clamp(static_cast<int>(std::floor(normalized_y * frame_height + 0.5)), 0, frame_height),
    };
}

std::uint8_t button_mask_for_sdl_button(std::uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return 1;
    case SDL_BUTTON_MIDDLE:
        return 2;
    case SDL_BUTTON_RIGHT:
        return 4;
    default:
        return 0;
    }
}

std::optional<std::uint32_t> aten_keyboard_usage_from_sdl_scancode(SDL_Scancode scancode)
{
    const auto usage = static_cast<int>(scancode);
    if ((usage >= SDL_SCANCODE_A && usage <= SDL_SCANCODE_APPLICATION) ||
        (usage >= SDL_SCANCODE_KP_EQUALS && usage <= SDL_SCANCODE_RGUI)) {
        return static_cast<std::uint32_t>(usage);
    }

    return std::nullopt;
}

int sampled_average_rgb(const std::vector<std::uint8_t>& rgba)
{
    if (rgba.empty()) {
        return 0;
    }

    std::uint64_t total = 0;
    std::uint64_t samples = 0;
    constexpr std::size_t stride_pixels = 257;
    for (std::size_t pixel = 0; pixel * 4 + 2 < rgba.size(); pixel += stride_pixels) {
        const std::size_t offset = pixel * 4;
        total += rgba[offset] + rgba[offset + 1] + rgba[offset + 2];
        ++samples;
    }
    return samples == 0 ? 0 : static_cast<int>(total / (samples * 3));
}

AtenCursorImage make_aten_cursor_image(
    std::uint32_t remote_x,
    std::uint32_t remote_y,
    std::uint32_t remote_width,
    std::uint32_t remote_height,
    const std::vector<std::uint8_t>& pattern)
{
    if (remote_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        remote_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        remote_x > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        remote_y > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("ATEN cursor dimensions are too large");
    }

    AtenCursorImage cursor;
    cursor.x = static_cast<int>(remote_x);
    cursor.y = static_cast<int>(remote_y);
    cursor.width = static_cast<int>(remote_width);
    cursor.height = static_cast<int>(remote_height);
    cursor.visible = cursor.width > 0 && cursor.height > 0;
    cursor.rgba.assign(
        static_cast<std::size_t>(cursor.width) * static_cast<std::size_t>(cursor.height) * 4U,
        0);

    const std::size_t pixel_count =
        static_cast<std::size_t>(cursor.width) * static_cast<std::size_t>(cursor.height);
    if (pattern.size() < pixel_count * 2U) {
        throw std::runtime_error("ATEN cursor pattern is truncated");
    }

    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const std::uint8_t first = pattern[pixel * 2U];
        const std::uint8_t second = pattern[pixel * 2U + 1U];
        const std::size_t output = pixel * 4U;
        cursor.rgba[output + 0] = static_cast<std::uint8_t>((second & 0x0fU) << 4U);
        cursor.rgba[output + 1] = static_cast<std::uint8_t>(first & 0xf0U);
        cursor.rgba[output + 2] = static_cast<std::uint8_t>((first & 0x0fU) << 4U);
        cursor.rgba[output + 3] = (first != 0 || (second & 0x0fU) != 0) ? 255U : 0U;
    }

    return cursor;
}

void store_aten_frame(AtenViewState& state, AtenViewFrame frame)
{
    std::lock_guard lock(state.frame_mutex);
    frame.sequence = ++state.frame_sequence;
    state.frame = std::make_shared<AtenViewFrame>(std::move(frame));
}

void store_aten_cursor(AtenViewState& state, AtenCursorImage cursor)
{
    std::lock_guard lock(state.cursor_mutex);
    cursor.sequence = ++state.cursor_sequence;
    state.cursor = std::make_shared<AtenCursorImage>(std::move(cursor));
}

std::shared_ptr<const AtenViewFrame> take_latest_aten_frame(
    AtenViewState& state,
    std::uint64_t last_sequence)
{
    std::lock_guard lock(state.frame_mutex);
    if (!state.frame || state.frame->sequence == last_sequence) {
        return {};
    }
    return state.frame;
}

std::shared_ptr<const AtenCursorImage> take_latest_aten_cursor(
    AtenViewState& state,
    std::uint64_t last_sequence)
{
    std::lock_guard lock(state.cursor_mutex);
    if (!state.cursor || state.cursor->sequence == last_sequence) {
        return {};
    }
    return state.cursor;
}

void set_aten_status(AtenViewState& state, std::string status)
{
    std::lock_guard lock(state.control_mutex);
    state.status = std::move(status);
}

std::string aten_status_snapshot(AtenViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.status;
}

void set_aten_exception(AtenViewState& state, std::exception_ptr exception)
{
    std::lock_guard lock(state.control_mutex);
    state.exception = exception;
}

std::exception_ptr take_aten_exception(AtenViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    return state.exception;
}

void set_aten_force_close(AtenViewState& state, std::function<void()> force_close)
{
    std::lock_guard lock(state.control_mutex);
    state.force_close = std::move(force_close);
}

std::vector<std::uint8_t> make_aten_key_event(std::uint32_t usage, bool down)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(17);
    packet.push_back(4);
    packet.push_back(0);
    packet.push_back(down ? 1U : 0U);
    append_be16(packet, 0);
    append_be32(packet, usage);
    packet.insert(packet.end(), 9, 0);
    return packet;
}

std::vector<std::uint8_t> make_aten_pointer_event(int x, int y, std::uint8_t mask)
{
    std::vector<std::uint8_t> packet;
    packet.reserve(17);
    packet.push_back(5);
    packet.push_back(0);
    packet.push_back(mask);
    append_be16(packet, static_cast<std::uint16_t>(std::clamp(x, 0, 65535)));
    append_be16(packet, static_cast<std::uint16_t>(std::clamp(y, 0, 65535)));
    packet.insert(packet.end(), 11, 0);
    return packet;
}

std::vector<std::uint8_t> make_aten_cursor_position_request()
{
    return std::vector<std::uint8_t>{25};
}

std::vector<std::uint8_t> make_aten_mouse_sync_request()
{
    std::vector<std::uint8_t> packet;
    packet.reserve(3);
    packet.push_back(7);
    append_be16(packet, 1920);
    return packet;
}

bool is_coalescible_aten_mouse_motion(const std::vector<std::uint8_t>& packet)
{
    return packet.size() >= 3 && packet[0] == 5 && packet[2] == 0;
}

void queue_aten_input_packet(
    AtenViewState& state,
    std::vector<std::uint8_t> packet,
    bool coalesce_mouse_motion)
{
    std::function<void(std::vector<std::uint8_t>, bool)> input_sink;
    {
        std::lock_guard lock(state.control_mutex);
        input_sink = state.input_sink;
        if (!input_sink && coalesce_mouse_motion &&
            !state.pending_input.empty() &&
            is_coalescible_aten_mouse_motion(state.pending_input.back().packet)) {
            state.pending_input.back() = PendingAtenInputPacket{std::move(packet), coalesce_mouse_motion};
            return;
        }
        if (!input_sink) {
            if (state.pending_input.size() >= kMaxQueuedInputPackets) {
                state.pending_input.pop_front();
            }
            state.pending_input.push_back(PendingAtenInputPacket{std::move(packet), coalesce_mouse_motion});
            return;
        }
    }

    input_sink(std::move(packet), coalesce_mouse_motion);
}

void install_aten_input_sink(
    AtenViewState& state,
    std::function<void(std::vector<std::uint8_t>, bool)> input_sink)
{
    std::deque<PendingAtenInputPacket> pending;
    {
        std::lock_guard lock(state.control_mutex);
        state.input_sink = input_sink;
        pending.swap(state.pending_input);
    }

    for (PendingAtenInputPacket& packet : pending) {
        input_sink(std::move(packet.packet), packet.coalesce_mouse_motion);
    }
}

void clear_aten_input_sink(AtenViewState& state)
{
    std::lock_guard lock(state.control_mutex);
    state.input_sink = {};
    state.pending_input.clear();
}

void queue_aten_key_event(
    AtenViewState& state,
    std::uint32_t usage,
    bool down,
    bool verbose)
{
    (void)verbose;
    queue_aten_input_packet(state, make_aten_key_event(usage, down), false);
}

void release_all_aten_keys(AtenViewState& state, AtenKeyDownState& key_down, bool verbose)
{
    for (std::size_t usage = 0; usage < key_down.size(); ++usage) {
        if (!key_down[usage]) {
            continue;
        }
        key_down[usage] = false;
        queue_aten_key_event(state, static_cast<std::uint32_t>(usage), false, verbose);
    }
}

void queue_aten_pointer_event(
    AtenViewState& state,
    const AtenRemoteMousePosition& position,
    std::uint8_t mask,
    bool coalesce,
    bool verbose)
{
    (void)verbose;
    queue_aten_input_packet(state, make_aten_pointer_event(position.x, position.y, mask), coalesce);
}

void stop_aten_network(
    AtenViewState& state,
    std::atomic_bool& stop_requested,
    std::thread& network_thread,
    bool verbose)
{
    const auto started_at = std::chrono::steady_clock::now();
    if (verbose) {
        log_debug() << "aten network stop begin";
    }
    stop_requested.store(true);

    std::function<void()> force_close;
    {
        std::lock_guard lock(state.control_mutex);
        force_close = state.force_close;
    }
    if (force_close) {
        if (verbose) {
            log_debug() << "aten network force-close begin";
        }
        force_close();
        if (verbose) {
            log_debug() << "aten network force-close returned";
        }
    } else {
        if (verbose) {
            log_debug() << "aten network force-close missing";
        }
    }
    const auto force_closed_at = std::chrono::steady_clock::now();

    if (network_thread.joinable()) {
        if (verbose) {
            log_debug() << "aten network join begin";
        }
        network_thread.join();
        if (verbose) {
            log_debug() << "aten network join returned";
        }
    }
    const auto stopped_at = std::chrono::steady_clock::now();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        stopped_at - started_at).count();
    if (stop_ms > 250) {
        const auto force_close_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            force_closed_at - started_at).count();
        log_warning() << "aten network stop waited"
                      << " total-ms=" << stop_ms
                      << " force-close-ms=" << force_close_ms;
    }
}

std::vector<std::uint8_t> buffer_bytes(const beast::flat_buffer& buffer)
{
    std::vector<std::uint8_t> bytes(boost::asio::buffer_size(buffer.data()));
    boost::asio::buffer_copy(boost::asio::buffer(bytes), buffer.data());
    return bytes;
}

std::string ascii_from_bytes(const std::vector<std::uint8_t>& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

std::string hex_preview(const std::vector<std::uint8_t>& bytes, std::size_t limit = 16)
{
    std::ostringstream output;
    const std::size_t preview = std::min(bytes.size(), limit);
    for (std::size_t i = 0; i < preview; ++i) {
        if (i != 0) {
            output << ' ';
        }
        output << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(bytes[i])
               << std::dec << std::setfill(' ');
    }
    if (bytes.size() > preview) {
        output << " ...";
    }
    return output.str();
}

std::string security_type_name(std::uint8_t type)
{
    switch (type) {
    case 1:
        return "None";
    case 2:
        return "VNCAuth";
    case 14:
        return "Tight";
    case 15:
        return "Insyde";
    case 16:
        return "Insyde";
    case 22:
        return "XVP";
    default:
        return "unknown";
    }
}

std::string encoding_name(std::int32_t encoding)
{
    switch (encoding) {
    case 0:
        return "RAW";
    case 1:
        return "COPYRECT";
    case 2:
        return "RRE";
    case 5:
        return "HEXTILE";
    case 7:
        return "TIGHT";
    case 87:
        return "AST2100";
    case 88:
        return "AST2100_JPEG";
    case 89:
        return "RAW_NUVOTON";
    case -223:
        return "DesktopSize";
    case -224:
        return "last_rect";
    case -239:
        return "Cursor";
    case -260:
        return "TIGHT_PNG";
    case -309:
        return "xvp";
    default:
        return "unknown";
    }
}

std::string aten_server_message_name(std::uint8_t type)
{
    switch (type) {
    case 0:
        return "FRAMEBUFFER_UPDATE";
    case 2:
        return "BELL";
    case 4:
        return "INSYDE_CURSOR_POSITION";
    case 22:
        return "INSYDE_MESSAGE_22";
    case 53:
        return "INSYDE_MOUSE_MODE_53";
    case 54:
        return "INSYDE_MOUSE_MODE_54";
    case 55:
        return "INSYDE_MOUSE_MODE_55";
    case 57:
        return "INSYDE_CONTROL_MESSAGE";
    case 60:
        return "INSYDE_SERVICE_60";
    case 63:
        return "INSYDE_SERVICE_63";
    default:
        return "unknown";
    }
}

std::string aten_client_packet_name(std::uint8_t type)
{
    switch (type) {
    case 3:
        return "FRAMEBUFFER_UPDATE_REQUEST";
    case 4:
        return "KEY_EVENT";
    case 5:
        return "POINTER_EVENT";
    case 7:
        return "MOUSE_SYNC";
    case 25:
        return "CURSOR_POSITION_REQUEST";
    case 54:
        return "SET_MOUSE_MODE";
    case 55:
        return "GET_MOUSE_MODE";
    case 63:
        return "VM_SERVICE";
    default:
        return "unknown";
    }
}

std::string describe_aten_client_packet(const std::vector<std::uint8_t>& packet)
{
    if (packet.empty()) {
        return "empty";
    }

    std::ostringstream description;
    description << "type=" << static_cast<int>(packet.front())
                << " " << aten_client_packet_name(packet.front())
                << " bytes=" << packet.size();
    if (packet.front() == 3 && packet.size() >= 10) {
        description << " incremental=" << static_cast<int>(packet[1])
                    << " xy=" << load_be16(packet, 2) << ',' << load_be16(packet, 4)
                    << " size=" << load_be16(packet, 6) << 'x' << load_be16(packet, 8);
    } else if (packet.front() == 4 && packet.size() >= 9) {
        description << " down=" << static_cast<int>(packet[2])
                    << " usage=" << load_be32(packet, 5);
    } else if (packet.front() == 5 && packet.size() >= 7) {
        description << " buttons=" << static_cast<int>(packet[2])
                    << " xy=" << load_be16(packet, 3) << ',' << load_be16(packet, 5);
    }
    return description.str();
}

bool is_supported_js_security_type(std::uint8_t type)
{
    return type <= 16 || type == 22 || type == 15;
}

void append_be16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::uint16_t load_be16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes.at(offset)) << 8)
        | static_cast<std::uint16_t>(bytes.at(offset + 1)));
}

std::uint32_t load_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes.at(offset)) << 24)
        | (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 16)
        | (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 8)
        | static_cast<std::uint32_t>(bytes.at(offset + 3));
}

std::uint32_t load_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes.at(offset))
        | (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8)
        | (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 16)
        | (static_cast<std::uint32_t>(bytes.at(offset + 3)) << 24);
}

std::int32_t load_be32_signed(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(load_be32(bytes, offset));
}

class RfbStream {
public:
    explicit RfbStream(AtenWebSocket& ws)
        : ws_(ws)
    {
    }

    std::vector<std::uint8_t> read_exact(std::size_t count)
    {
        while (queue_.size() < count) {
            read_message();
        }

        std::vector<std::uint8_t> bytes;
        bytes.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            bytes.push_back(queue_.front());
            queue_.pop_front();
        }
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

    std::int32_t read_be32_signed()
    {
        const std::vector<std::uint8_t> bytes = read_exact(4);
        return load_be32_signed(bytes, 0);
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

    std::deque<std::uint8_t> take_queued_bytes()
    {
        std::deque<std::uint8_t> queued;
        queued.swap(queue_);
        return queued;
    }

private:
    void read_message()
    {
        beast::flat_buffer buffer;
        ws_.read(buffer);
        const std::vector<std::uint8_t> bytes = buffer_bytes(buffer);
        queue_.insert(queue_.end(), bytes.begin(), bytes.end());
    }

    AtenWebSocket& ws_;
    std::deque<std::uint8_t> queue_;
};

std::string client_protocol_version(std::string_view server_version)
{
    if (server_version == "055.008") {
        return "055.008";
    }
    if (server_version == "004.000" || server_version == "004.001") {
        return "003.008";
    }
    if (server_version == "003.008") {
        return "003.008";
    }
    if (server_version == "003.007") {
        return "003.007";
    }
    if (server_version == "003.003" || server_version == "003.006" || server_version == "003.889") {
        return "003.003";
    }
    throw std::runtime_error("unsupported ATEN RFB protocol version: " + std::string(server_version));
}

bool version_uses_security_types(std::string_view client_version)
{
    return client_version != "003.003";
}

bool version_uses_security_result_for_none(std::string_view client_version)
{
    return client_version == "003.008" || client_version == "055.008";
}

std::uint8_t negotiate_security(RfbStream& rfb, std::string_view client_version)
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

void authenticate_insyde(RfbStream& rfb, std::string_view credential, bool verbose)
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

void authenticate(RfbStream& rfb, std::uint8_t security_type, std::string_view credential, bool verbose)
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

void read_security_result(RfbStream& rfb, bool verbose)
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

AtenRfbServerInit read_server_init(RfbStream& rfb, bool insyde_extension)
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

std::vector<std::uint8_t> make_framebuffer_update_request(
    std::uint16_t width,
    std::uint16_t height,
    bool incremental)
{
    std::vector<std::uint8_t> request;
    request.push_back(3);
    request.push_back(incremental ? 1 : 0);
    append_be16(request, 0);
    append_be16(request, 0);
    append_be16(request, width);
    append_be16(request, height);
    return request;
}

AtenFramebufferRect read_vendor_rect_header(RfbStream& rfb)
{
    const std::vector<std::uint8_t> header = rfb.read_exact(20);
    AtenFramebufferRect rect;
    rect.x = load_be16(header, 0);
    rect.y = load_be16(header, 2);
    rect.width = load_be16(header, 4);
    rect.height = load_be16(header, 6);
    rect.encoding = load_be32_signed(header, 8);
    rect.mode = load_be32_signed(header, 12);
    rect.data_length = load_be32(header, 16);
    return rect;
}

AtenAstPayloadHeader read_ast_payload_header(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < 4) {
        throw std::runtime_error("ATEN AST2100 payload is too short");
    }

    AtenAstPayloadHeader header;
    header.y_selector = payload[0];
    header.uv_selector = payload[1];
    header.mode = (static_cast<unsigned>(payload[2]) << 8) | static_cast<unsigned>(payload[3]);
    header.mode420 = header.mode == 444 ? 0U : 1U;
    return header;
}

bool ast_payload_is_frame_end_only(const std::vector<std::uint8_t>& payload)
{
    return payload.size() >= 8 && load_le32(payload, 4) == kAtenAstFrameEndWord;
}

class AtenProtocolRfbSession : public std::enable_shared_from_this<AtenProtocolRfbSession> {
public:
    AtenProtocolRfbSession(
        asio::io_context& io,
        AtenViewState& state,
        AtenViewOptions options,
        AtenRfbServerInit init,
        std::atomic_bool& stop_requested)
        : io_(io)
        , state_(state)
        , options_(std::move(options))
        , init_(std::move(init))
        , stop_requested_(stop_requested)
        , strand_(asio::make_strand(io))
        , framebuffer_request_timer_(io)
    {
    }

    void set_send_packet_callback(std::function<void(std::vector<std::uint8_t>, bool)> callback)
    {
        send_packet_ = std::move(callback);
    }

    void set_request_close_callback(std::function<void()> callback)
    {
        request_close_ = std::move(callback);
    }

    void start(std::deque<std::uint8_t> initial_bytes)
    {
        auto self = shared_from_this();
        asio::post(strand_, [self, initial_bytes = std::move(initial_bytes)]() mutable {
            if (self->closed_ || self->stop_requested_.load()) {
                return;
            }
            set_aten_status(self->state_, "waiting for video");
            self->receive_queue_ = std::move(initial_bytes);
            self->queue_framebuffer_update_request(false);
            if (self->options_.login.verbose) {
                log_info() << "requested ATEN framebuffer update";
            }
            self->schedule_receive_parse();
        });
    }

    void enqueue_bytes(std::vector<std::uint8_t> bytes)
    {
        auto self = shared_from_this();
        asio::post(strand_, [self, bytes = std::move(bytes)] {
            if (self->closed_ || self->stop_requested_.load()) {
                return;
            }
            self->stats_rx_bytes_ += bytes.size();
            self->append_receive_bytes(bytes);
            self->schedule_receive_parse();
        });
    }

    void request_stop()
    {
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            if (self->closed_) {
                return;
            }
            self->closed_ = true;
            self->framebuffer_request_timer_.cancel();
            self->receive_queue_.clear();
        });
    }

private:
    bool can_read(std::size_t offset, std::size_t count) const
    {
        return offset <= receive_queue_.size() && count <= receive_queue_.size() - offset;
    }

    std::size_t receive_buffered_bytes() const
    {
        return receive_queue_.size();
    }

    std::uint8_t peek_u8(std::size_t offset) const
    {
        return receive_queue_.at(offset);
    }

    std::uint16_t peek_be16(std::size_t offset) const
    {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(receive_queue_.at(offset)) << 8)
            | static_cast<std::uint16_t>(receive_queue_.at(offset + 1)));
    }

    std::uint32_t peek_be32(std::size_t offset) const
    {
        return (static_cast<std::uint32_t>(receive_queue_.at(offset)) << 24)
            | (static_cast<std::uint32_t>(receive_queue_.at(offset + 1)) << 16)
            | (static_cast<std::uint32_t>(receive_queue_.at(offset + 2)) << 8)
            | static_cast<std::uint32_t>(receive_queue_.at(offset + 3));
    }

    std::uint8_t read_u8()
    {
        const std::uint8_t value = receive_queue_.front();
        receive_queue_.pop_front();
        return value;
    }

    std::uint16_t read_be16()
    {
        const std::uint16_t value = peek_be16(0);
        discard(2);
        return value;
    }

    std::uint32_t read_be32()
    {
        const std::uint32_t value = peek_be32(0);
        discard(4);
        return value;
    }

    std::int32_t read_be32_signed()
    {
        return static_cast<std::int32_t>(read_be32());
    }

    std::vector<std::uint8_t> read_bytes(std::size_t count)
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            bytes.push_back(receive_queue_.front());
            receive_queue_.pop_front();
        }
        return bytes;
    }

    void discard(std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i) {
            receive_queue_.pop_front();
        }
    }

    void append_receive_bytes(const std::vector<std::uint8_t>& bytes)
    {
        receive_queue_.insert(receive_queue_.end(), bytes.begin(), bytes.end());
    }

    AtenFramebufferRect read_rect_header()
    {
        AtenFramebufferRect rect;
        rect.x = read_be16();
        rect.y = read_be16();
        rect.width = read_be16();
        rect.height = read_be16();
        rect.encoding = read_be32_signed();
        rect.mode = read_be32_signed();
        rect.data_length = read_be32();
        return rect;
    }

    bool framebuffer_update_available() const
    {
        if (!can_read(0, 4)) {
            return false;
        }

        const std::uint16_t rect_count = peek_be16(2);
        std::size_t offset = 4;
        for (std::uint16_t i = 0; i < rect_count; ++i) {
            if (!can_read(offset, 20)) {
                return false;
            }
            const std::uint32_t data_length = peek_be32(offset + 16);
            offset += 20;
            if (!can_read(offset, data_length)) {
                return false;
            }
            offset += data_length;
        }
        return true;
    }

    bool cursor_position_available() const
    {
        if (!can_read(0, 21)) {
            return false;
        }

        const std::uint32_t width = peek_be32(9);
        const std::uint32_t height = peek_be32(13);
        const std::uint32_t valid = peek_be32(17);
        if (valid != 1) {
            return true;
        }

        const std::uint64_t pattern_size =
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 2U;
        if (pattern_size > 16U * 1024U * 1024U) {
            throw std::runtime_error("ATEN RFB cursor pattern is implausibly large");
        }
        return can_read(0, 25 + static_cast<std::size_t>(pattern_size));
    }

    bool message_available() const
    {
        if (receive_buffered_bytes() == 0) {
            return false;
        }

        switch (peek_u8(0)) {
        case 0:
            return framebuffer_update_available();
        case 2:
            return can_read(0, 1);
        case 4:
            return cursor_position_available();
        case 22:
            return can_read(0, 2);
        case 53:
        case 54:
        case 55:
            return can_read(0, 4);
        case 57:
            return can_read(0, 265);
        case 60:
        case 63:
            return can_read(0, 2);
        default:
            return true;
        }
    }

    bool parse_available_messages()
    {
        std::size_t parsed_messages = 0;
        while (!closed_ && !stop_requested_.load() && message_available()) {
            parse_one_message();
            ++parsed_messages;
            if (parsed_messages >= kMaxAtenMessagesPerReceiveDrain) {
                return false;
            }
        }
        return true;
    }

    void parse_one_message()
    {
        const std::uint8_t message_type = read_u8();
        ++total_messages_;
        ++stats_messages_;
        if (options_.login.verbose && message_type != 0) {
            log_info() << "aten rfb message #" << total_messages_
                       << " type=" << static_cast<int>(message_type)
                       << " " << aten_server_message_name(message_type)
                       << " buffered=" << receive_buffered_bytes();
        }
        switch (message_type) {
        case 0:
            handle_framebuffer_update();
            break;
        case 2:
            if (options_.login.verbose) {
                log_info() << "aten rfb bell";
            }
            break;
        case 4:
            handle_cursor_position();
            break;
        case 22:
            if (options_.login.verbose) {
                log_info() << "aten rfb message 22 value=" << static_cast<int>(read_u8());
            } else {
                discard(1);
            }
            break;
        case 53:
        case 54:
        case 55:
            handle_mouse_control(message_type);
            break;
        case 57:
            handle_control_message();
            break;
        case 60:
        case 63:
            if (options_.login.verbose) {
                log_info() << "aten rfb service"
                           << " type=" << static_cast<int>(message_type)
                           << " value=" << static_cast<int>(read_u8());
            } else {
                discard(1);
            }
            break;
        default:
            throw std::runtime_error(
                "unhandled ATEN RFB server message type " + std::to_string(message_type));
        }
    }

    void handle_framebuffer_update()
    {
        if (!init_.insyde_extension) {
            throw std::runtime_error("standard RFB framebuffer updates are not implemented yet");
        }

        discard(1);
        const std::uint16_t rect_count = read_be16();
        ++updates_;
        ++stats_framebuffer_updates_;
        if (options_.login.verbose && (updates_ <= 5 || updates_ % 120 == 0)) {
            log_info() << "aten rfb framebuffer update #" << updates_
                       << " rects=" << rect_count;
        }

        for (std::uint16_t i = 0; i < rect_count; ++i) {
            const AtenFramebufferRect rect = read_rect_header();
            std::vector<std::uint8_t> payload;
            if (rect.data_length != 0) {
                payload = read_bytes(rect.data_length);
            }

            if (options_.login.verbose && (updates_ <= 5 || updates_ % 120 == 0)) {
                LogLine line = log_info();
                line << "aten rfb rect"
                     << " index=" << (i + 1)
                     << " xy=" << rect.x << ',' << rect.y
                     << " size=" << rect.width << 'x' << rect.height
                     << " encoding=" << rect.encoding << '(' << encoding_name(rect.encoding) << ')'
                     << " mode=" << rect.mode
                     << " payload=" << rect.data_length;
                if (!payload.empty()) {
                    line << " first-bytes=" << hex_preview(payload);
                }
            }

            // TODO: ATEN cursor replay is disabled until it can be matched to the vendor JS exactly.
            // if (rect.encoding == 87) {
            //     observe_cursor_mode(rect.mode);
            // }

            if (rect.encoding != 87 || payload.empty()) {
                continue;
            }

            decode_ast_rect(rect, payload);
        }

        framebuffer_request_pending_ = false;

        schedule_framebuffer_update_request();
    }

    void decode_ast_rect(const AtenFramebufferRect& rect, const std::vector<std::uint8_t>& payload)
    {
        const AtenAstPayloadHeader ast = read_ast_payload_header(payload);
        // TODO: ATEN cursor/mouse side-channel replay is disabled until it can be matched
        // to the vendor JS exactly.
        // if ((previous_width_ != rect.width || previous_height_ != rect.height) &&
        //     rect.width > 0 && rect.height > 0) {
        //     queue_mouse_sync_request();
        // }

        if (ast_payload_is_frame_end_only(payload)) {
            previous_width_ = rect.width;
            previous_height_ = rect.height;
            if (options_.login.verbose && (updates_ <= 20 || updates_ % 60 == 0)) {
                log_info() << "skipped ATEN no-op frame #" << updates_
                           << " size=" << rect.width << 'x' << rect.height
                           << " payload=" << payload.size()
                           << " mode=" << ast.mode;
            }
            return;
        }

        std::vector<std::uint8_t> compressed(payload.begin() + 4, payload.end());
        if (compressed.empty()) {
            return;
        }

        AspeedDecodeOptions decode_options;
        decode_options.width = rect.width;
        decode_options.height = rect.height;
        decode_options.mode420 = ast.mode420;
        decode_options.jpeg_table_selector = ast.y_selector;
        decode_options.chroma_table_selector = ast.uv_selector;
        decode_options.advance_table_selector = 0;
        decode_options.advance_chroma_table_selector = 0;
        decode_options.use_separate_chroma_selectors = true;

        const bool can_reuse_previous =
            previous_width_ == rect.width &&
            previous_height_ == rect.height &&
            previous_rgba_.size() == static_cast<std::size_t>(rect.width) *
                    static_cast<std::size_t>(rect.height) * 4U;
        std::vector<std::uint8_t> rgba = decoder_.decode_rgba(
            decode_options,
            compressed,
            can_reuse_previous ? &previous_rgba_ : nullptr);

        AtenViewFrame frame;
        frame.width = rect.width;
        frame.height = rect.height;
        frame.rgba = rgba;
        store_aten_frame(state_, std::move(frame));

        previous_width_ = rect.width;
        previous_height_ = rect.height;
        previous_rgba_ = std::move(rgba);

        if (options_.login.verbose && (updates_ <= 20 || updates_ % 60 == 0)) {
            log_info() << "rendered ATEN frame #" << updates_
                       << " size=" << rect.width << 'x' << rect.height
                       << " compressed=" << compressed.size()
                       << " mode=" << ast.mode
                       << " y-sel=" << ast.y_selector
                       << " uv-sel=" << ast.uv_selector
                       << " avg-rgb=" << sampled_average_rgb(previous_rgba_);
        }
    }

    void observe_cursor_mode(std::int32_t mode)
    {
        (void)mode;
        // TODO: ATEN cursor replay is disabled until it can be matched to the vendor JS exactly.
#if 0
        if (!cursor_mode_initialized_ || cursor_mode_ != mode) {
            cursor_mode_initialized_ = true;
            cursor_mode_ = mode;
            cursor_position_request_needed_ = true;
            if (mode == 0) {
                AtenCursorImage cursor;
                cursor.visible = false;
                store_aten_cursor(state_, std::move(cursor));
            }
        }
#endif
    }

    void handle_cursor_position()
    {
        const std::uint32_t x = read_be32();
        const std::uint32_t y = read_be32();
        const std::uint32_t width = read_be32();
        const std::uint32_t height = read_be32();
        const std::uint32_t valid = read_be32();
        cursor_position_request_pending_ = false;

        std::uint32_t pattern_type = 0;
        std::uint64_t pattern_size = 0;
        if (valid == 1) {
            pattern_type = read_be32();
            pattern_size =
                static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 2U;
            if (pattern_size > 16U * 1024U * 1024U) {
                throw std::runtime_error("ATEN RFB cursor pattern is implausibly large");
            }
            discard(static_cast<std::size_t>(pattern_size));
        }

        if (options_.login.verbose) {
            LogLine line = log_info();
            line << "ignored ATEN cursor"
                 << " xy=" << x << ',' << y
                 << " size=" << width << 'x' << height
                 << " valid=" << valid;
            if (valid == 1) {
                line << " pattern-type=" << pattern_type
                     << " pattern-bytes=" << pattern_size;
            }
        }
    }

    void handle_mouse_control(std::uint8_t message_type)
    {
        const std::uint8_t crypto = read_u8();
        const std::uint8_t mode = read_u8();
        const std::uint8_t status = read_u8();
        if (options_.login.verbose) {
            log_info() << "aten rfb mouse/control"
                       << " type=" << static_cast<int>(message_type)
                       << " crypto=" << static_cast<int>(crypto)
                       << " mode=" << static_cast<int>(mode)
                       << " status=" << static_cast<int>(status);
        }
    }

    void handle_control_message()
    {
        const std::uint32_t count = read_be32();
        const std::uint32_t code_digits = read_be32();
        const std::vector<std::uint8_t> message = read_bytes(256);
        if (options_.login.verbose) {
            log_info() << "aten rfb control-message"
                       << " count=" << count
                       << " code-digits=" << code_digits
                       << " first-bytes=" << hex_preview(message);
        }
    }

    void schedule_receive_parse()
    {
        if (closed_ || receive_parse_posted_) {
            return;
        }

        receive_parse_posted_ = true;
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            self->parse_receive_queue();
        });
    }

    void parse_receive_queue()
    {
        receive_parse_posted_ = false;
        if (closed_ || stop_requested_.load()) {
            return;
        }

        try {
            const bool drained = parse_available_messages();
            log_stats_if_due();
            if (!drained && !closed_ && !stop_requested_.load() && message_available()) {
                schedule_receive_parse();
            }
        } catch (...) {
            set_aten_exception(state_, std::current_exception());
            close_protocol_and_network();
        }
    }

    void log_stats_if_due()
    {
        if (!options_.login.verbose) {
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
                   << " receive-buffer=" << receive_buffered_bytes();

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
        framebuffer_request_timer_.expires_after(kAtenFramebufferRequestBackoff);
        framebuffer_request_timer_.async_wait(
            asio::bind_executor(strand_, [self](beast::error_code error) {
                self->framebuffer_request_timer_active_ = false;
                if (error == boost::asio::error::operation_aborted || self->closed_ ||
                    self->stop_requested_.load()) {
                    return;
                }
                if (error) {
                    set_aten_exception(self->state_, std::make_exception_ptr(
                        beast::system_error(error, "ATEN framebuffer request timer failed")));
                    self->close_protocol_and_network();
                    return;
                }

                self->queue_framebuffer_update_request(true);
                // TODO: ATEN cursor replay is disabled until it can be matched to the vendor JS exactly.
                // self->queue_cursor_position_request_if_needed();
            }));
    }

    void queue_framebuffer_update_request(bool incremental)
    {
        if (closed_ || stop_requested_.load() || framebuffer_request_pending_) {
            return;
        }

        const auto request_width = static_cast<std::uint16_t>(
            previous_width_ > 0 ? previous_width_ : init_.width);
        const auto request_height = static_cast<std::uint16_t>(
            previous_height_ > 0 ? previous_height_ : init_.height);
        framebuffer_request_pending_ = true;
        send_packet(make_framebuffer_update_request(request_width, request_height, incremental), false);
    }

    void queue_cursor_position_request_if_needed()
    {
        if (!cursor_position_request_needed_) {
            return;
        }

        cursor_position_request_needed_ = false;
        queue_cursor_position_request();
    }

    void queue_cursor_position_request()
    {
        if (closed_ || stop_requested_.load() || cursor_position_request_pending_) {
            return;
        }

        cursor_position_request_pending_ = true;
        send_packet(make_aten_cursor_position_request(), false);
    }

    void queue_mouse_sync_request()
    {
        if (closed_ || stop_requested_.load()) {
            return;
        }

        send_packet(make_aten_mouse_sync_request(), false);
    }

    void send_packet(std::vector<std::uint8_t> packet, bool coalesce_mouse_motion)
    {
        if (send_packet_) {
            send_packet_(std::move(packet), coalesce_mouse_motion);
        }
    }

    void close_protocol_and_network()
    {
        if (closed_) {
            return;
        }
        closed_ = true;
        framebuffer_request_timer_.cancel();
        if (request_close_) {
            request_close_();
        }
    }

    asio::io_context& io_;
    AtenViewState& state_;
    AtenViewOptions options_;
    AtenRfbServerInit init_;
    std::deque<std::uint8_t> receive_queue_;
    std::function<void(std::vector<std::uint8_t>, bool)> send_packet_;
    std::function<void()> request_close_;
    std::atomic_bool& stop_requested_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer framebuffer_request_timer_;
    AspeedDecoder decoder_;
    std::vector<std::uint8_t> previous_rgba_;
    std::chrono::steady_clock::time_point stats_started_at_ = std::chrono::steady_clock::now();
    std::uint64_t total_messages_ = 0;
    std::uint64_t stats_rx_bytes_ = 0;
    std::uint64_t stats_messages_ = 0;
    std::uint64_t stats_framebuffer_updates_ = 0;
    int previous_width_ = 0;
    int previous_height_ = 0;
    int updates_ = 0;
    std::int32_t cursor_mode_ = 0;
    bool framebuffer_request_pending_ = false;
    bool framebuffer_request_timer_active_ = false;
    bool cursor_mode_initialized_ = false;
    bool cursor_position_request_needed_ = false;
    bool cursor_position_request_pending_ = false;
    bool receive_parse_posted_ = false;
    bool closed_ = false;
};

class AtenNetworkRfbSession : public std::enable_shared_from_this<AtenNetworkRfbSession> {
public:
    AtenNetworkRfbSession(
        asio::io_context& io,
        AtenWebSocket& ws,
        AtenViewState& state,
        AtenViewOptions options,
        std::shared_ptr<AtenProtocolRfbSession> protocol,
        std::atomic_bool& stop_requested)
        : io_(io)
        , ws_(ws)
        , state_(state)
        , options_(std::move(options))
        , protocol_(std::move(protocol))
        , stop_requested_(stop_requested)
        , strand_(asio::make_strand(io))
    {
    }

    void start()
    {
        auto weak = weak_from_this();
        install_aten_input_sink(
            state_,
            [weak](
                std::vector<std::uint8_t> packet,
                bool coalesce_mouse_motion) mutable {
                if (auto self = weak.lock()) {
                    self->enqueue_input(std::move(packet), coalesce_mouse_motion);
                }
            });

        auto self = shared_from_this();
        asio::dispatch(strand_, [self] {
            self->start_read();
        });
    }

    void send_packet(std::vector<std::uint8_t> packet, bool coalesce_mouse_motion)
    {
        auto self = shared_from_this();
        asio::post(strand_, [self, packet = std::move(packet), coalesce_mouse_motion]() mutable {
            if (self->closed_ || self->stop_requested_.load()) {
                return;
            }
            self->queue_write(std::move(packet), coalesce_mouse_motion);
        });
    }

    void request_stop()
    {
        if (options_.login.verbose) {
            log_debug() << "aten websocket stop requested";
        }
        auto self = shared_from_this();
        asio::post(strand_, [self] {
            if (self->options_.login.verbose) {
                log_debug() << "aten websocket stop handler begin";
            }
            self->stop_requested_.store(true);
            self->close_now();
            if (self->options_.login.verbose) {
                log_debug() << "aten websocket stop handler end";
            }
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

    void enqueue_input(std::vector<std::uint8_t> packet, bool coalesce_mouse_motion)
    {
        if (coalesce_mouse_motion) {
            bool should_post = false;
            {
                std::lock_guard lock(input_mutex_);
                pending_mouse_motion_ = std::move(packet);
                if (!mouse_motion_flush_posted_) {
                    mouse_motion_flush_posted_ = true;
                    should_post = true;
                }
            }

            if (should_post) {
                auto self = shared_from_this();
                asio::post(strand_, [self] {
                    self->flush_pending_mouse_motion();
                });
            }
            return;
        }

        auto self = shared_from_this();
        asio::post(strand_, [self, packet = std::move(packet), coalesce_mouse_motion]() mutable {
            if (self->closed_ || self->stop_requested_.load()) {
                return;
            }
            self->queue_write(std::move(packet), coalesce_mouse_motion);
        });
    }

    void flush_pending_mouse_motion()
    {
        std::vector<std::uint8_t> packet;
        {
            std::lock_guard lock(input_mutex_);
            if (!pending_mouse_motion_) {
                mouse_motion_flush_posted_ = false;
                return;
            }
            packet = std::move(*pending_mouse_motion_);
            pending_mouse_motion_.reset();
            mouse_motion_flush_posted_ = false;
        }

        if (closed_ || stop_requested_.load()) {
            return;
        }

        queue_write(std::move(packet), true);
    }

    void queue_write(std::vector<std::uint8_t> packet, bool coalesce_mouse_motion)
    {
        if (closed_) {
            return;
        }

        if (coalesce_mouse_motion && !write_queue_.empty() &&
            is_coalescible_aten_mouse_motion(write_queue_.back().packet)) {
            write_queue_.back() = AtenQueuedWrite{std::move(packet)};
        } else {
            if (write_queue_.size() >= kMaxQueuedInputPackets) {
                write_queue_.pop_front();
            }
            write_queue_.push_back(AtenQueuedWrite{std::move(packet)});
        }
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
            if (options_.login.verbose || !stop_requested_.load()) {
                log_error() << "aten websocket write callback error="
                            << error.message();
            }
            if (!stop_requested_.load()) {
                set_aten_exception(state_, std::make_exception_ptr(
                    beast::system_error(error, "ATEN websocket write failed")));
            }
            close_now();
            return;
        }

        if (options_.login.verbose && !is_hot_write_packet(active_write_.packet)) {
            log_info() << "sent ATEN packet " << describe_aten_client_packet(active_write_.packet);
        }
        active_write_ = {};
        start_write();
    }

    void start_read()
    {
        if (closed_) {
            return;
        }

        auto self = shared_from_this();
        ws_.async_read(
            read_buffer_,
            asio::bind_executor(strand_, [self](beast::error_code error, std::size_t) {
                self->on_read(error);
            }));
    }

    void on_read(beast::error_code error)
    {
        if (closed_) {
            return;
        }
        if (error) {
            if (options_.login.verbose || (!stop_requested_.load() && error != websocket::error::closed)) {
                log_error() << "aten websocket read callback error="
                            << error.message();
            }
            if (!stop_requested_.load() && error != websocket::error::closed) {
                set_aten_exception(state_, std::make_exception_ptr(
                    beast::system_error(error, "ATEN websocket read failed")));
            }
            close_now();
            return;
        }

        std::vector<std::uint8_t> bytes = buffer_bytes(read_buffer_);
        read_buffer_.consume(read_buffer_.size());
        ++total_websocket_messages_;
        if (options_.login.verbose &&
            (total_websocket_messages_ <= 5 || total_websocket_messages_ % 120 == 0)) {
            LogLine line = log_info();
            line << "aten websocket message"
                 << " bytes=" << bytes.size();
            if (!bytes.empty()) {
                line << " first-bytes=" << hex_preview(bytes);
            }
        }
        if (protocol_) {
            protocol_->enqueue_bytes(std::move(bytes));
        }

        start_read();
    }

    void close_now()
    {
        if (closed_) {
            if (options_.login.verbose) {
                log_debug() << "aten websocket close_now ignored already-closed";
            }
            return;
        }
        if (options_.login.verbose) {
            log_debug() << "aten websocket close_now begin";
        }
        closed_ = true;
        clear_aten_input_sink(state_);
        if (protocol_) {
            protocol_->request_stop();
        }

        clear_pending_mouse_motion();

        beast::error_code error;
        beast::get_lowest_layer(ws_).socket().cancel(error);
        if (error && options_.login.verbose) {
            log_error() << "aten websocket stream cancel error="
                        << error.message();
        }
        error.clear();
        beast::get_lowest_layer(ws_).socket().shutdown(tcp::socket::shutdown_both, error);
        if (error && options_.login.verbose) {
            log_error() << "aten websocket socket shutdown error="
                        << error.message();
        }
        error.clear();
        beast::get_lowest_layer(ws_).socket().close(error);
        if (error && options_.login.verbose) {
            log_error() << "aten websocket socket close error="
                        << error.message();
        }
        io_.stop();
        if (options_.login.verbose) {
            log_debug() << "aten websocket close_now end";
        }
    }

    void clear_pending_mouse_motion()
    {
        std::lock_guard lock(input_mutex_);
        pending_mouse_motion_.reset();
        mouse_motion_flush_posted_ = false;
    }

    asio::io_context& io_;
    AtenWebSocket& ws_;
    AtenViewState& state_;
    AtenViewOptions options_;
    std::shared_ptr<AtenProtocolRfbSession> protocol_;
    std::atomic_bool& stop_requested_;
    asio::strand<asio::io_context::executor_type> strand_;
    beast::flat_buffer read_buffer_;
    std::deque<AtenQueuedWrite> write_queue_;
    AtenQueuedWrite active_write_;
    std::mutex input_mutex_;
    std::optional<std::vector<std::uint8_t>> pending_mouse_motion_;
    std::uint64_t total_websocket_messages_ = 0;
    bool write_in_progress_ = false;
    bool mouse_motion_flush_posted_ = false;
    bool closed_ = false;
};

} // namespace

namespace {

void run_aten_network_session(const AtenViewOptions& options, AtenViewState& state, std::atomic_bool& stop_requested)
{
    set_aten_status(state, "logging in");
    AtenSession session = login_aten(options.login);
    AtenLogoutGuard logout_guard(options.login);
    logout_guard.arm(session);
    log_info() << "aten login succeeded";
    if (options.login.verbose) {
        log_info() << "cookies stored: " << session.cookies.size();
    }

    std::string rfb_credential = options.login.username;
    const std::string bootstrap_credential =
        fetch_aten_ikvm_bootstrap(options.login, session.cookies);
    if (!bootstrap_credential.empty()) {
        rfb_credential = bootstrap_credential;
    } else {
        log_warning() << "aten warning: bootstrap did not expose entry_value; "
                         "falling back to login username for RFB auth";
    }

    set_aten_status(state, "connecting websocket");
    asio::io_context io;
    ssl::context tls_context(ssl::context::tls_client);
    AtenWebSocket ws(io, tls_context);
    tcp::resolver resolver(io);

    configure_tls(tls_context, ws.next_layer(), options.login.base_url.host, options.login.insecure);
    set_server_name_indication(ws.next_layer(), options.login.base_url.host);

    const auto endpoints = resolver.resolve(options.login.base_url.host, options.login.base_url.port);
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(ws).connect(endpoints);
    beast::get_lowest_layer(ws).socket().set_option(tcp::no_delay(true));
    ws.next_layer().handshake(ssl::stream_base::client);
    beast::get_lowest_layer(ws).expires_never();

    websocket::stream_base::timeout timeout;
    timeout.handshake_timeout = std::chrono::seconds(30);
    if (options.idle_timeout_seconds > 0) {
        timeout.idle_timeout = std::chrono::seconds(options.idle_timeout_seconds);
        timeout.keep_alive_pings = true;
    } else {
        timeout.idle_timeout = websocket::stream_base::none();
        timeout.keep_alive_pings = false;
    }
    ws.set_option(timeout);

    const std::string host = make_host_header(options.login.base_url);
    ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& request) {
        request.set(http::field::user_agent, std::string(kName) + "/" + std::string(BOOST_LIB_VERSION));
        request.set(http::field::origin, make_origin(options.login.base_url));

        const std::string cookie_header = session.cookies.header();
        if (!cookie_header.empty()) {
            request.set(http::field::cookie, cookie_header);
        }
    }));

    const std::string path = "/";
    websocket::response_type response;
    ws.handshake(response, host, path);
    {
        LogLine line = log_info();
        line << "aten websocket connected"
             << " path=" << path;
        if (options.idle_timeout_seconds > 0) {
            line << " idle-timeout=" << options.idle_timeout_seconds << "s";
        } else {
            line << " idle-timeout=disabled";
        }
    }
    set_aten_force_close(state, [&ws] {
        beast::error_code error;
        beast::get_lowest_layer(ws).socket().cancel(error);
        error.clear();
        beast::get_lowest_layer(ws).socket().shutdown(tcp::socket::shutdown_both, error);
        error.clear();
        beast::get_lowest_layer(ws).socket().close(error);
    });
    struct ForceCloseReset {
        AtenViewState& state;

        ~ForceCloseReset()
        {
            set_aten_force_close(state, {});
        }
    } force_close_reset{state};

    RfbStream rfb(ws);
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

    asio::io_context protocol_io;
    auto protocol_work = asio::make_work_guard(protocol_io);
    std::thread protocol_thread([&protocol_io] {
        protocol_io.run();
    });
    struct ProtocolIoGuard {
        asio::io_context& io;
        decltype(protocol_work)& work;
        std::thread& thread;

        ~ProtocolIoGuard()
        {
            work.reset();
            io.stop();
            if (thread.joinable()) {
                thread.join();
            }
        }
    } protocol_guard{protocol_io, protocol_work, protocol_thread};

    auto protocol_session = std::make_shared<AtenProtocolRfbSession>(
        protocol_io,
        state,
        options,
        init,
        stop_requested);
    auto network_session = std::make_shared<AtenNetworkRfbSession>(
        io,
        ws,
        state,
        options,
        protocol_session,
        stop_requested);
    std::weak_ptr<AtenNetworkRfbSession> weak_network = network_session;
    protocol_session->set_send_packet_callback(
        [weak_network](std::vector<std::uint8_t> packet, bool coalesce_mouse_motion) {
            if (auto network = weak_network.lock()) {
                network->send_packet(std::move(packet), coalesce_mouse_motion);
            }
        });
    protocol_session->set_request_close_callback([weak_network] {
        if (auto network = weak_network.lock()) {
            network->request_stop();
        }
    });
    {
        set_aten_force_close(state, [weak_network] {
            if (auto network = weak_network.lock()) {
                network->request_stop();
            }
        });
    }

    network_session->start();
    protocol_session->start(rfb.take_queued_bytes());
    const auto io_started_at = std::chrono::steady_clock::now();
    if (options.login.verbose) {
        log_debug() << "aten network io running";
    }
    io.run();
    if (options.login.verbose) {
        const auto io_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - io_started_at).count();
        log_debug() << "aten network io stopped duration-ms=" << io_elapsed_ms;
    }
    protocol_session->request_stop();
    clear_aten_input_sink(state);
    set_aten_force_close(state, {});
}

} // namespace

void run_aten_view(const AtenViewOptions& options)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw_sdl_error("SDL_Init");
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Texture* cursor_texture = nullptr;
    auto state = std::make_shared<AtenViewState>();
    auto stop_requested = std::make_shared<std::atomic_bool>(false);
    auto network_done = std::make_shared<std::atomic_bool>(false);
    std::thread network_thread;

    try {
        window = SDL_CreateWindow("hitsc - ATEN", 1024, 768, SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            throw_sdl_error("SDL_CreateWindow");
        }

        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) {
            throw_sdl_error("SDL_CreateRenderer");
        }

        AtenViewOptions network_options = options;
        network_thread = std::thread([network_options, state, stop_requested, network_done] {
            try {
                run_aten_network_session(network_options, *state, *stop_requested);
            } catch (...) {
                set_aten_exception(*state, std::current_exception());
            }
            set_aten_force_close(*state, {});
            network_done->store(true);
        });

        bool running = true;
        std::uint64_t last_sequence = 0;
        std::uint64_t last_cursor_sequence = 0;
        std::uint64_t last_status_tick = 0;
        int texture_width = 0;
        int texture_height = 0;
        int cursor_texture_width = 0;
        int cursor_texture_height = 0;
        std::shared_ptr<const AtenCursorImage> current_cursor;
        int presented_frames = 0;
        std::uint8_t mouse_buttons = 0;
        std::uint64_t last_mouse_motion_ticks = 0;
        AtenKeyDownState key_down{};
        bool first_render = true;
        bool close_event_logged = false;
        while (running) {
            bool render_needed = first_render;
            SDL_Event event{};
            bool have_event = SDL_WaitEventTimeout(&event, 16);
            while (have_event) {
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    if (!close_event_logged) {
                        close_event_logged = true;
                        log_info() << "aten window close event"
                                   << " type=" << event.type;
                    }
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                           event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                           event.type == SDL_EVENT_WINDOW_EXPOSED) {
                    render_needed = true;
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    release_all_aten_keys(*state, key_down, options.login.verbose);
                } else if (event.type == SDL_EVENT_KEY_DOWN ||
                           event.type == SDL_EVENT_KEY_UP) {
                    if (!(event.type == SDL_EVENT_KEY_DOWN && event.key.repeat)) {
                        const std::optional<std::uint32_t> usage =
                            aten_keyboard_usage_from_sdl_scancode(event.key.scancode);
                        if (!usage || *usage >= key_down.size()) {
                            if (options.login.verbose) {
                                log_info() << "ignored ATEN key"
                                           << " scancode=" << event.key.scancode
                                           << " key=" << event.key.key;
                            }
                        } else {
                            const bool down = event.type == SDL_EVENT_KEY_DOWN;
                            if (key_down[*usage] != down) {
                                key_down[*usage] = down;
                                queue_aten_key_event(*state, *usage, down, options.login.verbose);
                            }
                        }
                    }
                } else if (texture_width > 0 && texture_height > 0 &&
                           (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
                    const std::uint8_t mask = button_mask_for_sdl_button(event.button.button);
                    if (mask != 0) {
                        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                            mouse_buttons |= mask;
                        } else {
                            mouse_buttons &= static_cast<std::uint8_t>(~mask);
                        }
                        SDL_CaptureMouse(mouse_buttons != 0);

                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
                            event.button.x,
                            event.button.y,
                            target,
                            texture_width,
                            texture_height);
                        if (position) {
                            queue_aten_pointer_event(
                                *state,
                                *position,
                                mouse_buttons,
                                false,
                                options.login.verbose);
                        }
                    }
                } else if (texture_width > 0 && texture_height > 0 &&
                           event.type == SDL_EVENT_MOUSE_MOTION) {
                    const std::uint64_t ticks = SDL_GetTicks();
                    const bool throttled =
                        mouse_buttons == 0 &&
                        ticks - last_mouse_motion_ticks < kMouseMotionIntervalMilliseconds;
                    if (!throttled) {
                        const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                        const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
                            event.motion.x,
                            event.motion.y,
                            target,
                            texture_width,
                            texture_height);
                        if (position) {
                            queue_aten_pointer_event(
                                *state,
                                *position,
                                mouse_buttons,
                                mouse_buttons == 0,
                                options.login.verbose);
                            last_mouse_motion_ticks = ticks;
                        }
                    }
                } else if (texture_width > 0 && texture_height > 0 &&
                           event.type == SDL_EVENT_MOUSE_WHEEL) {
                    const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                    const std::optional<AtenRemoteMousePosition> position = remote_mouse_position(
                        event.wheel.mouse_x,
                        event.wheel.mouse_y,
                        target,
                        texture_width,
                        texture_height);
                    if (position) {
                        const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                            ? -event.wheel.y
                            : event.wheel.y;
                        if (y != 0.0f) {
                            const std::uint8_t wheel_mask = y > 0.0f ? 8U : 16U;
                            queue_aten_pointer_event(*state, *position, wheel_mask, false, options.login.verbose);
                            queue_aten_pointer_event(*state, *position, 0, false, options.login.verbose);
                        }
                    }
                }
                have_event = SDL_PollEvent(&event);
            }

            const std::shared_ptr<const AtenViewFrame> frame =
                take_latest_aten_frame(*state, last_sequence);
            if (frame) {
                last_sequence = frame->sequence;
                if (texture == nullptr || texture_width != frame->width || texture_height != frame->height) {
                    if (texture != nullptr) {
                        SDL_DestroyTexture(texture);
                    }
                    texture = SDL_CreateTexture(
                        renderer,
                        SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_STREAMING,
                        frame->width,
                        frame->height);
                    if (texture == nullptr) {
                        throw_sdl_error("SDL_CreateTexture");
                    }
                    texture_width = frame->width;
                    texture_height = frame->height;
                    SDL_SetWindowTitle(
                        window,
                        ("hitsc - ATEN - " + options.login.base_url.host + " - "
                         + std::to_string(frame->width) + "x" + std::to_string(frame->height))
                            .c_str());
                }

                if (!SDL_UpdateTexture(texture, nullptr, frame->rgba.data(), frame->width * 4)) {
                    throw_sdl_error("SDL_UpdateTexture");
                }

                ++presented_frames;
                if (options.login.verbose && (presented_frames <= 20 || presented_frames % 60 == 0)) {
                    log_info() << "presented ATEN frame #" << presented_frames
                               << " sequence=" << frame->sequence
                               << " avg-rgb=" << sampled_average_rgb(frame->rgba);
                }
                render_needed = true;
            }

            const std::shared_ptr<const AtenCursorImage> cursor =
                take_latest_aten_cursor(*state, last_cursor_sequence);
            if (cursor) {
                last_cursor_sequence = cursor->sequence;
                current_cursor = cursor;
                if (!cursor->visible || cursor->width <= 0 || cursor->height <= 0) {
                    if (cursor_texture != nullptr) {
                        SDL_DestroyTexture(cursor_texture);
                        cursor_texture = nullptr;
                    }
                    cursor_texture_width = 0;
                    cursor_texture_height = 0;
                } else {
                    if (cursor_texture == nullptr ||
                        cursor_texture_width != cursor->width ||
                        cursor_texture_height != cursor->height) {
                        if (cursor_texture != nullptr) {
                            SDL_DestroyTexture(cursor_texture);
                        }
                        cursor_texture = SDL_CreateTexture(
                            renderer,
                            SDL_PIXELFORMAT_RGBA32,
                            SDL_TEXTUREACCESS_STREAMING,
                            cursor->width,
                            cursor->height);
                        if (cursor_texture == nullptr) {
                            throw_sdl_error("SDL_CreateTexture(cursor)");
                        }
                        SDL_SetTextureBlendMode(cursor_texture, SDL_BLENDMODE_BLEND);
                        cursor_texture_width = cursor->width;
                        cursor_texture_height = cursor->height;
                    }
                    if (!SDL_UpdateTexture(cursor_texture, nullptr, cursor->rgba.data(), cursor->width * 4)) {
                        throw_sdl_error("SDL_UpdateTexture(cursor)");
                    }
                }
                render_needed = true;
            }

            if (render_needed) {
                SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
                SDL_RenderClear(renderer);
                if (texture != nullptr && texture_width > 0 && texture_height > 0) {
                    const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                    SDL_RenderTexture(renderer, texture, nullptr, &target);
                    if (cursor_texture != nullptr && current_cursor && current_cursor->visible) {
                        const float scale_x = target.w / static_cast<float>(texture_width);
                        const float scale_y = target.h / static_cast<float>(texture_height);
                        SDL_FRect cursor_target{};
                        cursor_target.x = target.x + static_cast<float>(current_cursor->x) * scale_x;
                        cursor_target.y = target.y + static_cast<float>(current_cursor->y) * scale_y;
                        cursor_target.w = static_cast<float>(cursor_texture_width) * scale_x;
                        cursor_target.h = static_cast<float>(cursor_texture_height) * scale_y;
                        SDL_RenderTexture(renderer, cursor_texture, nullptr, &cursor_target);
                    }
                }
                SDL_RenderPresent(renderer);
                first_render = false;
            }

            const std::uint64_t ticks = SDL_GetTicks();
            if (texture == nullptr && ticks - last_status_tick > 1000) {
                last_status_tick = ticks;
                SDL_SetWindowTitle(window, ("hitsc - ATEN - " + aten_status_snapshot(*state)).c_str());
            }

            if (network_done->load()) {
                if (std::exception_ptr exception = take_aten_exception(*state)) {
                    std::rethrow_exception(exception);
                }
            }
        }
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "aten view ui thread");
        stop_aten_network(*state, *stop_requested, network_thread, options.login.verbose);
        if (cursor_texture != nullptr) {
            SDL_DestroyTexture(cursor_texture);
        }
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        throw;
    }

    stop_aten_network(*state, *stop_requested, network_thread, options.login.verbose);

    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
    if (cursor_texture != nullptr) {
        SDL_DestroyTexture(cursor_texture);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace hitsc
