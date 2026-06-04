#include "cert_prompt.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QSemaphore>
#include <QString>
#include <QThread>
#include <QWidget>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace hitsc {
namespace {

QString to_qstring(const std::string& value)
{
    return QString::fromStdString(value);
}

QString build_content(
    const std::string& host,
    const CertPromptInfo& info,
    bool changed,
    const std::optional<std::string>& previous_fingerprint)
{
    QString content = QStringLiteral("hitsc could not confirm the identity of ") + to_qstring(host)
        + QLatin1Char('.');

    if (!info.reason.empty()) {
        content += QStringLiteral("\n\n") + to_qstring(info.reason);
    }

    content += QLatin1Char('\n');
    if (!info.subject.empty()) {
        content += QStringLiteral("\nName:  ") + to_qstring(info.subject);
    }
    if (!info.issuer.empty()) {
        content += QStringLiteral("\nIssuer:  ") + to_qstring(info.issuer);
    }
    if (!info.valid_from.empty() || !info.valid_to.empty()) {
        content += QStringLiteral("\nValid:  ") + to_qstring(info.valid_from) + QStringLiteral("  to  ")
            + to_qstring(info.valid_to);
    }
    content += QStringLiteral("\nSHA-256:  ") + to_qstring(info.fingerprint);

    if (changed && previous_fingerprint) {
        content += QStringLiteral("\n\nPreviously pinned:\n") + to_qstring(*previous_fingerprint);
    }

    return content;
}

// Build and run the modal prompt. Must be called on the GUI thread.
CertPromptResult show_cert_prompt_gui(
    QWidget* parent,
    const std::string& host,
    const CertPromptInfo& info,
    bool changed,
    const std::optional<std::string>& previous_fingerprint,
    bool allow_pin)
{
    QMessageBox box(parent);
    box.setWindowTitle(QStringLiteral("hitsc"));
    box.setIcon(changed ? QMessageBox::Critical : QMessageBox::Warning);
    // Plain text: the subject/issuer come from an untrusted certificate, so never
    // let them be interpreted as rich text.
    box.setTextFormat(Qt::PlainText);
    box.setText(
        changed ? QStringLiteral("This host's certificate has changed")
                : QStringLiteral("Untrusted certificate"));
    box.setInformativeText(build_content(host, info, changed, previous_fingerprint));

    QPushButton* connect_button = box.addButton(QStringLiteral("Connect"), QMessageBox::AcceptRole);
    QPushButton* cancel_button = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(changed ? cancel_button : connect_button);
    box.setEscapeButton(cancel_button);

    QCheckBox* pin_box = nullptr;
    if (allow_pin) {
        // QMessageBox takes ownership of the check box.
        pin_box = new QCheckBox(
            changed ? QStringLiteral("Replace the pinned certificate with this one")
                    : QStringLiteral("Pin this certificate and remember it for this host"));
        box.setCheckBox(pin_box);
    }

    box.exec();

    CertPromptResult choice;
    choice.proceed = box.clickedButton() == connect_button;
    choice.pin = allow_pin && choice.proceed && pin_box != nullptr && pin_box->isChecked();
    return choice;
}

// The wait of an in-flight prompt, so the GUI shutdown path can release a network
// thread blocked waiting for us to show the dialog. Single slot: hitsc talks to
// one server with a stable cert for its lifetime, so prompts never overlap.
std::mutex g_pending_mutex;
std::shared_ptr<QSemaphore> g_pending;

} // namespace

CertPromptResult show_cert_prompt(
    void* native_parent,
    const std::string& host,
    const CertPromptInfo& info,
    bool changed,
    const std::optional<std::string>& previous_fingerprint,
    bool allow_pin)
{
    // No Qt application (headless probe) -> cannot prompt; strict verification wins.
    if (qApp == nullptr) {
        return {};
    }

    auto* parent = static_cast<QWidget*>(native_parent);

    // Already on the GUI thread (not expected from the verify callback, but cheap
    // to honour): show it directly.
    if (QThread::currentThread() == qApp->thread()) {
        return show_cert_prompt_gui(parent, host, info, changed, previous_fingerprint, allow_pin);
    }

    // cert_trust_evaluate runs on the network thread; QWidgets live on the GUI
    // thread. Post the dialog there (non-blocking) and wait on our own semaphore,
    // which cancel_pending_cert_prompt() releases on shutdown -- otherwise a
    // window-close mid-prompt would deadlock (network thread blocked here while the
    // GUI thread blocks joining the network thread). The dialog lambda and cancel
    // both run on the GUI thread, so claiming g_pending serialises them: whoever
    // runs first wins, the loser is a no-op.
    CertPromptResult result;
    auto sem = std::make_shared<QSemaphore>();
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending = sem;
    }
    QMetaObject::invokeMethod(
        qApp,
        [&, sem]() {
            {
                std::lock_guard<std::mutex> lock(g_pending_mutex);
                if (g_pending != sem) {
                    return;  // cancelled before we ran; result stays {proceed:false}
                }
                g_pending.reset();
            }
            result = show_cert_prompt_gui(parent, host, info, changed, previous_fingerprint, allow_pin);
            sem->release();
        },
        Qt::QueuedConnection);
    sem->acquire();
    return result;
}

void cancel_pending_cert_prompt()
{
    std::shared_ptr<QSemaphore> sem;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        sem = std::move(g_pending);
    }
    if (sem) {
        sem->release();  // unblock show_cert_prompt with the default {proceed:false}
    }
}

} // namespace hitsc
