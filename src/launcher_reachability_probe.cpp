#include "launcher_reachability_probe.hpp"

#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#endif

namespace hitsc {
namespace {

constexpr unsigned long kPingTimeoutMs = 750;

#ifdef _WIN32

class IcmpHandle {
public:
    IcmpHandle()
        : handle_(IcmpCreateFile())
    {
    }

    ~IcmpHandle()
    {
        if (valid()) {
            IcmpCloseHandle(handle_);
        }
    }

    [[nodiscard]] bool valid() const
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const
    {
        return handle_;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool ensure_winsock()
{
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [] {
        WSADATA data{};
        initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return initialized;
}

std::wstring to_wide(const QString& value)
{
    return std::wstring(
        reinterpret_cast<const wchar_t*>(value.utf16()),
        static_cast<std::size_t>(value.size()));
}

std::size_t reply_buffer_size()
{
#ifdef _WIN64
    return std::max(sizeof(ICMP_ECHO_REPLY), sizeof(ICMP_ECHO_REPLY32)) + 16;
#else
    return sizeof(ICMP_ECHO_REPLY) + 16;
#endif
}

std::optional<IPAddr> resolve_ipv4_address(const QString& host)
{
    const QString trimmed_host = host.trimmed();
    if (trimmed_host.isEmpty() || !ensure_winsock()) {
        return std::nullopt;
    }

    const std::wstring wide_host = to_wide(trimmed_host);
    IN_ADDR literal_address{};
    if (InetPtonW(AF_INET, wide_host.c_str(), &literal_address) == 1) {
        return literal_address.S_un.S_addr;
    }

    addrinfoW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfoW* results = nullptr;
    const int resolved = GetAddrInfoW(wide_host.c_str(), nullptr, &hints, &results);
    if (resolved != 0 || results == nullptr) {
        return std::nullopt;
    }

    std::optional<IPAddr> address;
    for (addrinfoW* current = results; current != nullptr; current = current->ai_next) {
        if (current->ai_family != AF_INET || current->ai_addr == nullptr) {
            continue;
        }

        address = reinterpret_cast<const sockaddr_in*>(current->ai_addr)->sin_addr.S_un.S_addr;
        break;
    }

    FreeAddrInfoW(results);
    return address;
}

bool ping_ipv4_address(IPAddr address)
{
    IcmpHandle icmp;
    if (!icmp.valid()) {
        return false;
    }

    std::array<char, 8> payload{'h', 'i', 't', 's', 'c', '\0', '\0', '\0'};
    std::vector<unsigned char> reply(reply_buffer_size() + payload.size());

    const DWORD reply_count = IcmpSendEcho(
        icmp.get(),
        address,
        payload.data(),
        static_cast<WORD>(payload.size()),
        nullptr,
        reply.data(),
        static_cast<DWORD>(reply.size()),
        kPingTimeoutMs);

    if (reply_count == 0) {
        return false;
    }

    const auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
    return echo->Status == IP_SUCCESS;
}

#endif

bool probe_host(const QString& host)
{
#ifdef _WIN32
    const std::optional<IPAddr> address = resolve_ipv4_address(host);
    if (!address) {
        return false;
    }
    return ping_ipv4_address(*address);
#else
    (void)host;
    return false;
#endif
}

} // namespace

ReachabilityProbe::ReachabilityProbe(QObject* parent)
    : QObject(parent)
{
    pool_.setMaxThreadCount(std::max(4, QThread::idealThreadCount()));
}

ReachabilityProbe::~ReachabilityProbe()
{
    shutdown();
}

void ReachabilityProbe::shutdown()
{
    if (shutting_down_.exchange(true)) {
        return;
    }

    pool_.clear();
    pool_.waitForDone();
}

void ReachabilityProbe::probe(
    QString host_id,
    QString host,
    QObject* receiver,
    Callback callback)
{
    if (shutting_down_.load()) {
        return;
    }

    QPointer<QObject> target(receiver);
    pool_.start(
        [this,
         host_id = std::move(host_id),
         host = std::move(host),
         target,
         callback = std::move(callback)]() mutable {
            const bool online = probe_host(host);
            deliver_result(std::move(host_id), target, std::move(callback), online);
        });
}

void ReachabilityProbe::deliver_result(
    QString host_id,
    QPointer<QObject> receiver,
    Callback callback,
    bool online)
{
    if (!receiver || !callback || shutting_down_.load()) {
        return;
    }

    if (receiver->thread() == QThread::currentThread()) {
        callback(std::move(host_id), online);
        return;
    }

    QMetaObject::invokeMethod(
        receiver.data(),
        [host_id = std::move(host_id),
         receiver,
         callback = std::move(callback),
         online]() mutable {
            if (receiver) {
                callback(std::move(host_id), online);
            }
        },
        Qt::QueuedConnection);
}

} // namespace hitsc
