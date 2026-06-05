#pragma once

#include "backends/aspeed/aspeed_decoder.hpp"
#include "backends/aspeed/aspeed_view_state.hpp"
#include "hardware_cursor.hpp"
#include "view_base.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace hitsc {

class KvmInputController;

// Shared base for the software-decoded ASPEED backends (ATEN + MegaRAC). Owns the
// stateful decoder and the BMC hardware-cursor overlay state, and implements the
// per-tick frame production once: drain the frame queue -> decode each delta ->
// union the dirty rects -> wrap the decoder's framebuffer zero-copy -> build the
// cursor sprite -> assemble a SoftwareFrame. Backends supply only the small
// protocol-specific hooks below, plus their own encoder / network / options.
class AspeedView : public KvmViewBase {
public:
    AspeedView(
        AspeedViewState& state,
        std::string host,
        std::string log_name,
        std::function<void()> network_cleanup);

    std::optional<SoftwareFrame> latest_frame() override;
    std::optional<std::pair<int, int>> latest_frame_size() override;

protected:
    void reset_for_reconnect() override;
    void on_minimized() override;
    void on_focus_lost() override;

    // Request a full keyframe refresh from the BMC (frame-queue overflow, and the
    // backends' on_restored). ATEN sets a refresh flag; MegaRAC enqueues a command.
    virtual void request_full_refresh() = 0;

    // Called once per successfully decoded+presented frame. Default: nothing.
    // (MegaRAC uses it to drive its video-feedback frame counter.)
    virtual void on_frame_presented() {}

    // Clear backend-specific per-session state on reconnect (e.g. the typed input
    // queue, which can't live in the shared state). Default: nothing.
    virtual void on_reset() {}

private:
    CursorOverlay build_cursor_overlay() const;

    AspeedViewState& aspeed_state_;
    AspeedDecoder decoder_;
    HardwareCursor hosted_cursor_;
    bool has_hosted_cursor_ = false;
    std::uint64_t hosted_last_sequence_ = 0;
    std::uint64_t hosted_cursor_sequence_ = 0;
};

} // namespace hitsc
