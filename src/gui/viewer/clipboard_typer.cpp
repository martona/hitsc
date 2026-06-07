#include "gui/viewer/clipboard_typer.hpp"

#include "gui/viewer/keyboard_layout.hpp"

#include <QTimer>

#include <algorithm>
#include <iterator>
#include <utility>

namespace hitsc {
namespace {

// Deliberately conservative. Each tick is one HID state change; the guest's emulated
// keyboard is polled at a fixed rate, so anything faster risks a dropped down/up.
constexpr int kTypeIntervalMs = 30;

} // namespace

ClipboardTyper::ClipboardTyper(QObject* parent)
    : QObject(parent)
{
    timer_ = new QTimer(this);
    timer_->setInterval(kTypeIntervalMs);
    connect(timer_, &QTimer::timeout, this, &ClipboardTyper::tick);
}

ClipboardTyper::~ClipboardTyper() = default;

void ClipboardTyper::set_hooks(
    std::function<bool()> connected, std::function<void(const KvmKeyEvent&)> feed)
{
    connected_ = std::move(connected);
    feed_ = std::move(feed);
}

void ClipboardTyper::start(const TypePlan& plan)
{
    if (active_) {
        return;  // already typing; a click cancels (the control routes that to cancel())
    }
    queue_.clear();
    pressed_.clear();
    next_ = 0;

    // Expand each chord into key events, holding shared modifiers across consecutive
    // chords instead of releasing and re-pressing them (roughly halves the events for runs
    // of shifted/AltGr text). Any modifiers still held at the end are released.
    const auto contains = [](const std::vector<KvmScancode>& v, KvmScancode m) {
        return std::find(v.begin(), v.end(), m) != v.end();
    };
    std::vector<KvmScancode> held;
    for (const KeyChord& chord : plan.chords) {
        // Release held modifiers this chord doesn't want (newest first).
        for (std::size_t i = held.size(); i-- > 0;) {
            if (!contains(chord.modifiers, held[i])) {
                queue_.push_back(KvmKeyEvent{held[i], false, false});
                held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        // Press modifiers this chord wants that aren't held yet.
        for (const KvmScancode mod : chord.modifiers) {
            if (!contains(held, mod)) {
                queue_.push_back(KvmKeyEvent{mod, true, false});
                held.push_back(mod);
            }
        }
        queue_.push_back(KvmKeyEvent{chord.key, true, false});
        queue_.push_back(KvmKeyEvent{chord.key, false, false});
    }
    for (std::size_t i = held.size(); i-- > 0;) {
        queue_.push_back(KvmKeyEvent{held[i], false, false});
    }

    if (queue_.empty()) {
        return;
    }
    active_ = true;
    emit started(static_cast<int>(queue_.size()));
    timer_->start();
}

void ClipboardTyper::cancel()
{
    if (active_) {
        stop_with(false);
    }
}

void ClipboardTyper::tick()
{
    if (!active_) {
        return;
    }
    // Guest went away mid-paste: release whatever's held and bail.
    if (connected_ && !connected_()) {
        stop_with(false);
        return;
    }

    const KvmKeyEvent& event = queue_[next_++];
    if (feed_) {
        feed_(event);
    }
    // Track in-flight presses so an abort can release exactly what's down.
    if (event.down) {
        pressed_.push_back(event.scancode);
    } else {
        const auto it = std::find(pressed_.rbegin(), pressed_.rend(), event.scancode);
        if (it != pressed_.rend()) {
            pressed_.erase(std::next(it).base());
        }
    }

    const int remaining = static_cast<int>(queue_.size() - next_);
    if (remaining > 0) {
        emit progress(remaining);
    } else {
        stop_with(true);
    }
}

void ClipboardTyper::release_pressed()
{
    if (feed_) {
        for (auto it = pressed_.rbegin(); it != pressed_.rend(); ++it) {
            feed_(KvmKeyEvent{*it, false, false});
        }
    }
    pressed_.clear();
}

void ClipboardTyper::stop_with(bool completed)
{
    timer_->stop();
    release_pressed();  // no-op on normal completion (the plan already released everything)
    active_ = false;
    queue_.clear();
    next_ = 0;
    emit finished(completed);
}

} // namespace hitsc
