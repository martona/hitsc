#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <functional>

namespace hitsc {

class ReachabilityProbe : public QObject {
    Q_OBJECT

public:
    using Callback = std::function<void(QString host_id, bool online)>;

    explicit ReachabilityProbe(QObject* parent = nullptr);
    ~ReachabilityProbe() override;

    void probe(QString host_id, QString host, QObject* receiver, Callback callback);

    void shutdown();

private:
    void deliver_result(
        QString host_id,
        QPointer<QObject> receiver,
        Callback callback,
        bool online);

    QThreadPool pool_;
    std::atomic_bool shutting_down_{false};
};

} // namespace hitsc
