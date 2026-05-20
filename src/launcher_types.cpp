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

bool is_allowed_host_character(QChar ch)
{
    return ch.isLetterOrNumber()
        || ch == QLatin1Char('.')
        || ch == QLatin1Char('-')
        || ch == QLatin1Char(':')
        || ch == QLatin1Char('[')
        || ch == QLatin1Char(']');
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

} // namespace

bool LauncherCredentials::empty() const
{
    return username.isEmpty() && password.isEmpty();
}

QString launcher_host_type_key(LauncherHostType type)
{
    switch (type) {
    case LauncherHostType::Megarac:
        return QStringLiteral("megarac");
    case LauncherHostType::Aten:
        return QStringLiteral("aten");
    case LauncherHostType::Pikvm:
        return QStringLiteral("pikvm");
    }

    return QStringLiteral("megarac");
}

QString launcher_host_type_label(LauncherHostType type)
{
    switch (type) {
    case LauncherHostType::Megarac:
        return QStringLiteral("MegaRAC");
    case LauncherHostType::Aten:
        return QStringLiteral("ATEN");
    case LauncherHostType::Pikvm:
        return QStringLiteral("PiKVM");
    }

    return QStringLiteral("MegaRAC");
}

std::optional<LauncherHostType> parse_launcher_host_type(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
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

QString sanitize_host_name_to_url(const QString& name)
{
    QString value = strip_url_scheme(name.trimmed()).toLower();
    QString host;
    bool previous_was_dash = false;

    for (const QChar ch : value) {
        if (ch.isSpace() || ch == QLatin1Char('_')) {
            if (!host.isEmpty() && !previous_was_dash) {
                host += QLatin1Char('-');
                previous_was_dash = true;
            }
            continue;
        }

        if (is_allowed_host_character(ch)) {
            host += ch;
            previous_was_dash = ch == QLatin1Char('-');
            continue;
        }

        if (!host.isEmpty() && !previous_was_dash) {
            host += QLatin1Char('-');
            previous_was_dash = true;
        }
    }

    while (host.startsWith(QLatin1Char('-')) || host.startsWith(QLatin1Char('.'))) {
        host.remove(0, 1);
    }
    while (host.endsWith(QLatin1Char('-')) || host.endsWith(QLatin1Char('.'))) {
        host.chop(1);
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

} // namespace hitsc
