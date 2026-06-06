#include "gui/viewer/viewer_surface.hpp"

#include "gui/viewer/qt_console.hpp"
#include "log.hpp"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QColor>
#include <QEnterEvent>
#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QWheelEvent>

#include <algorithm>
#include <mutex>
#include <optional>

#ifdef HITSC_DEBUG_DIRTY_RECT
#include <cstdio>
#endif

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace hitsc {
namespace {

std::optional<KvmMouseButton> kvm_button(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return KvmMouseButton::LEFT;
    case Qt::MiddleButton:
        return KvmMouseButton::MIDDLE;
    case Qt::RightButton:
        return KvmMouseButton::RIGHT;
    case Qt::BackButton:
        return KvmMouseButton::X1;
    case Qt::ForwardButton:
        return KvmMouseButton::X2;
    default:
        return std::nullopt;
    }
}

KvmPointerPos to_pos(const QPointF& point)
{
    return KvmPointerPos{static_cast<float>(point.x()), static_cast<float>(point.y())};
}

QShader load_shader(const QString& path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        return QShader::fromSerialized(file.readAll());
    }
    return {};
}

std::unique_ptr<QRhiGraphicsPipeline> make_quad_pipeline(
    QRhi* rhi,
    QRhiRenderTarget* render_target,
    const QString& frag_qsb,
    QRhiShaderResourceBindings* bindings,
    bool blend = false)
{
    std::unique_ptr<QRhiGraphicsPipeline> pipeline(rhi->newGraphicsPipeline());
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    if (blend) {
        // Straight (non-premultiplied) alpha over the opaque base: the cursor
        // sprite stores real alpha, so src * a + dst * (1 - a).
        QRhiGraphicsPipeline::TargetBlend target_blend;
        target_blend.enable = true;
        target_blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        target_blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        target_blend.srcAlpha = QRhiGraphicsPipeline::One;
        target_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        pipeline->setTargetBlends({target_blend});
    }
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, load_shader(QStringLiteral(":/hitsc/shaders/viewer_quad.vert.qsb"))},
        {QRhiShaderStage::Fragment, load_shader(frag_qsb)},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    pipeline->setVertexInputLayout(layout);
    pipeline->setShaderResourceBindings(bindings);
    pipeline->setRenderPassDescriptor(render_target->renderPassDescriptor());
    if (!pipeline->create()) {
        return nullptr;
    }
    return pipeline;
}

} // namespace

ViewerSurface::ViewerSurface(QWidget* parent)
    : QRhiWidget(parent)
{
    setApi(QRhiWidget::Api::Direct3D11);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // QRhiWidget emits renderFailed() when it can't bring up the RHI or a frame
    // fails. If it never came up at all, D3D11 is unavailable here -> tell the host
    // once so it can show an error and bail. A failure AFTER a good init is a
    // device loss, left to the reconnect/restart path.
    connect(this, &QRhiWidget::renderFailed, this, [this]() {
        if (!rhi_ready_emitted_ && !rhi_unavailable_emitted_) {
            rhi_unavailable_emitted_ = true;
            emit rhiUnavailable();
        }
    });
}

ViewerSurface::~ViewerSurface()
{
#ifdef HITSC_DEBUG_DIRTY_RECT
    const std::uint64_t total = dbg_uploads_full_ + dbg_uploads_partial_;
    if (total > 0) {
        const double pct_full =
            100.0 * static_cast<double>(dbg_uploads_full_) / static_cast<double>(total);
        const double mean_area = dbg_uploads_partial_ > 0
            ? 100.0 * dbg_partial_area_sum_ / static_cast<double>(dbg_uploads_partial_)
            : 0.0;
        std::fprintf(
            stderr,
            "[dirty-rect] %llu uploads: %llu partial / %llu full (%.1f%% full); "
            "mean partial area %.1f%% (peak %.1f%%)\n",
            static_cast<unsigned long long>(total),
            static_cast<unsigned long long>(dbg_uploads_partial_),
            static_cast<unsigned long long>(dbg_uploads_full_),
            pct_full,
            mean_area,
            100.0 * dbg_partial_area_peak_);
    }
#endif
    release_nv12_resources();
}

void ViewerSurface::show_console(const ConsoleScreen& screen)
{
    // The frame timer calls this every tick while disconnected. Only (re)paint when
    // something actually changed -- the screen text, or the live log tail (its
    // revision bumps per appended line). An unchanged console does ZERO work: no
    // raster, no upload, no repaint; QRhiWidget keeps showing the last frame.
    // (Resizes are caught in render_image via the size check.) Without this gate the
    // console re-rasterized the full QPainter text + re-uploaded at 60 Hz -- a
    // pegged core whenever the disconnected/connecting screen was up.
    const std::uint64_t log_revision = recent_log_revision();
    const bool changed = !console_active_
        || screen != console_
        || log_revision != console_log_revision_;

    hardware_active_ = false;
    console_active_ = true;
    console_ = screen;
    console_log_revision_ = log_revision;

    if (!changed) {
        return;
    }

    // Entering the console, or its content changed: drop the cursor overlay and any
    // non-owning video wrap (so a torn-down view can't leave us a dangling pointer),
    // and flag a re-raster. render_console_to_image rebuilds image_ as an owning QImage.
    cursor_active_ = false;
    image_ = QImage();
    image_dirty_ = false;
    image_dirty_full_ = false;
    image_dirty_rect_ = QRect();
    console_dirty_ = true;
    update();
}

void ViewerSurface::show_frame(const QImage& frame, std::optional<QRect> dirty)
{
    hardware_active_ = false;
    console_active_ = false;
    const QImage rgba = frame.format() == QImage::Format_RGBA8888
                            ? frame
                            : frame.convertToFormat(QImage::Format_RGBA8888);

    // Coalesce with any upload still pending from an earlier call this frame: a size
    // change or any full (nullopt/empty) request forces a full upload; two partials
    // union. image_ always becomes the latest pixels regardless.
    const bool had_pending = image_dirty_;
    const bool size_changed = had_pending && rgba.size() != image_.size();
    image_ = rgba;
    if (!dirty || dirty->isEmpty() || size_changed || (had_pending && image_dirty_full_)) {
        image_dirty_full_ = true;
    } else if (!had_pending) {
        image_dirty_full_ = false;
        image_dirty_rect_ = *dirty;
    } else {
        image_dirty_rect_ = image_dirty_rect_.united(*dirty);
    }
    image_dirty_ = true;
    update();
}

void ViewerSurface::show_hardware_frame(const HardwareVideoFrame& frame)
{
    hardware_active_ = true;
    console_active_ = false;
    hardware_frame_ = frame;
    update();
}

void ViewerSurface::update_cursor(const QImage& sprite, int x, int y)
{
    if (sprite.isNull()) {
        // Hide. Keep the texture around (cheap) so a re-show needs no realloc.
        if (cursor_active_) {
            cursor_active_ = false;
            update();
        }
        return;
    }
    cursor_sprite_ = sprite.format() == QImage::Format_RGBA8888
                         ? sprite
                         : sprite.convertToFormat(QImage::Format_RGBA8888);
    cursor_pos_ = QPoint(x, y);
    cursor_active_ = true;
    cursor_texture_dirty_ = true;
    update();
}

void* ViewerSurface::d3d11_device() const
{
    if (rhi_ == nullptr) {
        return nullptr;
    }
    const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi_->nativeHandles());
    return handles != nullptr ? handles->dev : nullptr;
}

void ViewerSurface::render_console_to_image()
{
    const QSize logical = size();
    if (logical.isEmpty()) {
        return;
    }
    const qreal dpr = devicePixelRatio();
    QImage image(logical * dpr, QImage::Format_RGBA8888);
    image.setDevicePixelRatio(dpr);
    image.fill(QColor(12, 14, 18));
    QPainter painter(&image);
    render_console_qpainter(painter, logical, console_);
    painter.end();

    image_ = image;
    image_dirty_ = true;
    image_dirty_full_ = true;  // a freshly painted full image; never a partial patch
    console_rendered_size_ = logical;
}

void ViewerSurface::initialize(QRhiCommandBuffer*)
{
    if (rhi_ != rhi()) {
        pipeline_.reset();
        bindings_.reset();
        sampler_.reset();
        texture_.reset();
        vertex_buffer_.reset();
        texture_size_ = QSize();
        cursor_pipeline_.reset();
        cursor_bindings_.reset();
        cursor_texture_.reset();
        cursor_vertex_buffer_.reset();
        cursor_texture_size_ = QSize();
        release_nv12_resources();
        rhi_ = rhi();
    }

    if (!vertex_buffer_) {
        vertex_buffer_.reset(rhi_->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 4 * 4 * sizeof(float)));
        vertex_buffer_->create();
    }

    if (!cursor_vertex_buffer_) {
        cursor_vertex_buffer_.reset(rhi_->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 4 * 4 * sizeof(float)));
        cursor_vertex_buffer_->create();
    }

    if (!sampler_) {
        sampler_.reset(rhi_->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        sampler_->create();
    }

    if (!pipeline_) {
        texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
        texture_->create();
        texture_size_ = QSize(1, 1);

        bindings_.reset(rhi_->newShaderResourceBindings());
        bindings_->setBindings({
            QRhiShaderResourceBinding::sampledTexture(
                0, QRhiShaderResourceBinding::FragmentStage, texture_.get(), sampler_.get()),
        });
        bindings_->create();

        pipeline_ = make_quad_pipeline(
            rhi_, renderTarget(), QStringLiteral(":/hitsc/shaders/viewer_quad.frag.qsb"),
            bindings_.get());
    }

    if (!cursor_pipeline_) {
        cursor_texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
        cursor_texture_->create();
        cursor_texture_size_ = QSize(1, 1);

        cursor_bindings_.reset(rhi_->newShaderResourceBindings());
        cursor_bindings_->setBindings({
            QRhiShaderResourceBinding::sampledTexture(
                0, QRhiShaderResourceBinding::FragmentStage, cursor_texture_.get(), sampler_.get()),
        });
        cursor_bindings_->create();

        cursor_pipeline_ = make_quad_pipeline(
            rhi_, renderTarget(), QStringLiteral(":/hitsc/shaders/viewer_cursor.frag.qsb"),
            cursor_bindings_.get(), /*blend=*/true);
    }

    if (!rhi_ready_emitted_) {
        rhi_ready_emitted_ = true;
        emit rhiReady();
    }
}

void ViewerSurface::update_quad_geometry(
    QRhiResourceUpdateBatch* batch, int image_width, int image_height)
{
    const QSize output = renderTarget()->pixelSize();
    const float view_w = static_cast<float>(output.width());
    const float view_h = static_cast<float>(output.height());
    const float image_w = static_cast<float>(image_width);
    const float image_h = static_cast<float>(image_height);
    if (view_w <= 0.0f || view_h <= 0.0f || image_w <= 0.0f || image_h <= 0.0f) {
        return;
    }

    const float scale = std::min(view_w / image_w, view_h / image_h);
    const float w = image_w * scale;
    const float h = image_h * scale;
    const float x0 = (view_w - w) / 2.0f;
    const float y0 = (view_h - h) / 2.0f;
    const float x1 = x0 + w;
    const float y1 = y0 + h;

    const auto ndc_x = [view_w](float px) { return 2.0f * px / view_w - 1.0f; };
    const auto ndc_y = [view_h](float py) { return 1.0f - 2.0f * py / view_h; };

    const float vertices[16] = {
        ndc_x(x0), ndc_y(y0), 0.0f, 0.0f,  // top-left
        ndc_x(x1), ndc_y(y0), 1.0f, 0.0f,  // top-right
        ndc_x(x0), ndc_y(y1), 0.0f, 1.0f,  // bottom-left
        ndc_x(x1), ndc_y(y1), 1.0f, 1.0f,  // bottom-right
    };
    batch->updateDynamicBuffer(vertex_buffer_.get(), 0, sizeof(vertices), vertices);
}

void ViewerSurface::update_cursor_quad_geometry(
    QRhiResourceUpdateBatch* batch,
    int image_width,
    int image_height,
    int cursor_x,
    int cursor_y,
    int cursor_width,
    int cursor_height)
{
    const QSize output = renderTarget()->pixelSize();
    const float view_w = static_cast<float>(output.width());
    const float view_h = static_cast<float>(output.height());
    const float image_w = static_cast<float>(image_width);
    const float image_h = static_cast<float>(image_height);
    if (view_w <= 0.0f || view_h <= 0.0f || image_w <= 0.0f || image_h <= 0.0f
        || cursor_width <= 0 || cursor_height <= 0) {
        return;
    }

    // Identical aspect-fit transform to the base quad, then place the cursor's
    // pixel rect inside the fitted image rect so the sprite tracks the video.
    const float scale = std::min(view_w / image_w, view_h / image_h);
    const float base_w = image_w * scale;
    const float base_h = image_h * scale;
    const float base_x0 = (view_w - base_w) / 2.0f;
    const float base_y0 = (view_h - base_h) / 2.0f;

    const float x0 = base_x0 + static_cast<float>(cursor_x) * scale;
    const float y0 = base_y0 + static_cast<float>(cursor_y) * scale;
    const float x1 = x0 + static_cast<float>(cursor_width) * scale;
    const float y1 = y0 + static_cast<float>(cursor_height) * scale;

    const auto ndc_x = [view_w](float px) { return 2.0f * px / view_w - 1.0f; };
    const auto ndc_y = [view_h](float py) { return 1.0f - 2.0f * py / view_h; };

    const float vertices[16] = {
        ndc_x(x0), ndc_y(y0), 0.0f, 0.0f,  // top-left
        ndc_x(x1), ndc_y(y0), 1.0f, 0.0f,  // top-right
        ndc_x(x0), ndc_y(y1), 0.0f, 1.0f,  // bottom-left
        ndc_x(x1), ndc_y(y1), 1.0f, 1.0f,  // bottom-right
    };
    batch->updateDynamicBuffer(cursor_vertex_buffer_.get(), 0, sizeof(vertices), vertices);
}

void ViewerSurface::render(QRhiCommandBuffer* cb)
{
    if (rhi_ == nullptr) {
        return;
    }
    if (hardware_active_ && hardware_frame_) {
        render_hardware(cb);
    } else {
        render_image(cb);
    }
}

void ViewerSurface::render_image(QRhiCommandBuffer* cb)
{
    if (!pipeline_) {
        return;
    }

    if (console_active_ && (console_dirty_ || console_rendered_size_ != size())) {
        render_console_to_image();
        console_dirty_ = false;
    }

    QRhiResourceUpdateBatch* batch = rhi_->nextResourceUpdateBatch();

    if (image_dirty_ && !image_.isNull()) {
        bool must_full = image_dirty_full_;
        if (texture_size_ != image_.size()) {
            texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, image_.size()));
            texture_->create();
            texture_size_ = image_.size();
            bindings_->setBindings({
                QRhiShaderResourceBinding::sampledTexture(
                    0, QRhiShaderResourceBinding::FragmentStage, texture_.get(), sampler_.get()),
            });
            bindings_->create();
            must_full = true;  // fresh texture has no prior contents to patch onto
        }
        if (must_full) {
            batch->uploadTexture(texture_.get(), image_);
        } else {
            // Patch only the changed rectangle into the persistent texture. Source
            // and destination top-left are the same frame-pixel point: QRhi reads
            // that sub-rect of image_ and writes it in place over the prior frame.
            const QRect r = image_dirty_rect_.intersected(QRect(QPoint(0, 0), image_.size()));
            if (!r.isEmpty()) {
                QRhiTextureSubresourceUploadDescription desc(image_);
                desc.setSourceTopLeft(r.topLeft());
                desc.setSourceSize(r.size());
                desc.setDestinationTopLeft(r.topLeft());
                const QRhiTextureUploadEntry entry(0, 0, desc);
                batch->uploadTexture(texture_.get(), QRhiTextureUploadDescription(entry));
            }
        }
#ifdef HITSC_DEBUG_DIRTY_RECT
        if (must_full) {
            ++dbg_uploads_full_;
        } else {
            ++dbg_uploads_partial_;
            const double frame_px =
                static_cast<double>(image_.width()) * static_cast<double>(image_.height());
            const QRect r = image_dirty_rect_.intersected(QRect(QPoint(0, 0), image_.size()));
            const double frac = frame_px > 0.0
                ? static_cast<double>(r.width()) * static_cast<double>(r.height()) / frame_px
                : 0.0;
            dbg_partial_area_sum_ += frac;
            if (frac > dbg_partial_area_peak_) {
                dbg_partial_area_peak_ = frac;
            }
        }
#endif
        image_dirty_ = false;
        image_dirty_full_ = false;
        image_dirty_rect_ = QRect();
    }

    const bool have_image =
        texture_size_.width() > 1 && texture_size_.height() > 1 && !image_.isNull();
    if (have_image) {
        update_quad_geometry(batch, image_.width(), image_.height());
    }

    // Cursor overlay: only over live video (not the console), and only once we
    // have a base to anchor it to. The sprite is tiny, so re-uploading it on every
    // move is cheap -- the point is that the large base texture stays untouched.
    const bool draw_cursor = have_image && !console_active_ && cursor_active_
        && cursor_pipeline_ && !cursor_sprite_.isNull();
    if (draw_cursor) {
        if (cursor_texture_dirty_) {
            if (cursor_texture_size_ != cursor_sprite_.size()) {
                cursor_texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, cursor_sprite_.size()));
                cursor_texture_->create();
                cursor_texture_size_ = cursor_sprite_.size();
                cursor_bindings_->setBindings({
                    QRhiShaderResourceBinding::sampledTexture(
                        0, QRhiShaderResourceBinding::FragmentStage, cursor_texture_.get(),
                        sampler_.get()),
                });
                cursor_bindings_->create();
            }
            batch->uploadTexture(cursor_texture_.get(), cursor_sprite_);
            cursor_texture_dirty_ = false;
        }
        update_cursor_quad_geometry(
            batch, image_.width(), image_.height(), cursor_pos_.x(), cursor_pos_.y(),
            cursor_sprite_.width(), cursor_sprite_.height());
    }

    const QColor clear(12, 14, 18);
    const QSize output = renderTarget()->pixelSize();
    const QRhiViewport viewport(
        0.0f, 0.0f, static_cast<float>(output.width()), static_cast<float>(output.height()));
    cb->beginPass(renderTarget(), clear, {1.0f, 0}, batch);
    if (have_image) {
        cb->setGraphicsPipeline(pipeline_.get());
        cb->setViewport(viewport);
        cb->setShaderResources(bindings_.get());
        const QRhiCommandBuffer::VertexInput vertex_input(vertex_buffer_.get(), 0);
        cb->setVertexInput(0, 1, &vertex_input);
        cb->draw(4);
    }
    if (draw_cursor) {
        cb->setGraphicsPipeline(cursor_pipeline_.get());
        cb->setViewport(viewport);
        cb->setShaderResources(cursor_bindings_.get());
        const QRhiCommandBuffer::VertexInput cursor_input(cursor_vertex_buffer_.get(), 0);
        cb->setVertexInput(0, 1, &cursor_input);
        cb->draw(4);
    }
    cb->endPass();
}

void ViewerSurface::render_hardware(QRhiCommandBuffer* cb)
{
    const QColor clear(12, 14, 18);

#ifdef _WIN32
    const HardwareVideoFrame& frame = *hardware_frame_;
    auto* device = static_cast<ID3D11Device*>(d3d11_device());
    if (frame.texture == nullptr || frame.width <= 0 || frame.height <= 0 || !frame.lock
        || device == nullptr) {
        cb->beginPass(renderTarget(), clear, {1.0f, 0});
        cb->endPass();
        return;
    }

    // Hold the decode lock across the whole render frame: the decoder (network
    // thread) and QRhi (this thread) share the device. See [[hitsc-viewer-threading]].
    std::lock_guard<std::recursive_mutex> guard(*frame.lock);

    ensure_nv12_resources(frame.width, frame.height);
    if (!nv12_pipeline_ || nv12_staging_ == nullptr) {
        cb->beginPass(renderTarget(), clear, {1.0f, 0});
        cb->endPass();
        return;
    }

    // Copy this frame's array slice into the standalone NV12 texture the QRhi
    // views wrap. Cheap GPU->GPU copy on the shared device; no CPU roundtrip.
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC source_desc{};
    frame.texture->GetDesc(&source_desc);
    const UINT subresource =
        D3D11CalcSubresource(0, static_cast<UINT>(frame.array_slice), source_desc.MipLevels);
    context->CopySubresourceRegion(nv12_staging_, 0, 0, 0, 0, frame.texture, subresource, nullptr);

    QRhiResourceUpdateBatch* batch = rhi_->nextResourceUpdateBatch();
    update_quad_geometry(batch, frame.width, frame.height);

    cb->beginPass(renderTarget(), clear, {1.0f, 0}, batch);
    const QSize output = renderTarget()->pixelSize();
    cb->setGraphicsPipeline(nv12_pipeline_.get());
    cb->setViewport(QRhiViewport(
        0.0f, 0.0f, static_cast<float>(output.width()), static_cast<float>(output.height())));
    cb->setShaderResources(nv12_bindings_.get());
    const QRhiCommandBuffer::VertexInput vertex_input(vertex_buffer_.get(), 0);
    cb->setVertexInput(0, 1, &vertex_input);
    cb->draw(4);
    cb->endPass();
#else
    cb->beginPass(renderTarget(), clear, {1.0f, 0});
    cb->endPass();
#endif
}

void ViewerSurface::ensure_nv12_resources(int width, int height)
{
#ifdef _WIN32
    if (nv12_size_ == QSize(width, height) && nv12_staging_ != nullptr && nv12_pipeline_) {
        return;
    }
    release_nv12_resources();

    auto* device = static_cast<ID3D11Device*>(d3d11_device());
    if (device == nullptr) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &nv12_staging_))) {
        nv12_staging_ = nullptr;
        return;
    }

    // Import the NV12 planes as two QRhi textures: Y = R8 full-res, UV = RG8
    // half-res. (If this turns out garbage, QRhi's createFrom isn't making plane
    // SRVs for NV12 -- fall back to a raw-D3D11 NV12->RGBA pass.)
    const auto object = reinterpret_cast<quint64>(nv12_staging_);
    y_texture_.reset(rhi_->newTexture(QRhiTexture::R8, QSize(width, height)));
    uv_texture_.reset(rhi_->newTexture(QRhiTexture::RG8, QSize((width + 1) / 2, (height + 1) / 2)));
    if (!y_texture_->createFrom({object, 0}) || !uv_texture_->createFrom({object, 0})) {
        release_nv12_resources();
        return;
    }

    nv12_bindings_.reset(rhi_->newShaderResourceBindings());
    nv12_bindings_->setBindings({
        QRhiShaderResourceBinding::sampledTexture(
            0, QRhiShaderResourceBinding::FragmentStage, y_texture_.get(), sampler_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, uv_texture_.get(), sampler_.get()),
    });
    nv12_bindings_->create();

    nv12_pipeline_ = make_quad_pipeline(
        rhi_, renderTarget(), QStringLiteral(":/hitsc/shaders/viewer_nv12.frag.qsb"),
        nv12_bindings_.get());
    if (!nv12_pipeline_) {
        release_nv12_resources();
        return;
    }

    nv12_size_ = QSize(width, height);
#else
    (void)width;
    (void)height;
#endif
}

void ViewerSurface::release_nv12_resources()
{
    nv12_pipeline_.reset();
    nv12_bindings_.reset();
    y_texture_.reset();
    uv_texture_.reset();
    nv12_size_ = QSize();
#ifdef _WIN32
    if (nv12_staging_ != nullptr) {
        nv12_staging_->Release();
        nv12_staging_ = nullptr;
    }
#endif
}

void ViewerSurface::releaseResources()
{
    pipeline_.reset();
    bindings_.reset();
    sampler_.reset();
    texture_.reset();
    vertex_buffer_.reset();
    texture_size_ = QSize();
    cursor_pipeline_.reset();
    cursor_bindings_.reset();
    cursor_texture_.reset();
    cursor_vertex_buffer_.reset();
    cursor_texture_size_ = QSize();
    release_nv12_resources();
    rhi_ = nullptr;
}

void ViewerSurface::mousePressEvent(QMouseEvent* event)
{
    // Capture the mouse for the duration of a drag so motion and release events
    // keep arriving even when the pointer leaves the widget (the input controller
    // clamps to the target rect while a button is held). Also fixes the prior gap
    // where dragging outside the window lost capture.
    grabMouse();
    if (const auto button = kvm_button(event->button())) {
        emit pointerButton(KvmPointerButton{*button, true, to_pos(event->position())});
    }
}

void ViewerSurface::mouseReleaseEvent(QMouseEvent* event)
{
    if (const auto button = kvm_button(event->button())) {
        emit pointerButton(KvmPointerButton{*button, false, to_pos(event->position())});
    }
    if (event->buttons() == Qt::NoButton) {
        releaseMouse();
    }
}

void ViewerSurface::mouseMoveEvent(QMouseEvent* event)
{
    emit pointerMotion(KvmPointerMotion{to_pos(event->position())});
}

void ViewerSurface::wheelEvent(QWheelEvent* event)
{
    const QPoint steps = event->angleDelta();
    emit pointerWheel(KvmPointerWheel{
        static_cast<float>(steps.x()) / 120.0f,
        static_cast<float>(steps.y()) / 120.0f,
        to_pos(event->position())});
}

void ViewerSurface::focusOutEvent(QFocusEvent*)
{
    emit focusLost();
}

void ViewerSurface::enterEvent(QEnterEvent* event)
{
    // The title bar's hit-test-visible caption buttons (HTCLIENT to QWindowKit) get no
    // Qt leaveEvent when the cursor exits them into this surface, so their hover sticks.
    // We do get a normal enter -- let the window drop any stuck caption highlight.
    emit cursorEntered();
    QRhiWidget::enterEvent(event);
}

} // namespace hitsc
