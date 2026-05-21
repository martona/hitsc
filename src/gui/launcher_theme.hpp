#pragma once

#include <QObject>
#include <Qt>

namespace hitsc {

bool launcher_should_use_dark_theme(Qt::ColorScheme color_scheme);

class LauncherTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool darkMode READ darkMode NOTIFY darkModeChanged)

public:
    explicit LauncherTheme(Qt::ColorScheme color_scheme, QObject* parent = nullptr);

    bool darkMode() const;
    void setColorScheme(Qt::ColorScheme color_scheme);

signals:
    void darkModeChanged();

private:
    bool dark_mode_ = false;
};

} // namespace hitsc
