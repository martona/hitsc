#pragma once

// Diagnostic toggle: uncomment to print a one-line dirty-rect upload summary
// (partial vs full counts, mean/peak partial area) when the surface is destroyed.
// Leave commented for normal builds.
#define HITSC_DEBUG_DIRTY_RECT 1

#include "console_screen.hpp"    // ConsoleScreen / ConsoleSeverity
#include "view_input_types.hpp"  // KvmPointer*, KvmMouseButton, HardwareVideoFrame

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QRhiWidget>
#include <QSize>

#include <cstdint>
#include <memory>
#include <optional>

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiResourceUpdateBatch;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
QT_END_NAMESPACE

namespace hitsc {

// The Qt-native video surface, rendered through QRhi. Two display paths share one
// aspect-fit quad:
//   - RGBA (console rendered to a QImage, or software-decoded video frames):
//     uploaded to an RGBA texture, drawn with viewer_quad.frag.
//   - NV12 zero-copy (pikvm hardware decode): the decoder's D3D11 NV12 texture is
//     imported as two QRhi textures (Y=R8, UV=RG8) and converted in
//     viewer_nv12.frag -- no CPU roundtrip. The decode lock is held across the
//     render frame (see [[hitsc-viewer-threading]]).
// Mouse input is turned into backend-neutral Kvm pointer events (signals);
// keyboard lives in ViewerWindow's native filter.
class ViewerSurface : public QRhiWidget {
    Q_OBJECT

public:
    explicit ViewerSurface(QWidget* parent = nullptr);
    ~ViewerSurface() override;

    void show_console(const ConsoleScreen& screen);
    // Software / RGBA video base. dirty (if set) is the sub-region of frame that
    // changed since the last call, patched into the persistent texture; absent =>
    // upload the whole frame. The QImage may be a non-owning wrap of the decoder's
    // buffer (zero-copy), so it must stay valid until the next render -- guaranteed
    // because show_frame and render run on the same (main) thread.
    void show_frame(const QImage& frame, std::optional<QRect> dirty = std::nullopt);
    void show_hardware_frame(const HardwareVideoFrame& frame);  // NV12 zero-copy

    // Set/move/hide the BMC hardware-cursor overlay (drawn as a separate blended
    // quad over the software video base). A null/empty sprite hides it. Position
    // is the sprite's top-left in base-frame pixels. Only the software backends
    // (ASPEED) drive this; pikvm/console leave it unset.
    void update_cursor(const QImage& sprite, int x, int y);

    // QRhi's D3D11 device (ID3D11Device*), or null until RHI is initialized. The
    // entry point hands this to the view (for FFmpeg D3D11VA) once rhiReady fires.
    void* d3d11_device() const;

signals:
    void pointerButton(const hitsc::KvmPointerButton& button);
    void pointerMotion(const hitsc::KvmPointerMotion& motion);
    void pointerWheel(const hitsc::KvmPointerWheel& wheel);
    void focusLost();
    void cursorEntered();   // pointer entered the surface (drops a stuck caption hover)
    void rhiReady();        // QRhi (and its device) is available
    void rhiUnavailable();  // QRhi could not initialize (no usable D3D11)

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void enterEvent(QEnterEvent* event) override;

private:
    void render_console_to_image();
    void update_quad_geometry(QRhiResourceUpdateBatch* batch, int image_width, int image_height);
    void update_cursor_quad_geometry(
        QRhiResourceUpdateBatch* batch,
        int image_width,
        int image_height,
        int cursor_x,
        int cursor_y,
        int cursor_width,
        int cursor_height);
    void render_image(QRhiCommandBuffer* cb);
    void render_hardware(QRhiCommandBuffer* cb);
    void ensure_nv12_resources(int width, int height);
    void release_nv12_resources();

    QRhi* rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> vertex_buffer_;
    std::unique_ptr<QRhiSampler> sampler_;
    bool rhi_ready_emitted_ = false;
    bool rhi_unavailable_emitted_ = false;

    // RGBA path (console + software video).
    std::unique_ptr<QRhiTexture> texture_;
    std::unique_ptr<QRhiShaderResourceBindings> bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    QSize texture_size_;
    QImage image_;
    bool image_dirty_ = false;
    bool image_dirty_full_ = false;  // pending upload must cover the whole frame
    QRect image_dirty_rect_;         // else: accumulated changed region (frame pixels)
    bool console_active_ = true;
    bool console_dirty_ = false;              // console image needs a re-raster (content changed)
    ConsoleScreen console_;
    std::uint64_t console_log_revision_ = 0;  // log-tail revision baked into the last raster
    QSize console_rendered_size_;

    // Cursor overlay (software/ASPEED path): a small straight-alpha sprite drawn
    // as a second, blended quad on top of the base. Decoupled from the base so a
    // mouse move re-uploads only the tiny sprite, never the full framebuffer.
    std::unique_ptr<QRhiTexture> cursor_texture_;
    std::unique_ptr<QRhiShaderResourceBindings> cursor_bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> cursor_pipeline_;
    std::unique_ptr<QRhiBuffer> cursor_vertex_buffer_;
    QSize cursor_texture_size_;
    QImage cursor_sprite_;
    QPoint cursor_pos_;
    bool cursor_active_ = false;    // a visible cursor sprite is set
    bool cursor_texture_dirty_ = false;

    // NV12 zero-copy path (pikvm hardware).
    std::unique_ptr<QRhiTexture> y_texture_;
    std::unique_ptr<QRhiTexture> uv_texture_;
    std::unique_ptr<QRhiShaderResourceBindings> nv12_bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> nv12_pipeline_;
    ID3D11Texture2D* nv12_staging_ = nullptr;  // standalone NV12 copy target (owns a ref)
    QSize nv12_size_;
    std::optional<HardwareVideoFrame> hardware_frame_;
    bool hardware_active_ = false;

#ifdef HITSC_DEBUG_DIRTY_RECT
    // One-line upload-efficiency summary, printed in the destructor.
    std::uint64_t dbg_uploads_full_ = 0;
    std::uint64_t dbg_uploads_partial_ = 0;
    double dbg_partial_area_sum_ = 0.0;   // sum of (dirty px / frame px)
    double dbg_partial_area_peak_ = 0.0;
#endif
};

} // namespace hitsc
