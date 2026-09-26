// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_scene_graph_renderer.h"
#include "render_frame.h"
#include "scintillaquick_platqt.h"

#include "Indicator.h"
#include "LineMarker.h"
#include "XPM.h"

#include <ScintillaTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QSGClipNode>
#include <QSGGeometry>
#include <QSGImageNode>
#include <QSGTexture>
#include <QQuickWindow>
#include <QSGNode>
#include <QHash>
#include <QSGRectangleNode>
#include <QSGTransformNode>
#include <QSGTextNode>
#include <QMatrix4x4>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Scintilla::Internal
{

namespace
{

QSGTextNode::RenderType map_render_type()
{
    switch (QQuickWindow::textRenderType()) {
        case QQuickWindow::QtTextRendering:     return QSGTextNode::QtRendering;
        case QQuickWindow::NativeTextRendering: return QSGTextNode::NativeRendering;
        case QQuickWindow::CurveTextRendering:  return QSGTextNode::CurveRendering;
        default:                                return QSGTextNode::QtRendering;
    }
}

void update_rectangle_node(
    QQuickWindow*       window,
    QSGNode*            parent,
    QSGRectangleNode*&  node,
    const QRectF&       rect,
    const QColor&       color)
{
    if (!window || !parent || !rect.isValid() || rect.isEmpty() || !color.isValid() || color.alpha() == 0) {
        if (node) {
            parent->removeChildNode(node);
            delete node;
            node = nullptr;
        }
        return;
    }

    if (!node) {
        node = window->createRectangleNode();
        parent->appendChildNode(node);
    }

    node->setRect(rect);
    node->setColor(color);
}

void update_clip_node(QSGClipNode* node, const QRectF& rect)
{
    if (!node) {
        return;
    }

    const QRectF normalized = rect.normalized();
    node->setIsRectangular(true);
    node->setClipRect(normalized);
    QSGGeometry* geometry = node->geometry();
    if (!geometry) {
        geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry, true);
    }

    QSGGeometry::Point2D* vertices = geometry->vertexDataAsPoint2D();
    vertices[0].set(normalized.left(),  normalized.top());
    vertices[1].set(normalized.right(), normalized.top());
    vertices[2].set(normalized.left(),  normalized.bottom());
    vertices[3].set(normalized.right(), normalized.bottom());
    node->markDirty(QSGNode::DirtyGeometry);
}

qreal physical_pixel_size(QQuickWindow* window)
{
    if (!window) {
        return 1.0;
    }

    return 1.0 / std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
}

qreal snap_to_device_pixel(qreal value, qreal dpr)
{
    return std::round(value * dpr) / dpr;
}

struct Colored_rect
{
    QRectF rect;
    QColor color;
};

QRectF snapped_underline_rect(const QRectF& rect, QQuickWindow* window)
{
    if (!window) {
        return rect;
    }

    const qreal dpr    = std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
    const qreal height = physical_pixel_size(window);
    const qreal top    = snap_to_device_pixel(rect.top(), dpr);
    const qreal left   = std::floor(rect.left() * dpr) / dpr;
    const qreal right  = std::ceil(rect.right() * dpr) / dpr;
    return QRectF(left, top, std::max<qreal>(0.0, right - left), height);
}

QRectF snapped_outline_rect(const QRectF& rect, QQuickWindow* window)
{
    if (!window) {
        return rect.normalized();
    }

    const qreal dpr         = std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
    const QRectF normalized = rect.normalized();
    const qreal left        = std::floor(normalized.left()  * dpr) / dpr;
    const qreal top         = std::floor(normalized.top()   * dpr) / dpr;
    const qreal right       = std::ceil(normalized.right()  * dpr) / dpr;
    const qreal bottom      = std::ceil(normalized.bottom() * dpr) / dpr;
    return QRectF(left, top, std::max<qreal>(0.0, right - left), std::max<qreal>(0.0, bottom - top));
}

QRectF aligned_fill_rect(const QRectF& rect, QQuickWindow* window)
{
    // Match Surface_impl::FillRectangleAligned followed by QPainter's
    // aliased fill coverage, including a half-device-pixel right edge.
    const PRectangle logical = PixelAlign(PRectFromQRectF(rect), 1);
    const qreal dpr = window->effectiveDevicePixelRatio();
    const auto edge = [dpr](qreal value) { return std::floor(value * dpr + 0.5) / dpr; };
    return QRectF(QPointF(edge(logical.left), edge(logical.top)),
        QPointF(edge(logical.right), edge(logical.bottom)));
}

void append_horizontal_rect(
    std::vector<Colored_rect>&  rects,
    const QRectF&               rect,
    const QColor&               color,
    QQuickWindow*               window)
{
    const qreal dpr = window
        ? std::max<qreal>(1.0, window->effectiveDevicePixelRatio())
        : 1.0;
    const qreal physical_pixel = physical_pixel_size(window);
    const QRectF snapped       = snapped_underline_rect(rect, window);
    if (!snapped.isValid() || snapped.isEmpty()) {
        return;
    }

    const int width_pixels = std::max(1, static_cast<int>(std::ceil(snapped.width() * dpr)));
    rects.push_back({
        QRectF(snapped.left(), snapped.top(), width_pixels * physical_pixel, physical_pixel),
        color,
    });
}

void append_outline_pixel_rects(
    std::vector<Colored_rect>&  rects,
    const QRectF&               rect,
    const QColor&               color,
    QQuickWindow*               window)
{
    if (!color.isValid() || color.alpha() == 0) {
        return;
    }

    const QRectF snapped = snapped_outline_rect(rect, window);
    if (!snapped.isValid() || snapped.isEmpty()) {
        return;
    }

    const qreal thickness = physical_pixel_size(window);
    const qreal left      = snapped.left();
    const qreal top       = snapped.top();
    const qreal right     = snapped.right();
    const qreal bottom    = snapped.bottom();
    const qreal width     = std::max<qreal>(thickness, right  - left);
    const qreal height    = std::max<qreal>(thickness, bottom - top);

    rects.push_back({QRectF(left, top, width, thickness), color});
    rects.push_back({QRectF(left, std::max<qreal>(top, bottom - thickness), width, thickness), color});
    rects.push_back({QRectF(left, top, thickness, height), color});
    rects.push_back({QRectF(std::max<qreal>(left, right - thickness), top, thickness, height), color});
}

// Walk a primitive vector once, filter by `is_boxed(primitive)`, and
// emit the per-pixel outline rects for the surviving primitives using
// each primitive's foreground colour. `outer_rect(primitive)` lets the
// caller pick the rect to outline (e.g. fold/eol annotations want the
// rect shrunk by one device pixel on the right; plain annotations want
// the raw rect). Replaces three near-identical "build an index list,
// then iterate the indices, then call append_outline_pixel_rects"
// blocks that walked the same vector twice.
template <typename Primitive, typename IsBoxedFn, typename OuterRectFn>
std::vector<Colored_rect> collect_boxed_outline_rects(
    const std::vector<Primitive>& primitives,
    QQuickWindow*                 window,
    IsBoxedFn&&                   is_boxed,
    OuterRectFn&&                 outer_rect)
{
    std::vector<Colored_rect> rects;
    for (const Primitive& primitive : primitives) {
        if (is_boxed(primitive)) {
            append_outline_pixel_rects(rects, outer_rect(primitive), primitive.foreground, window);
        }
    }
    return rects;
}

void append_corner_mask_rects(
    std::vector<Colored_rect>&  rects,
    const QRectF&               rect,
    const QColor&               mask_color,
    QQuickWindow*               window)
{
    if (!mask_color.isValid() || mask_color.alpha() == 0) {
        return;
    }

    const QRectF snapped = snapped_outline_rect(rect, window);
    if (!snapped.isValid() || snapped.isEmpty()) {
        return;
    }

    const qreal pixel = physical_pixel_size(window);
    if ((snapped.width() <= pixel * 2.0) || (snapped.height() <= pixel * 2.0)) {
        return;
    }

    rects.push_back({
        QRectF(
            snapped.left(),
            snapped.top(),
            pixel,
            pixel),
        mask_color});
    rects.push_back({
        QRectF(
            std::max<qreal>(snapped.left(), snapped.right() - pixel),
            snapped.top(),
            pixel,
            pixel),
        mask_color});
    rects.push_back({
        QRectF(
            snapped.left(),
            std::max<qreal>(snapped.top(), snapped.bottom() - pixel),
            pixel,
            pixel),
        mask_color});
    rects.push_back({
        QRectF(
            std::max<qreal>(snapped.left(), snapped.right() - pixel),
            std::max<qreal>(snapped.top(), snapped.bottom() - pixel),
            pixel,
            pixel),
        mask_color});
}

QRectF represented_blob_body_rect(const QRectF& inner_rect, const QRectF& text_clip_rect)
{
    if (!inner_rect.isValid() || inner_rect.isEmpty()) {
        return inner_rect;
    }
    if (!text_clip_rect.isValid() || text_clip_rect.isEmpty()) {
        return inner_rect;
    }

    // DrawTextBlob captures rcCentral as the inner fill and rcChar as the text clip.
    // The visible blob body uses rcChar's leading/top/bottom edges with rcCentral's trailing edge.
    return QRectF(
        QPointF(
            std::max(inner_rect.left(),   text_clip_rect.left()),
            std::min(inner_rect.top(),    text_clip_rect.top())),
        QPointF(
            std::max(inner_rect.right(),  text_clip_rect.right()),
            std::max(inner_rect.bottom(), text_clip_rect.bottom())));
}

void append_rasterized_tab_arrow_rects(
    std::vector<Colored_rect>& rects, const Whitespace_mark_primitive& primitive, QQuickWindow* window)
{
    if (!window) {
        return;
    }

    const QRectF rect = primitive.rect.normalized();
    if (!rect.isValid() || rect.isEmpty() || !primitive.color.isValid() || primitive.color.alpha() == 0) {
        return;
    }

    const qreal dpr     = std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
    const qreal pixel   = physical_pixel_size(window);
    const int width_px  = std::max(1, static_cast<int>(std::round(rect.width()  * dpr)));
    const int height_px = std::max(1, static_cast<int>(std::round(rect.height() * dpr)));

    QImage image(width_px, height_px, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QPen pen(primitive.color);
    pen.setCosmetic(true);
    pen.setWidth(1);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const qreal half_width  = pixel / 2.0;
    const qreal origin_left = snap_to_device_pixel(rect.left(), dpr);
    const qreal origin_top  = snap_to_device_pixel(rect.top(),  dpr);

    const qreal left_stroke   = std::round(std::min(rect.left() + 2.0, rect.right() - 1.0)) + half_width;
    const qreal right_stroke  = std::max(left_stroke, std::round(rect.right()) - 1.0 - half_width);
    const qreal y_mid         = primitive.mid_y != 0.0 ? primitive.mid_y : std::floor(rect.center().y());
    const qreal y_mid_aligned = y_mid + half_width;
    const qreal y_diff        = std::floor(rect.height() / 2.0);
    qreal x_head              = right_stroke - y_diff;
    qreal head_height         = y_diff;
    if (x_head <= rect.left()) {
        head_height  -= rect.left() - x_head;
        x_head        = rect.left();
    }

    const QPointF arrow_point(right_stroke, y_mid_aligned);
    if (right_stroke > left_stroke) {
        painter.drawLine(QLineF(
            QPointF(left_stroke - origin_left, y_mid_aligned - origin_top),
            QPointF(arrow_point.x() - origin_left, arrow_point.y() - origin_top)));
    }

    painter.drawPolyline(
        QPolygonF({
            QPointF(x_head           - origin_left, y_mid_aligned   - head_height - origin_top),
            QPointF(arrow_point.x()  - origin_left, arrow_point.y() - origin_top),
            QPointF(x_head           - origin_left, y_mid_aligned   + head_height - origin_top),
        }));
    painter.end();

    rects.reserve(rects.size() + static_cast<size_t>(width_px * height_px / 2));
    for (int y = 0; y < height_px; ++y) {
        int x = 0;
        while (x < width_px) {
            const QColor sample = image.pixelColor(x, y);
            if (sample.alpha() == 0) {
                ++x;
                continue;
            }

            int run_end = x + 1;
            while (run_end < width_px && image.pixelColor(run_end, y).alpha() != 0) {
                ++run_end;
            }

            rects.push_back({
                QRectF(
                    origin_left + static_cast<qreal>(x) * pixel,
                    origin_top  + static_cast<qreal>(y) * pixel,
                    static_cast<qreal>(run_end - x)     * pixel,
                    pixel),
                primitive.color,
            });
            x = run_end;
        }
    }
}

ColourRGBA scintilla_color(const QColor& color)
{
    return ColourRGBA(color.red(), color.green(), color.blue(), color.alpha());
}

Indicator_primitive shape_cache_key(Indicator_primitive primitive)
{
    primitive.indicator_number = 0;
    primitive.under_text = false;
    primitive.is_main = false;
    return primitive;
}

Marker_primitive shape_cache_key(Marker_primitive primitive)
{
    primitive.document_line = 0;
    primitive.marker_number = 0;
    return primitive;
}

Indent_guide_primitive shape_cache_key(Indent_guide_primitive primitive)
{
    return primitive;
}

void draw_shape(Surface_impl& surface, const Indent_guide_primitive& primitive)
{
    QPainter* painter = surface.GetPainter();
    const qreal dpr = std::max<qreal>(1.0, painter->device()->devicePixelRatioF());
    const qreal pixel = 1.0 / dpr;
    const auto snap = [dpr](qreal value) { return std::round(value * dpr) / dpr; };
    QColor color = primitive.color.isValid() ? primitive.color
        : primitive.highlight ? QColor(192, 192, 192) : QColor(128, 128, 128);
    const int target_alpha = primitive.highlight ? 80 : 42;
    color.setAlpha(std::min(color.alpha() > 0 ? color.alpha() : 255, target_alpha));
    for (qreal y = snap(primitive.top + pixel); y < primitive.bottom; y += 4 * pixel) {
        painter->fillRect(QRectF(snap(primitive.x), snap(y), pixel, pixel), color);
    }
}

Gutter_band shape_cache_key(Gutter_band primitive)
{
    return primitive;
}

void draw_shape(Surface_impl& surface, const Gutter_band& primitive)
{
    QPainter* painter = surface.GetPainter();
    if (!primitive.pattern_color.isValid() || primitive.pattern_color == primitive.color) {
        painter->fillRect(primitive.rect, primitive.color);
        return;
    }
    // MarginView tiles an 8 x 8, one logical pixel checkerboard from the
    // margin rectangle's top-left. A QImage brush keeps this render-local.
    QImage tile(8, 8, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < tile.height(); ++y) {
        for (int x = 0; x < tile.width(); ++x) {
            tile.setPixelColor(x, y, ((x + y + primitive.pattern_phase) & 1)
                ? primitive.pattern_color : primitive.color);
        }
    }
    const QRect rect = QRectFromPRect(PRectFromQRectF(primitive.rect));
    painter->setBrushOrigin(rect.topLeft());
    painter->fillRect(rect, QBrush(tile));
}

void draw_shape(Surface_impl& surface, const Indicator_primitive& primitive)
{
    Indicator indicator((IndicatorStyle)primitive.indicator_style, scintilla_color(primitive.color),
        false, primitive.fill_alpha, primitive.outline_alpha);
    indicator.strokeWidth = primitive.stroke_width;
    indicator.Draw(&surface, PRectFromQRectF(primitive.rect), PRectFromQRectF(primitive.line_rect),
        PRectFromQRectF(primitive.character_rect), Indicator::State::normal, 0);
}

void draw_shape(Surface_impl& surface, const Marker_primitive& primitive)
{
    LineMarker marker;
    marker.markType = (MarkerSymbol)primitive.marker_type;
    marker.fore = scintilla_color(primitive.foreground);
    marker.back = scintilla_color(primitive.background);
    marker.backSelected = scintilla_color(primitive.background_selected);
    marker.strokeWidth = primitive.stroke_width;
    const std::shared_ptr<Font> font = marker.markType >= MarkerSymbol::Character
        ? font_from_qfont(primitive.font) : nullptr;
    marker.Draw(&surface, PRectFromQRectF(primitive.rect), font.get(),
        (LineMarker::FoldPart)primitive.fold_part, (MarginType)primitive.margin_style);
}

// Each shape owns a viewport-bounded image and texture. Preserve original
// geometry in drawing and cache keys: normalizing coordinates can change
// upstream integer rounding and QPainter's fractional-DPR raster phase.
// No editor or GUI-owned Scintilla object crosses this path.
template <typename Primitive>
class Scene_graph_shape_node final : public QSGNode
{
public:
    void update(QQuickWindow* window, const Primitive& primitive, const QRectF& visible_bounds)
    {
        if (visible_bounds.isEmpty()) {
            if (m_image_node) {
                removeChildNode(m_image_node);
                delete m_image_node;
                m_image_node = nullptr;
            }
            m_texture.reset();
            m_primitive.reset();
            return;
        }

        const qreal dpr = window->effectiveDevicePixelRatio();
        const QRectF image_rect = snapped_outline_rect(visible_bounds, window);
        const Primitive key = shape_cache_key(primitive);
        if (!m_primitive || *m_primitive != key || m_image_rect != image_rect || m_dpr != dpr) {
            QImage image(qRound(image_rect.width() * dpr), qRound(image_rect.height() * dpr),
                QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            {
                QPainter painter(&image);
                painter.translate(-image_rect.topLeft());
                Surface_impl surface;
                surface.Init(true, &painter);
                draw_shape(surface, primitive);
            }
            auto texture = std::unique_ptr<QSGTexture>(window->createTextureFromImage(image));
            if (!m_image_node) {
                m_image_node = window->createImageNode();
                m_image_node->setFiltering(QSGTexture::Nearest);
                m_image_node->setTexture(texture.get());
                m_image_node->setRect(image_rect);
                // The software renderer inspects a newly attached image node
                // immediately, so the texture must be valid before attachment.
                appendChildNode(m_image_node);
            }
            else {
                m_image_node->setTexture(texture.get());
            }
            m_texture = std::move(texture);
            m_primitive = key;
            m_image_rect = image_rect;
            m_dpr = dpr;
        }
        m_image_node->setRect(image_rect);
    }

private:
    QSGImageNode* m_image_node = nullptr;
    std::unique_ptr<QSGTexture> m_texture;
    std::optional<Primitive> m_primitive;
    QRectF m_image_rect;
    qreal m_dpr = 0.0;
};

// Content-keyed shaped-layout cache. Re-shaping a QTextLayout per syntax run on
// every large scroll dominated scroll latency: a PageDown reveals a whole
// viewport of runs, and runs (keywords, punctuation, indentation, common
// identifiers) repeat heavily within and across frames. Caching the shaped
// layout by content lets recurring runs skip the QTextLayout shaping step.
// Only left-to-right runs are cached: their glyph positions are relative to x=0
// and are placed via addTextLayout's position argument, so one shaped layout is
// reusable at any position. RTL/mixed runs depend on run.width and are not
// cached. addTextLayout only reads the layout, so sharing one across nodes is
// safe. The cache is thread_local because shaping runs on the scene-graph
// render thread.
struct Shaped_run
{
    std::shared_ptr<QTextLayout> layout;
    qreal ascent = 0.0;
};

std::shared_ptr<QTextLayout> shape_text_run(const Text_run& run, qreal& ascent_out)
{
    auto layout = std::make_shared<QTextLayout>(run.text, run.font);

    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    if (run.direction == Capture_text_direction::right_to_left) {
        option.setTextDirection(Qt::RightToLeft);
    }
    else
    if (run.direction == Capture_text_direction::mixed) {
        option.setTextDirection(Qt::LayoutDirectionAuto);
    }
    else {
        option.setTextDirection(Qt::LeftToRight);
    }
    layout->setTextOption(option);

    QTextCharFormat format;
    format.setForeground(run.foreground);
    format.setFont(run.font);

    QTextLayout::FormatRange range;
    range.start  = 0;
    range.length = run.text.length();
    range.format = format;
    layout->setFormats({range});

    ascent_out = 0.0;
    layout->beginLayout();
    QTextLine line = layout->createLine();
    if (line.isValid()) {
        if (run.direction == Capture_text_direction::right_to_left ||
            run.direction == Capture_text_direction::mixed)
        {
            line.setLineWidth(std::max<qreal>(1.0, run.width));
        }
        else {
            line.setLineWidth(1000000.0);
        }
        line.setPosition(QPointF(0.0, 0.0));
        ascent_out = line.ascent();
    }
    layout->endLayout();
    return layout;
}

Shaped_run get_shaped_run(const Text_run& run)
{
    // RTL / mixed runs depend on run.width, so the shaped result is
    // position-dependent and must not be shared by content.
    if (run.direction != Capture_text_direction::left_to_right) {
        Shaped_run shaped;
        shaped.layout = shape_text_run(run, shaped.ascent);
        return shaped;
    }

    static thread_local QHash<QString, Shaped_run> cache;

    QString key = run.text;
    key += QLatin1Char('\x1f');
    key += run.font.key();
    key += QLatin1Char('\x1f');
    key += QString::number(run.foreground.rgba());

    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return it.value();
    }

    Shaped_run shaped;
    shaped.layout = shape_text_run(run, shaped.ascent);

    // Bound the cache; recurring runs repopulate immediately after a clear.
    constexpr int k_max_shaped_run_cache_entries = 4096;
    if (cache.size() >= k_max_shaped_run_cache_entries) {
        cache.clear();
    }
    cache.insert(key, shaped);
    return shaped;
}

class Scene_graph_frame_text_node final : public QSGNode
{
public:
    ~Scene_graph_frame_text_node() override
    {
        // The active `m_text_node` is owned by the scene graph parent chain
        // (optional clip -> transform -> text node) and will be
        // cleaned up automatically when `this` is destroyed. The backup
        // slot's `text_node`, however, is deliberately detached from the
        // transform node while inactive; nothing in the scene graph owns it,
        // so it must be deleted here explicitly. Everything else in the
        // backup struct is value-typed and cleans itself up.
        delete m_backup.text_node;
        m_backup.text_node = nullptr;
    }

    void update_from_visual_line(
        QQuickWindow*            window,
        const Visual_line_frame& visual_line,
        const QRectF&            viewport)
    {
        if (!window) {
            return;
        }

        ensure_nodes(window, false);

        const bool same_key = m_has_cached_key &&
            m_cached_key.document_line == visual_line.key.document_line &&
            m_cached_key.subline_index == visual_line.key.subline_index;
        const bool same_viewport_size = m_cached_viewport.size() == viewport.size();

        {
            if (same_key && same_viewport_size && layouts_match_content(visual_line)) {
                if (positions_match(visual_line, viewport)) {
                    return;
                }
                QPointF delta;
                if (uniform_translation_delta(visual_line, delta)) {
                    m_text_node->setViewport(viewport);
                    set_translation(delta);
                    m_cached_viewport = viewport;
                    return;
                }

                // Same content, but positions are no longer a pure translation.
            }
        }

        // Secondary-slot cache. Before rebuilding the active slot from
        // scratch, see whether the most recently evicted state (stored in
        // `m_backup`) already matches this request. The canonical case is
        // zoom-bounce: alternating zoom-in / zoom-out reaches the same two
        // font configurations for the same visible line over and over.
        // Rebuilding both QTextLayouts and the QSGTextNode glyph runs on
        // every such paint dominates `zoom_wheel_bounce_latency_*`, so
        // keeping one prior snapshot per node lets the bounce path hit a
        // cheap "swap QSGTextNode pointers + update transform" fast path
        // instead of re-shaping and re-attaching every run.
        if (m_backup.viewport.size() == viewport.size()                 &&
            m_backup.populated                                          &&
            m_backup.has_key                                            &&
            m_backup.key.document_line == visual_line.key.document_line &&
            m_backup.key.subline_index == visual_line.key.subline_index &&
            backup_layouts_match_content(visual_line))
        {
            // A previously-evicted snapshot matches content + key. We can
            // only reuse the glyph runs it already baked into its
            // QSGTextNode if the new request's per-run positions are a
            // uniform translation of the backup's cached positions; the
            // glyph positions inside a QSGTextNode are baked in at
            // `addTextLayout` time and cannot be shifted per-run after the
            // fact, only by the transform node. Compute the translation
            // against the backup *before* committing to the swap so that a
            // non-uniform mismatch falls through to the normal rebuild
            // path rather than leaving glyphs at stale positions.
            QPointF restore_delta(0.0, 0.0);
            bool have_delta   = false;
            bool uniform      = true;
            size_t cached_idx = 0;
            for (const Text_run& run : visual_line.text_runs) {
                if (run.text.isEmpty() || run.represented_as_blob) {
                    continue;
                }
                if (cached_idx >= m_backup.layout_positions.size()) {
                    uniform = false;
                    break;
                }
                const QPointF d = run.position - m_backup.layout_positions[cached_idx];
                if (!have_delta) {
                    restore_delta = d;
                    have_delta    = true;
                }
                else
                if (d != restore_delta) {
                    uniform = false;
                    break;
                }
                ++cached_idx;
            }
            if (uniform && cached_idx == m_backup.layout_positions.size()) {
                swap_active_with_backup();
                m_text_node->setRenderType(map_render_type());
                m_text_node->setColor(Qt::white);
                m_text_node->setViewport(viewport);
                set_translation(restore_delta);
                m_cached_key      = visual_line.key;
                m_has_cached_key  = true;
                m_cached_viewport = viewport;
                return;
            }
        }

        // Neither the active slot nor the backup can serve the request.
        // Save whatever is currently active into the backup slot before we
        // rebuild, so that if the *next* frame flips back to this state we
        // can restore from backup instead of paying the rebuild cost again.
        // This is what turns the cache into a true 2-state LRU instead of
        // a single-state cache.
        evict_active_to_backup(window);

        {
            m_layouts.clear();
            m_cached_runs.clear();
            m_layout_positions.clear();
            m_text_node->setRenderType(map_render_type());
            m_text_node->setColor(Qt::white);
            m_text_node->setViewport(viewport);
            m_text_node->clear();
            set_translation(QPointF(0.0, 0.0));
        }

        for (const Text_run& run : visual_line.text_runs) {
            if (run.text.isEmpty() || run.represented_as_blob) {
                continue;
            }

            const Shaped_run shaped = get_shaped_run(run);
            const QPointF pos(
                run.position.x(),
                run.position.y() - shaped.ascent);

            m_text_node->addTextLayout(pos, shaped.layout.get());
            m_layouts.push_back(shaped.layout);
            m_cached_runs.push_back(run);
            m_layout_positions.push_back(run.position);
        }

        m_cached_key      = visual_line.key;
        m_has_cached_key  = true;
        m_cached_viewport = viewport;
    }

    const Visual_line_key& cached_key() const { return m_cached_key;      }
    bool has_valid_key() const                { return m_has_cached_key;  }
    void clear_cached_key()                   { m_has_cached_key = false; }

    void update_from_margin_text(QQuickWindow* window, const Margin_text_primitive& margin, const QRectF& viewport)
    {
        if (!window) {
            return;
        }

        ensure_nodes(window);

        const Visual_line_key margin_key{margin.document_line, margin.subline_index};
        const bool same_key = m_has_cached_key && m_cached_key == margin_key;
        const bool same_viewport_size = m_cached_viewport.size() == viewport.size();

        if (same_key && same_viewport_size && margin_layouts_match(margin)) {
            if (margin_position_matches(margin, viewport)) {
                return;
            }
            QPointF delta;
            if (uniform_translation_delta(margin, delta)) {
                const QRectF clip_rect =
                    (margin.clip_rect.isValid() && !margin.clip_rect.isEmpty())
                    ? margin.clip_rect
                    : viewport;
                m_text_node->setColor(margin.foreground);
                m_text_node->setViewport(viewport);
                update_clip_node(m_clip_node, clip_rect);
                set_translation(delta);
                m_cached_viewport  = viewport;
                m_cached_clip_rect = clip_rect;
                return;
            }
        }

        const QRectF clip_rect =
            (margin.clip_rect.isValid() && !margin.clip_rect.isEmpty())
            ? margin.clip_rect
            : viewport;
        m_layouts.clear();
        m_layout_positions.clear();
        m_text_node->setRenderType(map_render_type());
        m_text_node->setColor(margin.foreground);
        m_text_node->setViewport(viewport);
        m_text_node->clear();
        update_clip_node(m_clip_node, clip_rect);
        set_translation(QPointF(0.0, 0.0));

        if (margin.text.isEmpty()) {
            m_cached_margin_text.clear();
            m_cached_margin_font       = margin.font;
            m_cached_margin_foreground = margin.foreground;
            m_cached_clip_rect         = clip_rect;
            m_cached_key               = margin_key;
            m_has_cached_key           = true;
            m_cached_viewport          = viewport;
            return;
        }

        auto layout = std::make_shared<QTextLayout>(margin.text, margin.font);

        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout->setTextOption(option);

        QTextCharFormat format;
        format.setForeground(margin.foreground);
        format.setFont(margin.font);

        QTextLayout::FormatRange range;
        range.start  = 0;
        range.length = margin.text.length();
        range.format = format;
        layout->setFormats({range});

        qreal line_ascent = 0.0;
        qreal line_left   = 0.0;
        layout->beginLayout();
        QTextLine line = layout->createLine();
        if (line.isValid()) {
            line.setLineWidth(1000000.0);
            line.setPosition(QPointF(0.0, 0.0));
            line_ascent = line.ascent();
            line_left   = line.naturalTextRect().left();
        }
        layout->endLayout();

        const QPointF pos(
            margin.position.x() - line_left,
            margin.baseline_y   - line_ascent);
        m_text_node->addTextLayout(pos, layout.get());
        m_layouts.push_back(std::move(layout));
        m_layout_positions.push_back(margin.position);
        m_cached_margin_text       = margin.text;
        m_cached_margin_font       = margin.font;
        m_cached_margin_foreground = margin.foreground;
        m_cached_clip_rect         = clip_rect;

        m_cached_key      = margin_key;
        m_has_cached_key  = true;
        m_cached_viewport = viewport;
    }

    void ensure_nodes(QQuickWindow* window, bool clipped = true)
    {
        if (clipped && !m_clip_node) {
            m_clip_node = new QSGClipNode();
            appendChildNode(m_clip_node);
        }
        if (!m_transform_node) {
            m_transform_node = new QSGTransformNode();
            (m_clip_node ? static_cast<QSGNode*>(m_clip_node) : this)->appendChildNode(m_transform_node);
        }
        if (!m_text_node) {
            m_text_node = window->createTextNode();
            m_transform_node->appendChildNode(m_text_node);
        }
    }

    void set_translation(const QPointF& delta)
    {
        if (!m_transform_node) {
            return;
        }
        m_translation = delta;
        QMatrix4x4 matrix;
        matrix.translate(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
        m_transform_node->setMatrix(matrix);
    }

    bool margin_layouts_match(const Margin_text_primitive& margin) const
    {
        return
            margin.text       == m_cached_margin_text       &&
            margin.font       == m_cached_margin_font       &&
            margin.foreground == m_cached_margin_foreground &&
            ((margin.text.isEmpty() && m_layouts.empty()) ||
            (!margin.text.isEmpty() && m_layouts.size() == 1));
    }

    bool margin_position_matches(const Margin_text_primitive& margin, const QRectF& viewport) const
    {
        const QRectF clip_rect =
            (margin.clip_rect.isValid() && !margin.clip_rect.isEmpty())
            ? margin.clip_rect
            : viewport;
        return
            m_cached_viewport                     == viewport  &&
            m_cached_clip_rect                    == clip_rect &&
            m_layout_positions.size()             == 1         &&
            m_layout_positions[0] + m_translation == margin.position;
    }

    static bool runs_match_content(const std::vector<Text_run>& cached_runs, const Visual_line_frame& vl)
    {
        // Compare only translation-invariant fields. Viewport-relative
        // coordinates (top/bottom and the blob rects) change on every
        // scroll even for otherwise-identical runs, and are handled
        // separately by `uniform_translation_delta`, so including them
        // here would make the cache miss on every scroll.
        size_t cached_idx = 0;
        for (const Text_run& run : vl.text_runs) {
            if (run.text.isEmpty() || run.represented_as_blob) {
                continue;
            }
            if (cached_idx >= cached_runs.size()) {
                return false;
            }
            const Text_run& cached_run = cached_runs[cached_idx];
            if (cached_run.style_id            != run.style_id            ||
                cached_run.direction           != run.direction           ||
                cached_run.is_represented_text != run.is_represented_text ||
                cached_run.represented_as_blob != run.represented_as_blob ||
                cached_run.width               != run.width               ||
                cached_run.text                != run.text                ||
                cached_run.foreground          != run.foreground          ||
                cached_run.font                != run.font)
            {
                return false;
            }
            ++cached_idx;
        }
        return cached_idx == cached_runs.size();
    }

    bool layouts_match_content(const Visual_line_frame& vl) const
    {
        return runs_match_content(m_cached_runs, vl);
    }

    bool backup_layouts_match_content(const Visual_line_frame& vl) const
    {
        return runs_match_content(m_backup.cached_runs, vl);
    }

    void evict_active_to_backup(QQuickWindow* window)
    {
        // Move the currently-active cached state into the backup slot so
        // that a subsequent rebuild of the active slot does not destroy
        // the ability to restore this snapshot later. This is only useful
        // when the active slot actually holds content worth preserving:
        // if there is no cached layout there is nothing to save.
        if (!m_has_cached_key || m_cached_runs.empty()) {
            return;
        }
        if (!m_transform_node) {
            return;
        }

        m_backup.layouts          = std::move(m_layouts);
        m_backup.cached_runs      = std::move(m_cached_runs);
        m_backup.layout_positions = std::move(m_layout_positions);
        m_backup.translation      = m_translation;
        m_backup.key              = m_cached_key;
        m_backup.viewport         = m_cached_viewport;
        m_backup.has_key          = true;
        m_backup.populated        = true;

        // Ensure the active-slot vectors are empty (move-from leaves them
        // in a valid but unspecified state; clear() makes that explicit).
        m_layouts.clear();
        m_cached_runs.clear();
        m_layout_positions.clear();

        // Swap which QSGTextNode is attached to the transform node. The
        // previously-active text_node becomes the backup (still carrying
        // its shaped glyph runs), and a fresh text_node takes its place
        // as the active one. If a backup text_node already existed from a
        // prior eviction it is reused as the new active so we do not
        // allocate a new scene-graph node every eviction.
        QSGTextNode* prev_active = m_text_node;
        QSGTextNode* reuse_node  = m_backup.text_node;
        if (prev_active) {
            m_transform_node->removeChildNode(prev_active);
        }
        m_backup.text_node = prev_active;
        m_text_node        = reuse_node;
        if (!m_text_node && window) {
            m_text_node = window->createTextNode();
        }
        if (m_text_node) {
            m_transform_node->appendChildNode(m_text_node);
            // The freshly-activated text_node is either a brand-new one
            // or an older cache node whose glyph runs belonged to some
            // prior request. Its content will be explicitly cleared and
            // re-populated in the rebuild scope that follows this call,
            // so no state carries over.
        }
    }

    void swap_active_with_backup()
    {
        // Restore the backup snapshot into the active slot. Used when the
        // backup already matches the new request so no re-shape is needed.
        std::swap(m_layouts, m_backup.layouts);
        std::swap(m_cached_runs, m_backup.cached_runs);
        std::swap(m_layout_positions, m_backup.layout_positions);
        std::swap(m_translation, m_backup.translation);
        std::swap(m_cached_key, m_backup.key);
        std::swap(m_cached_viewport, m_backup.viewport);
        std::swap(m_has_cached_key, m_backup.has_key);
        // The backup slot's `populated` flag tracks whether the data now
        // sitting in the backup is a valid, restorable snapshot. After
        // the swap, the backup holds whatever the active slot held a
        // moment ago, so re-derive the flag from the moved-in state.
        m_backup.populated = !m_backup.cached_runs.empty();

        // Swap the QSGTextNode children so the one that holds the
        // previously-cached glyph runs becomes the active, visible one.
        if (!m_transform_node) {
            return;
        }
        QSGTextNode* prev_active = m_text_node;
        QSGTextNode* cached_node = m_backup.text_node;
        if (prev_active) {
            m_transform_node->removeChildNode(prev_active);
        }
        m_text_node = cached_node;
        m_backup.text_node = prev_active;
        if (m_text_node) {
            m_transform_node->appendChildNode(m_text_node);
        }
    }

    bool positions_match(const Visual_line_frame& vl, const QRectF& viewport) const
    {
        if (m_cached_viewport != viewport) {
            return false;
        }
        size_t idx = 0;
        for (const Text_run& run : vl.text_runs) {
            if (run.text.isEmpty() || run.represented_as_blob) {
                continue;
            }
            if (idx >= m_layout_positions.size()) {
                return false;
            }
            if (m_layout_positions[idx] + m_translation != run.position) {
                return false;
            }
            ++idx;
        }
        return idx == m_layout_positions.size();
    }

    bool uniform_translation_delta(const Visual_line_frame& vl, QPointF& delta) const
    {
        delta = QPointF(0.0, 0.0);
        bool have_delta = false;
        size_t idx = 0;
        for (const Text_run& run : vl.text_runs) {
            if (run.text.isEmpty() || run.represented_as_blob) {
                continue;
            }
            if (idx >= m_layout_positions.size()) {
                return false;
            }
            const QPointF current_delta = run.position - m_layout_positions[idx];
            if (!have_delta) {
                delta = current_delta;
                have_delta = true;
            }
            else
            if (current_delta != delta) {
                return false;
            }
            ++idx;
        }
        return idx == m_layout_positions.size();
    }

    bool uniform_translation_delta(const Margin_text_primitive& margin, QPointF& delta) const
    {
        if (m_layout_positions.size() != 1) {
            return false;
        }
        delta = margin.position - m_layout_positions[0];
        return true;
    }

    struct Backup_shape_state
    {
        QSGTextNode* text_node = nullptr; // detached from the scene graph
        std::vector<std::shared_ptr<QTextLayout>> layouts;
        std::vector<Text_run> cached_runs;
        std::vector<QPointF> layout_positions;
        QPointF translation{0.0, 0.0};
        Visual_line_key key{};
        QRectF viewport;
        bool has_key = false;
        bool populated = false;
    };

    QSGClipNode* m_clip_node = nullptr;
    QSGTextNode* m_text_node = nullptr;
    QSGTransformNode* m_transform_node = nullptr;
    std::vector<std::shared_ptr<QTextLayout>> m_layouts;
    std::vector<Text_run> m_cached_runs;
    std::vector<QPointF> m_layout_positions;
    Visual_line_key m_cached_key;
    bool m_has_cached_key = false;
    QRectF m_cached_viewport;
    QRectF m_cached_clip_rect;
    QString m_cached_margin_text;
    QFont m_cached_margin_font;
    QColor m_cached_margin_foreground;
    QPointF m_translation;
    Backup_shape_state m_backup;
};

// Resize the children of `parent` so it owns exactly `count` nodes,
// creating any missing ones via `make_node(window)`, then invoke
// `update_fn(node, index)` over all surviving nodes. The three concrete
// sync_*_nodes helpers below differ only in how they allocate a new
// node; the rest of the resize/diff/update loop is shared here.
template <typename NodeT, typename MakeNode, typename UpdateFn>
void sync_nodes_with_factory(
    QQuickWindow*        window,
    QSGNode*             parent,
    std::vector<NodeT*>& nodes,
    qsizetype            count,
    MakeNode&&           make_node,
    UpdateFn&&           update_fn)
{
    if (!window || !parent) {
        return;
    }

    while (static_cast<qsizetype>(nodes.size()) > count) {
        NodeT* node = nodes.back();
        nodes.pop_back();
        parent->removeChildNode(node);
        delete node;
    }

    while (static_cast<qsizetype>(nodes.size()) < count) {
        NodeT* node = make_node(window);
        parent->appendChildNode(node);
        nodes.push_back(node);
    }

    for (qsizetype i = 0; i < count; ++i) {
        update_fn(nodes[static_cast<size_t>(i)], static_cast<size_t>(i));
    }
}

template <typename NodeT, typename UpdateFn>
void sync_frame_text_nodes(
    QQuickWindow*        window,
    QSGNode*             parent,
    std::vector<NodeT*>& nodes,
    qsizetype            count,
    UpdateFn&&           update_fn)
{
    sync_nodes_with_factory(
        window, parent, nodes, count,
        [](QQuickWindow*) { return new NodeT(); },
        std::forward<UpdateFn>(update_fn));
}

template <typename UpdateFn>
void sync_rectangle_nodes(
    QQuickWindow*                   window,
    QSGNode*                        parent,
    std::vector<QSGRectangleNode*>& nodes,
    qsizetype                       count,
    UpdateFn&&                      update_fn)
{
    sync_nodes_with_factory(
        window, parent, nodes, count,
        [](QQuickWindow* w) { return w->createRectangleNode(); },
        std::forward<UpdateFn>(update_fn));
}

template <typename NodeT> void reorder_child_nodes(QSGNode* parent, const std::vector<NodeT*>& nodes)
{
    if (!parent) {
        return;
    }

    for (NodeT* node : nodes) {
        parent->removeChildNode(node);
        parent->appendChildNode(node);
    }
}

template <typename Primitive>
Margin_text_primitive to_margin_text(const Primitive& primitive)
{
    Margin_text_primitive margin;
    margin.text          = primitive.text;
    margin.position      = primitive.position;
    margin.baseline_y    = primitive.baseline_y;
    margin.foreground    = primitive.foreground;
    margin.font          = primitive.font;
    margin.clip_rect     = primitive.rect;
    margin.document_line = primitive.document_line;
    margin.style_id      = primitive.style_id;
    return margin;
}

uint64_t pack_visual_line_key(const Visual_line_key& key)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(key.document_line)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(key.subline_index));
}

template <typename Item, typename Key_fn, typename Update_fn>
void sync_keyed_text_nodes(
    QQuickWindow*                              window,
    QSGNode*                                   parent,
    std::vector<Scene_graph_frame_text_node*>& nodes,
    const std::vector<Item>&                  items,
    Key_fn&&                                  key_of,
    Update_fn&&                               update)
{
    if (!window || !parent) {
        return;
    }

    const size_t new_count = items.size();

    std::unordered_map<uint64_t, size_t> key_to_old_index;
    key_to_old_index.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i]->has_valid_key()) {
            key_to_old_index[pack_visual_line_key(nodes[i]->cached_key())] = i;
        }
    }

    std::vector<Scene_graph_frame_text_node*> new_nodes(new_count, nullptr);
    std::vector<bool> old_used(nodes.size(), false);

    for (size_t i = 0; i < new_count; ++i) {
        const uint64_t key = pack_visual_line_key(key_of(items[i]));
        auto it = key_to_old_index.find(key);
        if (it != key_to_old_index.end() && !old_used[it->second]) {
            new_nodes[i] = nodes[it->second];
            old_used[it->second] = true;
        }
    }

    size_t unused_cursor = 0;
    for (size_t i = 0; i < new_count; ++i) {
        if (new_nodes[i]) {
            continue;
        }
        while (unused_cursor < nodes.size() && old_used[unused_cursor]) {
            ++unused_cursor;
        }
        if (unused_cursor < nodes.size()) {
            new_nodes[i] = nodes[unused_cursor];
            old_used[unused_cursor] = true;
            new_nodes[i]->clear_cached_key();
        }
        else {
            auto* node = new Scene_graph_frame_text_node();
            parent->appendChildNode(node);
            new_nodes[i] = node;
        }
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!old_used[i]) {
            parent->removeChildNode(nodes[i]);
            delete nodes[i];
        }
    }

    const bool order_unchanged =
        nodes.size() == new_nodes.size() && std::equal(nodes.begin(), nodes.end(), new_nodes.begin());

    nodes = std::move(new_nodes);
    if (!order_unchanged) {
        reorder_child_nodes(parent, nodes);
    }

    for (size_t i = 0; i < new_count; ++i) {
        update(nodes[i], items[i]);
    }
}

class Scene_graph_root_node final : public QSGTransformNode
{
public:
    Scene_graph_root_node()
    {
        m_background_group            = new QSGNode();
        for (size_t layer = 0; layer < m_current_line_groups.size(); ++layer) {
            m_current_line_groups[layer] = new QSGNode();
            m_line_background_groups[layer] = new QSGNode();
            m_selection_groups[layer] = new QSGNode();
        }
        m_base_underline_group        = new QSGNode();
        m_gutter_group                = new QSGNode();
        m_marker_group                = new QSGNode();
        m_indicator_under_group       = new QSGNode();
        m_text_clip_node              = new QSGClipNode();
        m_representation_group        = new QSGNode();
        m_representation_text_group   = new QSGNode();
        m_text_group                  = new QSGNode();
        m_annotation_background_group = new QSGNode();
        m_annotation_text_group       = new QSGNode();
        m_whitespace_group            = new QSGNode();
        m_decoration_group            = new QSGNode();
        m_indent_guide_group          = new QSGNode();
        m_indicator_over_group        = new QSGNode();
        m_overlay_group               = new QSGNode();

        appendChildNode(m_background_group);
        appendChildNode(m_marker_group);
        appendChildNode(m_gutter_group);
        appendChildNode(m_text_clip_node);
        m_text_clip_node->appendChildNode(m_selection_groups[0]);
        m_text_clip_node->appendChildNode(m_line_background_groups[0]);
        m_text_clip_node->appendChildNode(m_current_line_groups[0]);
        m_text_clip_node->appendChildNode(m_indicator_under_group);
        m_text_clip_node->appendChildNode(m_base_underline_group);
        m_text_clip_node->appendChildNode(m_selection_groups[1]);
        m_text_clip_node->appendChildNode(m_current_line_groups[1]);
        m_text_clip_node->appendChildNode(m_line_background_groups[1]);
        m_text_clip_node->appendChildNode(m_representation_group);
        m_text_clip_node->appendChildNode(m_representation_text_group);
        m_text_clip_node->appendChildNode(m_text_group);
        m_text_clip_node->appendChildNode(m_annotation_background_group);
        m_text_clip_node->appendChildNode(m_annotation_text_group);
        m_text_clip_node->appendChildNode(m_whitespace_group);
        m_text_clip_node->appendChildNode(m_indent_guide_group);
        m_text_clip_node->appendChildNode(m_decoration_group);
        m_text_clip_node->appendChildNode(m_indicator_over_group);
        m_text_clip_node->appendChildNode(m_selection_groups[2]);
        m_text_clip_node->appendChildNode(m_current_line_groups[2]);
        m_text_clip_node->appendChildNode(m_line_background_groups[2]);
        m_text_clip_node->appendChildNode(m_overlay_group);
    }

    void update_from_frame(
        QQuickWindow*          window,
        const Render_snapshot& snapshot,
        const Render_frame&    frame)
    {
        update_clip_node(m_text_clip_node, frame.text_rect);

        // Current-line highlight from frame (authoritative capture)
        for (size_t layer = 0; layer < m_current_line_groups.size(); ++layer) {
            std::vector<Colored_rect> current_lines;
            for (const Current_line_primitive& primitive : frame.current_line_primitives) {
                if ((size_t)primitive.layer != layer) {
                    continue;
                }
                if (primitive.framed) {
                    append_outline_pixel_rects(current_lines, primitive.rect, primitive.color, window);
                }
                else if (primitive.layer != Layer::Base) {
                    // Base fills are captured after TextBackground resolves
                    // style overrides, including brace-highlight backgrounds.
                    current_lines.push_back({primitive.rect, primitive.color});
                }
            }
            sync_rectangle_nodes(window, m_current_line_groups[layer], m_current_line_nodes[layer],
                (qsizetype)current_lines.size(), [&](QSGRectangleNode* node, size_t i) {
                    node->setRect(current_lines[i].rect);
                    node->setColor(current_lines[i].color);
                });

            std::vector<const Background_primitive*> backgrounds;
            for (const Background_primitive& primitive : frame.background_primitives) {
                if ((size_t)primitive.layer == layer && !primitive.marker_underline) {
                    backgrounds.push_back(&primitive);
                }
            }
            sync_rectangle_nodes(window, m_line_background_groups[layer], m_line_background_nodes[layer],
                (qsizetype)backgrounds.size(), [&](QSGRectangleNode* node, size_t i) {
                    node->setRect(aligned_fill_rect(backgrounds[i]->rect, window));
                    node->setColor(backgrounds[i]->color);
                });
        }

        std::vector<const Background_primitive*> base_underlines;
        for (const Background_primitive& primitive : frame.background_primitives) {
            if (primitive.marker_underline) {
                base_underlines.push_back(&primitive);
            }
        }
        sync_rectangle_nodes(window, m_base_underline_group, m_base_underline_nodes,
            (qsizetype)base_underlines.size(), [&](QSGRectangleNode* node, size_t i) {
                node->setRect(aligned_fill_rect(base_underlines[i]->rect, window));
                node->setColor(base_underlines[i]->color);
            });

        // Selection participates in the same Base / UnderText / OverText order
        // as Scintilla's drawing phases. Base background captures include the
        // resolved selection fill and later marker underlines.
        for (size_t layer = 0; layer < m_selection_groups.size(); ++layer) {
            std::vector<const Selection_primitive*> selections;
            for (const Selection_primitive& primitive : frame.selection_primitives) {
                if ((size_t)primitive.layer == layer) {
                    selections.push_back(&primitive);
                }
            }
            sync_rectangle_nodes(window, m_selection_groups[layer], m_selection_nodes[layer],
                (qsizetype)selections.size(), [&](QSGRectangleNode* node, size_t i) {
                    node->setRect(selections[i]->rect);
                    node->setColor(selections[i]->color);
                });
        }

        // Caret rectangles from frame
        sync_rectangle_nodes(
            window,
            m_overlay_group,
            m_caret_nodes,
            static_cast<qsizetype>(frame.caret_primitives.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(aligned_fill_rect(frame.caret_primitives[i].rect, window));
                node->setColor(frame.caret_primitives[i].color);
            });

        const qreal static_dpr = window->effectiveDevicePixelRatio();
        if (snapshot.static_revision != 0 && snapshot.static_revision == m_static_revision &&
            frame.text_rect == m_static_text_rect && static_dpr == m_static_dpr)
        {
            return;
        }
        m_static_revision = snapshot.static_revision;
        m_static_text_rect = frame.text_rect;
        m_static_dpr = static_dpr;

        std::vector<Colored_rect> representation_blob_fill_rects;
        std::vector<Margin_text_primitive> representation_texts;
        for (const Visual_line_frame& visual_line : frame.visual_lines) {
            for (const Text_run& run : visual_line.text_runs) {
                if (!run.represented_as_blob) {
                    continue;
                }
                const QRectF outer_rect = run.blob_outer_rect;
                if (outer_rect.isEmpty()) {
                    continue;
                }
                const QRectF inner_rect = run.blob_inner_rect;
                if (inner_rect.isEmpty()) {
                    representation_blob_fill_rects.push_back({outer_rect, run.blob_outer});
                    continue;
                }

                const QRectF blob_body_rect = represented_blob_body_rect(
                    inner_rect,
                    run.blob_text_clip_rect);
                representation_blob_fill_rects.push_back({outer_rect, run.blob_outer});
                representation_blob_fill_rects.push_back({blob_body_rect, run.blob_inner});
                append_corner_mask_rects(
                    representation_blob_fill_rects,
                    blob_body_rect,
                    run.blob_outer,
                    window);
                Margin_text_primitive primitive;
                primitive.text          = run.text;
                primitive.position      = run.position;
                primitive.baseline_y    = run.position.y();
                primitive.foreground    = run.foreground;
                primitive.font          = run.font;
                primitive.clip_rect     = QRectF();
                primitive.document_line = visual_line.key.document_line;
                primitive.subline_index = visual_line.key.subline_index * 1000
                    + static_cast<int>(representation_texts.size());
                primitive.style_id      = run.style_id;
                representation_texts.push_back(std::move(primitive));
            }
        }
        sync_rectangle_nodes(
            window,
            m_representation_group,
            m_representation_blob_nodes,
            static_cast<qsizetype>(representation_blob_fill_rects.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(snapped_outline_rect(representation_blob_fill_rects[i].rect, window));
                node->setColor(representation_blob_fill_rects[i].color);
            });
        sync_frame_text_nodes(
            window,
            m_representation_text_group,
            m_representation_text_nodes,
            static_cast<qsizetype>(representation_texts.size()),
            [&](Scene_graph_frame_text_node* node, size_t i) {
                node->update_from_margin_text(window, representation_texts[i], frame.text_rect);
            });

        // Backgrounds from snapshot
        update_rectangle_node(
            window,
            m_background_group,
            m_background_node,
            QRectF(QPointF(0.0, 0.0), snapshot.item_size),
            snapshot.background);
        sync_frame_text_nodes(window, m_background_group, m_gutter_background_nodes,
            (qsizetype)snapshot.gutter_bands.size(),
            [&](Scene_graph_shape_node<Gutter_band>* node, size_t i) {
                const Gutter_band& band = snapshot.gutter_bands[i];
                node->update(window, band, band.rect.intersected(
                    QRectF(QPointF(0.0, 0.0), snapshot.item_size)));
            });

        {
            // Body text from frame visual lines (key-based reuse)
            sync_keyed_text_nodes(
                window,
                m_text_group,
                m_text_nodes,
                frame.visual_lines,
                [](const Visual_line_frame& line) { return line.key; },
                [&](Scene_graph_frame_text_node* node, const Visual_line_frame& line) {
                    node->update_from_visual_line(window, line, frame.text_rect);
                });

            // Gutter text from frame margin text primitives (key-based reuse)
            sync_keyed_text_nodes(
                window,
                m_gutter_group,
                m_gutter_nodes,
                frame.margin_text_primitives,
                [](const Margin_text_primitive& margin) {
                    return Visual_line_key{margin.document_line, margin.subline_index};
                },
                [&](Scene_graph_frame_text_node* node, const Margin_text_primitive& margin) {
                    node->update_from_margin_text(window, margin, frame.margin_rect);
                });
        }

        std::vector<const Indicator_primitive*> under_indicators;
        std::vector<const Indicator_primitive*> over_indicators;
        for (const Indicator_primitive& indicator : frame.indicator_primitives) {
            if (indicator.indicator_style == (int)IndicatorStyle::Hidden ||
                indicator.indicator_style == (int)IndicatorStyle::TextFore) {
                continue;
            }
            (indicator.under_text ? under_indicators : over_indicators).push_back(&indicator);
        }
        const auto sync_indicators = [&](QSGNode* group, auto& nodes, const auto& indicators) {
            sync_frame_text_nodes(window, group, nodes, (qsizetype)indicators.size(),
                [&](Scene_graph_shape_node<Indicator_primitive>* node, size_t i) {
                    const Indicator_primitive& indicator = *indicators[i];
                    const qreal padding = std::max(indicator.rect.height(), indicator.stroke_width * 2) + 2;
                    const QRectF shape_bounds = indicator.rect.united(indicator.line_rect)
                        .united(indicator.character_rect).adjusted(-padding, -padding, padding, padding);
                    // Keep the raw shape's edges inside the item when possible.
                    // An artificial raster clip at textStart changes QPainter's
                    // stroke endpoint coverage. The parent supplies text clipping.
                    const QRectF bounds = shape_bounds.intersects(frame.text_rect)
                        ? shape_bounds.intersected(QRectF(QPointF(0.0, 0.0), snapshot.item_size))
                        : QRectF();
                    node->update(window, indicator, bounds);
                });
        };
        sync_indicators(m_indicator_under_group, m_indicator_under_nodes, under_indicators);
        sync_indicators(m_indicator_over_group, m_indicator_over_nodes, over_indicators);

        sync_frame_text_nodes(window, m_marker_group, m_marker_nodes,
            (qsizetype)frame.marker_primitives.size(),
            [&](Scene_graph_shape_node<Marker_primitive>* node, size_t i) {
                const Marker_primitive& marker = frame.marker_primitives[i];
                const qreal padding = marker.stroke_width + 2;
                const QRectF bounds = marker.rect.adjusted(-padding, -padding, padding, padding)
                    .intersected(frame.margin_rect);
                node->update(window, marker, bounds);
            });

        // Fold display text: backgrounds first (behind), then text on top
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_fold_display_bg_nodes,
            static_cast<qsizetype>(frame.fold_display_texts.size()),
            [&](QSGRectangleNode* node, size_t i) {
                QRectF rect = frame.fold_display_texts[i].rect;
                if (frame.fold_display_texts[i].boxed) {
                    rect.adjust(0.0, 0.0, -physical_pixel_size(window), 0.0);
                }
                node->setRect(rect);
                node->setColor(frame.fold_display_texts[i].background);
            });

        sync_frame_text_nodes(
            window,
            m_annotation_text_group,
            m_fold_display_text_nodes,
            static_cast<qsizetype>(frame.fold_display_texts.size()),
            [&](Scene_graph_frame_text_node* node, size_t i) {
                node->update_from_margin_text(
                    window, to_margin_text(frame.fold_display_texts[i]), frame.text_rect);
            });

        // Fold display text: boxed outlines
        const qreal pixel_size = physical_pixel_size(window);
        std::vector<Colored_rect> fold_display_box_rects = collect_boxed_outline_rects(
            frame.fold_display_texts, window,
            [](const Fold_display_text_primitive& p) { return p.boxed; },
            [pixel_size](const Fold_display_text_primitive& p) {
                return p.rect.adjusted(0.0, 0.0, -pixel_size, 0.0);
            });
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_fold_display_box_nodes,
            static_cast<qsizetype>(fold_display_box_rects.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(fold_display_box_rects[i].rect);
                node->setColor(fold_display_box_rects[i].color);
            });

        // EOL annotations: backgrounds first, then text
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_eol_annotation_bg_nodes,
            static_cast<qsizetype>(frame.eol_annotations.size()),
            [&](QSGRectangleNode* node, size_t i) {
                QRectF rect = frame.eol_annotations[i].rect;
                if (frame.eol_annotations[i].visible_style == static_cast<int>(EOLAnnotationVisible::Boxed)) {
                    rect.adjust(0.0, 0.0, -physical_pixel_size(window), 0.0);
                }
                node->setRect(rect);
                node->setColor(frame.eol_annotations[i].background);
            });

        sync_frame_text_nodes(
            window,
            m_annotation_text_group,
            m_eol_annotation_nodes,
            static_cast<qsizetype>(frame.eol_annotations.size()),
            [&](Scene_graph_frame_text_node* node, size_t i) {
                node->update_from_margin_text(
                    window, to_margin_text(frame.eol_annotations[i]), frame.text_rect);
            });

        // EOL annotation boxed outlines
        std::vector<Colored_rect> eol_annotation_box_rects = collect_boxed_outline_rects(
            frame.eol_annotations, window,
            [](const Eol_annotation_primitive& p) {
                return p.visible_style == static_cast<int>(EOLAnnotationVisible::Boxed);
            },
            [pixel_size](const Eol_annotation_primitive& p) {
                return p.rect.adjusted(0.0, 0.0, -pixel_size, 0.0);
            });
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_eol_annotation_box_nodes,
            static_cast<qsizetype>(eol_annotation_box_rects.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(eol_annotation_box_rects[i].rect);
                node->setColor(eol_annotation_box_rects[i].color);
            });

        // Annotations: backgrounds first, then text
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_annotation_bg_nodes,
            static_cast<qsizetype>(frame.annotations.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(frame.annotations[i].rect);
                node->setColor(frame.annotations[i].background);
            });

        sync_frame_text_nodes(
            window,
            m_annotation_text_group,
            m_annotation_nodes,
            static_cast<qsizetype>(frame.annotations.size()),
            [&](Scene_graph_frame_text_node* node, size_t i) {
                node->update_from_margin_text(
                    window, to_margin_text(frame.annotations[i]), frame.text_rect);
            });

        // Annotation boxed outlines
        std::vector<Colored_rect> annotation_box_rects = collect_boxed_outline_rects(
            frame.annotations, window,
            [](const Annotation_primitive& p) { return p.boxed; },
            [](const Annotation_primitive& p) { return p.rect; });
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_annotation_box_nodes,
            static_cast<qsizetype>(annotation_box_rects.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(annotation_box_rects[i].rect);
                node->setColor(annotation_box_rects[i].color);
            });

        // Whitespace marks (dots vs rasterized tab arrows)
        std::vector<Colored_rect> whitespace_dot_rects;
        std::vector<Colored_rect> whitespace_tab_rects;
        whitespace_dot_rects.reserve(frame.whitespace_marks.size());
        for (const Whitespace_mark_primitive& mark : frame.whitespace_marks) {
            if (mark.kind == Whitespace_mark_kind::space_dot) {
                whitespace_dot_rects.push_back({mark.rect, mark.color});
            }
            else {
                append_rasterized_tab_arrow_rects(whitespace_tab_rects, mark, window);
            }
        }

        sync_rectangle_nodes(
            window,
            m_whitespace_group,
            m_whitespace_dot_nodes,
            static_cast<qsizetype>(whitespace_dot_rects.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(whitespace_dot_rects[i].rect);
                node->setColor(whitespace_dot_rects[i].color);
            });

        sync_rectangle_nodes(
            window,
            m_whitespace_group,
            m_whitespace_tab_nodes,
            static_cast<qsizetype>(whitespace_tab_rects.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(whitespace_tab_rects[i].rect);
                node->setColor(whitespace_tab_rects[i].color);
            });

        // Decoration underlines (hotspot + style underlines as rectangles)
        std::vector<Colored_rect> decoration_underline_rects;
        for (const Decoration_underline_primitive& underline : frame.decoration_underlines) {
            append_horizontal_rect(decoration_underline_rects, underline.rect, underline.color, window);
        }

        sync_rectangle_nodes(
            window,
            m_decoration_group,
            m_decoration_underline_nodes,
            static_cast<qsizetype>(decoration_underline_rects.size()),
            [&](QSGRectangleNode* node, size_t i) {
                node->setRect(decoration_underline_rects[i].rect);
                node->setColor(decoration_underline_rects[i].color);
            });

        sync_frame_text_nodes(window, m_indent_guide_group, m_indent_guide_nodes,
            (qsizetype)frame.indent_guides.size(),
            [&](Scene_graph_shape_node<Indent_guide_primitive>* node, size_t i) {
                const Indent_guide_primitive& guide = frame.indent_guides[i];
                const qreal pixel = physical_pixel_size(window);
                const QRectF bounds(guide.x - pixel, guide.top,
                    3 * pixel, guide.bottom - guide.top + pixel);
                node->update(window, guide, bounds.intersected(frame.text_rect));
            });

    }

private:
    std::uint64_t m_static_revision = 0;
    QRectF m_static_text_rect;
    qreal m_static_dpr = 0.0;
    QSGNode* m_background_group      = nullptr;
    std::array<QSGNode*, 3> m_current_line_groups{};
    std::array<QSGNode*, 3> m_line_background_groups{};
    std::array<QSGNode*, 3> m_selection_groups{};
    QSGNode* m_base_underline_group  = nullptr;
    QSGNode* m_gutter_group          = nullptr;
    QSGNode* m_marker_group          = nullptr;
    QSGNode* m_indicator_under_group = nullptr;
    QSGClipNode* m_text_clip_node          = nullptr;
    QSGNode* m_representation_group        = nullptr;
    QSGNode* m_representation_text_group   = nullptr;
    QSGNode* m_text_group                  = nullptr;
    QSGNode* m_annotation_background_group = nullptr;
    QSGNode* m_annotation_text_group       = nullptr;
    QSGNode* m_whitespace_group            = nullptr;
    QSGNode* m_decoration_group            = nullptr;
    QSGNode* m_indent_guide_group          = nullptr;
    QSGNode* m_indicator_over_group        = nullptr;
    QSGNode* m_overlay_group               = nullptr;

    QSGRectangleNode* m_background_node              = nullptr;
    std::vector<Scene_graph_shape_node<Gutter_band>*> m_gutter_background_nodes;

    std::array<std::vector<QSGRectangleNode*>, 3> m_current_line_nodes;
    std::array<std::vector<QSGRectangleNode*>, 3> m_line_background_nodes;
    std::array<std::vector<QSGRectangleNode*>, 3> m_selection_nodes;
    std::vector<QSGRectangleNode*> m_base_underline_nodes;
    std::vector<QSGRectangleNode*>            m_representation_blob_nodes;
    std::vector<Scene_graph_frame_text_node*> m_representation_text_nodes;
    std::vector<Scene_graph_frame_text_node*> m_text_nodes;
    std::vector<Scene_graph_frame_text_node*> m_gutter_nodes;
    std::vector<Scene_graph_shape_node<Marker_primitive>*> m_marker_nodes;
    std::vector<Scene_graph_shape_node<Indicator_primitive>*> m_indicator_under_nodes;
    std::vector<Scene_graph_shape_node<Indicator_primitive>*> m_indicator_over_nodes;
    std::vector<QSGRectangleNode*>            m_caret_nodes;

    std::vector<Scene_graph_frame_text_node*> m_fold_display_text_nodes;
    std::vector<QSGRectangleNode*>            m_fold_display_bg_nodes;
    std::vector<QSGRectangleNode*>            m_fold_display_box_nodes;
    std::vector<Scene_graph_frame_text_node*> m_eol_annotation_nodes;
    std::vector<QSGRectangleNode*>            m_eol_annotation_bg_nodes;
    std::vector<QSGRectangleNode*>            m_eol_annotation_box_nodes;
    std::vector<Scene_graph_frame_text_node*> m_annotation_nodes;
    std::vector<QSGRectangleNode*>            m_annotation_bg_nodes;
    std::vector<QSGRectangleNode*>            m_annotation_box_nodes;
    std::vector<QSGRectangleNode*>            m_whitespace_dot_nodes;
    std::vector<QSGRectangleNode*>            m_whitespace_tab_nodes;
    std::vector<QSGRectangleNode*>            m_decoration_underline_nodes;
    std::vector<Scene_graph_shape_node<Indent_guide_primitive>*> m_indent_guide_nodes;
};

} // namespace

QSGNode* Scene_graph_renderer::update(
    QQuickWindow*           window,
    QSGNode*                old_node,
    const Render_snapshot&  snapshot,
    const Render_frame&     frame)
{
    if (!window) {
        delete old_node;
        return nullptr;
    }

    auto* root = dynamic_cast<Scene_graph_root_node*>(old_node);
    if (!root) {
        delete old_node;
        root = new Scene_graph_root_node();
    }

    root->update_from_frame(window, snapshot, frame);
    // Software node additions inherit cached state from their immediate
    // parent; plain grouping nodes do not retain it. Refresh the complete
    // subtree from the identity transform after attaching descendants, so
    // both margin and body nodes inherit the item's transform and opacity.
    root->markDirty(QSGNode::DirtyMatrix);
    return root;
}

} // namespace Scintilla::Internal
