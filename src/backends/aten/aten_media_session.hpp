#pragma once

#include "options.hpp"
#include "virtual_media/virtual_media.hpp"  // MediaSessionState (shared across backends)

#include <atomic>
#include <string>

namespace hitsc {

// Runs one ATEN (Supermicro) virtual-CD redirection session: logs in (its own BMC session),
// fetches the iKVM bootstrap credential, opens the /vm WebSocket, emulates a USB mass-storage
// device backed by `iso_path` (USB-BOT: plug-in descriptors, then CBW/CSW), and serves SCSI
// reads until `stop_requested` is set or the socket closes. Blocking (owns its io_context).
// Publishes mount state + outcomes to state.media; never throws -- failures are reported through
// the channel, so a media problem never disturbs the video session.
void run_aten_media_session(
    const AtenViewOptions& options,
    const std::string& iso_path,
    MediaSessionState& state,
    const std::atomic_bool& stop_requested);

// CLI test entry: a blocking, standalone mount (own MediaSessionState + Ctrl-C -> graceful
// unmount) that exercises the transport against a real BMC without the GUI.
void run_aten_media(const AtenViewOptions& options, const std::string& iso_path);

} // namespace hitsc
