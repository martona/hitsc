#include "aten_view.hpp"

#include "app_info.hpp"
#include "aspeed_decoder.hpp"
#include "aten_session.hpp"
#include "diagnostics.hpp"
#include "tls.hpp"
#include "url.hpp"

#include <SDL3/SDL.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
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

struct AtenRemoteMousePosition {
    int x = 0;
    int y = 0;
};

struct AtenViewState {
    std::mutex frame_mutex;
    std::mutex control_mutex;
    AtenViewFrame frame;
    bool has_frame = false;
    std::uint64_t frame_sequence = 0;
    std::string status = "starting";
    std::exception_ptr exception;
    std::function<void()> force_close;
    std::deque<std::vector<std::uint8_t>> pending_input;
};

struct AtenAstPayloadHeader {
    unsigned y_selector = 0;
    unsigned uv_selector = 0;
    unsigned mode = 0;
    unsigned mode420 = 0;
};

constexpr std::uint64_t kMouseMotionIntervalMilliseconds = 10;
constexpr std::size_t kMaxQueuedInputPackets = 512;
using AtenKeyDownState = std::array<bool, 256>;

void append_be16(std::vector<std::uint8_t>& bytes, std::uint16_t value);
void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value);

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

void store_aten_frame(AtenViewState& state, AtenViewFrame frame)
{
    std::lock_guard lock(state.frame_mutex);
    frame.sequence = ++state.frame_sequence;
    state.frame = std::move(frame);
    state.has_frame = true;
}

std::optional<AtenViewFrame> take_latest_aten_frame(AtenViewState& state, std::uint64_t last_sequence)
{
    std::lock_guard lock(state.frame_mutex);
    if (!state.has_frame || state.frame.sequence == last_sequence) {
        return std::nullopt;
    }
    return state.frame;
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

void queue_aten_input_packet(
    AtenViewState& state,
    std::vector<std::uint8_t> packet,
    bool coalesce_mouse_motion)
{
    std::lock_guard lock(state.control_mutex);
    if (coalesce_mouse_motion &&
        !state.pending_input.empty() &&
        state.pending_input.back().size() == packet.size() &&
        state.pending_input.back().size() >= 3 &&
        state.pending_input.back()[0] == 5 &&
        state.pending_input.back()[2] == 0) {
        state.pending_input.back() = std::move(packet);
        return;
    }

    if (state.pending_input.size() >= kMaxQueuedInputPackets) {
        state.pending_input.pop_front();
    }
    state.pending_input.push_back(std::move(packet));
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

void stop_aten_network(AtenViewState& state, std::atomic_bool& stop_requested, std::thread& network_thread)
{
    stop_requested.store(true);

    std::function<void()> force_close;
    {
        std::lock_guard lock(state.control_mutex);
        force_close = state.force_close;
    }
    if (force_close) {
        force_close();
    }

    if (network_thread.joinable()) {
        network_thread.join();
    }
}

std::string normalized_websocket_path(std::string path)
{
    if (path.empty()) {
        return "/";
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
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

void drain_aten_input_packets(RfbStream& rfb, AtenViewState& state, bool verbose)
{
    std::deque<std::vector<std::uint8_t>> packets;
    {
        std::lock_guard lock(state.control_mutex);
        packets.swap(state.pending_input);
    }

    while (!packets.empty()) {
        const std::vector<std::uint8_t>& packet = packets.front();
        rfb.write(packet);
        if (verbose) {
            std::cout << "hitsc: sent ATEN input"
                      << " type=" << static_cast<int>(packet.empty() ? 0 : packet.front())
                      << " bytes=" << packet.size() << '\n';
        }
        packets.pop_front();
    }
}

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
        std::cout << "hitsc: aten rfb security scheme=" << auth_scheme
                  << " " << security_type_name(static_cast<std::uint8_t>(auth_scheme)) << '\n';
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

    std::cout << "hitsc: aten rfb security types=";
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << static_cast<int>(types[i]) << "(" << security_type_name(types[i]) << ")";
    }
    std::cout << " selected=" << static_cast<int>(selected)
              << "(" << security_type_name(selected) << ")\n";

    rfb.write(std::vector<std::uint8_t>{selected});
    return selected;
}

void authenticate_insyde(RfbStream& rfb, std::string_view credential)
{
    const std::vector<std::uint8_t> challenge = rfb.read_exact(24);
    std::cout << "hitsc: aten rfb insyde challenge bytes=" << challenge.size()
              << " first-bytes=" << hex_preview(challenge) << '\n';

    std::vector<std::uint8_t> reply(49, 0);
    const std::size_t copy_size = std::min<std::size_t>(24, credential.size());
    for (std::size_t i = 0; i < copy_size; ++i) {
        reply[i] = static_cast<std::uint8_t>(credential[i]);
    }
    reply[48] = 0;
    rfb.write(reply);

    std::cout << "hitsc: sent ATEN/Insyde username auth reply"
              << " credential-bytes=" << copy_size
              << " client-type=0\n";
}

void authenticate(RfbStream& rfb, std::uint8_t security_type, std::string_view credential)
{
    switch (security_type) {
    case 1:
        return;
    case 15:
    case 16:
        authenticate_insyde(rfb, credential);
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

void read_security_result(RfbStream& rfb)
{
    const std::uint32_t result = rfb.read_be32();
    if (result == 0) {
        std::cout << "hitsc: aten rfb authentication succeeded\n";
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

void send_framebuffer_update_request(
    RfbStream& rfb,
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
    rfb.write(request);
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

void handle_insyde_framebuffer_update(RfbStream& rfb, int update_number)
{
    rfb.read_u8();
    const std::uint16_t rect_count = rfb.read_be16();
    std::cout << "hitsc: aten rfb framebuffer update #" << update_number
              << " rects=" << rect_count << '\n';

    for (std::uint16_t i = 0; i < rect_count; ++i) {
        const AtenFramebufferRect rect = read_vendor_rect_header(rfb);
        std::vector<std::uint8_t> payload;
        if (rect.data_length != 0) {
            payload = rfb.read_exact(rect.data_length);
        }

        std::cout << "hitsc: aten rfb rect"
                  << " index=" << (i + 1)
                  << " xy=" << rect.x << ',' << rect.y
                  << " size=" << rect.width << 'x' << rect.height
                  << " encoding=" << rect.encoding << '(' << encoding_name(rect.encoding) << ')'
                  << " mode=" << rect.mode
                  << " payload=" << rect.data_length;
        if (!payload.empty()) {
            std::cout << " first-bytes=" << hex_preview(payload);
        }
        std::cout << '\n';
    }
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

void handle_insyde_framebuffer_update(
    RfbStream& rfb,
    int update_number,
    AspeedDecoder& decoder,
    AtenViewState& state,
    const AtenViewOptions& options,
    std::vector<std::uint8_t>& previous_rgba,
    int& previous_width,
    int& previous_height)
{
    rfb.read_u8();
    const std::uint16_t rect_count = rfb.read_be16();
    if (options.login.verbose) {
        std::cout << "hitsc: aten rfb framebuffer update #" << update_number
                  << " rects=" << rect_count << '\n';
    }

    for (std::uint16_t i = 0; i < rect_count; ++i) {
        const AtenFramebufferRect rect = read_vendor_rect_header(rfb);
        std::vector<std::uint8_t> payload;
        if (rect.data_length != 0) {
            payload = rfb.read_exact(rect.data_length);
        }

        if (options.login.verbose) {
            std::cout << "hitsc: aten rfb rect"
                      << " index=" << (i + 1)
                      << " xy=" << rect.x << ',' << rect.y
                      << " size=" << rect.width << 'x' << rect.height
                      << " encoding=" << rect.encoding << '(' << encoding_name(rect.encoding) << ')'
                      << " mode=" << rect.mode
                      << " payload=" << rect.data_length;
            if (!payload.empty()) {
                std::cout << " first-bytes=" << hex_preview(payload);
            }
            std::cout << '\n';
        }

        if (rect.encoding != 87 || payload.empty()) {
            continue;
        }

        const AtenAstPayloadHeader ast = read_ast_payload_header(payload);
        std::vector<std::uint8_t> compressed(payload.begin() + 4, payload.end());
        if (compressed.empty()) {
            continue;
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
            previous_width == rect.width &&
            previous_height == rect.height &&
            previous_rgba.size() == static_cast<std::size_t>(rect.width) *
                    static_cast<std::size_t>(rect.height) * 4U;
        std::vector<std::uint8_t> rgba = decoder.decode_rgba(
            decode_options,
            compressed,
            can_reuse_previous ? &previous_rgba : nullptr);

        AtenViewFrame frame;
        frame.width = rect.width;
        frame.height = rect.height;
        frame.rgba = rgba;
        store_aten_frame(state, std::move(frame));

        previous_width = rect.width;
        previous_height = rect.height;
        previous_rgba = std::move(rgba);

        if (options.login.verbose && (update_number <= 20 || update_number % 60 == 0)) {
            std::cout << "hitsc: rendered ATEN frame #" << update_number
                      << " size=" << rect.width << 'x' << rect.height
                      << " compressed=" << compressed.size()
                      << " mode=" << ast.mode
                      << " y-sel=" << ast.y_selector
                      << " uv-sel=" << ast.uv_selector
                      << " avg-rgb=" << sampled_average_rgb(previous_rgba)
                      << '\n';
        }
    }
}

void handle_cursor_position(RfbStream& rfb)
{
    const std::uint32_t x = rfb.read_be32();
    const std::uint32_t y = rfb.read_be32();
    const std::uint32_t width = rfb.read_be32();
    const std::uint32_t height = rfb.read_be32();
    const std::uint32_t valid = rfb.read_be32();
    std::cout << "hitsc: aten rfb cursor"
              << " xy=" << x << ',' << y
              << " size=" << width << 'x' << height
              << " valid=" << valid << '\n';

    if (valid == 1) {
        rfb.read_be32();
        const std::uint64_t pattern_size =
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 2U;
        if (pattern_size > 16U * 1024U * 1024U) {
            throw std::runtime_error("ATEN RFB cursor pattern is implausibly large");
        }
        rfb.read_exact(static_cast<std::size_t>(pattern_size));
    }
}

void run_rfb_event_probe(RfbStream& rfb, const AtenRfbServerInit& init, const AtenViewOptions& options)
{
    int updates = 0;
    while (options.framebuffer_update_limit == 0 || updates < options.framebuffer_update_limit) {
        const std::uint8_t message_type = rfb.read_u8();
        switch (message_type) {
        case 0:
            if (!init.insyde_extension) {
                throw std::runtime_error("standard RFB framebuffer updates are not implemented yet");
            }
            ++updates;
            handle_insyde_framebuffer_update(rfb, updates);
            std::this_thread::sleep_for(std::chrono::milliseconds(66));
            send_framebuffer_update_request(rfb, init.width, init.height, false);
            break;
        case 2:
            std::cout << "hitsc: aten rfb bell\n";
            break;
        case 4:
            handle_cursor_position(rfb);
            break;
        case 22:
            std::cout << "hitsc: aten rfb message 22 value=" << static_cast<int>(rfb.read_u8()) << '\n';
            break;
        case 53:
        case 54:
        case 55: {
            const std::uint8_t crypto = rfb.read_u8();
            const std::uint8_t mode = rfb.read_u8();
            const std::uint8_t status = rfb.read_u8();
            std::cout << "hitsc: aten rfb mouse/control"
                      << " type=" << static_cast<int>(message_type)
                      << " crypto=" << static_cast<int>(crypto)
                      << " mode=" << static_cast<int>(mode)
                      << " status=" << static_cast<int>(status) << '\n';
            break;
        }
        case 57: {
            const std::uint32_t count = rfb.read_be32();
            const std::uint32_t code_digits = rfb.read_be32();
            const std::vector<std::uint8_t> message = rfb.read_exact(256);
            std::cout << "hitsc: aten rfb control-message"
                      << " count=" << count
                      << " code-digits=" << code_digits
                      << " first-bytes=" << hex_preview(message) << '\n';
            break;
        }
        case 60:
        case 63:
            std::cout << "hitsc: aten rfb service"
                      << " type=" << static_cast<int>(message_type)
                      << " value=" << static_cast<int>(rfb.read_u8()) << '\n';
            break;
        default:
            throw std::runtime_error(
                "unhandled ATEN RFB server message type " + std::to_string(message_type));
        }
    }

    std::cout << "hitsc: aten rfb update limit reached"
              << " updates=" << updates << '\n';
}

} // namespace

namespace {

void run_aten_network_session(const AtenViewOptions& options, AtenViewState& state, std::atomic_bool& stop_requested)
{
    set_aten_status(state, "logging in");
    AtenSession session = login_aten(options.login);
    AtenLogoutGuard logout_guard(options.login);
    logout_guard.arm(session);
    std::cout << "hitsc: aten login succeeded\n";
    std::cout << "hitsc: cookies stored: " << session.cookies.size() << '\n';

    std::string rfb_credential = options.login.username;
    if (options.fetch_bootstrap) {
        const std::string bootstrap_credential =
            fetch_aten_ikvm_bootstrap(options.login, session.cookies);
        if (!bootstrap_credential.empty()) {
            rfb_credential = bootstrap_credential;
        } else {
            std::cerr << "hitsc: aten warning: bootstrap did not expose entry_value; "
                         "falling back to login username for RFB auth\n";
        }
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
    ws.next_layer().handshake(ssl::stream_base::client);
    beast::get_lowest_layer(ws).expires_never();

    websocket::stream_base::timeout timeout;
    timeout.handshake_timeout = std::chrono::seconds(30);
    timeout.idle_timeout = std::chrono::seconds(options.idle_timeout_seconds);
    timeout.keep_alive_pings = true;
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

    const std::string path = normalized_websocket_path(options.websocket_path);
    websocket::response_type response;
    ws.handshake(response, host, path);
    std::cout << "hitsc: aten websocket connected"
              << " path=" << path
              << " idle-timeout=" << options.idle_timeout_seconds << "s\n";
    set_aten_force_close(state, [&ws] {
        beast::error_code error;
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
    std::cout << "hitsc: aten rfb server-version=" << server_version
              << " client-version=" << client_version << '\n';
    rfb.write_string("RFB " + client_version + "\n");

    const std::uint8_t security_type = negotiate_security(rfb, client_version);
    authenticate(rfb, security_type, rfb_credential);
    if (security_type != 1 || version_uses_security_result_for_none(client_version)) {
        read_security_result(rfb);
    }

    rfb.write(std::vector<std::uint8_t>{options.shared ? 1U : 0U});
    const bool insyde_extension = security_type == 15 || security_type == 16;
    const AtenRfbServerInit init = read_server_init(rfb, insyde_extension);
    std::cout << "hitsc: aten rfb server-init"
              << " size=" << init.width << 'x' << init.height
              << " name=\"" << init.name << "\""
              << " bpp=" << static_cast<int>(init.bits_per_pixel)
              << " depth=" << static_cast<int>(init.depth)
              << " true-color=" << (init.true_color ? "yes" : "no")
              << " endian=" << (init.big_endian ? "big" : "little") << '\n';
    if (init.insyde_extension) {
        std::cout << "hitsc: aten rfb insyde extension"
                  << " session-id=" << init.session_id
                  << " video=" << static_cast<int>(init.video_enable)
                  << " kbms=" << static_cast<int>(init.keyboard_mouse_enable)
                  << " kick-user=" << static_cast<int>(init.kick_user_enable)
                  << " vmedia=" << static_cast<int>(init.virtual_media_enable) << '\n';
    }

    send_framebuffer_update_request(rfb, init.width, init.height, false);
    std::cout << "hitsc: requested ATEN framebuffer update\n";
    set_aten_status(state, "waiting for video");

    AspeedDecoder decoder;
    std::vector<std::uint8_t> previous_rgba;
    int previous_width = 0;
    int previous_height = 0;
    int updates = 0;
    while (!stop_requested.load() &&
           (options.framebuffer_update_limit == 0 || updates < options.framebuffer_update_limit)) {
        drain_aten_input_packets(rfb, state, options.login.verbose);
        const std::uint8_t message_type = rfb.read_u8();
        switch (message_type) {
        case 0:
            if (!init.insyde_extension) {
                throw std::runtime_error("standard RFB framebuffer updates are not implemented yet");
            }
            ++updates;
            handle_insyde_framebuffer_update(
                rfb,
                updates,
                decoder,
                state,
                options,
                previous_rgba,
                previous_width,
                previous_height);
            std::this_thread::sleep_for(std::chrono::milliseconds(66));
            send_framebuffer_update_request(
                rfb,
                static_cast<std::uint16_t>(previous_width > 0 ? previous_width : init.width),
                static_cast<std::uint16_t>(previous_height > 0 ? previous_height : init.height),
                true);
            break;
        case 2:
            std::cout << "hitsc: aten rfb bell\n";
            break;
        case 4:
            handle_cursor_position(rfb);
            break;
        case 22:
            std::cout << "hitsc: aten rfb message 22 value=" << static_cast<int>(rfb.read_u8()) << '\n';
            break;
        case 53:
        case 54:
        case 55: {
            const std::uint8_t crypto = rfb.read_u8();
            const std::uint8_t mode = rfb.read_u8();
            const std::uint8_t status = rfb.read_u8();
            std::cout << "hitsc: aten rfb mouse/control"
                      << " type=" << static_cast<int>(message_type)
                      << " crypto=" << static_cast<int>(crypto)
                      << " mode=" << static_cast<int>(mode)
                      << " status=" << static_cast<int>(status) << '\n';
            break;
        }
        case 57: {
            const std::uint32_t count = rfb.read_be32();
            const std::uint32_t code_digits = rfb.read_be32();
            const std::vector<std::uint8_t> message = rfb.read_exact(256);
            std::cout << "hitsc: aten rfb control-message"
                      << " count=" << count
                      << " code-digits=" << code_digits
                      << " first-bytes=" << hex_preview(message) << '\n';
            break;
        }
        case 60:
        case 63:
            std::cout << "hitsc: aten rfb service"
                      << " type=" << static_cast<int>(message_type)
                      << " value=" << static_cast<int>(rfb.read_u8()) << '\n';
            break;
        default:
            throw std::runtime_error(
                "unhandled ATEN RFB server message type " + std::to_string(message_type));
        }
        drain_aten_input_packets(rfb, state, options.login.verbose);
    }

    beast::error_code error;
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
    ws.close(stop_requested.load() ? websocket::close_code::going_away : websocket::close_code::normal, error);
    if (error) {
        std::cerr << "hitsc: aten websocket close warning: " << error.message() << '\n';
        error.clear();
        beast::get_lowest_layer(ws).socket().shutdown(tcp::socket::shutdown_both, error);
        error.clear();
        beast::get_lowest_layer(ws).socket().close(error);
    }
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
        std::uint64_t last_status_tick = 0;
        int texture_width = 0;
        int texture_height = 0;
        int presented_frames = 0;
        std::uint8_t mouse_buttons = 0;
        std::uint64_t last_mouse_motion_ticks = 0;
        AtenKeyDownState key_down{};
        bool first_render = true;
        while (running) {
            bool render_needed = first_render;
            SDL_Event event{};
            bool have_event = SDL_WaitEventTimeout(&event, 16);
            while (have_event) {
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                           event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                           event.type == SDL_EVENT_WINDOW_EXPOSED) {
                    render_needed = true;
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    release_all_aten_keys(*state, key_down, options.login.verbose);
                } else if (event.type == SDL_EVENT_KEY_DOWN ||
                           event.type == SDL_EVENT_KEY_UP) {
                    if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat) {
                        continue;
                    }
                    const std::optional<std::uint32_t> usage =
                        aten_keyboard_usage_from_sdl_scancode(event.key.scancode);
                    if (!usage || *usage >= key_down.size()) {
                        if (options.login.verbose) {
                            std::cout << "hitsc: ignored ATEN key"
                                      << " scancode=" << event.key.scancode
                                      << " key=" << event.key.key << '\n';
                        }
                        continue;
                    }

                    const bool down = event.type == SDL_EVENT_KEY_DOWN;
                    if (key_down[*usage] == down) {
                        continue;
                    }
                    key_down[*usage] = down;
                    queue_aten_key_event(*state, *usage, down, options.login.verbose);
                } else if (texture_width > 0 && texture_height > 0 &&
                           (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
                    const std::uint8_t mask = button_mask_for_sdl_button(event.button.button);
                    if (mask == 0) {
                        continue;
                    }

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
                } else if (texture_width > 0 && texture_height > 0 &&
                           event.type == SDL_EVENT_MOUSE_MOTION) {
                    const std::uint64_t ticks = SDL_GetTicks();
                    if (mouse_buttons == 0 &&
                        ticks - last_mouse_motion_ticks < kMouseMotionIntervalMilliseconds) {
                        continue;
                    }

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
                        if (y == 0.0f) {
                            continue;
                        }
                        const std::uint8_t wheel_mask = y > 0.0f ? 8U : 16U;
                        queue_aten_pointer_event(*state, *position, wheel_mask, false, options.login.verbose);
                        queue_aten_pointer_event(*state, *position, 0, false, options.login.verbose);
                    }
                }
                have_event = SDL_PollEvent(&event);
            }

            const std::optional<AtenViewFrame> frame = take_latest_aten_frame(*state, last_sequence);
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
                    std::cout << "hitsc: presented ATEN frame #" << presented_frames
                              << " sequence=" << frame->sequence
                              << " avg-rgb=" << sampled_average_rgb(frame->rgba)
                              << '\n';
                }
                render_needed = true;
            }

            if (render_needed) {
                SDL_SetRenderDrawColor(renderer, 12, 14, 18, 255);
                SDL_RenderClear(renderer);
                if (texture != nullptr && texture_width > 0 && texture_height > 0) {
                    const SDL_FRect target = current_target_rect(window, texture_width, texture_height);
                    SDL_RenderTexture(renderer, texture, nullptr, &target);
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
                if (options.framebuffer_update_limit != 0) {
                    running = false;
                }
            }
        }
    } catch (...) {
        print_current_exception_with_stack(std::cerr, "aten view ui thread");
        stop_aten_network(*state, *stop_requested, network_thread);
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

    stop_aten_network(*state, *stop_requested, network_thread);

    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace hitsc
