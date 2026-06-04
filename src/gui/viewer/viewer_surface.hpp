#pragma once

#include "view_console.hpp"      // ConsoleScreen (NOTE: still pulls SDL transitively;
                                 // extract ConsoleScreen to an SDL-free header in cleanup)
#include "view_input_types.hpp"  // KvmPointer*, KvmMouseButton, HardwareVideoFrame

#include <QImage>
#include <QRhiWidget>
#include <QSize>

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
    void show_frame(const QImage& frame);                  // software / RGBA video
    void show_hardware_frame(const HardwareVideoFrame& frame);  // NV12 zero-copy

    // QRhi's D3D11 device (ID3D11Device*), or null until RHI is initialized. The
    // entry point hands this to the view (for FFmpeg D3D11VA) once rhiReady fires.
    void* d3d11_device() const;

signals:
    void pointerButton(const hitsc::KvmPointerButton& button);
    void pointerMotion(const hitsc::KvmPointerMotion& motion);
    void pointerWheel(const hitsc::KvmPointerWheel& wheel);
    void focusLost();
    void rhiReady();  // QRhi (and its device) is available

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void render_console_to_image();
    void update_quad_geometry(QRhiResourceUpdateBatch* batch, int image_width, int image_height);
    void render_image(QRhiCommandBuffer* cb);
    void render_hardware(QRhiCommandBuffer* cb);
    void ensure_nv12_resources(int width, int height);
    void release_nv12_resources();

    QRhi* rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> vertex_buffer_;
    std::unique_ptr<QRhiSampler> sampler_;
    bool rhi_ready_emitted_ = false;

    // RGBA path (console + software video).
    std::unique_ptr<QRhiTexture> texture_;
    std::unique_ptr<QRhiShaderResourceBindings> bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    QSize texture_size_;
    QImage image_;
    bool image_dirty_ = false;
    bool console_active_ = true;
    ConsoleScreen console_;
    QSize console_rendered_size_;

    // NV12 zero-copy path (pikvm hardware).
    std::unique_ptr<QRhiTexture> y_texture_;
    std::unique_ptr<QRhiTexture> uv_texture_;
    std::unique_ptr<QRhiShaderResourceBindings> nv12_bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> nv12_pipeline_;
    ID3D11Texture2D* nv12_staging_ = nullptr;  // standalone NV12 copy target (owns a ref)
    QSize nv12_size_;
    std::optional<HardwareVideoFrame> hardware_frame_;
    bool hardware_active_ = false;
};

} // namespace hitsc
