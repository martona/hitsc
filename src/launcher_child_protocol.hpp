#pragma once

#include "launcher_types.hpp"

#include <QByteArray>
#include <QString>

namespace hitsc {

struct ChildSessionLaunchRequest {
    LauncherHostType type = LauncherHostType::Megarac;
    QString url;
    LauncherCredentials credentials;
};

constexpr int kChildSessionProtocolVersion = 1;

QByteArray serialize_child_session_launch_request(const ChildSessionLaunchRequest& request);

} // namespace hitsc
