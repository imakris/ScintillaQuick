// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_validation_access.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJSEngine>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QThread>
#include <rhi/qrhi.h>

#include <cstdio>
#include <memory>

namespace {

using Access = Scintilla::Internal::ScintillaQuick_validation_access;
constexpr int k_width = 96;
constexpr int k_height = 160;

QVariantMap bookmark_projection(ScintillaQuick_item& owner, const QVariantList& lines, bool valid = true)
{
    return {
        {QStringLiteral("lines"), lines},
        {QStringLiteral("valid"), valid},
        {QStringLiteral("source"), QVariant::fromValue(QPointer<ScintillaQuick_item>(&owner))},
        {QStringLiteral("binding_revision"), QString::number(owner.native_binding_revision())},
        {QStringLiteral("line_revision"), QString::number(owner.native_line_revision())},
        {QStringLiteral("marker_revision"), QString::number(owner.native_marker_revision())}
    };
}

// This one fixture uses Qt's existing render-control grab (software) and
// offscreen QRhi readback (RHI). No window is shown or native input delivered.
class Preview_scene
{
public:
    Preview_scene()
    :
        m_window(&m_control)
    {
        m_window.setGeometry(0, 0, k_width, k_height);
        m_window.contentItem()->setSize(QSizeF(k_width, k_height));
    }

    ~Preview_scene()
    {
        m_window.setRenderTarget(QQuickRenderTarget());
        m_control.invalidate();
    }

    bool initialize(bool software)
    {
        m_software = software;
        if (software) {
            return m_window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
        }
        if (!m_control.initialize() || !m_control.rhi()) {
            return false;
        }
        const auto api = m_window.rendererInterface()->graphicsApi();
        if (api == QSGRendererInterface::Unknown || api == QSGRendererInterface::Software ||
            api == QSGRendererInterface::Null)
        {
            return false;
        }
        auto* rhi = m_control.rhi();
        m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(k_width, k_height), 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        m_depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(k_width, k_height), 1));
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
        m_window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(m_target.get()));
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
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderBuffer> m_depth;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    bool m_software = false;
};

bool near_color(const QImage& image, int x, int y, QRgb expected)
{
    if (image.size() != QSize(k_width, k_height)) {
        return false;
    }
    const QRgb actual = image.pixel(x, y);
    return qAbs(qRed(actual) - qRed(expected)) <= 2 &&
        qAbs(qGreen(actual) - qGreen(expected)) <= 2 &&
        qAbs(qBlue(actual) - qBlue(expected)) <= 2;
}

template<typename Predicate>
QImage wait_for_frame(Preview_scene& scene, Predicate predicate)
{
    QElapsedTimer timeout;
    timeout.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QImage image = scene.frame();
        if (predicate(image)) {
            return image;
        }
        QThread::msleep(1);
    } while (timeout.elapsed() < 5000);
    return {};
}

}

int run_source_preview_scene_graph(int argc, char** argv)
{
    const QByteArray backend(argv[2]);
    if (backend != "software" && backend != "rhi") {
        return 2;
    }
    qputenv("QT_SCALE_FACTOR", "1");
    qputenv("QT_FONT_DPI", "96");
    qputenv("QT_QUICK_BACKEND", backend == "software" ? "software" : "");
    if (backend == "software") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }
    QGuiApplication app(argc, argv);
    Preview_scene scene;
    if (!scene.initialize(backend == "software")) {
        std::fprintf(stderr, "UNAVAILABLE source preview scene graph: %s backend initialization\n", backend.constData());
        return 77;
    }
    if (scene.window().isVisible() || scene.window().isExposed()) {
        std::fprintf(stderr, "FAIL source preview fixture must stay offscreen\n");
        return 1;
    }

    ScintillaQuick_item editor;
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(9.75);
    editor.setProperty("font", font);
    editor.send(SCI_STYLESETBACK, STYLE_DEFAULT, 0x242120);
    editor.send(SCI_STYLESETFORE, STYLE_DEFAULT, 0xdddddd);
    editor.send(SCI_STYLESETFORE, 0, 0xdddddd);
    QString text;
    for (int line = 0; line < 200; ++line) {
        if (line) { text += QLatin1Char('\n'); }
        text += QStringLiteral("int item_%1 = 16;").arg(line);
    }
    QElapsedTimer boundary;
    boundary.start();
    editor.setProperty("text", text);
    // This owner has history before the displayed view exists. Its revision
    // values must never be compared directly with that later view's counters.
    editor.sends(SCI_INSERTTEXT, 0, "\n");
    editor.send(SCI_DELETERANGE, 0, 1);
    editor.send(SCI_MARKERADD, 40, 0);
    ScintillaQuick_item view;
    view.send(SCI_SETDOCPOINTER, 0, editor.send(SCI_GETDOCPOINTER));
    view.setProperty("font", font);
    view.send(SCI_STYLESETBACK, STYLE_DEFAULT, 0x242120);
    view.send(SCI_STYLESETFORE, STYLE_DEFAULT, 0xdddddd);
    view.send(SCI_STYLESETFORE, 0, 0xdddddd);
    if (editor.native_line_revision() == view.native_line_revision()) {
        std::fprintf(stderr, "FAIL fixture did not establish distinct owner/view revision histories\n");
        return 1;
    }
    ScintillaQuick_source_preview preview(scene.window().contentItem());
    preview.setSize(QSizeF(k_width, k_height));
    preview.set_editor(&view);
    QJSEngine engine;
    const QVariantMap original_projection = bookmark_projection(editor,
        {int(editor.send(SCI_MARKERNEXT, 0, 1) + 1)});
    // Exercise the QVariant -> JavaScript -> QVariant transport used by the
    // single QML binding, retaining the guarded pointer rather than a raw one.
    const auto transported = engine.toScriptValue(original_projection).toVariant().toMap();
    preview.set_bookmark_projection(transported);
    preview.set_bookmark_color(Qt::red);
    preview.set_occurrence_color(Qt::green);
    QByteArray occurrences(200, '\0');
    occurrences[80] = 1;
    preview.set_occurrence_rows(occurrences);
    // 200 logical rows fit 160 pixels: row40 ->32, row80 ->64.
    const QImage marked = wait_for_frame(scene, [&](const QImage& image) {
        return Access::preview_image_serial(preview) != 0 &&
            near_color(image, 1, 32, qRgb(255, 0, 0)) && near_color(image, 94, 64, qRgb(0, 255, 0));
    });
    if (marked.isNull()) {
        std::fprintf(stderr, "FAIL source preview: known bookmark/occurrence colors were not rendered\n");
        return 1;
    }
    const auto api_to_readback_ms = boundary.elapsed();
    const auto serial = Access::preview_image_serial(preview);
    const QRect source_rect(4, 0, k_width - 8, k_height);
    const QImage source = marked.copy(source_rect);
    bool has_glyphs = false;
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            has_glyphs |= source.pixel(x, y) != qRgb(32, 33, 36);
        }
    }
    if (!has_glyphs) {
        std::fprintf(stderr, "FAIL source preview: source image has no glyph coverage\n");
        return 1;
    }
    if (!near_color(marked, 1, 16, qRgb(32, 33, 36))) {
        std::fprintf(stderr, "FAIL source preview: an unmarked lane row changed background\n");
        return 1;
    }

    preview.set_bookmark_color(Qt::blue);
    const QImage recolored = wait_for_frame(scene, [&](const QImage& image) {
        return near_color(image, 1, 32, qRgb(0, 0, 255));
    });
    preview.set_bookmark_projection({});
    preview.set_occurrence_rows({});
    const QImage cleared = wait_for_frame(scene, [&](const QImage& image) {
        return near_color(image, 1, 32, qRgb(32, 33, 36)) &&
            !near_color(image, 94, 64, qRgb(0, 255, 0));
    });
    if (recolored.isNull() || cleared.isNull() || recolored.copy(source_rect) != source ||
        cleared.copy(source_rect) != source || Access::preview_image_serial(preview) != serial ||
        marked.pixel(94, 16) != cleared.pixel(94, 16))
    {
        std::fprintf(stderr, "FAIL source preview: color/clear changed source pixels or rerasterized source\n");
        return 1;
    }

    preview.set_bookmark_projection(original_projection);
    const QImage restored = wait_for_frame(scene, [&](const QImage& image) {
        return near_color(image, 1, 32, qRgb(0, 0, 255));
    });
    if (restored.isNull()) { return 1; }
    preview.set_bookmark_projection(bookmark_projection(editor, {41}, false));
    const QImage invalid = wait_for_frame(scene, [&](const QImage& image) {
        return near_color(image, 1, 32, qRgb(32, 33, 36));
    });
    preview.set_bookmark_projection(original_projection);
    const QImage revalidated = wait_for_frame(scene, [&](const QImage& image) {
        return near_color(image, 1, 32, qRgb(0, 0, 255));
    });
    if (invalid.isNull() || revalidated.isNull()) {
        std::fprintf(stderr, "FAIL source preview: unchanged line list was not invalidated/revalidated\n");
        return 1;
    }

    int requested_line = 0;
    QObject::connect(&preview, &ScintillaQuick_source_preview::line_requested,
        &preview, [&](int line) { requested_line = line; });
    const auto before_resize = Access::preview_image_serial(preview);
    const int resized_height = k_height * 3 / 4;
    preview.setSize(QSizeF(k_width, resized_height));
    Access::request_preview_line(preview, resized_height / 2);
    const QImage resized = scene.frame();
    if (Access::preview_image_serial(preview) != before_resize || requested_line != 101 ||
        !near_color(resized, 1, 24, qRgb(0, 0, 255)))
    {
        std::fprintf(stderr, "FAIL source preview: size-only change lost the scaled image or row mapping\n");
        return 1;
    }
    preview.setSize(QSizeF(k_width, k_height));
    requested_line = 0;
    Access::request_preview_line(preview, k_height / 2);
    const QImage restored_size = scene.frame();
    if (Access::preview_image_serial(preview) != before_resize || requested_line != 101 ||
        !near_color(restored_size, 1, 32, qRgb(0, 0, 255)))
    {
        std::fprintf(stderr, "FAIL source preview: original size did not reuse its exact publication\n");
        return 1;
    }
    const QByteArray inserted_lines(50, '\n');
    editor.sends(SCI_INSERTTEXT, 0, inserted_lines.constData());
    Access::request_preview_line(preview, k_height);
    if (Access::preview_image_serial(preview) != 0 || requested_line != 250 ||
        editor.send(SCI_MARKERNEXT, 0, 1) != 90)
    {
        std::fprintf(stderr, "FAIL source preview: current proportional navigation was withheld or image not retired\n");
        return 1;
    }
    preview.set_bookmark_projection(original_projection);
    const QImage stale_rejected = wait_for_frame(scene, [&](const QImage& image) {
        return Access::preview_image_serial(preview) != 0 &&
            near_color(image, 1, 32, qRgb(32, 33, 36)) &&
            near_color(image, 1, 57, qRgb(32, 33, 36));
    });
    preview.set_bookmark_projection(bookmark_projection(editor, {91}));
    const QImage moved = wait_for_frame(scene, [&](const QImage& image) {
        return near_color(image, 1, 57, qRgb(0, 0, 255));
    });
    if (stale_rejected.isNull() || moved.isNull()) {
        std::fprintf(stderr, "FAIL source preview: stale authority was accepted or current moved marker missing\n");
        return 1;
    }

    const auto before_same_line = Access::preview_image_serial(preview);
    editor.sends(SCI_INSERTTEXT, editor.send(SCI_GETLENGTH), "x");
    requested_line = 0;
    Access::request_preview_line(preview, k_height);
    if (Access::preview_image_serial(preview) != before_same_line || requested_line != 250) {
        std::fprintf(stderr, "FAIL source preview: same-line stale image/navigation was lost\n");
        return 1;
    }

    // A whole-buffer replacement may keep the count but invalidate every row.
    const QVariantMap before_replacement = bookmark_projection(editor, {91});
    const QByteArray replacement = QByteArray("replacement\n").repeated(249) + "end";
    editor.send(SCI_SETTARGETRANGE, 0, editor.send(SCI_GETLENGTH));
    editor.sends(SCI_REPLACETARGET, replacement.size(), replacement.constData());
    preview.set_bookmark_projection(before_replacement);
    requested_line = 0;
    Access::request_preview_line(preview, k_height);
    if (editor.send(SCI_GETLINECOUNT) != 250 || requested_line != 250 ||
        Access::preview_image_serial(preview) != 0)
    {
        std::fprintf(stderr, "FAIL source preview: equal-count topology replacement was mistaken for continuity\n");
        return 1;
    }
    const QImage replaced = wait_for_frame(scene, [&](const QImage& image) {
        return Access::preview_image_serial(preview) != 0 && near_color(image, 1, 57, qRgb(32, 33, 36));
    });
    if (replaced.isNull()) { return 1; }

    ScintillaQuick_item unrelated;
    unrelated.setProperty("text", QStringLiteral("other source"));
    preview.set_bookmark_projection(bookmark_projection(unrelated, {1}));
    const QImage wrong_source = wait_for_frame(scene, [&](const QImage& image) {
        return near_color(image, 1, 0, qRgb(32, 33, 36));
    });
    auto transient_owner = std::make_unique<ScintillaQuick_item>();
    const QVariantMap dead_source = bookmark_projection(*transient_owner, {1});
    transient_owner.reset();
    preview.set_bookmark_projection(engine.toScriptValue(dead_source).toVariant().toMap());
    const QImage expired_source = wait_for_frame(scene, [&](const QImage& image) {
        return near_color(image, 1, 0, qRgb(32, 33, 36));
    });
    if (wrong_source.isNull() || expired_source.isNull()) {
        std::fprintf(stderr, "FAIL source preview: foreign/expired authority was accepted\n");
        return 1;
    }
    std::printf("PASS source preview ticks+clear (%s, API %d); native API to offscreen readback %lld ms; no input/GPU timing claim\n",
        backend.constData(), int(scene.window().rendererInterface()->graphicsApi()), (long long)api_to_readback_ms);
    return 0;
}
