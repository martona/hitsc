#include "launcher_reachability_probe.hpp"

#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <array>
#include <mutex>
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

#ifdef _WIN32

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

bool ping_ipv4(const sockaddr_in& address)
{
    const HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) {
        return false;
    }

    const std::array<char, 8> payload{'h', 'i', 't', 's', 'c', '\0', '\0', '\0'};
    std::vector<unsigned char> reply(sizeof(ICMP_ECHO_REPLY) + payload.size() + 8);
    const DWORD reply_count = IcmpSendEcho(
        icmp,
        address.sin_addr.S_un.S_addr,
        const_cast<char*>(payload.data()),
        static_cast<WORD>(payload.size()),
        nullptr,
        reply.data(),
        static_cast<DWORD>(reply.size()),
        750);

    IcmpCloseHandle(icmp);
    if (reply_count == 0) {
        return false;
    }

    const auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
    return echo->Status == IP_SUCCESS;
}

bool ping_host(const QString& host)
{
    if (host.trimmed().isEmpty() || !ensure_winsock()) {
        return false;
    }

    addrinfoW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfoW* results = nullptr;
    const std::wstring wide_host = to_wide(host);
    const int resolved = GetAddrInfoW(wide_host.c_str(), nullptr, &hints, &results);
    if (resolved != 0 || results == nullptr) {
        return false;
    }

    bool online = false;
    for (addrinfoW* current = results; current != nullptr; current = current->ai_next) {
        if (current->ai_family != AF_INET || current->ai_addr == nullptr) {
            continue;
        }

        online = ping_ipv4(*reinterpret_cast<const sockaddr_in*>(current->ai_addr));
        if (online) {
            break;
        }
    }

    FreeAddrInfoW(results);
    return online;
}

#else

bool ping_host(const QString& host)
{
    (void)host;
    return false;
}

#endif

} // namespace

ReachabilityProbe::ReachabilityProbe(QObject* parent)
    : QObject(parent)
{
    thread_pool_.setMaxThreadCount(std::max(thread_pool_.maxThreadCount(), 64));
}

ReachabilityProbe::~ReachabilityProbe()
{
    shutdown();
}

void ReachabilityProbe::shutdown()
{
    thread_pool_.clear();
    thread_pool_.waitForDone();
}

void ReachabilityProbe::probe(
    QString host_id,
    QString host,
    QObject* receiver,
    Callback callback)
{
    QPointer<QObject> target(receiver);
    thread_pool_.start([host_id = std::move(host_id),
                        host = std::move(host),
                        target,
                        callback = std::move(callback)]() mutable {
        const bool online = ping_host(host);
        if (!target) {
            return;
        }

        QMetaObject::invokeMethod(
            target,
            [host_id = std::move(host_id),
             online,
             callback = std::move(callback)]() mutable {
                callback(std::move(host_id), online);
            },
            Qt::QueuedConnection);
    });
}

} // namespace hitsc
