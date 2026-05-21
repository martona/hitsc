#pragma once

#include "gui/launcher_types.hpp"

#include <QByteArray>
#include <QString>

#include <optional>

namespace hitsc {

struct ChildSessionLaunchRequest {
    QString session_id;
    LauncherHostType type = LauncherHostType::Auto;
    QString display_name;
    QString url;
    LauncherCredentials credentials;
    qint64 parent_process_id = 0;
};

constexpr int kChildSessionProtocolVersion = 1;

QByteArray serialize_child_session_launch_request(const ChildSessionLaunchRequest& request);
ChildSessionLaunchRequest parse_child_session_launch_request(const QByteArray& line);

QByteArray serialize_child_session_status(
    const QString& session_id,
    const QString& state,
    const QString& message = {});

} // namespace hitsc
