#pragma once

#include "launcher_host_store.hpp"
#include "launcher_reachability_probe.hpp"

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QTimer>
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
        NameRole,
        UrlRole,
        HostRole,
        StatusRole,
        StatusLabelRole,
        HasCredentialsRole,
    };

    explicit LauncherHostModel(QObject* parent = nullptr);
    ~LauncherHostModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    Q_INVOKABLE QVariantMap addHost(
        const QString& type,
        const QString& name,
        const QString& url,
        const QString& username,
        const QString& password,
        const QString& repeat_password);
    Q_INVOKABLE QVariantMap hostDetails(const QString& host_id) const;
    Q_INVOKABLE QVariantMap updateHost(
        const QString& host_id,
        const QString& type,
        const QString& name,
        const QString& url,
        const QString& username,
        const QString& password,
        const QString& repeat_password);
    Q_INVOKABLE QVariantMap deleteHost(const QString& host_id);
    Q_INVOKABLE QString defaultUrlForName(const QString& name) const;

    void shutdown();

signals:
    void countChanged();

private:
    void start_probes();
    void update_reachability(const QString& host_id, bool online);
    int index_for_id(const QString& host_id) const;

    HostStore store_;
    QList<SavedHost> hosts_;
    ReachabilityProbe reachability_probe_;
    QTimer probe_timer_;
    QSet<QString> probes_in_flight_;
};

} // namespace hitsc
