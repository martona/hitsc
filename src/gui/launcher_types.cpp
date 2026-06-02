#include "launcher_types.hpp"

#include "errors.hpp"
#include "url.hpp"

#include <QChar>

#include <string>

namespace hitsc {
namespace {

std::string to_utf8_string(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

QString strip_url_scheme(QString value)
{
    if (value.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        return value.mid(8);
    }
    if (value.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
        return value.mid(7);
    }
    return value;
}

QString host_sort_key(const SavedHost& host)
{
    QString value = launcher_display_host(host.url);
    if (value.isEmpty()) {
        value = host.url.trimmed();
    }
    return value.toCaseFolded();
}

} // namespace

bool LauncherCredentials::empty() const
{
    return username.isEmpty() && password.isEmpty();
}

QString launcher_host_type_key(LauncherHostType type)
{
    switch (type) {
    case LauncherHostType::Auto:
        return QStringLiteral("auto");
    case LauncherHostType::Megarac:
        return QStringLiteral("megarac");
    case LauncherHostType::Aten:
        return QStringLiteral("aten");
    case LauncherHostType::Pikvm:
        return QStringLiteral("pikvm");
    }

    return QStringLiteral("auto");
}

QString launcher_host_type_label(LauncherHostType type)
{
    switch (type) {
    case LauncherHostType::Auto:
        return QStringLiteral("Auto");
    case LauncherHostType::Megarac:
        return QStringLiteral("MegaRAC");
    case LauncherHostType::Aten:
        return QStringLiteral("ATEN");
    case LauncherHostType::Pikvm:
        return QStringLiteral("PiKVM");
    }

    return QStringLiteral("Auto");
}

std::optional<LauncherHostType> parse_launcher_host_type(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("auto")) {
        return LauncherHostType::Auto;
    }
    if (normalized == QStringLiteral("megarac")) {
        return LauncherHostType::Megarac;
    }
    if (normalized == QStringLiteral("aten")) {
        return LauncherHostType::Aten;
    }
    if (normalized == QStringLiteral("pikvm")) {
        return LauncherHostType::Pikvm;
    }
    return std::nullopt;
}

QString reachability_status_key(ReachabilityStatus status)
{
    switch (status) {
    case ReachabilityStatus::Unknown:
        return QStringLiteral("unknown");
    case ReachabilityStatus::Checking:
        return QStringLiteral("checking");
    case ReachabilityStatus::Online:
        return QStringLiteral("online");
    case ReachabilityStatus::Offline:
        return QStringLiteral("offline");
    }

    return QStringLiteral("unknown");
}

QString reachability_status_label(ReachabilityStatus status)
{
    switch (status) {
    case ReachabilityStatus::Unknown:
        return QStringLiteral("Unknown");
    case ReachabilityStatus::Checking:
        return QStringLiteral("Checking");
    case ReachabilityStatus::Online:
        return QStringLiteral("Online");
    case ReachabilityStatus::Offline:
        return QStringLiteral("Offline");
    }

    return QStringLiteral("Unknown");
}

QString launcher_display_host(const QString& url)
{
    return strip_url_scheme(url.trimmed());
}

QString launcher_url_from_host(const QString& host_input)
{
    const QString host = strip_url_scheme(host_input.trimmed());
    if (host.isEmpty()) {
        return {};
    }
    return QStringLiteral("https://") + host;
}

bool validate_launcher_url(const QString& url, QString* error_message)
{
    try {
        (void)parse_https_url(to_utf8_string(url));
        return true;
    } catch (const UserError& ex) {
        if (error_message != nullptr) {
            *error_message = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

QString host_from_launcher_url(const QString& url)
{
    try {
        const Url parsed = parse_https_url(to_utf8_string(url));
        return QString::fromStdString(parsed.host);
    } catch (const UserError&) {
        return {};
    }
}

bool saved_host_hostname_less(const SavedHost& left, const SavedHost& right)
{
    const int host_order = QString::localeAwareCompare(host_sort_key(left), host_sort_key(right));
    if (host_order != 0) {
        return host_order < 0;
    }

    return QString::compare(left.id, right.id, Qt::CaseSensitive) < 0;
}

} // namespace hitsc
