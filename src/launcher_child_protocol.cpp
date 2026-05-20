#include "launcher_child_protocol.hpp"

#include <QJsonDocument>
#include <QJsonObject>

namespace hitsc {

QByteArray serialize_child_session_launch_request(const ChildSessionLaunchRequest& request)
{
    QJsonObject credentials;
    credentials.insert(QStringLiteral("username"), request.credentials.username);
    credentials.insert(QStringLiteral("password"), request.credentials.password);

    QJsonObject root;
    root.insert(QStringLiteral("protocol"), QStringLiteral("hitsc-child-session"));
    root.insert(QStringLiteral("version"), kChildSessionProtocolVersion);
    root.insert(QStringLiteral("command"), QStringLiteral("launch"));
    root.insert(QStringLiteral("type"), launcher_host_type_key(request.type));
    root.insert(QStringLiteral("url"), request.url);
    root.insert(QStringLiteral("credentials"), credentials);

    QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    payload.append('\n');
    return payload;
}

} // namespace hitsc
