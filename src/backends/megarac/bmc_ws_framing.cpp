#include "backends/megarac/bmc_ws_framing.hpp"

#include "text.hpp"  // trim_copy

#include <boost/asio/buffer.hpp>
#include <boost/beast/http/field.hpp>

namespace hitsc {
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;

std::string bmc_base64_encode(const std::vector<std::uint8_t>& input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const std::uint32_t a = input[i];
        const std::uint32_t b = i + 1 < input.size() ? input[i + 1] : 0;
        const std::uint32_t c = i + 2 < input.size() ? input[i + 2] : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;

        encoded.push_back(alphabet[(triple >> 18) & 0x3f]);
        encoded.push_back(alphabet[(triple >> 12) & 0x3f]);
        encoded.push_back(i + 1 < input.size() ? alphabet[(triple >> 6) & 0x3f] : '=');
        encoded.push_back(i + 2 < input.size() ? alphabet[triple & 0x3f] : '=');
    }
    return encoded;
}

std::vector<std::uint8_t> bmc_base64_decode(std::string_view input)
{
    auto value_of = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') {
            return ch - 'A';
        }
        if (ch >= 'a' && ch <= 'z') {
            return ch - 'a' + 26;
        }
        if (ch >= '0' && ch <= '9') {
            return ch - '0' + 52;
        }
        if (ch == '+') {
            return 62;
        }
        if (ch == '/') {
            return 63;
        }
        return -1;
    };

    std::vector<std::uint8_t> decoded;
    int accumulator = 0;
    int bits = -8;
    for (const char ch : input) {
        if (ch == '=') {
            break;
        }
        const int value = value_of(ch);
        if (value < 0) {
            continue;
        }
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return decoded;
}

bool bmc_ws_is_binary_mode(std::string_view subprotocol)
{
    return subprotocol == "binary";
}

std::string bmc_ws_selected_subprotocol(const websocket::response_type& response)
{
    const auto field = response.find(http::field::sec_websocket_protocol);
    if (field == response.end()) {
        return "base64";
    }

    std::string protocol = trim_copy(field->value());
    return protocol.empty() ? "base64" : protocol;
}

std::vector<std::uint8_t> bmc_ws_message_bytes(
    const beast::flat_buffer& buffer, std::string_view subprotocol)
{
    const auto data = buffer.data();
    if (bmc_ws_is_binary_mode(subprotocol)) {
        std::vector<std::uint8_t> bytes(boost::asio::buffer_size(data));
        boost::asio::buffer_copy(boost::asio::buffer(bytes), data);
        return bytes;
    }

    std::string text(boost::asio::buffer_size(data), '\0');
    boost::asio::buffer_copy(boost::asio::buffer(text), data);
    return bmc_base64_decode(text);
}

} // namespace hitsc
