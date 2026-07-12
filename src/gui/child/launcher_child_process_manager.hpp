#pragma once

#include "gui/launcher_types.hpp"
#include "options.hpp"

#include <QObject>
#include <QMultiHash>
#include <QVariantMap>

class QProcess;

namespace hitsc {

class ChildProcessManager : public QObject {
    Q_OBJECT

public:
    explicit ChildProcessManager(VerbosityOptions verbosity = {}, QObject* parent = nullptr);
    ~ChildProcessManager() override;

    QVariantMap launch_host(const SavedHost& host);
    // Spawns the "coldreset" verb instead of "child": same stdin handshake, but the
    // child asks the BMC to cold-reset itself rather than opening a KVM session.
    // Tracked under its own session key so it never activates (or is swallowed by)
    // a running KVM window for the same host.
    QVariantMap cold_reset_host(const SavedHost& host);

private:
    struct Session;

    enum class LaunchAction {
        Connect,
        ColdReset,
    };

    QVariantMap activate_or_launch(const SavedHost& host, LaunchAction action);
    void drain_stdout(Session& session);
    void drain_stderr(Session& session);
    void flush_stderr_line(Session& session, QByteArray line);
    void detach_running_children();

    QMultiHash<QString, Session*> sessions_;
    VerbosityOptions verbosity_;
};

} // namespace hitsc
