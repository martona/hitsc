#include "launcher_child_protocol.hpp"

#include "errors.hpp"

#include <QJsonDocument>
#include <QJsonObject>

namespace hitsc {
namespace {

constexpr auto kProtocolName = "hitsc-child-session";

QString required_string(const QJsonObject& root, const QString& key)
{
    const QJsonValue value = root.value(key);
    if (!value.isString()) {
        throw UserError(
            QStringLiteral("missing or invalid child session field: %1").arg(key).toStdString());
    }
    return value.toString();
}

void validate_protocol_header(const QJsonObject& root)
{
    if (root.value(QStringLiteral("protocol")).toString() != QString::fromLatin1(kProtocolName)) {
        throw UserError("invalid child session protocol");
    }

    if (root.value(QStringLiteral("version")).toInt() != kChildSessionProtocolVersion) {
        throw UserError("unsupported child session protocol version");
    }
}

} // namespace

QByteArray serialize_child_session_launch_request(const ChildSessionLaunchRequest& request)
{
    QJsonObject credentials;
    credentials.insert(QStringLiteral("username"), request.credentials.username);
    credentials.insert(QStringLiteral("password"), request.credentials.password);

    QJsonObject root;
    root.insert(QStringLiteral("protocol"), QStringLiteral("hitsc-child-session"));
    root.insert(QStringLiteral("version"), kChildSessionProtocolVersion);
    root.insert(QStringLiteral("command"), QStringLiteral("launch"));
    root.insert(QStringLiteral("sessionId"), request.session_id);
    root.insert(QStringLiteral("type"), launcher_host_type_key(request.type));
    root.insert(QStringLiteral("displayName"), request.display_name);
    root.insert(QStringLiteral("url"), request.url);
    root.insert(QStringLiteral("parentPid"), QString::number(request.parent_process_id));
    root.insert(QStringLiteral("credentials"), credentials);

    QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    payload.append('\n');
    return payload;
}

ChildSessionLaunchRequest parse_child_session_launch_request(const QByteArray& line)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        throw UserError("failed to parse child session launch request");
    }

    const QJsonObject root = document.object();
    validate_protocol_header(root);

    if (root.value(QStringLiteral("command")).toString() != QStringLiteral("launch")) {
        throw UserError("unsupported child session command");
    }

    const std::optional<LauncherHostType> type =
        parse_launcher_host_type(required_string(root, QStringLiteral("type")));
    if (!type) {
        throw UserError("unsupported child session host type");
    }

    const QJsonObject credentials = root.value(QStringLiteral("credentials")).toObject();

    ChildSessionLaunchRequest request;
    request.session_id = required_string(root, QStringLiteral("sessionId"));
    request.type = *type;
    request.display_name = required_string(root, QStringLiteral("displayName"));
    request.url = required_string(root, QStringLiteral("url"));
    request.parent_process_id =
        root.value(QStringLiteral("parentPid")).toString().toLongLong();
    request.credentials.username = credentials.value(QStringLiteral("username")).toString();
    request.credentials.password = credentials.value(QStringLiteral("password")).toString();
    return request;
}

QByteArray serialize_child_session_status(
    const QString& session_id,
    const QString& state,
    const QString& message)
{
    QJsonObject root;
    root.insert(QStringLiteral("protocol"), QString::fromLatin1(kProtocolName));
    root.insert(QStringLiteral("version"), kChildSessionProtocolVersion);
    root.insert(QStringLiteral("type"), QStringLiteral("status"));
    root.insert(QStringLiteral("sessionId"), session_id);
    root.insert(QStringLiteral("state"), state);
    if (!message.isEmpty()) {
        root.insert(QStringLiteral("message"), message);
    }

    QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    payload.append('\n');
    return payload;
}

} // namespace hitsc
