#pragma once

#include "gui/child/launcher_child_process_manager.hpp"
#include "launcher_host_store.hpp"
#include "launcher_reachability_probe.hpp"
#include "options.hpp"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace hitsc {

class LauncherHostModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TypeRole,
        TypeLabelRole,
        UrlRole,
        HostRole,
        StatusRole,
        StatusLabelRole,
        HasCredentialsRole,
    };

    explicit LauncherHostModel(VerbosityOptions verbosity = {}, QObject* parent = nullptr);
    ~LauncherHostModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    Q_INVOKABLE QVariantMap addHost(
        const QString& type,
        const QString& host,
        const QString& username,
        const QString& password,
        const QString& repeat_password);
    Q_INVOKABLE QVariantMap hostDetails(const QString& host_id) const;
    Q_INVOKABLE QVariantMap updateHost(
        const QString& host_id,
        const QString& type,
        const QString& host,
        const QString& username,
        const QString& password,
        const QString& repeat_password);
    Q_INVOKABLE QVariantMap deleteHost(const QString& host_id);
    Q_INVOKABLE QVariantMap connectHost(const QString& host_id);

    // Mini-launcher: filtered/ranked host list for the editable picker, and a
    // connect path that resolves typed-or-saved credentials, upserts the host
    // (saving the password only when asked), then launches.
    Q_INVOKABLE QVariantList searchHosts(const QString& text) const;
    Q_INVOKABLE QVariantMap quickConnect(
        const QString& existing_id,
        const QString& host,
        const QString& type,
        const QString& username,
        const QString& password,
        bool save_credentials);

    // Display host of the most recently connected saved host (empty if none or
    // it has since been deleted). Used to seed the mini launcher on open.
    Q_INVOKABLE QString lastConnectedHost() const;

    void shutdown();

signals:
    void countChanged();

private:
    void start_probes();
    void update_reachability(const QString& host_id, bool online);
    int index_for_id(const QString& host_id) const;
    int index_for_url(const QString& url) const;
    int insertion_row_for_host(const SavedHost& host) const;
    void sort_hosts();

    HostStore store_;
    QList<SavedHost> hosts_;
    ChildProcessManager child_processes_;
    ReachabilityProbe reachability_probe_;
    QTimer probe_timer_;
    QSet<QString> probes_in_flight_;
    QHash<QString, int> consecutive_probe_failures_;
};

} // namespace hitsc
