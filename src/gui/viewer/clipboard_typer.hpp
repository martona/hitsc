#pragma once

#include "view_input_types.hpp"  // KvmKeyEvent, KvmScancode

#include <QObject>

#include <cstddef>
#include <functional>
#include <vector>

class QTimer;

namespace hitsc {

struct TypePlan;

// Drives a TypePlan onto the wire as paced key events. Each chord is expanded to
// down/up events (mods down, key down, key up, mods up) and fed one event per timer
// tick, deliberately slow: every HID state change must persist at least one guest poll
// or BMC keyboard emulation drops it, so bursts are unsafe. GUI-thread only -- same path
// and thread as live keystrokes, just clocked.
class ClipboardTyper : public QObject {
    Q_OBJECT

public:
    explicit ClipboardTyper(QObject* parent = nullptr);
    ~ClipboardTyper() override;

    // connected(): may we still type into the guest? feed(): deliver one event.
    void set_hooks(std::function<bool()> connected, std::function<void(const KvmKeyEvent&)> feed);

    void start(const TypePlan& plan);  // no-op if already typing
    void cancel();                     // release anything still held, then stop
    bool busy() const { return active_; }

signals:
    void started(int characters);
    void finished(bool completed);  // false = canceled by user or guest disconnected

private:
    void tick();
    void release_pressed();
    void stop_with(bool completed);

    std::function<bool()> connected_;
    std::function<void(const KvmKeyEvent&)> feed_;

    QTimer* timer_ = nullptr;
    std::vector<KvmKeyEvent> queue_;
    std::size_t next_ = 0;
    std::vector<KvmScancode> pressed_;  // down-but-not-yet-up, for a clean abort
    int characters_ = 0;
    bool active_ = false;
};

} // namespace hitsc
