#pragma once

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket/rfc6455.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hitsc {

// WebSocket payload (de)framing shared by the MegaRAC /kvm and /cd-server sessions. Both
// negotiate the "binary" / "base64" subprotocols the AMI BMCs offer: in binary mode the
// payload is the raw bytes, otherwise every message is base64. (The /kvm view session still
// carries its own copies of these for now; migrate it to this header in a later cleanup.)

std::string bmc_base64_encode(const std::vector<std::uint8_t>& input);
std::vector<std::uint8_t> bmc_base64_decode(std::string_view input);

bool bmc_ws_is_binary_mode(std::string_view subprotocol);

// The subprotocol the server selected (defaults to "base64" if the header is absent/empty).
std::string bmc_ws_selected_subprotocol(const boost::beast::websocket::response_type& response);

// Decode one received WebSocket message into raw bytes per the negotiated subprotocol.
std::vector<std::uint8_t> bmc_ws_message_bytes(
    const boost::beast::flat_buffer& buffer, std::string_view subprotocol);

} // namespace hitsc
