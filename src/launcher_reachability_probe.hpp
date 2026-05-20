#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>

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
    QThreadPool thread_pool_;
};

} // namespace hitsc
