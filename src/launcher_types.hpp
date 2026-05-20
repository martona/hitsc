#pragma once

#include <optional>

#include <QString>

namespace hitsc {

enum class LauncherHostType {
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
    LauncherHostType type = LauncherHostType::Megarac;
    QString name;
    QString url;
    std::optional<LauncherCredentials> credentials;
    ReachabilityStatus reachability = ReachabilityStatus::Unknown;
};

QString launcher_host_type_key(LauncherHostType type);
QString launcher_host_type_label(LauncherHostType type);
std::optional<LauncherHostType> parse_launcher_host_type(const QString& value);

QString reachability_status_key(ReachabilityStatus status);
QString reachability_status_label(ReachabilityStatus status);

QString sanitize_host_name_to_url(const QString& name);
bool validate_launcher_url(const QString& url, QString* error_message = nullptr);
QString host_from_launcher_url(const QString& url);

} // namespace hitsc
