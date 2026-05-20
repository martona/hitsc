#include "launcher_host_model.hpp"

#include <QMetaObject>
#include <QModelIndex>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <exception>

namespace hitsc {
namespace {

QVariantMap ok_result()
{
    return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("error"), QString{}}};
}

QVariantMap error_result(const QString& message)
{
    return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

} // namespace

LauncherHostModel::LauncherHostModel(QObject* parent)
    : QAbstractListModel(parent)
    , hosts_(store_.load_hosts())
    , reachability_probe_(this)
{
    probe_timer_.setInterval(1000);
    connect(&probe_timer_, &QTimer::timeout, this, &LauncherHostModel::start_probes);
    probe_timer_.start();
    QTimer::singleShot(0, this, &LauncherHostModel::start_probes);
}

LauncherHostModel::~LauncherHostModel()
{
    shutdown();
}

void LauncherHostModel::shutdown()
{
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, [this] { shutdown(); }, Qt::BlockingQueuedConnection);
        return;
    }

    probe_timer_.stop();
    probes_in_flight_.clear();
    reachability_probe_.shutdown();
}

int LauncherHostModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return hosts_.size();
}

QVariant LauncherHostModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= hosts_.size()) {
        return {};
    }

    const SavedHost& host = hosts_.at(index.row());
    switch (role) {
    case IdRole:
        return host.id;
    case TypeRole:
        return launcher_host_type_key(host.type);
    case TypeLabelRole:
        return launcher_host_type_label(host.type);
    case NameRole:
        return host.name;
    case UrlRole:
        return host.url;
    case HostRole:
        return host_from_launcher_url(host.url);
    case StatusRole:
        return reachability_status_key(host.reachability);
    case StatusLabelRole:
        return reachability_status_label(host.reachability);
    case HasCredentialsRole:
        return host.credentials.has_value() && !host.credentials->empty();
    default:
        return {};
    }
}

QHash<int, QByteArray> LauncherHostModel::roleNames() const
{
    return {
        {IdRole, "hostId"},
        {TypeRole, "type"},
        {TypeLabelRole, "typeLabel"},
        {NameRole, "name"},
        {UrlRole, "url"},
        {HostRole, "host"},
        {StatusRole, "status"},
        {StatusLabelRole, "statusLabel"},
        {HasCredentialsRole, "hasCredentials"},
    };
}

int LauncherHostModel::count() const
{
    return hosts_.size();
}

QVariantMap LauncherHostModel::addHost(
    const QString& type,
    const QString& name,
    const QString& url,
    const QString& username,
    const QString& password,
    const QString& repeat_password)
{
    const std::optional<LauncherHostType> parsed_type = parse_launcher_host_type(type);
    if (!parsed_type) {
        return error_result(QStringLiteral("Choose a supported host type."));
    }

    const QString trimmed_name = name.trimmed();
    if (trimmed_name.isEmpty()) {
        return error_result(QStringLiteral("Name is required."));
    }

    const QString trimmed_url = url.trimmed();
    QString url_error;
    if (trimmed_url.isEmpty() || !validate_launcher_url(trimmed_url, &url_error)) {
        return error_result(
            url_error.isEmpty() ? QStringLiteral("Enter a valid https:// URL.") : url_error);
    }

    if (password != repeat_password) {
        return error_result(QStringLiteral("Passwords do not match."));
    }

    SavedHost host;
    host.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    host.type = *parsed_type;
    host.name = trimmed_name;
    host.url = trimmed_url;
    host.reachability = ReachabilityStatus::Unknown;

    LauncherCredentials credentials;
    credentials.username = username.trimmed();
    credentials.password = password;
    if (!credentials.empty()) {
        host.credentials = credentials;
    }

    try {
        store_.save_host(host);
    } catch (const std::exception& ex) {
        return error_result(QString::fromUtf8(ex.what()));
    }

    const int row = hosts_.size();
    beginInsertRows(QModelIndex(), row, row);
    hosts_.append(host);
    endInsertRows();
    emit countChanged();
    start_probes();
    return ok_result();
}

QVariantMap LauncherHostModel::hostDetails(const QString& host_id) const
{
    const int row = index_for_id(host_id);
    if (row < 0) {
        return error_result(QStringLiteral("Saved host was not found."));
    }

    const SavedHost& host = hosts_.at(row);
    return QVariantMap{
        {QStringLiteral("ok"), true},
        {QStringLiteral("error"), QString{}},
        {QStringLiteral("id"), host.id},
        {QStringLiteral("type"), launcher_host_type_key(host.type)},
        {QStringLiteral("typeLabel"), launcher_host_type_label(host.type)},
        {QStringLiteral("name"), host.name},
        {QStringLiteral("url"), host.url},
        {QStringLiteral("username"), host.credentials ? host.credentials->username : QString{}},
        {QStringLiteral("hasCredentials"), host.credentials.has_value() && !host.credentials->empty()},
    };
}

QVariantMap LauncherHostModel::updateHost(
    const QString& host_id,
    const QString& type,
    const QString& name,
    const QString& url,
    const QString& username,
    const QString& password,
    const QString& repeat_password)
{
    const int row = index_for_id(host_id);
    if (row < 0) {
        return error_result(QStringLiteral("Saved host was not found."));
    }

    const std::optional<LauncherHostType> parsed_type = parse_launcher_host_type(type);
    if (!parsed_type) {
        return error_result(QStringLiteral("Choose a supported host type."));
    }

    const QString trimmed_name = name.trimmed();
    if (trimmed_name.isEmpty()) {
        return error_result(QStringLiteral("Name is required."));
    }

    const QString trimmed_url = url.trimmed();
    QString url_error;
    if (trimmed_url.isEmpty() || !validate_launcher_url(trimmed_url, &url_error)) {
        return error_result(
            url_error.isEmpty() ? QStringLiteral("Enter a valid https:// URL.") : url_error);
    }

    if (password != repeat_password) {
        return error_result(QStringLiteral("Passwords do not match."));
    }

    SavedHost host = hosts_.at(row);
    const bool url_changed = host.url != trimmed_url;
    host.type = *parsed_type;
    host.name = trimmed_name;
    host.url = trimmed_url;

    LauncherCredentials credentials;
    credentials.username = username.trimmed();
    if (password.isEmpty() && repeat_password.isEmpty() && host.credentials) {
        credentials.password = host.credentials->password;
    } else {
        credentials.password = password;
    }

    if (!credentials.empty()) {
        host.credentials = credentials;
    } else {
        host.credentials.reset();
    }

    if (url_changed) {
        probes_in_flight_.remove(host.id);
        host.reachability = ReachabilityStatus::Unknown;
    }

    try {
        store_.save_host(host);
    } catch (const std::exception& ex) {
        return error_result(QString::fromUtf8(ex.what()));
    }

    hosts_[row] = host;
    const QModelIndex changed = index(row, 0);
    emit dataChanged(
        changed,
        changed,
        {TypeRole,
         TypeLabelRole,
         NameRole,
         UrlRole,
         HostRole,
         StatusRole,
         StatusLabelRole,
         HasCredentialsRole});

    if (url_changed) {
        start_probes();
    }

    return ok_result();
}

QVariantMap LauncherHostModel::deleteHost(const QString& host_id)
{
    const int row = index_for_id(host_id);
    if (row < 0) {
        return error_result(QStringLiteral("Saved host was not found."));
    }

    try {
        store_.delete_host(host_id);
    } catch (const std::exception& ex) {
        return error_result(QString::fromUtf8(ex.what()));
    }

    probes_in_flight_.remove(host_id);
    beginRemoveRows(QModelIndex(), row, row);
    hosts_.removeAt(row);
    endRemoveRows();
    emit countChanged();
    return ok_result();
}

QString LauncherHostModel::defaultUrlForName(const QString& name) const
{
    return sanitize_host_name_to_url(name);
}

void LauncherHostModel::start_probes()
{
    for (int row = 0; row < hosts_.size(); ++row) {
        SavedHost& saved_host = hosts_[row];
        if (probes_in_flight_.contains(saved_host.id)) {
            continue;
        }

        const QString host = host_from_launcher_url(saved_host.url);
        if (host.isEmpty()) {
            saved_host.reachability = ReachabilityStatus::Offline;
            const QModelIndex changed = index(row, 0);
            emit dataChanged(changed, changed, {StatusRole, StatusLabelRole});
            continue;
        }

        probes_in_flight_.insert(saved_host.id);
        if (saved_host.reachability == ReachabilityStatus::Unknown) {
            saved_host.reachability = ReachabilityStatus::Checking;
            const QModelIndex changed = index(row, 0);
            emit dataChanged(changed, changed, {StatusRole, StatusLabelRole});
        }

        reachability_probe_.probe(
            saved_host.id,
            host,
            this,
            [this](QString host_id, bool online) {
                update_reachability(host_id, online);
            });
    }
}

void LauncherHostModel::update_reachability(const QString& host_id, bool online)
{
    probes_in_flight_.remove(host_id);
    const int row = index_for_id(host_id);
    if (row < 0) {
        return;
    }

    const ReachabilityStatus next_status =
        online ? ReachabilityStatus::Online : ReachabilityStatus::Offline;
    if (hosts_[row].reachability == next_status) {
        return;
    }

    hosts_[row].reachability = next_status;
    const QModelIndex changed = index(row, 0);
    emit dataChanged(changed, changed, {StatusRole, StatusLabelRole});
}

int LauncherHostModel::index_for_id(const QString& host_id) const
{
    for (int row = 0; row < hosts_.size(); ++row) {
        if (hosts_.at(row).id == host_id) {
            return row;
        }
    }
    return -1;
}

} // namespace hitsc
