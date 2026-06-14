#pragma once

#include "options.hpp"
#include "virtual_media/virtual_media.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace hitsc {

// Owned by the caller (MegaracView in Phase 3, or the CLI test entry). Bundles the GUI-facing
// MediaChannel with the network thread's force-close hook. Deliberately separate from
// MegaracViewSessionState: the media session must not share the video session's single
// force-close slot, or stopping one would tear down the other.
class MediaSessionState {
public:
    MediaChannel media;

    void set_force_close(std::function<void()> force_close)
    {
        std::lock_guard lock(mutex_);
        force_close_ = std::move(force_close);
    }

    std::function<void()> force_close_snapshot()
    {
        std::lock_guard lock(mutex_);
        return force_close_;
    }

private:
    std::mutex mutex_;
    std::function<void()> force_close_;
};

// Runs one MegaRAC virtual-CD redirection session: logs in (its own BMC session), fetches a
// token, opens the /cd-server WebSocket, emulates a CD device backed by `iso_path`, and serves
// SCSI reads until `stop_requested` is set or the socket closes. Blocking (owns its
// io_context). Publishes mount state + outcomes to state.media; never throws -- failures are
// reported through the channel, so a media problem never disturbs the video session.
void run_megarac_media_session(
    const MegaracViewOptions& options,
    const std::string& iso_path,
    MediaSessionState& state,
    const std::atomic_bool& stop_requested);

// CLI test entry: a blocking, standalone mount (own MediaSessionState + Ctrl-C -> graceful
// unmount) that exercises the transport against a real BMC without the GUI. Returns when the
// session ends.
void run_megarac_media(const MegaracViewOptions& options, const std::string& iso_path);

} // namespace hitsc
