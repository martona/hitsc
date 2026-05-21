#include "megarac_hid.hpp"

#include "megarac_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace hitsc {
namespace {

constexpr std::size_t kIusbHeaderSize = 32;
constexpr std::size_t kIusbHidHeaderSize = 34;
constexpr std::size_t kUsbKeyboardReportSize = 8;
constexpr std::size_t kUsbMouseAbsReportSize = 6;
constexpr std::size_t kUsbMouseRelReportSize = 4;
constexpr std::uint8_t kIusbMajor = 1;
constexpr std::uint8_t kIusbMinor = 0;
constexpr std::uint8_t kIusbDeviceKeyboard = 48;
constexpr std::uint8_t kIusbDeviceMouse = 49;
constexpr std::uint8_t kIusbProtoKeyboardData = 16;
constexpr std::uint8_t kIusbProtoMouseData = 32;
constexpr std::uint8_t kIusbFromRemote = 128;
constexpr std::uint8_t kIusbKeyboardDeviceNumber = 2;
constexpr std::uint8_t kIusbKeyboardInterfaceNumber = 0;
constexpr std::uint8_t kIusbMouseDeviceNumber = 2;
constexpr std::uint8_t kIusbMouseInterfaceNumber = 1;
constexpr std::size_t kIusbChecksumOffset = 11;
constexpr int kAbsoluteMouseMax = 32767;

void append_iusb_hid_header(
    std::vector<std::uint8_t>& payload,
    std::size_t report_size,
    std::uint32_t sequence,
    std::uint8_t device,
    std::uint8_t protocol,
    std::uint8_t device_number,
    std::uint8_t interface_number)
{
    constexpr std::string_view signature = "IUSB    ";
    payload.insert(payload.end(), signature.begin(), signature.end());
    payload.push_back(kIusbMajor);
    payload.push_back(kIusbMinor);
    payload.push_back(static_cast<std::uint8_t>(kIusbHeaderSize));
    payload.push_back(0);
    append_le32(
        payload,
        static_cast<std::uint32_t>(kIusbHidHeaderSize - 1 + report_size - kIusbHeaderSize));
    payload.push_back(0);
    payload.push_back(device);
    payload.push_back(protocol);
    payload.push_back(kIusbFromRemote);
    payload.push_back(device_number);
    payload.push_back(interface_number);
    payload.push_back(0);
    payload.push_back(0);
    append_le32(payload, sequence);
    payload.push_back(0);
    payload.push_back(0);
    payload.push_back(0);
    payload.push_back(0);
}

void append_iusb_mouse_header(
    std::vector<std::uint8_t>& payload,
    std::size_t report_size,
    std::uint32_t sequence)
{
    append_iusb_hid_header(
        payload,
        report_size,
        sequence,
        kIusbDeviceMouse,
        kIusbProtoMouseData,
        kIusbMouseDeviceNumber,
        kIusbMouseInterfaceNumber);
}

void set_iusb_checksum(std::vector<std::uint8_t>& payload)
{
    if (payload.size() < kIusbHeaderSize) {
        return;
    }

    payload[kIusbChecksumOffset] = 0;
    int sum = 0;
    for (std::size_t index = 0; index < kIusbHeaderSize; ++index) {
        sum = (sum + payload[index]) & 0xff;
    }
    payload[kIusbChecksumOffset] = to_byte(-sum);
}

std::uint16_t scale_absolute_mouse_coordinate(int value, int size)
{
    if (size <= 0) {
        return 0;
    }

    const int clamped = std::clamp(value, 0, size);
    const double scaled =
        static_cast<double>(clamped) * static_cast<double>(kAbsoluteMouseMax) / static_cast<double>(size);
    return static_cast<std::uint16_t>(std::clamp(
        static_cast<int>(std::floor(scaled + 0.5)),
        0,
        kAbsoluteMouseMax));
}

} // namespace

std::vector<std::uint8_t> make_megarac_absolute_mouse_packet(
    const MegaracAbsoluteMouseReport& report,
    std::uint32_t sequence)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kIusbHidHeaderSize - 1 + kUsbMouseAbsReportSize);
    append_iusb_mouse_header(payload, kUsbMouseAbsReportSize, sequence);
    payload.push_back(static_cast<std::uint8_t>(kUsbMouseAbsReportSize));
    payload.push_back(report.buttons);
    append_le16(payload, scale_absolute_mouse_coordinate(report.x, report.width));
    append_le16(payload, scale_absolute_mouse_coordinate(report.y, report.height));
    payload.push_back(to_byte(report.wheel));
    set_iusb_checksum(payload);
    return make_payload_packet(MegaracCommand::SendHidPacket, 0, payload);
}

std::vector<std::uint8_t> make_megarac_relative_mouse_packet(
    const MegaracRelativeMouseReport& report,
    std::uint32_t sequence)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kIusbHidHeaderSize - 1 + kUsbMouseRelReportSize);
    append_iusb_mouse_header(payload, kUsbMouseRelReportSize, sequence);
    payload.push_back(static_cast<std::uint8_t>(kUsbMouseAbsReportSize));
    payload.push_back(report.buttons);
    payload.push_back(to_byte(std::clamp(report.dx, -127, 127)));
    payload.push_back(to_byte(std::clamp(report.dy, -127, 127)));
    payload.push_back(to_byte(report.wheel));
    set_iusb_checksum(payload);
    return make_payload_packet(MegaracCommand::SendHidPacket, 0, payload);
}

std::vector<std::uint8_t> make_megarac_keyboard_packet(
    const MegaracKeyboardReport& report,
    std::uint32_t sequence)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kIusbHidHeaderSize - 1 + kUsbKeyboardReportSize);
    append_iusb_hid_header(
        payload,
        kUsbKeyboardReportSize,
        sequence,
        kIusbDeviceKeyboard,
        kIusbProtoKeyboardData,
        kIusbKeyboardDeviceNumber,
        kIusbKeyboardInterfaceNumber);
    payload.push_back(static_cast<std::uint8_t>(kUsbKeyboardReportSize));
    payload.push_back(report.modifiers);
    payload.push_back(0);
    payload.insert(payload.end(), report.keys.begin(), report.keys.end());
    set_iusb_checksum(payload);
    return make_payload_packet(MegaracCommand::SendHidPacket, 0, payload);
}

} // namespace hitsc
