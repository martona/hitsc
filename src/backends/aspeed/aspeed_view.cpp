#include "backends/aspeed/aspeed_view.hpp"

#include "backends/aspeed/aspeed_presenter.hpp"  // aspeed_frame_rgba_size
#include "hardware_cursor.hpp"                    // make_cursor_image, CursorImage
#include "view_input.hpp"                         // KvmInputController

#include <QImage>
#include <QRect>

#include <memory>
#include <utility>

namespace hitsc {
namespace {

#ifdef HITSC_DEBUG_HW_CURSOR
// Debug: true if the sprite carries real cursor content (any pixel-to-pixel
// variation), false if it is a uniform blob / fully transparent. Distinguishes a
// live hardware cursor from a degenerate/blank one in the title diagnostic.
bool cursor_sprite_has_variation(const QImage& sprite)
{
    if (sprite.isNull()) {
        return false;
    }
    const QImage rgba = sprite.format() == QImage::Format_RGBA8888
        ? sprite
        : sprite.convertToFormat(QImage::Format_RGBA8888);
    bool seen = false;
    std::uint32_t first = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(rgba.constScanLine(y));
        for (int x = 0; x < rgba.width(); ++x) {
            if (!seen) {
                first = row[x];
                seen = true;
            } else if (row[x] != first) {
                return true;
            }
        }
    }
    return false;
}
#endif

} // namespace

AspeedView::AspeedView(
    AspeedViewState& state,
    std::string host,
    std::string log_name,
    std::function<void()> network_cleanup,
    PowerCapabilities power_caps)
    : KvmViewBase(state, std::move(host), std::move(log_name), std::move(network_cleanup))
    , aspeed_state_(state)
    , power_controller_(power_caps, state.power)
{
}

PowerController* AspeedView::power_controller()
{
    return &power_controller_;
}

std::optional<SoftwareFrame> AspeedView::latest_frame()
{
#ifdef HITSC_DEBUG_HW_CURSOR
    const std::uint64_t debug_cursor_seq_before = hosted_cursor_sequence_;
#endif
    // BMC hardware-cursor packets arrive faster than video frames; cache the latest
    // cursor so the sprite tracks at tick rate, not frame rate.
    bool cursor_changed = false;
    if (const std::shared_ptr<const HardwareCursor> cursor =
            aspeed_state_.cursors.latest(hosted_cursor_sequence_)) {
        hosted_cursor_ = *cursor;
        hosted_cursor_sequence_ = cursor->sequence;
        has_hosted_cursor_ = true;
        cursor_changed = true;
    }

    // ASPEED is a differential stream (see FrameQueue): decode EVERY queued frame in
    // arrival order. Skipping any (as the old latest-wins mailbox did) corrupts the
    // SKIP/PASS2 delta chain until the next full refresh.
    bool base_changed = false;
    bool dirty_full = false;
    QRect dirty_rect;  // empty until a frame reports a partial change
    for (const std::shared_ptr<const AspeedCompressedFrame>& frame : aspeed_state_.frames.drain()) {
        hosted_last_sequence_ = frame->sequence;
        const AspeedDecodedDelta delta =
            decoder_.decode(frame->decode_options, frame->compressed, frame->width, frame->height);
        if (!delta.ok) {
            continue;
        }
        frame_presented(frame->width, frame->height);
        on_frame_presented();
        base_changed = true;
        if (delta.full) {
            dirty_full = true;  // first frame / resolution change => whole frame
        } else if (delta.rect.valid) {
            dirty_rect = dirty_rect.united(QRect(delta.rect.x, delta.rect.y, delta.rect.w, delta.rect.h));
        }
    }
    if (aspeed_state_.frames.overflowed()) {
        // Fell too far behind to preserve the delta chain; ask for a full refresh to
        // resync. The dropped frames also break the dirty-rect chain -> full upload.
        request_full_refresh();
        dirty_full = true;
    }

    SoftwareFrame out;
    // A successful decode with an empty dirty rect is a no-op delta (e.g. a frame
    // that is just FRAME_END): nothing to upload, so skip the base.
    const bool nothing_to_draw = base_changed && !dirty_full && dirty_rect.isEmpty();
    if (base_changed && !nothing_to_draw && decoder_.width() > 0 && decoder_.height() > 0
        && decoder_.framebuffer().size()
            == aspeed_frame_rgba_size(decoder_.width(), decoder_.height())) {
        // Zero-copy: hand the surface a non-owning wrap of the decoder's persistent
        // framebuffer (no per-frame alloc or memcpy). Safe because decode and render
        // are serialized on the main thread, and the buffer only reallocs on a
        // resolution change -- which sets dirty_full and recreates the texture anyway.
        // The surface drops this wrap when it leaves video (show_console).
        out.base = QImage(
            reinterpret_cast<const uchar*>(decoder_.framebuffer().data()),
            decoder_.width(),
            decoder_.height(),
            QImage::Format_RGBA8888);
        out.dirty = dirty_full ? std::optional<QRect>{} : std::optional<QRect>{dirty_rect};
    }
    // Rebuild the cursor sprite when it moved OR when the video under it changed:
    // type-non-1 XOR cursors invert the pixels beneath them, so a base change must
    // re-bake the sprite even if the cursor stayed put. The large base texture stays
    // decoupled -- only the tiny sprite re-uploads.
    if ((cursor_changed || base_changed) && has_hosted_cursor_) {
        out.cursor = build_cursor_overlay();
    }

#ifdef HITSC_DEBUG_HW_CURSOR
    {
        const unsigned debug_packets =
            static_cast<unsigned>(hosted_cursor_sequence_ - debug_cursor_seq_before);
        const bool debug_valid = out.cursor && !out.cursor->sprite.isNull()
            && cursor_sprite_has_variation(out.cursor->sprite);
        aspeed_state_.view_status.debug_cursor_tick(
            debug_packets, debug_valid, hosted_cursor_.width, hosted_cursor_.height,
            hosted_cursor_.x, hosted_cursor_.y, hosted_cursor_.type);
    }
#endif

    // nullopt when nothing changed this tick => the surface re-draws its existing
    // base + cursor textures for free (no convert, no upload).
    if (!out.base && !out.cursor) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::pair<int, int>> AspeedView::latest_frame_size()
{
    if (decoder_.width() > 0 && decoder_.height() > 0) {
        return std::make_pair(decoder_.width(), decoder_.height());
    }
    return std::nullopt;
}

void AspeedView::reset_for_reconnect()
{
    aspeed_state_.frames.clear();
    aspeed_state_.cursors.clear();
    decoder_.reset();
    hosted_cursor_ = HardwareCursor{};
    has_hosted_cursor_ = false;
    hosted_last_sequence_ = 0;
    hosted_cursor_sequence_ = 0;
    if (KvmInputController* controller = hosted_input_controller()) {
        controller->reset();
    }
    on_reset();  // backend clears its typed input queue + any extras
}

void AspeedView::on_minimized()
{
    aspeed_state_.frames.clear();
}

void AspeedView::on_focus_lost()
{
    if (KvmInputController* controller = hosted_input_controller()) {
        controller->release_all_keys();
    }
}

// Build the cursor sprite for the GPU overlay. make_cursor_image bakes the pattern
// x/y_offset into the sampled sprite, so its top-left maps to (cursor.x, cursor.y).
// type-non-1 XOR cursors read the framebuffer to invert the background, which is why
// latest_frame() rebuilds this whenever the base changes. A null sprite => hide.
CursorOverlay AspeedView::build_cursor_overlay() const
{
    CursorOverlay overlay;
    overlay.x = hosted_cursor_.x;
    overlay.y = hosted_cursor_.y;
    if (!hosted_cursor_.visible) {
        return overlay;  // empty sprite => hide
    }
    const CursorImage cursor_image = make_cursor_image(
        hosted_cursor_, decoder_.framebuffer(), decoder_.width(), decoder_.height());
    if (cursor_image.rgba.empty() || cursor_image.width <= 0 || cursor_image.height <= 0) {
        return overlay;  // nothing to show => hide
    }
    const QImage wrapped(
        reinterpret_cast<const uchar*>(cursor_image.rgba.data()),
        cursor_image.width,
        cursor_image.height,
        QImage::Format_RGBA8888);
    overlay.sprite = wrapped.copy();  // own the bytes (cursor_image is a local)
    return overlay;
}

} // namespace hitsc
