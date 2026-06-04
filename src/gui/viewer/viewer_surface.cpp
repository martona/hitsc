#include "gui/viewer/viewer_surface.hpp"

#include "gui/viewer/qt_console.hpp"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QColor>
#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QWheelEvent>

#include <algorithm>
#include <mutex>
#include <optional>

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
    QRhiShaderResourceBindings* bindings)
{
    std::unique_ptr<QRhiGraphicsPipeline> pipeline(rhi->newGraphicsPipeline());
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
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
}

ViewerSurface::~ViewerSurface()
{
    release_nv12_resources();
}

void ViewerSurface::show_console(const ConsoleScreen& screen)
{
    hardware_active_ = false;
    console_active_ = true;
    console_ = screen;
    console_rendered_size_ = QSize();
    update();
}

void ViewerSurface::show_frame(const QImage& frame)
{
    hardware_active_ = false;
    console_active_ = false;
    image_ = frame.format() == QImage::Format_RGBA8888
                 ? frame
                 : frame.convertToFormat(QImage::Format_RGBA8888);
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
        release_nv12_resources();
        rhi_ = rhi();
    }

    if (!vertex_buffer_) {
        vertex_buffer_.reset(rhi_->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 4 * 4 * sizeof(float)));
        vertex_buffer_->create();
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

    if (console_active_ && console_rendered_size_ != size()) {
        render_console_to_image();
    }

    QRhiResourceUpdateBatch* batch = rhi_->nextResourceUpdateBatch();

    if (image_dirty_ && !image_.isNull()) {
        if (texture_size_ != image_.size()) {
            texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, image_.size()));
            texture_->create();
            texture_size_ = image_.size();
            bindings_->setBindings({
                QRhiShaderResourceBinding::sampledTexture(
                    0, QRhiShaderResourceBinding::FragmentStage, texture_.get(), sampler_.get()),
            });
            bindings_->create();
        }
        batch->uploadTexture(texture_.get(), image_);
        image_dirty_ = false;
    }

    const bool have_image =
        texture_size_.width() > 1 && texture_size_.height() > 1 && !image_.isNull();
    if (have_image) {
        update_quad_geometry(batch, image_.width(), image_.height());
    }

    const QColor clear(12, 14, 18);
    cb->beginPass(renderTarget(), clear, {1.0f, 0}, batch);
    if (have_image) {
        const QSize output = renderTarget()->pixelSize();
        cb->setGraphicsPipeline(pipeline_.get());
        cb->setViewport(QRhiViewport(
            0.0f, 0.0f, static_cast<float>(output.width()), static_cast<float>(output.height())));
        cb->setShaderResources(bindings_.get());
        const QRhiCommandBuffer::VertexInput vertex_input(vertex_buffer_.get(), 0);
        cb->setVertexInput(0, 1, &vertex_input);
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
    release_nv12_resources();
    rhi_ = nullptr;
}

void ViewerSurface::mousePressEvent(QMouseEvent* event)
{
    if (const auto button = kvm_button(event->button())) {
        emit pointerButton(KvmPointerButton{*button, true, to_pos(event->position())});
    }
}

void ViewerSurface::mouseReleaseEvent(QMouseEvent* event)
{
    if (const auto button = kvm_button(event->button())) {
        emit pointerButton(KvmPointerButton{*button, false, to_pos(event->position())});
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

} // namespace hitsc
