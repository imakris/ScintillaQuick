// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QImage>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSize>
#include <rhi/qrhi.h>

#include <memory>

// Both software grabs and RHI readbacks keep conformance frames off the desktop.
class Render_scene
{
public:
    explicit Render_scene(QSize size, qreal dpr = 1.0)
    :
        m_window(&m_control),
        m_size(size),
        m_dpr(dpr)
    {
        m_window.setColor(Qt::white);
        m_window.setGeometry(0, 0, size.width(), size.height());
        m_window.contentItem()->setSize(QSizeF(size));
    }

    ~Render_scene()
    {
        m_window.setRenderTarget(QQuickRenderTarget());
        m_control.invalidate();
    }

    static QSGRendererInterface::GraphicsApi graphics_api(bool software)
    {
        if (software) {
            return QSGRendererInterface::Software;
        }
#ifdef Q_OS_WIN
        return QSGRendererInterface::Direct3D11;
#elif defined(Q_OS_MACOS)
        return QSGRendererInterface::Metal;
#else
        return QSGRendererInterface::OpenGL;
#endif
    }

    static void select_backend(bool software)
    {
        qputenv("QT_QUICK_BACKEND", software ? "software" : "rhi");
        QQuickWindow::setGraphicsApi(graphics_api(software));
    }

    bool initialize(bool software)
    {
        m_software = software;
        if (software) {
            return m_window.rendererInterface()->graphicsApi() == graphics_api(true);
        }
        if (!m_control.initialize() || !m_control.rhi() ||
            m_window.rendererInterface()->graphicsApi() != graphics_api(false))
        {
            return false;
        }
        auto* rhi = m_control.rhi();
        const QSize pixel_size(qRound(m_size.width() * m_dpr), qRound(m_size.height() * m_dpr));
        m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, pixel_size, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        m_depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixel_size, 1));
        if (!m_texture->create() || !m_depth->create()) {
            return false;
        }
        QRhiTextureRenderTargetDescription description(QRhiColorAttachment(m_texture.get()));
        description.setDepthStencilBuffer(m_depth.get());
        m_target.reset(rhi->newTextureRenderTarget(description));
        m_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_pass.get());
        if (!m_target->create()) {
            return false;
        }
        auto target = QQuickRenderTarget::fromRhiRenderTarget(m_target.get());
        target.setDevicePixelRatio(m_dpr);
        m_window.setRenderTarget(target);
        return true;
    }

    QQuickWindow& window() { return m_window; }

    QImage frame()
    {
        if (m_software) {
            return m_window.grabWindow().convertToFormat(QImage::Format_ARGB32);
        }
        auto* rhi = m_control.rhi();
        m_control.polishItems();
        m_control.beginFrame();
        m_control.sync();
        m_control.render();
        QImage image;
        QRhiReadbackResult readback;
        readback.completed = [&] {
            const QImage pixels(reinterpret_cast<const uchar*>(readback.data.constData()),
                readback.pixelSize.width(), readback.pixelSize.height(), QImage::Format_RGBA8888_Premultiplied);
            image = rhi->isYUpInFramebuffer() ? pixels.flipped() : pixels.copy();
            image.setDevicePixelRatio(m_dpr);
        };
        auto* updates = rhi->nextResourceUpdateBatch();
        updates->readBackTexture(m_texture.get(), &readback);
        m_control.commandBuffer()->resourceUpdate(updates);
        // Qt's offscreen frame submission completes the readback here.
        m_control.endFrame();
        return image.convertToFormat(QImage::Format_ARGB32);
    }

private:
    QQuickRenderControl m_control;
    QQuickWindow m_window;
    QSize m_size;
    qreal m_dpr;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderBuffer> m_depth;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    bool m_software = false;
};
