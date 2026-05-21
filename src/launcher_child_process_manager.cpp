#include "launcher_child_process_manager.hpp"

#include "launcher_child_protocol.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

#include <iostream>
#include <memory>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

QString log_prefix_for_host(const SavedHost& host)
{
    const QString name = host.name.trimmed();
    if (!name.isEmpty()) {
        return name;
    }
    return host.url;
}

#ifdef _WIN32

struct WindowSearch {
    DWORD process_id = 0;
    HWND window = nullptr;
};

BOOL CALLBACK find_process_window(HWND hwnd, LPARAM context)
{
    auto* search = reinterpret_cast<WindowSearch*>(context);
    if (search == nullptr || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id != search->process_id) {
        return TRUE;
    }

    search->window = hwnd;
    return FALSE;
}

bool activate_process_window(qint64 process_id)
{
    WindowSearch search;
    search.process_id = static_cast<DWORD>(process_id);
    EnumWindows(&find_process_window, reinterpret_cast<LPARAM>(&search));
    if (search.window == nullptr) {
        return false;
    }

    ShowWindow(search.window, SW_RESTORE);
    SetForegroundWindow(search.window);
    return true;
}

bool process_has_window(qint64 process_id)
{
    WindowSearch search;
    search.process_id = static_cast<DWORD>(process_id);
    EnumWindows(&find_process_window, reinterpret_cast<LPARAM>(&search));
    return search.window != nullptr;
}

#else

bool activate_process_window(qint64 process_id)
{
    (void)process_id;
    return false;
}

bool process_has_window(qint64 process_id)
{
    (void)process_id;
    return true;
}

#endif

} // namespace

struct ChildProcessManager::Session {
    QString host_id;
    QString log_prefix;
    QProcess* process = nullptr;
    QByteArray stdout_buffer;
    QByteArray stderr_buffer;
};

ChildProcessManager::ChildProcessManager(QObject* parent)
    : QObject(parent)
{
}

ChildProcessManager::~ChildProcessManager()
{
    detach_running_children();
}

QVariantMap ChildProcessManager::launch_host(const SavedHost& host)
{
    return activate_or_launch(host);
}

QVariantMap ChildProcessManager::activate_or_launch(const SavedHost& host)
{
    const QList<Session*> existing_sessions = sessions_.values(host.id);
    for (Session* session : existing_sessions) {
        if (session != nullptr
            && session->process != nullptr
            && session->process->state() != QProcess::NotRunning) {
            const qint64 process_id = session->process->processId();
            if (process_has_window(process_id)) {
                activate_process_window(process_id);
                return ok_result();
            }
        }
    }

    if (!host.credentials || host.credentials->username.trimmed().isEmpty()
        || host.credentials->password.isEmpty()) {
        return error_result(QStringLiteral("Saved credentials are required to connect."));
    }

    auto session = std::make_unique<Session>();
    session->host_id = host.id;
    session->log_prefix = log_prefix_for_host(host);
    session->process = new QProcess();
    QProcess* process = session->process;

    process->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("QT_FATAL_WARNINGS"));
    process->setProcessEnvironment(environment);
#ifdef _WIN32
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        if (args != nullptr) {
            args->flags |= CREATE_NO_WINDOW;
        }
    });
#endif

    ChildSessionLaunchRequest request;
    request.session_id = host.id;
    request.type = host.type;
    request.display_name = host.name;
    request.url = host.url;
    request.credentials = *host.credentials;
    request.parent_process_id = QCoreApplication::applicationPid();

    const QByteArray payload = serialize_child_session_launch_request(request);
    Session* raw_session = session.get();
    sessions_.insert(host.id, session.release());

    connect(process, &QProcess::readyReadStandardOutput, this, [this, raw_session] {
        drain_stdout(*raw_session);
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, raw_session] {
        drain_stderr(*raw_session);
    });
    connect(
        process,
        &QProcess::finished,
        this,
        [this, raw_session](int exit_code, QProcess::ExitStatus exit_status) {
            drain_stdout(*raw_session);
            drain_stderr(*raw_session);
            if (!raw_session->stderr_buffer.isEmpty()) {
                flush_stderr_line(*raw_session, std::exchange(raw_session->stderr_buffer, {}));
            }

            if (exit_status != QProcess::NormalExit || exit_code != 0) {
                std::cerr << raw_session->log_prefix.toStdString()
                          << ": child exited with code " << exit_code << "\n";
            }

            sessions_.remove(raw_session->host_id, raw_session);
            raw_session->process->deleteLater();
            delete raw_session;
        });
    connect(process, &QProcess::errorOccurred, this, [raw_session](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            std::cerr << raw_session->log_prefix.toStdString()
                      << ": failed to start child process\n";
        }
    });

    process->start(QCoreApplication::applicationFilePath(), QStringList{QStringLiteral("child")});
    if (!process->waitForStarted(3000)) {
        sessions_.remove(host.id, raw_session);
        const QString message = process->errorString();
        process->disconnect(this);
        process->deleteLater();
        delete raw_session;
        return error_result(message);
    }

    process->write(payload);
    process->closeWriteChannel();
    return ok_result();
}

void ChildProcessManager::drain_stdout(Session& session)
{
    if (session.process == nullptr) {
        return;
    }

    session.stdout_buffer.append(session.process->readAllStandardOutput());
    while (true) {
        const qsizetype newline = session.stdout_buffer.indexOf('\n');
        if (newline < 0) {
            break;
        }

        const QByteArray line = session.stdout_buffer.left(newline).trimmed();
        session.stdout_buffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            std::cerr << session.log_prefix.toStdString()
                      << ": ignored malformed child stdout\n";
        }
    }
}

void ChildProcessManager::drain_stderr(Session& session)
{
    if (session.process == nullptr) {
        return;
    }

    session.stderr_buffer.append(session.process->readAllStandardError());
    while (true) {
        const qsizetype newline = session.stderr_buffer.indexOf('\n');
        if (newline < 0) {
            break;
        }

        QByteArray line = session.stderr_buffer.left(newline);
        session.stderr_buffer.remove(0, newline + 1);
        flush_stderr_line(session, std::move(line));
    }
}

void ChildProcessManager::flush_stderr_line(Session& session, QByteArray line)
{
    while (!line.isEmpty() && (line.endsWith('\n') || line.endsWith('\r'))) {
        line.chop(1);
    }

    std::cerr << session.log_prefix.toStdString() << ": " << line.constData() << "\n";
}

void ChildProcessManager::detach_running_children()
{
    const QList<Session*> existing_sessions = sessions_.values();
    for (Session* session : existing_sessions) {
        if (session == nullptr) {
            continue;
        }

        if (session->process != nullptr && session->process->state() != QProcess::NotRunning) {
            session->process->disconnect(this);
            session->process->closeWriteChannel();
            session->process->setParent(nullptr);
            session->process = nullptr;
            delete session;
            continue;
        }

        if (session->process != nullptr) {
            session->process->deleteLater();
        }
        delete session;
    }
    sessions_.clear();
}

} // namespace hitsc
