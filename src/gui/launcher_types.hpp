#pragma once

#include <optional>

#include <QString>

namespace hitsc {

enum class LauncherHostType {
    Auto,
    Megarac,
    Aten,
    Pikvm,
};

enum class ReachabilityStatus {
    Unknown,
    Checking,
    Online,
    Offline,
};

struct LauncherCredentials {
    QString username;
    QString password;

    bool empty() const;
};

struct SavedHost {
    QString id;
    LauncherHostType type = LauncherHostType::Auto;
    QString url;
    std::optional<LauncherCredentials> credentials;
    ReachabilityStatus reachability = ReachabilityStatus::Unknown;
};

QString launcher_host_type_key(LauncherHostType type);
QString launcher_host_type_label(LauncherHostType type);
std::optional<LauncherHostType> parse_launcher_host_type(const QString& value);

QString reachability_status_key(ReachabilityStatus status);
QString reachability_status_label(ReachabilityStatus status);

// Lossless projection between the scheme-less host the user types/sees
// (e.g. "ipmi-box.lan:444") and the stored canonical URL ("https://ipmi-box.lan:444").
QString launcher_url_from_host(const QString& host_input);
QString launcher_display_host(const QString& url);

bool validate_launcher_url(const QString& url, QString* error_message = nullptr);

// Parsed host only (no scheme/port/path) — used for reachability probing.
QString host_from_launcher_url(const QString& url);
bool saved_host_hostname_less(const SavedHost& left, const SavedHost& right);

} // namespace hitsc
