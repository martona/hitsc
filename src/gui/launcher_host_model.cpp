#include "launcher_host_model.hpp"

#include <QMetaObject>
#include <QModelIndex>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <exception>
#include <iterator>

namespace hitsc {
namespace {

constexpr int kOnlineOfflineFailureThreshold = 2;

QVariantMap ok_result()
{
    return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("error"), QString{}}};
}

QVariantMap error_result(const QString& message)
{
    return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

} // namespace

LauncherHostModel::LauncherHostModel(VerbosityOptions verbosity, QObject* parent)
    : QAbstractListModel(parent)
    , hosts_(store_.load_hosts())
    , child_processes_(verbosity)
{
    sort_hosts();
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
    consecutive_probe_failures_.clear();
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
    case UrlRole:
        return host.url;
    case HostRole:
        return launcher_display_host(host.url);
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
    const QString& host,
    const QString& username,
    const QString& password,
    const QString& repeat_password)
{
    const std::optional<LauncherHostType> parsed_type = parse_launcher_host_type(type);
    if (!parsed_type) {
        return error_result(QStringLiteral("Choose a supported host type."));
    }

    const QString url = launcher_url_from_host(host);
    QString url_error;
    if (url.isEmpty() || !validate_launcher_url(url, &url_error)) {
        return error_result(
            url_error.isEmpty() ? QStringLiteral("Enter a valid host.") : url_error);
    }

    if (index_for_url(url) >= 0) {
        return error_result(QStringLiteral("That host already exists."));
    }

    if (password != repeat_password) {
        return error_result(QStringLiteral("Passwords do not match."));
    }

    SavedHost saved;
    saved.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    saved.type = *parsed_type;
    saved.url = url;
    saved.reachability = ReachabilityStatus::Unknown;

    LauncherCredentials credentials;
    credentials.username = username.trimmed();
    credentials.password = password;
    if (!credentials.empty()) {
        saved.credentials = credentials;
    }

    try {
        store_.save_host(saved);
    } catch (const std::exception& ex) {
        return error_result(QString::fromUtf8(ex.what()));
    }

    const int row = insertion_row_for_host(saved);
    beginInsertRows(QModelIndex(), row, row);
    hosts_.insert(row, saved);
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
        {QStringLiteral("url"), host.url},
        {QStringLiteral("host"), launcher_display_host(host.url)},
        {QStringLiteral("username"), host.credentials ? host.credentials->username : QString{}},
        {QStringLiteral("hasCredentials"), host.credentials.has_value() && !host.credentials->empty()},
    };
}

QVariantMap LauncherHostModel::updateHost(
    const QString& host_id,
    const QString& type,
    const QString& host,
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

    const QString url = launcher_url_from_host(host);
    QString url_error;
    if (url.isEmpty() || !validate_launcher_url(url, &url_error)) {
        return error_result(
            url_error.isEmpty() ? QStringLiteral("Enter a valid host.") : url_error);
    }

    const int existing = index_for_url(url);
    if (existing >= 0 && existing != row) {
        return error_result(QStringLiteral("That host already exists."));
    }

    if (password != repeat_password) {
        return error_result(QStringLiteral("Passwords do not match."));
    }

    SavedHost updated = hosts_.at(row);
    const bool url_changed = updated.url != url;
    updated.type = *parsed_type;
    updated.url = url;

    LauncherCredentials credentials;
    credentials.username = username.trimmed();
    if (password.isEmpty() && repeat_password.isEmpty() && updated.credentials) {
        credentials.password = updated.credentials->password;
    } else {
        credentials.password = password;
    }

    if (!credentials.empty()) {
        updated.credentials = credentials;
    } else {
        updated.credentials.reset();
    }

    if (url_changed) {
        probes_in_flight_.remove(updated.id);
        consecutive_probe_failures_.remove(updated.id);
        updated.reachability = ReachabilityStatus::Unknown;
    }

    try {
        store_.save_host(updated);
    } catch (const std::exception& ex) {
        return error_result(QString::fromUtf8(ex.what()));
    }

    emit layoutAboutToBeChanged();
    hosts_[row] = updated;
    sort_hosts();
    emit layoutChanged();
    if (!hosts_.empty()) {
        const QModelIndex first = index(0, 0);
        const QModelIndex last = index(hosts_.size() - 1, 0);
        emit dataChanged(
            first,
            last,
            {TypeRole,
             TypeLabelRole,
             UrlRole,
             HostRole,
             StatusRole,
             StatusLabelRole,
             HasCredentialsRole});
    }

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
    consecutive_probe_failures_.remove(host_id);
    beginRemoveRows(QModelIndex(), row, row);
    hosts_.removeAt(row);
    endRemoveRows();
    emit countChanged();
    return ok_result();
}

QVariantMap LauncherHostModel::connectHost(const QString& host_id)
{
    const int row = index_for_id(host_id);
    if (row < 0) {
        return error_result(QStringLiteral("Saved host was not found."));
    }

    return child_processes_.launch_host(hosts_.at(row));
}

QVariantList LauncherHostModel::searchHosts(const QString& text) const
{
    const QString needle = text.trimmed().toLower();

    QVariantList prefix_matches;
    QVariantList substring_matches;
    for (const SavedHost& host : hosts_) {
        const QString label = launcher_display_host(host.url);
        const QString label_lower = label.toLower();

        QString match_kind;
        QVariantList* bucket = nullptr;
        if (needle.isEmpty()) {
            match_kind = QStringLiteral("all");
            bucket = &prefix_matches;
        } else if (label_lower.startsWith(needle)) {
            match_kind = QStringLiteral("hostPrefix");
            bucket = &prefix_matches;
        } else if (label_lower.contains(needle)) {
            match_kind = QStringLiteral("hostSubstring");
            bucket = &substring_matches;
        } else {
            continue;
        }

        bucket->append(QVariantMap{
            {QStringLiteral("id"), host.id},
            {QStringLiteral("host"), label},
            {QStringLiteral("url"), host.url},
            {QStringLiteral("typeKey"), launcher_host_type_key(host.type)},
            {QStringLiteral("typeLabel"), launcher_host_type_label(host.type)},
            {QStringLiteral("status"), reachability_status_key(host.reachability)},
            {QStringLiteral("statusLabel"), reachability_status_label(host.reachability)},
            {QStringLiteral("hasCredentials"),
             host.credentials.has_value() && !host.credentials->empty()},
            {QStringLiteral("matchKind"), match_kind},
        });
    }

    prefix_matches.append(substring_matches);
    return prefix_matches;
}

QVariantMap LauncherHostModel::quickConnect(
    const QString& existing_id,
    const QString& host,
    const QString& type,
    const QString& username,
    const QString& password,
    bool save_credentials)
{
    int row = -1;
    if (!existing_id.isEmpty()) {
        row = index_for_id(existing_id);
        if (row < 0) {
            return error_result(QStringLiteral("Saved host was not found."));
        }
    }

    SavedHost target;
    bool is_new = false;
    if (row >= 0) {
        target = hosts_.at(row);
    } else {
        const QString url = launcher_url_from_host(host);
        QString url_error;
        if (url.isEmpty() || !validate_launcher_url(url, &url_error)) {
            return error_result(
                url_error.isEmpty() ? QStringLiteral("Enter a valid host.") : url_error);
        }

        row = index_for_url(url);
        if (row >= 0) {
            target = hosts_.at(row);
        } else {
            const std::optional<LauncherHostType> parsed_type =
                type.trimmed().isEmpty()
                ? std::optional<LauncherHostType>(LauncherHostType::Auto)
                : parse_launcher_host_type(type);
            if (!parsed_type) {
                return error_result(QStringLiteral("Choose a supported host type."));
            }
            target.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            target.type = *parsed_type;
            target.url = url;
            target.reachability = ReachabilityStatus::Unknown;
            is_new = true;
        }
    }

    bool type_changed = false;
    if (!is_new && !type.trimmed().isEmpty()) {
        const std::optional<LauncherHostType> parsed_type = parse_launcher_host_type(type);
        if (parsed_type && *parsed_type != target.type) {
            target.type = *parsed_type;
            type_changed = true;
        }
    }

    QString resolved_username = username.trimmed();
    QString resolved_password = password;
    if (resolved_username.isEmpty() && target.credentials) {
        resolved_username = target.credentials->username;
    }
    if (resolved_password.isEmpty() && target.credentials) {
        resolved_password = target.credentials->password;
    }
    if (resolved_username.isEmpty() || resolved_password.isEmpty()) {
        return error_result(QStringLiteral("Enter a username and password to connect."));
    }

    if (save_credentials) {
        target.credentials = LauncherCredentials{resolved_username, resolved_password};
    } else if (is_new) {
        target.credentials.reset();
    }

    if (is_new || save_credentials || type_changed) {
        try {
            store_.save_host(target);
        } catch (const std::exception& ex) {
            return error_result(QString::fromUtf8(ex.what()));
        }
    }

    if (is_new) {
        const int insert_row = insertion_row_for_host(target);
        beginInsertRows(QModelIndex(), insert_row, insert_row);
        hosts_.insert(insert_row, target);
        endInsertRows();
        emit countChanged();
        start_probes();
    } else if (save_credentials || type_changed) {
        hosts_[row] = target;
        const QModelIndex changed = index(row, 0);
        emit dataChanged(changed, changed, {TypeRole, TypeLabelRole, HasCredentialsRole});
    }

    SavedHost launch = target;
    launch.credentials = LauncherCredentials{resolved_username, resolved_password};
    return child_processes_.launch_host(launch);
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
            consecutive_probe_failures_.remove(saved_host.id);
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

    ReachabilityStatus next_status = ReachabilityStatus::Offline;
    if (online) {
        consecutive_probe_failures_.remove(host_id);
        next_status = ReachabilityStatus::Online;
    } else {
        const int failure_count = consecutive_probe_failures_.value(host_id, 0) + 1;
        consecutive_probe_failures_.insert(host_id, failure_count);
        if (hosts_[row].reachability == ReachabilityStatus::Online
            && failure_count < kOnlineOfflineFailureThreshold) {
            return;
        }
    }

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

int LauncherHostModel::index_for_url(const QString& url) const
{
    for (int row = 0; row < hosts_.size(); ++row) {
        if (QString::compare(hosts_.at(row).url, url, Qt::CaseInsensitive) == 0) {
            return row;
        }
    }
    return -1;
}

int LauncherHostModel::insertion_row_for_host(const SavedHost& host) const
{
    const auto position =
        std::lower_bound(hosts_.begin(), hosts_.end(), host, saved_host_hostname_less);
    return static_cast<int>(std::distance(hosts_.begin(), position));
}

void LauncherHostModel::sort_hosts()
{
    std::sort(hosts_.begin(), hosts_.end(), saved_host_hostname_less);
}

} // namespace hitsc
