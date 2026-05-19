#include "aten_protocol.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using hitsc::AtenRfbMessageBuffer;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void append_rect_header(
    std::vector<std::uint8_t>& bytes,
    std::uint16_t x,
    std::uint16_t y,
    std::uint16_t width,
    std::uint16_t height,
    std::int32_t encoding,
    std::int32_t mode,
    std::uint32_t payload_size)
{
    hitsc::append_be16(bytes, x);
    hitsc::append_be16(bytes, y);
    hitsc::append_be16(bytes, width);
    hitsc::append_be16(bytes, height);
    hitsc::append_be32(bytes, static_cast<std::uint32_t>(encoding));
    hitsc::append_be32(bytes, static_cast<std::uint32_t>(mode));
    hitsc::append_be32(bytes, payload_size);
}

std::vector<std::uint8_t> framebuffer_update(std::vector<std::uint8_t> payload)
{
    std::vector<std::uint8_t> bytes;
    bytes.push_back(0);
    bytes.push_back(0);
    hitsc::append_be16(bytes, 1);
    append_rect_header(bytes, 1, 2, 800, 600, 87, 444, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

void fragmented_framebuffer_update()
{
    AtenRfbMessageBuffer buffer;
    std::vector<std::uint8_t> message = framebuffer_update({1, 2, 1, 188, 0xaa, 0xbb});
    buffer.append(std::vector<std::uint8_t>(message.begin(), message.begin() + 12));
    require(!buffer.next().has_value(), "fragmented update parsed too early");
    buffer.append(std::vector<std::uint8_t>(message.begin() + 12, message.end()));
    const auto parsed = buffer.next();
    require(parsed.has_value(), "complete update did not parse");
    require(parsed->kind == hitsc::AtenRfbMessageKind::framebuffer_update, "wrong message kind");
    require(parsed->rects.size() == 1, "wrong rect count");
    require(parsed->rects[0].rect.width == 800, "wrong rect width");
    require(parsed->rects[0].payload.size() == 6, "wrong payload size");
    require(buffer.buffered_bytes() == 0, "buffer did not compact after parse");
}

void multiple_messages_one_append()
{
    AtenRfbMessageBuffer buffer;
    std::vector<std::uint8_t> bytes = framebuffer_update({1, 2, 1, 188, 0xcc});
    bytes.push_back(2);
    bytes.push_back(22);
    bytes.push_back(7);
    buffer.append(std::move(bytes));

    const auto first = buffer.next();
    require(first.has_value(), "first message missing");
    require(first->kind == hitsc::AtenRfbMessageKind::framebuffer_update, "first kind wrong");

    const auto second = buffer.next();
    require(second.has_value(), "second message missing");
    require(second->kind == hitsc::AtenRfbMessageKind::bell, "second kind wrong");

    const auto third = buffer.next();
    require(third.has_value(), "third message missing");
    require(third->kind == hitsc::AtenRfbMessageKind::message22, "third kind wrong");
    require(third->value == 7, "message 22 value wrong");
}

void incomplete_payload_stays_buffered()
{
    AtenRfbMessageBuffer buffer;
    std::vector<std::uint8_t> message = framebuffer_update({1, 2, 1, 188, 0xdd, 0xee});
    message.pop_back();
    buffer.append(std::move(message));
    require(!buffer.next().has_value(), "truncated payload parsed");
    require(buffer.buffered_bytes() > 0, "truncated payload was discarded");
}

void unknown_message_errors()
{
    AtenRfbMessageBuffer buffer;
    buffer.append(std::vector<std::uint8_t>{99});
    bool threw = false;
    try {
        (void)buffer.next();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "unknown message did not throw");
}

void ast_noop_detection()
{
    require(
        hitsc::ast_payload_is_frame_end_only({0, 0, 1, 188, 0, 0, 0, 0x90}),
        "AST no-op frame end was not detected");
    require(
        !hitsc::ast_payload_is_frame_end_only({0, 0, 1, 188, 1, 2, 3, 4}),
        "non-no-op AST payload detected as no-op");
}

void cursor_payload_length_guard()
{
    AtenRfbMessageBuffer buffer;
    std::vector<std::uint8_t> bytes;
    bytes.push_back(4);
    hitsc::append_be32(bytes, 1);
    hitsc::append_be32(bytes, 2);
    hitsc::append_be32(bytes, 4097);
    hitsc::append_be32(bytes, 2049);
    hitsc::append_be32(bytes, 1);
    bool threw = false;
    try {
        buffer.append(std::move(bytes));
        (void)buffer.next();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "implausible cursor payload did not throw");
}

} // namespace

int main()
{
    try {
        fragmented_framebuffer_update();
        multiple_messages_one_append();
        incomplete_payload_stays_buffered();
        unknown_message_errors();
        ast_noop_detection();
        cursor_payload_length_guard();
    } catch (const std::exception& ex) {
        std::cerr << "aten_protocol_tests failed: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
