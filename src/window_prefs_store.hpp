#pragma once

#include <optional>

#include <QRect>
#include <QString>

namespace hitsc {

class WindowPrefsStore {
public:
    explicit WindowPrefsStore(QString root_path = QStringLiteral("Software\\hitsc"));

    std::optional<QRect> load_window_rect(const QString& window_name) const;
    void save_window_rect(const QString& window_name, const QRect& rect) const;

    const QString& root_path() const;

private:
    QString root_path_;
};

} // namespace hitsc
