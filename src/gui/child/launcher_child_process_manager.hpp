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

private:
    struct Session;

    QVariantMap activate_or_launch(const SavedHost& host);
    void drain_stdout(Session& session);
    void drain_stderr(Session& session);
    void flush_stderr_line(Session& session, QByteArray line);
    void detach_running_children();

    QMultiHash<QString, Session*> sessions_;
    VerbosityOptions verbosity_;
};

} // namespace hitsc
