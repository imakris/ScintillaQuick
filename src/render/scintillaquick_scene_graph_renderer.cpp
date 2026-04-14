#include "scintillaquick_scene_graph_renderer.h"
#include "../core/scintillaquick_hierarchical_profiler.h"
#include "render_frame.h"

#include <ScintillaTypes.h>

#include <algorithm>
#include <cmath>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QSGClipNode>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGRendererInterface>
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

namespace Scintilla::Internal {

namespace {

QSGTextNode::RenderType map_render_type()
{
    switch (QQuickWindow::textRenderType()) {
        case QQuickWindow::QtTextRendering:     return QSGTextNode::QtRendering;
        case QQuickWindow::NativeTextRendering: return QSGTextNode::NativeRendering;
        case QQuickWindow::CurveTextRendering:  return QSGTextNode::CurveRendering;
    }

    return QSGTextNode::QtRendering;
}

void update_rectangle_node(
    QQuickWindow *window,
    QSGNode *parent,
    QSGRectangleNode *&node,
    const QRectF &rect,
    const QColor &color)
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

void update_geometry_node(
    QQuickWindow *window,
    QSGNode *parent,
    QSGGeometryNode *&node,
    const std::vector<QPointF> &points,
    QSGGeometry::DrawingMode mode,
    const QColor &color)
{
    if (!window || !parent || !node) {
        return;
    }
    static_cast<void>(parent);

    const bool visible                      = !points.empty() && color.isValid() && (color.alpha() > 0);
    const QColor materialColor              = visible ? color : QColor(0, 0, 0, 0);
    const QSGGeometry::DrawingMode drawMode = visible ? mode  : QSGGeometry::DrawLines;
    const std::vector<QPointF> hiddenPoints = {
        QPointF(0.0, 0.0),
        QPointF(0.0, 0.0),
    };
    const std::vector<QPointF> &geometryPoints = visible ? points : hiddenPoints;

    QSGGeometry *geometry = node->geometry();
    if (!geometry || geometry->vertexCount() != static_cast<int>(geometryPoints.size())) {
        geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), static_cast<int>(geometryPoints.size()));
        node->setGeometry(geometry);
    }
    geometry->setDrawingMode(drawMode);

    QSGGeometry::Point2D *vertices = geometry->vertexDataAsPoint2D();
    for (size_t i = 0; i < geometryPoints.size(); ++i) {
        vertices[i].set(geometryPoints[i].x(), geometryPoints[i].y());
    }

    QSGFlatColorMaterial *material = static_cast<QSGFlatColorMaterial *>(node->material());
    if (!material) {
        material = new QSGFlatColorMaterial();
        node->setMaterial(material);
    }
    material->setColor(materialColor);
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}

void update_clip_node(
    QSGClipNode *node,
    const QRectF &rect)
{
    if (!node) {
        return;
    }

    const QRectF normalized = rect.normalized();
    node->setIsRectangular(true);
    node->setClipRect(normalized);
    QSGGeometry *geometry = node->geometry();
    if (!geometry) {
        geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry, true);
    }

    QSGGeometry::Point2D *vertices = geometry->vertexDataAsPoint2D();
    vertices[0].set(normalized.left(),  normalized.top());
    vertices[1].set(normalized.right(), normalized.top());
    vertices[2].set(normalized.left(),  normalized.bottom());
    vertices[3].set(normalized.right(), normalized.bottom());
    node->markDirty(QSGNode::DirtyGeometry);
}

qreal physical_pixel_size(QQuickWindow *window)
{
    if (!window) {
        return 1.0;
    }

    return 1.0 / std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
}

bool is_software_backend(QQuickWindow *window)
{
    return window &&
        window->rendererInterface() &&
        window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
}

qreal snap_to_device_pixel(qreal value, qreal dpr)
{
    return std::round(value * dpr) / dpr;
}

struct colored_rect
{
    QRectF rect;
    QColor color;
};

struct fold_marker_colors
{
    QColor head;
    QColor body;
    QColor tail;
};

QRectF make_scintilla_circle_marker_rect(const QRectF &rect);
QColor marker_fill_color(const marker_primitive &primitive);
QColor marker_stroke_color(const marker_primitive &primitive);

QRectF snapped_underline_rect(const QRectF &rect, QQuickWindow *window)
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

QRectF snapped_outline_rect(const QRectF &rect, QQuickWindow *window)
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

void append_horizontal_pixel_rects(
    std::vector<colored_rect> &rects,
    const QRectF &rect,
    const QColor &color,
    QQuickWindow *window)
{
    const qreal dpr            = window ? std::max<qreal>(1.0, window->effectiveDevicePixelRatio()) : 1.0;
    const qreal physical_pixel = physical_pixel_size(window);
    const QRectF snapped       = snapped_underline_rect(rect, window);
    if (!snapped.isValid() || snapped.isEmpty()) {
        return;
    }

    const int width_pixels = std::max(1, static_cast<int>(std::ceil(snapped.width() * dpr)));
    rects.reserve(rects.size() + static_cast<size_t>(width_pixels));
    for (int pixel = 0; pixel < width_pixels; ++pixel) {
        rects.push_back({
            QRectF(
                snapped.left() + static_cast<qreal>(pixel) * physical_pixel,
                snapped.top(),
                physical_pixel,
                physical_pixel),
            color,
        });
    }
}

void append_outline_pixel_rects(
    std::vector<colored_rect> &rects,
    const QRectF &rect,
    const QColor &color,
    QQuickWindow *window)
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
    const qreal height    = std::max<qreal>(thickness, bottom - top );

    rects.push_back({QRectF(left, top, width, thickness), color});
    rects.push_back({QRectF(left, std::max<qreal>(top, bottom - thickness), width, thickness), color});
    rects.push_back({QRectF(left, top, thickness, height), color});
    rects.push_back({QRectF(std::max<qreal>(left, right - thickness), top, thickness, height), color});
}

void append_corner_mask_rects(
    std::vector<colored_rect> &rects,
    const QRectF &rect,
    const QColor &mask_color,
    QQuickWindow *window)
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

QRectF represented_blob_body_rect(
    const QRectF &inner_rect,
    const QRectF &text_clip_rect)
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

void append_line_pixel_rects(
    std::vector<colored_rect> &rects,
    const QPointF &from,
    const QPointF &to,
    const QColor &color,
    QQuickWindow *window)
{
    if (!color.isValid() || color.alpha() == 0) {
        return;
    }

    const qreal thickness = physical_pixel_size(window);
    const qreal dpr       = window ? std::max<qreal>(1.0, window->effectiveDevicePixelRatio()) : 1.0;
    const qreal dx        = to.x() - from.x();
    const qreal dy        = to.y() - from.y();
    const int steps = std::max(
        1,
        static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)) * dpr)));

    rects.reserve(rects.size() + static_cast<size_t>(steps + 1));
    for (int step = 0; step <= steps; ++step) {
        const qreal t = static_cast<qreal>(step) / static_cast<qreal>(steps);
        const qreal x = snap_to_device_pixel(from.x() + dx * t, dpr);
        const qreal y = snap_to_device_pixel(from.y() + dy * t, dpr);
        rects.push_back({QRectF(x, y, thickness, thickness), color});
    }
}

void append_raster_image_rects(
    std::vector<colored_rect> &rects,
    const QImage &image,
    const QRectF &logical_rect,
    QQuickWindow *window)
{
    if (!window || image.isNull() || !logical_rect.isValid() || logical_rect.isEmpty()) {
        return;
    }

    const qreal dpr     = std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
    const qreal pixel   = physical_pixel_size(window);
    const int width_px  = image.width();
    const int height_px = image.height();

    rects.reserve(rects.size() + static_cast<size_t>(width_px * height_px / 2));
    for (int y = 0; y < height_px; ++y) {
        int x = 0;
        while (x < width_px) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() == 0) {
                ++x;
                continue;
            }

            int run_end = x + 1;
            while (run_end < width_px && image.pixelColor(run_end, y) == color) {
                ++run_end;
            }

            rects.push_back({
                QRectF(
                    logical_rect.left() + static_cast<qreal>(x) / dpr,
                    logical_rect.top()  + static_cast<qreal>(y) / dpr,
                    static_cast<qreal>(run_end - x) * pixel,
                    pixel),
                color,
            });
            x = run_end;
        }
    }
}

bool is_fold_marker_symbol(int marker_type)
{
    switch (marker_type) {
        case static_cast<int>(MarkerSymbol::VLine):
        case static_cast<int>(MarkerSymbol::LCorner):
        case static_cast<int>(MarkerSymbol::LCornerCurve):
        case static_cast<int>(MarkerSymbol::TCorner):
        case static_cast<int>(MarkerSymbol::TCornerCurve):
        case static_cast<int>(MarkerSymbol::BoxPlus):
        case static_cast<int>(MarkerSymbol::BoxPlusConnected):
        case static_cast<int>(MarkerSymbol::BoxMinus):
        case static_cast<int>(MarkerSymbol::BoxMinusConnected):
        case static_cast<int>(MarkerSymbol::CirclePlus):
        case static_cast<int>(MarkerSymbol::CirclePlusConnected):
        case static_cast<int>(MarkerSymbol::CircleMinus):
        case static_cast<int>(MarkerSymbol::CircleMinusConnected):
            return true;
        default:
            return false;
    }
}

fold_marker_colors fold_marker_colors(const marker_primitive &primitive)
{
    const QColor normal = marker_fill_color(primitive);
    const QColor selected =
        (primitive.background_selected.isValid() && primitive.background_selected.alpha() > 0)
            ? primitive.background_selected
            : normal;

    fold_marker_colors colors{normal, normal, normal};
    switch (primitive.fold_part) {
        case 1:
        case 4:
            colors.head = selected;
            colors.tail = selected;
            break;
        case 2:
            colors.head = selected;
            colors.body = selected;
            break;
        case 3:
            colors.body = selected;
            colors.tail = selected;
            break;
        default:
            break;
    }
    return colors;
}

bool append_rasterized_fold_marker_rects(
    std::vector<colored_rect> &rects,
    const marker_primitive &primitive,
    QQuickWindow *window)
{
    if (!window || !is_fold_marker_symbol(primitive.marker_type)) {
        return false;
    }

    const QRectF whole_rect = primitive.rect.normalized();
    if (!whole_rect.isValid() || whole_rect.isEmpty()) {
        return true;
    }

    const qreal dpr     = std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
    const qreal pixel   = physical_pixel_size(window);
    const int width_px  = std::max(1, static_cast<int>(std::round(whole_rect.width() * dpr)));
    const int height_px = std::max(1, static_cast<int>(std::round(whole_rect.height() * dpr)));

    QImage image(width_px, height_px, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.translate(-whole_rect.left(), -whole_rect.top());

    const fold_marker_colors colors = fold_marker_colors(primitive);
    const QRectF symbol_rect        = make_scintilla_circle_marker_rect(whole_rect).normalized();
    const qreal symbol_center_x     = symbol_rect.center().x();
    const qreal symbol_center_y     = symbol_rect.center().y();
    const qreal line_left           = std::floor(symbol_center_x - pixel / 2.0);
    const QRectF vertical_line(line_left, whole_rect.top(), pixel, whole_rect.height());
    const QRectF right_stick(
        line_left + pixel,
        symbol_center_y,
        std::max<qreal>(0.0, whole_rect.right() - (line_left + pixel) - pixel),
        pixel);
    const qreal connector_center_x  = snap_to_device_pixel(whole_rect.center().x(), dpr);
    const qreal connector_center_y  = snap_to_device_pixel(whole_rect.center().y(), dpr);
    const qreal connector_line_left = std::floor(connector_center_x - pixel / 2.0);
    const QRectF connector_vertical_line(
        connector_line_left,
        whole_rect.top(),
        pixel,
        whole_rect.height());
    const QRectF connector_right_stick(
        connector_line_left,
        connector_center_y,
        std::max<qreal>(0.0, whole_rect.right() - connector_line_left - pixel),
        pixel);

    const auto fill_rect = [&](const QRectF &rect, const QColor &color) {
        if (!color.isValid() || color.alpha() == 0 || !rect.isValid() || rect.isEmpty()) {
            return;
        }
        painter.fillRect(rect, color);
    };

    const auto draw_box_symbol = [&](bool plus, bool connected) {
        const QRectF box_rect = symbol_rect.adjusted(-pixel, 0.0, 0.0, pixel);
        const QRectF above_symbol(line_left, whole_rect.top(),  pixel, std::max<qreal>(0.0, box_rect.top()      - whole_rect.top()));
        const QRectF below_symbol(line_left, box_rect.bottom(), pixel, std::max<qreal>(0.0, whole_rect.bottom() - box_rect.bottom()));
        if (connected) {
            fill_rect(below_symbol,
                plus && primitive.fold_part == 4 ? colors.tail :
                plus ? colors.body :
                colors.head);
            fill_rect(above_symbol, colors.body);
        }
        else
        if (!plus) {
            fill_rect(below_symbol, colors.head);
        }

        const QColor symbol_fill =
            (primitive.foreground.isValid() && primitive.foreground.alpha() > 0)
                ? primitive.foreground
                : marker_stroke_color(primitive);
        fill_rect(box_rect, symbol_fill);

        const QColor right_edge =
            connected && primitive.fold_part == 2 ? colors.tail : colors.head;
        fill_rect(QRectF(box_rect.left(), box_rect.top(), box_rect.width(), pixel), colors.head);
        fill_rect(QRectF(box_rect.left(), box_rect.bottom() - pixel, box_rect.width(), pixel), right_edge);
        fill_rect(QRectF(box_rect.left(), box_rect.top(), pixel, box_rect.height()), colors.head);
        fill_rect(QRectF(box_rect.right() - pixel, box_rect.top(), pixel, box_rect.height()), right_edge);

        const QRectF inner    = box_rect.adjusted(pixel + pixel, pixel + pixel, -(pixel + pixel), -(pixel + pixel));
        const qreal arm_width = std::max<qreal>(pixel, (inner.width() - pixel) / 2.0);
        const qreal mid_y     = inner.top() + arm_width;
        fill_rect(QRectF(inner.left(), mid_y, inner.width(), pixel), colors.tail);
        if (plus) {
            const qreal mid_x = inner.left() + arm_width;
            fill_rect(QRectF(mid_x, inner.top(), pixel, inner.height()), colors.tail);
        }
    };

    switch (primitive.marker_type) {
    case static_cast<int>(MarkerSymbol::VLine):
        fill_rect(connector_vertical_line, colors.body);
        break;
    case static_cast<int>(MarkerSymbol::LCorner):
    case static_cast<int>(MarkerSymbol::LCornerCurve):
        fill_rect(
            QRectF(
                connector_line_left,
                whole_rect.top(),
                pixel,
                std::max<qreal>(0.0, connector_center_y + pixel - whole_rect.top())),
            colors.tail);
        fill_rect(connector_right_stick, colors.tail);
        break;
    case static_cast<int>(MarkerSymbol::TCorner):
    case static_cast<int>(MarkerSymbol::TCornerCurve):
        fill_rect(
            QRectF(
                connector_line_left,
                whole_rect.top(),
                pixel,
                std::max<qreal>(0.0, connector_center_y + pixel - whole_rect.top())),
            colors.body);
        fill_rect(
            QRectF(
                connector_line_left,
                connector_center_y + pixel,
                pixel,
                std::max<qreal>(0.0, whole_rect.bottom() - (connector_center_y + pixel))),
            colors.head);
        fill_rect(connector_right_stick, colors.tail);
        break;
    case static_cast<int>(MarkerSymbol::BoxPlus):
        draw_box_symbol(true, false);
        break;
    case static_cast<int>(MarkerSymbol::BoxMinus):
        draw_box_symbol(false, false);
        break;
    case static_cast<int>(MarkerSymbol::BoxPlusConnected):
        draw_box_symbol(true, true);
        break;
    case static_cast<int>(MarkerSymbol::BoxMinusConnected):
        draw_box_symbol(false, true);
        break;
    default:
        return false;
    }

    painter.end();
    append_raster_image_rects(rects, image, whole_rect, window);
    return true;
}

void append_rasterized_circle_marker_rects(
    std::vector<colored_rect>& rects,
    const marker_primitive& primitive,
    QQuickWindow* window)
{
    if (!window) {
        return;
    }

    const QRectF circle_rect = make_scintilla_circle_marker_rect(primitive.rect).normalized();
    if (!circle_rect.isValid() || circle_rect.isEmpty()) {
        return;
    }

    const qreal dpr     = std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
    const qreal pixel   = physical_pixel_size(window);
    const int width_px  = std::max(1, static_cast<int>(std::round(circle_rect.width()  * dpr)));
    const int height_px = std::max(1, static_cast<int>(std::round(circle_rect.height() * dpr)));

    QImage image(width_px, height_px, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QPen pen(marker_stroke_color(primitive));
    pen.setCosmetic(true);
    pen.setWidth(1);
    painter.setPen(pen);
    painter.setBrush(marker_fill_color(primitive));

    QRectF local_rect(0.0, 0.0, circle_rect.width(), circle_rect.height());
    local_rect.adjust(pixel / 2.0, pixel / 2.0, -pixel / 2.0, -pixel / 2.0);
    painter.drawEllipse(local_rect);
    painter.end();

    append_raster_image_rects(rects, image, circle_rect, window);
}

void append_rasterized_tab_arrow_rects(
    std::vector<colored_rect> &rects,
    const whitespace_mark_primitive &primitive,
    QQuickWindow *window)
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

    const qreal half_width    = pixel / 2.0;
    const qreal origin_left   = snap_to_device_pixel(rect.left(), dpr);
    const qreal origin_top    = snap_to_device_pixel(rect.top(), dpr);

    const qreal left_stroke   =
        std::round(std::min(rect.left() + 2.0, rect.right() - 1.0)) + half_width;
    const qreal right_stroke  =
        std::max(left_stroke, std::round(rect.right()) - 1.0 - half_width);
    const qreal y_mid         = primitive.mid_y != 0.0 ? primitive.mid_y : std::floor(rect.center().y());
    const qreal y_mid_aligned = y_mid + half_width;
    const qreal ydiff         = std::floor(rect.height() / 2.0);
    qreal xhead               = right_stroke - ydiff;
    qreal head_height         = ydiff;
    if (xhead <= rect.left()) {
        head_height  -= rect.left() - xhead;
        xhead         = rect.left();
    }

    const QPointF arrow_point(right_stroke, y_mid_aligned);
    if (right_stroke > left_stroke) {
        painter.drawLine(QLineF(
            QPointF(left_stroke - origin_left, y_mid_aligned - origin_top),
            QPointF(arrow_point.x() - origin_left, arrow_point.y() - origin_top)));
    }

    painter.drawPolyline(
        QPolygonF({
            QPointF(xhead           - origin_left, y_mid_aligned   - head_height - origin_top),
            QPointF(arrow_point.x() - origin_left, arrow_point.y() - origin_top),
            QPointF(xhead           - origin_left, y_mid_aligned   + head_height - origin_top)
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

std::vector<QPointF> make_line_points(const QRectF &rect, qreal y)
{
    return {
        QPointF(rect.left(),  y),
        QPointF(rect.right(), y),
    };
}

std::vector<QPointF> make_rect_outline_points(const QRectF &rect)
{
    return {
        QPointF(rect.left(),  rect.top()),
        QPointF(rect.right(), rect.top()),
        QPointF(rect.right(), rect.bottom()),
        QPointF(rect.left(),  rect.bottom()),
        QPointF(rect.left(),  rect.top()),
    };
}

// Rectangle outline as DrawLines pairs (4 edges = 8 points).
std::vector<QPointF> make_rect_outline_as_lines(const QRectF &rect)
{
    const QPointF tl(rect.left(),  rect.top());
    const QPointF tr(rect.right(), rect.top());
    const QPointF br(rect.right(), rect.bottom());
    const QPointF bl(rect.left(),  rect.bottom());
    return { tl, tr, tr, br, br, bl, bl, tl };
}

// Convert DrawLineStrip sequence to DrawLines pairs.
std::vector<QPointF> line_strip_to_lines(const std::vector<QPointF> &strip)
{
    std::vector<QPointF> lines;
    if (strip.size() < 2) return lines;
    lines.reserve((strip.size() - 1) * 2);
    for (size_t i = 0; i + 1 < strip.size(); ++i) {
        lines.push_back(strip[i]);
        lines.push_back(strip[i + 1]);
    }
    return lines;
}

// Circle outline as DrawLines pairs (perimeter segments, no fill).
std::vector<QPointF> make_circle_outline_as_lines(const QRectF &rect, int segments = 16)
{
    std::vector<QPointF> lines;
    lines.reserve(static_cast<size_t>(segments) * 2);
    const QPointF center = rect.center();
    const qreal rx       = rect.width() / 2.0;
    const qreal ry       = rect.height() / 2.0;
    for (int i = 0; i < segments; ++i) {
        const qreal a0 = (static_cast<qreal>(i)     / static_cast<qreal>(segments)) * 6.28318530717958647692;
        const qreal a1 = (static_cast<qreal>(i + 1) / static_cast<qreal>(segments)) * 6.28318530717958647692;
        lines.emplace_back(center.x() + std::cos(a0) * rx, center.y() + std::sin(a0) * ry);
        lines.emplace_back(center.x() + std::cos(a1) * rx, center.y() + std::sin(a1) * ry);
    }
    return lines;
}

QColor marker_fill_color(const marker_primitive &primitive)
{
    if (primitive.background.isValid() && primitive.background.alpha() > 0) {
        return primitive.background;
    }
    return primitive.foreground;
}

QColor marker_stroke_color(const marker_primitive &primitive)
{
    if (primitive.foreground.isValid() && primitive.foreground.alpha() > 0) {
        return primitive.foreground;
    }
    return primitive.background;
}

// fold_part values: 0=undefined, 1=head, 2=body, 3=tail, 4=headWithTail
QColor fold_connector_color(const marker_primitive &primitive)
{
    // Use selected-background for active fold highlight, fall back to foreground
    if (primitive.fold_part > 0 &&
        primitive.background_selected.isValid() && primitive.background_selected.alpha() > 0) {
        return primitive.background_selected;
    }
    return marker_stroke_color(primitive);
}

// Generate fold-connector vertical/horizontal line segments based on fold_part.
// Returns line segments as point pairs for DrawLines mode.
std::vector<QPointF> make_fold_connector_points(const QRectF &rect, int fold_part)
{
    std::vector<QPointF> points;
    const qreal center_x = rect.center().x();
    const qreal center_y = rect.center().y();
    // body(2): full vertical line
    // head(1): vertical from center to bottom
    // tail(3): vertical from top to center, then horizontal from center to right
    // headWithTail(4): full vertical + horizontal from center to right
    if (fold_part == 2 || fold_part == 4) { // body or headWithTail: full vertical
        points.emplace_back(center_x, rect.top());
        points.emplace_back(center_x, rect.bottom());
    }
    else
    if (fold_part == 1) { // head: center to bottom
        points.emplace_back(center_x, center_y);
        points.emplace_back(center_x, rect.bottom());
    }
    else
    if (fold_part == 3) { // tail: top to center
        points.emplace_back(center_x, rect.top());
        points.emplace_back(center_x, center_y);
    }
    // tail and headWithTail: horizontal from center to right
    if (fold_part == 3 || fold_part == 4) {
        points.emplace_back(center_x, center_y);
        points.emplace_back(rect.right(), center_y);
    }
    return points;
}

std::vector<QPointF> make_marker_connector_points(const marker_primitive &primitive)
{
    const QRectF rect = primitive.rect;
    switch (primitive.marker_type) {
        case static_cast<int>(MarkerSymbol::VLine):
            return make_fold_connector_points(rect, 2);
        case static_cast<int>(MarkerSymbol::LCorner):
        case static_cast<int>(MarkerSymbol::LCornerCurve): {
            const qreal cx = rect.center().x();
            const qreal cy = rect.center().y();
            return {
                QPointF(cx, rect.top()), QPointF(cx, cy),
                QPointF(cx, cy), QPointF(rect.right(), cy),
            };
        }
        case static_cast<int>(MarkerSymbol::TCorner):
        case static_cast<int>(MarkerSymbol::TCornerCurve): {
            const qreal cx = rect.center().x();
            const qreal cy = rect.center().y();
            return {
                QPointF(cx, rect.top()), QPointF(cx, rect.bottom()),
                QPointF(cx, cy), QPointF(rect.right(), cy),
            };
        }
        case static_cast<int>(MarkerSymbol::BoxPlusConnected):
        case static_cast<int>(MarkerSymbol::CirclePlusConnected):
        case static_cast<int>(MarkerSymbol::BoxMinusConnected):
        case static_cast<int>(MarkerSymbol::CircleMinusConnected):
            return make_fold_connector_points(rect, primitive.fold_part);
        default:
            return {};
    }
}

std::vector<QPointF> make_rounded_rect_outline_points(const QRectF &rect)
{
    const qreal radius = std::max<qreal>(1.0, std::min(rect.width(), rect.height()) / 4.0);
    return {
        QPointF(rect.left()  + radius, rect.top()),
        QPointF(rect.right() - radius, rect.top()),
        QPointF(rect.right(),          rect.top()    + radius),
        QPointF(rect.right(),          rect.bottom() - radius),
        QPointF(rect.right() - radius, rect.bottom()),
        QPointF(rect.left()  + radius, rect.bottom()),
        QPointF(rect.left(),           rect.bottom() - radius),
        QPointF(rect.left(),           rect.top()    + radius),
        QPointF(rect.left()  + radius, rect.top()),
    };
}

QRectF make_scintilla_circle_marker_rect(const QRectF &rect)
{
    const qreal min_dim  = std::min(rect.width(), rect.height() - 2.0) - 1.0;
    const qreal center_x = std::floor(rect.center().x());
    const qreal center_y = std::floor(rect.center().y());
    const qreal half_dim = std::floor(min_dim / 2.0);
    return QRectF(
        center_x - half_dim,
        center_y - half_dim,
        half_dim * 2.0,
        half_dim * 2.0);
}

std::vector<QPointF> make_circle_fill_triangles(const QRectF &rect, int segments = 24)
{
    std::vector<QPointF> points;
    points.reserve(static_cast<size_t>(segments) * 3);
    const QPointF center = rect.center();
    const qreal rx       = rect.width() / 2.0;
    const qreal ry       = rect.height() / 2.0;
    for (int i = 0; i < segments; ++i) {
        const qreal a0 = (static_cast<qreal>(i)     / static_cast<qreal>(segments)) * 6.28318530717958647692;
        const qreal a1 = (static_cast<qreal>(i + 1) / static_cast<qreal>(segments)) * 6.28318530717958647692;
        points.emplace_back(center);
        points.emplace_back(center.x() + std::cos(a0) * rx, center.y() + std::sin(a0) * ry);
        points.emplace_back(center.x() + std::cos(a1) * rx, center.y() + std::sin(a1) * ry);
    }
    return points;
}

std::vector<QPointF> make_triangle_points(const QPointF &a, const QPointF &b, const QPointF &c)
{
    return {a, b, c};
}

void append_rect_triangles(
    std::vector<QPointF> &points,
    const QRectF &rect)
{
    if (!rect.isValid() || rect.isEmpty()) {
        return;
    }

    const QPointF top_left(    rect.left(),  rect.top());
    const QPointF top_right(   rect.right(), rect.top());
    const QPointF bottom_right(rect.right(), rect.bottom());
    const QPointF bottom_left( rect.left(),  rect.bottom());
    points.insert(points.end(), {
        top_left,  bottom_left, top_right,
        top_right, bottom_left, bottom_right
    });
}

std::vector<QPointF> make_squiggle_points(const QRectF &rect, bool low)
{
    std::vector<QPointF> points;
    const qreal baseline  = low ? (rect.top() + rect.height() * 0.7) : (rect.top() + rect.height() * 0.5);
    const qreal amplitude = std::max<qreal>(1.0, rect.height() * (low ? 0.16 : 0.24));
    const qreal step      = std::max<qreal>(2.0, rect.width() / 8.0);
    bool up               = true;
    for (qreal x = rect.left(); x <= rect.right(); x += step) {
        const qreal y = baseline + (up ? -amplitude : amplitude);
        points.emplace_back(x, y);
        up = !up;
    }
    if (points.empty() || points.back().x() < rect.right()) {
        points.emplace_back(rect.right(), baseline + (up ? -amplitude : amplitude));
    }
    return points;
}

std::vector<QPointF> make_indicator_squiggle_triangles(
    const QRectF &rect,
    bool low,
    QQuickWindow *window)
{
    const qreal dpr            = window ? std::max<qreal>(1.0, window->effectiveDevicePixelRatio()) : 1.0;
    const qreal physical_pixel = physical_pixel_size(window);
    const QRectF aligned(
        std::floor(rect.left() * dpr) / dpr,
        std::floor(rect.top() * dpr) / dpr,
        (std::ceil(rect.right() * dpr) / dpr) - (std::floor(rect.left() * dpr) / dpr),
        (std::ceil(rect.bottom() * dpr) / dpr) - (std::floor(rect.top() * dpr) / dpr));

    if (!aligned.isValid() || aligned.isEmpty()) {
        return {};
    }

    const int width_pixels = std::max(1, static_cast<int>(std::ceil(aligned.width() * dpr)));
    const int row_limit    = std::max(1, static_cast<int>(std::ceil(aligned.height() * dpr)));
    std::vector<QPointF> triangles;
    triangles.reserve(static_cast<size_t>(width_pixels) * 6);

    for (int pixel = 0; pixel < width_pixels; ++pixel) {
        int row = 0;
        if (low) {
            row = (pixel % 4 >= 2) ? 1 : 0;
        }
        else {
            switch (pixel % 4) {
            case 0:
                row = 0;
                break;
            case 1:
            case 3:
                row = 1;
                break;
            default:
                row = 2;
                break;
            }
        }
        row = std::min(row, row_limit - 1);

        append_rect_triangles(
            triangles,
            QRectF(
                aligned.left() + static_cast<qreal>(pixel) * physical_pixel,
                aligned.top()  + static_cast<qreal>(row)   * physical_pixel,
                physical_pixel,
                physical_pixel));
    }

    return triangles;
}

void append_indicator_squiggle_rects(
    std::vector<colored_rect> &rects,
    const indicator_primitive &primitive,
    QQuickWindow *window)
{
    const QRectF &rect         = primitive.rect;
    const QColor &color        = primitive.color;
    const bool low             = primitive.indicator_style == static_cast<int>(IndicatorStyle::SquiggleLow);
    const qreal dpr            = window ? std::max<qreal>(1.0, window->effectiveDevicePixelRatio()) : 1.0;
    const qreal physical_pixel = physical_pixel_size(window);
    const QRectF aligned(
        std::floor(rect.left()   * dpr) / dpr,
        std::floor(rect.top()    * dpr) / dpr,
        (std::ceil(rect.right()  * dpr) / dpr) - (std::floor(rect.left() * dpr) / dpr),
        (std::ceil(rect.bottom() * dpr) / dpr) - (std::floor(rect.top()  * dpr) / dpr));

    if (!aligned.isValid() || aligned.isEmpty()) {
        return;
    }

    const int width_pixels = std::max(1, static_cast<int>(std::ceil(aligned.width()  * dpr)));
    const int row_limit    = std::max(1, static_cast<int>(std::ceil(aligned.height() * dpr)));
    rects.reserve(rects.size() + static_cast<size_t>(width_pixels));

    for (int pixel = 0; pixel < width_pixels; ++pixel) {
        int row = 0;
        if (low) {
            row = (pixel % 4 >= 2) ? 1 : 0;
        }
        else {
            if (pixel == 1 || (pixel >= 4 && ((pixel % 4) == 0 || (pixel % 4) == 1))) {
                row = 0;
            }
            else {
                row = 1;
            }
        }
        row = std::min(row, row_limit - 1);
        rects.push_back({
            QRectF(
                aligned.left() +  static_cast<qreal>(pixel)      * physical_pixel,
                aligned.top()  + (static_cast<qreal>(row) + 1.0) * physical_pixel,
                physical_pixel,
                physical_pixel),
            color,
        });
    }
}

std::vector<QPointF> make_indicator_box_triangles(
    const QRectF &rect,
    QQuickWindow *window)
{
    const qreal dpr            = window ? std::max<qreal>(1.0, window->effectiveDevicePixelRatio()) : 1.0;
    const qreal physical_pixel = physical_pixel_size(window);
    const QRectF aligned(
        std::floor(rect.left()   * dpr) / dpr,
        std::floor(rect.top()    * dpr) / dpr,
        (std::ceil(rect.right()  * dpr) / dpr) - (std::floor(rect.left() * dpr) / dpr),
        (std::ceil(rect.bottom() * dpr) / dpr) - (std::floor(rect.top()  * dpr) / dpr));

    if (!aligned.isValid() || aligned.isEmpty()) {
        return {};
    }

    QRectF box = aligned;
    box.setTop(box.top() + physical_pixel);
    box.setBottom(snap_to_device_pixel(rect.center().y(), dpr) + physical_pixel);
    if (!box.isValid() || box.isEmpty()) {
        return {};
    }

    std::vector<QPointF> triangles;
    triangles.reserve(24);
    append_rect_triangles(triangles, QRectF(box.left(),                   box.top(),                     box.width(),    physical_pixel));
    append_rect_triangles(triangles, QRectF(box.left(),                   box.bottom() - physical_pixel, box.width(),    physical_pixel));
    append_rect_triangles(triangles, QRectF(box.left(),                   box.top(),                     physical_pixel, box.height()));
    append_rect_triangles(triangles, QRectF(box.right() - physical_pixel, box.top(),                     physical_pixel, box.height()));
    return triangles;
}

void append_indicator_box_rects(
    std::vector<colored_rect>& rects,
    const indicator_primitive& primitive,
    QQuickWindow* window)
{
    const qreal dpr            = window ? std::max<qreal>(1.0, window->effectiveDevicePixelRatio()) : 1.0;
    const qreal physical_pixel = physical_pixel_size(window);
    const QRectF& rect         = primitive.rect;
    QRectF line_rect           = primitive.line_rect;
    if (!line_rect.isValid() || line_rect.isEmpty()) {
        line_rect = rect;
    }
    const QRectF aligned(
        std::floor(rect.left()        * dpr) / dpr,
        std::floor(line_rect.top()    * dpr) / dpr,
        (std::ceil(rect.right()       * dpr) / dpr) - (std::floor(rect.left()     * dpr) / dpr),
        (std::ceil(line_rect.bottom() * dpr) / dpr) - (std::floor(line_rect.top() * dpr) / dpr));

    if (!aligned.isValid() || aligned.isEmpty()) {
        return;
    }

    QColor stroke_color = primitive.color;
    stroke_color.setAlpha(std::clamp(primitive.outline_alpha, 0, 255));
    const qreal stroke_width = std::max(physical_pixel, std::round(std::max<qreal>(1.0, primitive.stroke_width) * dpr) / dpr);
    QRectF box               = aligned;
    box.setTop(box.top() + stroke_width);
    box.setBottom(snap_to_device_pixel(rect.center().y(), dpr) + physical_pixel);
    if (!box.isValid() || box.isEmpty()) {
        return;
    }

    rects.reserve(rects.size() + 4);
    const qreal horizontal_left  = box.left() + stroke_width;
    const qreal horizontal_width = std::max<qreal>(0.0, box.width() - stroke_width * 2.0);
    rects.push_back({QRectF(horizontal_left,            box.top(),                   horizontal_width, stroke_width), stroke_color});
    rects.push_back({QRectF(horizontal_left,            box.bottom() - stroke_width, horizontal_width, stroke_width), stroke_color});
    rects.push_back({QRectF(box.left(),                 box.top(),                   stroke_width,     box.height()), stroke_color});
    rects.push_back({QRectF(box.right() - stroke_width, box.top(),                   stroke_width,     box.height()), stroke_color});
}

std::vector<QPointF> make_dashed_points(const QRectF &rect, qreal y, qreal dash_width, qreal gap_width)
{
    std::vector<QPointF> points;
    for (qreal x = rect.left(); x < rect.right();  x += dash_width + gap_width) {
        const qreal end_x = std::min(rect.right(), x +  dash_width);
        points.emplace_back(x, y);
        points.emplace_back(end_x, y);
    }
    return points;
}

std::vector<QPointF> make_plus_points(const QRectF &rect)
{
    const QPointF center = rect.center();
    const qreal arm_x    = std::max<qreal>(1.0, rect.width()  / 3.5);
    const qreal arm_y    = std::max<qreal>(1.0, rect.height() / 3.5);
    return {
        QPointF(center.x() - arm_x, center.y()),
        QPointF(center.x() + arm_x, center.y()),
        QPointF(center.x(),         center.y() - arm_y),
        QPointF(center.x(),         center.y() + arm_y),
    };
}

std::vector<QPointF> make_diagonal_points(const QRectF &rect)
{
    std::vector<QPointF> points;
    const qreal pitch = std::max<qreal>(3.0, rect.width() / 6.0);
    for (qreal x = rect.left(); x < rect.right(); x += pitch) {
        const qreal end_x = std::min(rect.right(), x + pitch);
        points.emplace_back(x, rect.bottom()  - 1.0);
        points.emplace_back(end_x, rect.top() + 1.0);
    }
    return points;
}

std::vector<QPointF> make_tt_points(const QRectF &rect)
{
    std::vector<QPointF> points;
    const qreal pitch       = std::max<qreal>(5.0, rect.width() / 4.0);
    const qreal top         = rect.top() + rect.height() * 0.35;
    const qreal stem_top    = top + 1.0;
    const qreal stem_bottom = rect.bottom() - 1.0;
    for (qreal x = rect.left(); x < rect.right(); x += pitch) {
        const qreal bar_left  = x;
        const qreal bar_right = std::min(rect.right(), x + pitch * 0.65);
        const qreal stem_x    = std::min(rect.right(), x + pitch * 0.45);
        points.emplace_back(bar_left, top);
        points.emplace_back(bar_right, top);
        points.emplace_back(stem_x, stem_top);
        points.emplace_back(stem_x, stem_bottom);
    }
    return points;
}

std::vector<QPointF> make_dotted_box_points(const QRectF &rect)
{
    std::vector<QPointF> points;
    const qreal dash = std::max<qreal>(2.0, rect.width() / 8.0);
    const qreal gap  = dash;
    for (qreal x = rect.left(); x < rect.right(); x += dash + gap) {
        const qreal end_x = std::min(rect.right(), x + dash);
        points.emplace_back(x, rect.top());
        points.emplace_back(end_x, rect.top());
        points.emplace_back(x, rect.bottom());
        points.emplace_back(end_x, rect.bottom());
    }
    for (qreal y = rect.top(); y < rect.bottom(); y += dash + gap) {
        const qreal end_y = std::min(rect.bottom(), y + dash);
        points.emplace_back(rect.left(),  y);
        points.emplace_back(rect.left(),  end_y);
        points.emplace_back(rect.right(), y);
        points.emplace_back(rect.right(), end_y);
    }
    return points;
}

std::vector<QPointF> make_marker_bar_points(const QRectF &rect)
{
    const qreal bar_width = std::max<qreal>(2.0, rect.width() / 3.0);
    const qreal left      = rect.center().x() - bar_width     / 2.0;
    const QRectF barRect(left, rect.top(), bar_width, rect.height());
    const QPointF center = barRect.center();
    return {
        center,
        QPointF(barRect.left(),  barRect.top()),
        QPointF(barRect.right(), barRect.top()),
        QPointF(barRect.right(), barRect.bottom()),
        QPointF(barRect.left(),  barRect.bottom()),
        QPointF(barRect.left(),  barRect.top()),
    };
}

std::vector<QPointF> make_filled_rect_points(const QRectF &rect)
{
    const QPointF center = rect.center();
    return {
        center,
        QPointF(rect.left(),  rect.top()),
        QPointF(rect.right(), rect.top()),
        QPointF(rect.right(), rect.bottom()),
        QPointF(rect.left(),  rect.bottom()),
        QPointF(rect.left(),  rect.top()),
    };
}

std::vector<QPointF> make_arrow_points(const QRectF &rect, bool down)
{
    const QPointF center    = rect.center();
    const qreal half_width  = std::max<qreal>(1.0, rect.width()  / 3.0);
    const qreal half_height = std::max<qreal>(1.0, rect.height() / 3.0);
    if (down) {
        return {
            QPointF(center.x() - half_width, rect.top()    + half_height),
            QPointF(center.x() + half_width, rect.top()    + half_height),
            QPointF(center.x(),              rect.bottom() - half_height),
        };
    }
    return {
        QPointF(center.x() - half_width, rect.bottom() - half_height),
        QPointF(center.x() + half_width, rect.bottom() - half_height),
        QPointF(center.x(),              rect.top()    + half_height),
    };
}

std::vector<QPointF> make_bookmark_points(const QRectF &rect, bool vertical)
{
    const QPointF center = rect.center();
    const qreal inset    = std::max<qreal>(1.0, rect.width() / 6.0);
    if (vertical) {
        return {
            QPointF(center.x() - inset, rect.top()),
            QPointF(center.x() + inset, rect.top()),
            QPointF(center.x() + inset, rect.bottom() - inset),
            QPointF(center.x(), rect.bottom()),
            QPointF(center.x() - inset, rect.bottom() - inset),
            QPointF(center.x() - inset, rect.top()),
        };
    }
    return {
        QPointF(rect.left(), center.y() - inset),
        QPointF(rect.right() - inset, center.y() - inset),
        QPointF(rect.right(), center.y()),
        QPointF(rect.right() - inset, center.y() + inset),
        QPointF(rect.left(), center.y() + inset),
        QPointF(rect.left(), center.y() - inset),
    };
}

std::vector<QPointF> make_short_arrow_points(const QRectF &rect)
{
    const QPointF center    = rect.center();
    const qreal half_width  = std::max<qreal>(1.0, rect.width()  / 4.0);
    const qreal half_height = std::max<qreal>(1.0, rect.height() / 4.0);
    return {
        QPointF(center.x(),              rect.bottom() - half_height),
        QPointF(center.x() + half_width, center.y()),
        QPointF(center.x(),              rect.top() + half_height),
        QPointF(center.x(),              center.y() - half_height),
        QPointF(center.x() - half_width, center.y()),
        QPointF(center.x(),              center.y() + half_height),
        QPointF(center.x(),              rect.bottom() - half_height),
    };
}

std::vector<QPointF> make_dotdotdot_points(const QRectF &rect)
{
    std::vector<QPointF> points;
    const qreal center_y = rect.center().y();
    const qreal radius   = std::max<qreal>(1.0, std::min(rect.width(), rect.height()) / 8.0);
    const qreal spacing  = radius * 3.0;
    const qreal start_x  = rect.center().x() - spacing;
    for (int i = 0; i < 3; ++i) {
        const qreal x = start_x + spacing * static_cast<qreal>(i);
        points.emplace_back(x - radius, center_y);
        points.emplace_back(x + radius, center_y);
    }
    return points;
}

std::vector<QPointF> make_diamond_points(const QRectF &rect)
{
    const QPointF center = rect.center();
    return {
        QPointF(center.x(), rect.top()),
        QPointF(rect.right(), center.y()),
        QPointF(center.x(), rect.bottom()),
        QPointF(rect.left(), center.y()),
        QPointF(center.x(), rect.top()),
    };
}

class Scene_graph_frame_text_node final : public QSGNode
{
public:
    void update_from_visual_line(
        QQuickWindow *window,
        const visual_line_frame &visual_line,
        const QRectF &viewport)
    {
        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line");

        if (!window) {
            return;
        }

        ensure_nodes(window);

        const bool same_key = m_has_cached_key &&
            m_cached_key.document_line == visual_line.key.document_line &&
            m_cached_key.subline_index == visual_line.key.subline_index;

        {
            SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line.reuse_check");
            if (same_key && layouts_match_content(visual_line)) {
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

        {
            SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line.rebuild");
            m_layouts.clear();
            m_cached_runs.clear();
            m_layout_positions.clear();
            m_text_node->setRenderType(map_render_type());
            m_text_node->setColor(Qt::white);
            m_text_node->setViewport(viewport);
            m_text_node->clear();
            update_clip_node(m_clip_node, viewport);
            set_translation(QPointF(0.0, 0.0));
        }

        for (const text_run &run : visual_line.text_runs) {
            if (run.text.isEmpty() || run.represented_as_blob) {
                continue;
            }

            SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line.layout_run");

            std::unique_ptr<QTextLayout> layout;
            qreal line_ascent = 0.0;
            {
                SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line.layout_run.create_layout");
                layout = std::make_unique<QTextLayout>(run.text, run.font);

                QTextOption option;
                option.setWrapMode(QTextOption::NoWrap);
                if (run.direction == text_direction::right_to_left) {
                    option.setTextDirection(Qt::RightToLeft);
                }
                else
                if (run.direction == text_direction::mixed) {
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
            }

            {
                SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line.layout_run.shape_text");
                layout->beginLayout();
                QTextLine line = layout->createLine();
                if (line.isValid()) {
                    if (run.direction == text_direction::right_to_left ||
                        run.direction == text_direction::mixed) {
                        line.setLineWidth(std::max<qreal>(1.0, run.width));
                    }
                    else {
                        line.setLineWidth(1000000.0);
                    }
                    line.setPosition(QPointF(0.0, 0.0));
                    line_ascent = line.ascent();
                }
                layout->endLayout();
            }

            {
                SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line.layout_run.attach_node");
                const QPointF pos(
                    run.position.x(),
                    run.position.y() - line_ascent);

                {
                    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line.layout_run.attach_node.add_text_layout");
                    m_text_node->addTextLayout(pos, layout.get());
                }
                {
                    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_visual_line.layout_run.attach_node.cache_run");
                    m_layouts.push_back(std::move(layout));
                    m_cached_runs.push_back(run);
                    m_layout_positions.push_back(run.position);
                }
            }
        }

        m_cached_key      = visual_line.key;
        m_has_cached_key  = true;
        m_cached_viewport = viewport;
    }

    const visual_line_key &cached_key() const { return m_cached_key; }
    bool has_valid_key() const { return m_has_cached_key; }
    void clear_cached_key() { m_has_cached_key = false; }

    void update_from_margin_text(
        QQuickWindow *window,
        const margin_text_primitive &margin,
        const QRectF &viewport)
    {
        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.text_node.update_from_margin_text");

        if (!window) {
            return;
        }

        ensure_nodes(window);

        const visual_line_key margin_key{margin.document_line, margin.subline_index};
        const bool same_key = m_has_cached_key &&
            m_cached_key == margin_key;

        if (same_key && margin_layouts_match(margin)) {
            if (margin_position_matches(margin, viewport)) {
                return;
            }
            QPointF delta;
            if (uniform_translation_delta(margin, delta)) {
                m_text_node->setColor(margin.foreground);
                m_text_node->setViewport(viewport);
                update_clip_node(m_clip_node, m_cached_clip_rect.translated(delta));
                set_translation(delta);
                m_cached_viewport  = viewport;
                m_cached_clip_rect = m_cached_clip_rect.translated(delta);
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

        auto layout = std::make_unique<QTextLayout>(margin.text, margin.font);

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
            margin.baseline_y - line_ascent);
        m_text_node->addTextLayout(pos, layout.get());
        m_layouts.push_back(std::move(layout));
        m_layout_positions.push_back(pos);
        m_cached_margin_text       = margin.text;
        m_cached_margin_font       = margin.font;
        m_cached_margin_foreground = margin.foreground;
        m_cached_clip_rect         = clip_rect;

        m_cached_key      = margin_key;
        m_has_cached_key  = true;
        m_cached_viewport = viewport;
    }

    void ensure_nodes(QQuickWindow *window)
    {
        if (!m_clip_node) {
            m_clip_node = new QSGClipNode();
            appendChildNode(m_clip_node);
        }
        if (!m_transform_node) {
            m_transform_node = new QSGTransformNode();
            m_clip_node->appendChildNode(m_transform_node);
        }
        if (!m_text_node) {
            m_text_node = window->createTextNode();
            m_transform_node->appendChildNode(m_text_node);
        }
    }

    void set_translation(const QPointF &delta)
    {
        if (!m_transform_node) {
            return;
        }
        m_translation = delta;
        QMatrix4x4 matrix;
        matrix.translate(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
        m_transform_node->setMatrix(matrix);
    }

    bool margin_layouts_match(const margin_text_primitive &margin) const
    {
        return margin.text == m_cached_margin_text &&
               margin.font == m_cached_margin_font &&
               margin.foreground == m_cached_margin_foreground &&
               ((margin.text.isEmpty() && m_layouts.empty()) ||
                (!margin.text.isEmpty() && m_layouts.size() == 1));
    }

    bool margin_position_matches(const margin_text_primitive &margin, const QRectF &viewport) const
    {
        const QRectF clip_rect =
            (margin.clip_rect.isValid() && !margin.clip_rect.isEmpty())
            ? margin.clip_rect
            : viewport;
        return m_cached_viewport == viewport &&
               m_cached_clip_rect == clip_rect &&
               m_layout_positions.size() == 1 &&
               m_layout_positions[0] + m_translation == margin.position;
    }

    bool layouts_match_content(const visual_line_frame &vl) const
    {
        size_t cached_idx = 0;
        for (const text_run &run : vl.text_runs) {
            if (run.text.isEmpty() || run.represented_as_blob) {
                continue;
            }
            if (cached_idx >= m_cached_runs.size()) {
                return false;
            }
            const text_run &cached_run = m_cached_runs[cached_idx];
            if (cached_run.text != run.text ||
                cached_run.font != run.font ||
                cached_run.foreground != run.foreground ||
                cached_run.blob_outer != run.blob_outer ||
                cached_run.blob_inner != run.blob_inner ||
                cached_run.blob_outer_rect != run.blob_outer_rect ||
                cached_run.blob_inner_rect != run.blob_inner_rect ||
                cached_run.direction != run.direction ||
                cached_run.width != run.width ||
                cached_run.top != run.top ||
                cached_run.bottom != run.bottom ||
                cached_run.is_represented_text != run.is_represented_text ||
                cached_run.represented_as_blob != run.represented_as_blob ||
                cached_run.style_id != run.style_id) {
                return false;
            }
            ++cached_idx;
        }
        return cached_idx == m_cached_runs.size();
    }

    bool positions_match(const visual_line_frame &vl, const QRectF &viewport) const
    {
        if (m_cached_viewport != viewport) {
            return false;
        }
        size_t idx = 0;
        for (const text_run &run : vl.text_runs) {
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

    bool uniform_translation_delta(const visual_line_frame &vl, QPointF &delta) const
    {
        delta           = QPointF(0.0, 0.0);
        bool have_delta = false;
        size_t idx      = 0;
        for (const text_run &run : vl.text_runs) {
            if (run.text.isEmpty() || run.represented_as_blob) {
                continue;
            }
            if (idx >= m_layout_positions.size()) {
                return false;
            }
            const QPointF current_delta = run.position - m_layout_positions[idx];
            if (!have_delta) {
                delta      = current_delta;
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

    bool uniform_translation_delta(const margin_text_primitive &margin, QPointF &delta) const
    {
        if (m_layout_positions.size() != 1) {
            return false;
        }
        delta = margin.position - m_layout_positions[0];
        return true;
    }

    QSGClipNode *m_clip_node           = nullptr;
    QSGTextNode *m_text_node           = nullptr;
    QSGTransformNode *m_transform_node = nullptr;
    std::vector<std::unique_ptr<QTextLayout>> m_layouts;
    std::vector<text_run> m_cached_runs;
    std::vector<QPointF> m_layout_positions;
    visual_line_key m_cached_key;
    bool m_has_cached_key = false;
    QRectF m_cached_viewport;
    QRectF m_cached_clip_rect;
    QString m_cached_margin_text;
    QFont m_cached_margin_font;
    QColor m_cached_margin_foreground;
    QPointF m_translation;
};

template <typename NodeT, typename UpdateFn>
void sync_frame_text_nodes(
    QQuickWindow *window,
    QSGNode *parent,
    std::vector<NodeT *> &nodes,
    qsizetype count,
    UpdateFn &&update_fn)
{
    if (!window || !parent) {
        return;
    }

    while (static_cast<qsizetype>(nodes.size()) > count) {
        NodeT *node = nodes.back();
        nodes.pop_back();
        parent->removeChildNode(node);
        delete node;
    }

    while (static_cast<qsizetype>(nodes.size()) < count) {
        auto *node = new NodeT();
        parent->appendChildNode(node);
        nodes.push_back(node);
    }

    for (qsizetype i = 0; i < count; ++i) {
        update_fn(nodes[static_cast<size_t>(i)], static_cast<size_t>(i));
    }
}

template <typename UpdateFn>
void sync_rectangle_nodes(
    QQuickWindow *window,
    QSGNode *parent,
    std::vector<QSGRectangleNode *> &nodes,
    qsizetype count,
    UpdateFn &&update_fn)
{
    if (!window || !parent) {
        return;
    }

    while (static_cast<qsizetype>(nodes.size()) > count) {
        QSGRectangleNode *node = nodes.back();
        nodes.pop_back();
        parent->removeChildNode(node);
        delete node;
    }

    while (static_cast<qsizetype>(nodes.size()) < count) {
        QSGRectangleNode *node = window->createRectangleNode();
        parent->appendChildNode(node);
        nodes.push_back(node);
    }

    for (qsizetype i = 0; i < count; ++i) {
        update_fn(nodes[static_cast<size_t>(i)], static_cast<size_t>(i));
    }
}

template <typename UpdateFn>
void sync_geometry_nodes(
    QQuickWindow *window,
    QSGNode *parent,
    std::vector<QSGGeometryNode *> &nodes,
    qsizetype count,
    UpdateFn &&update_fn)
{
    if (!window || !parent) {
        return;
    }

    while (static_cast<qsizetype>(nodes.size()) > count) {
        QSGGeometryNode *node = nodes.back();
        nodes.pop_back();
        parent->removeChildNode(node);
        delete node;
    }

    while (static_cast<qsizetype>(nodes.size()) < count) {
        auto *node = new QSGGeometryNode();
        node->setFlag(QSGNode::OwnsGeometry);
        node->setFlag(QSGNode::OwnsMaterial);
        node->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 2));
        auto *material = new QSGFlatColorMaterial();
        material->setColor(QColor(0, 0, 0, 0));
        node->setMaterial(material);
        parent->appendChildNode(node);
        nodes.push_back(node);
    }

    for (qsizetype i = 0; i < count; ++i) {
        update_fn(nodes[static_cast<size_t>(i)], static_cast<size_t>(i));
    }
}

template <typename NodeT>
void reorder_child_nodes(QSGNode *parent, const std::vector<NodeT *> &nodes)
{
    if (!parent) {
        return;
    }

    for (NodeT *node : nodes) {
        parent->removeChildNode(node);
        parent->appendChildNode(node);
    }
}

uint64_t pack_visual_line_key(const visual_line_key &key)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(key.document_line)) << 32) |
            static_cast<uint64_t>(static_cast<uint32_t>(key.subline_index));
}

void sync_text_nodes_by_key(
    QQuickWindow *window,
    QSGNode *parent,
    std::vector<Scene_graph_frame_text_node *> &nodes,
    const std::vector<visual_line_frame> &visual_lines,
    const QRectF &viewport)
{
    if (!window || !parent) {
        return;
    }

    const size_t new_count = visual_lines.size();

    std::unordered_map<uint64_t, size_t> key_to_old_index;
    key_to_old_index.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i]->has_valid_key()) {
            key_to_old_index[pack_visual_line_key(nodes[i]->cached_key())] = i;
        }
    }

    std::vector<Scene_graph_frame_text_node *> new_nodes(new_count, nullptr);
    std::vector<bool> old_used(nodes.size(), false);

    for (size_t i = 0; i < new_count; ++i) {
        const uint64_t key = pack_visual_line_key(visual_lines[i].key);
        auto it            = key_to_old_index.find(key);
        if (it != key_to_old_index.end() && !old_used[it->second]) {
            new_nodes[i]         = nodes[it->second];
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
            new_nodes[i]            = nodes[unused_cursor];
            old_used[unused_cursor] = true;
            new_nodes[i]->clear_cached_key();
        }
        else {
            auto *node = new Scene_graph_frame_text_node();
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
        nodes.size() == new_nodes.size() &&
        std::equal(nodes.begin(), nodes.end(), new_nodes.begin());

    nodes = std::move(new_nodes);
    if (!order_unchanged) {
        reorder_child_nodes(parent, nodes);
    }

    for (size_t i = 0; i < new_count; ++i) {
        nodes[i]->update_from_visual_line(window, visual_lines[i], viewport);
    }
}

void sync_margin_text_nodes_by_key(
    QQuickWindow *window,
    QSGNode *parent,
    std::vector<Scene_graph_frame_text_node *> &nodes,
    const std::vector<margin_text_primitive> &margins,
    const QRectF &viewport)
{
    if (!window || !parent) {
        return;
    }

    const size_t new_count = margins.size();

    std::unordered_map<uint64_t, size_t> key_to_old_index;
    key_to_old_index.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i]->has_valid_key()) {
            key_to_old_index[pack_visual_line_key(nodes[i]->cached_key())] = i;
        }
    }

    std::vector<Scene_graph_frame_text_node *> new_nodes(new_count, nullptr);
    std::vector<bool> old_used(nodes.size(), false);

    for (size_t i = 0; i < new_count; ++i) {
        const visual_line_key margin_key{margins[i].document_line, margins[i].subline_index};
        const uint64_t key = pack_visual_line_key(margin_key);
        auto it            = key_to_old_index.find(key);
        if (it != key_to_old_index.end() && !old_used[it->second]) {
            new_nodes[i]         = nodes[it->second];
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
            new_nodes[i]            = nodes[unused_cursor];
            old_used[unused_cursor] = true;
            new_nodes[i]->clear_cached_key();
        }
        else {
            auto *node = new Scene_graph_frame_text_node();
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
        nodes.size() == new_nodes.size() &&
        std::equal(nodes.begin(), nodes.end(), new_nodes.begin());

    nodes = std::move(new_nodes);
    if (!order_unchanged) {
        reorder_child_nodes(parent, nodes);
    }

    for (size_t i = 0; i < new_count; ++i) {
        nodes[i]->update_from_margin_text(window, margins[i], viewport);
    }
}

class Scene_graph_root_node final : public QSGNode
{
public:
    Scene_graph_root_node()
    {
        m_background_group            = new QSGNode();
        m_current_line_group          = new QSGNode();
        m_selection_group             = new QSGNode();
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
        appendChildNode(m_current_line_group);
        appendChildNode(m_selection_group);
        appendChildNode(m_marker_group);
        appendChildNode(m_indicator_under_group);
        appendChildNode(m_text_clip_node);
        m_text_clip_node->appendChildNode(m_representation_group);
        m_text_clip_node->appendChildNode(m_representation_text_group);
        m_text_clip_node->appendChildNode(m_text_group);
        appendChildNode(m_annotation_background_group);
        appendChildNode(m_annotation_text_group);
        appendChildNode(m_gutter_group);
        appendChildNode(m_whitespace_group);
        appendChildNode(m_indent_guide_group);
        appendChildNode(m_decoration_group);
        appendChildNode(m_indicator_over_group);
        appendChildNode(m_overlay_group);
    }

    void update_from_frame(
        QQuickWindow *window,
        const render_snapshot &snapshot,
        const render_frame &frame)
    {
        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.update_from_frame");

        update_clip_node(m_text_clip_node, frame.text_rect);

        std::vector<colored_rect> representation_blob_fill_rects;
        std::vector<margin_text_primitive> representation_texts;
        for (const visual_line_frame &visual_line : frame.visual_lines) {
            for (const text_run &run : visual_line.text_runs) {
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
                margin_text_primitive primitive;
                primitive.text          = run.text;
                primitive.position      = run.position;
                primitive.baseline_y    = run.position.y();
                primitive.foreground    = run.foreground;
                primitive.font          = run.font;
                primitive.clip_rect     = QRectF();
                primitive.document_line = visual_line.key.document_line;
                primitive.subline_index = visual_line.key.subline_index * 1000
                    + static_cast<int>(representation_texts.size());
                primitive.style_id = run.style_id;
                representation_texts.push_back(std::move(primitive));
            }
        }
        sync_rectangle_nodes(
            window,
            m_representation_group,
            m_representation_blob_nodes,
            static_cast<qsizetype>(representation_blob_fill_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(snapped_outline_rect(representation_blob_fill_rects[i].rect, window));
                node->setColor(representation_blob_fill_rects[i].color);
            });
        sync_frame_text_nodes(
            window,
            m_representation_text_group,
            m_representation_text_nodes,
            static_cast<qsizetype>(representation_texts.size()),
            [&](Scene_graph_frame_text_node *node, size_t i) {
                node->update_from_margin_text(window, representation_texts[i], frame.text_rect);
            });

        // Backgrounds from snapshot
        update_rectangle_node(
            window,
            m_background_group,
            m_background_node,
            QRectF(QPointF(0.0, 0.0), snapshot.item_size),
            snapshot.background);
        sync_rectangle_nodes(
            window,
            m_background_group,
            m_gutter_background_nodes,
            static_cast<qsizetype>(snapshot.gutter_bands.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(snapshot.gutter_bands[i].rect);
                node->setColor(snapshot.gutter_bands[i].color);
            });

        // Current-line highlight from frame (authoritative capture)
        std::vector<current_line_primitive> filled_current_lines;
        std::vector<current_line_primitive> framed_current_lines;
        filled_current_lines.reserve(frame.current_line_primitives.size());
        framed_current_lines.reserve(frame.current_line_primitives.size());
        for (const current_line_primitive &primitive : frame.current_line_primitives) {
            if (primitive.framed) {
                framed_current_lines.push_back(primitive);
            }
            else {
                filled_current_lines.push_back(primitive);
            }
        }

        sync_rectangle_nodes(
            window,
            m_current_line_group,
            m_current_line_fill_nodes,
            static_cast<qsizetype>(filled_current_lines.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect( filled_current_lines[i].rect);
                node->setColor(filled_current_lines[i].color);
            });

        std::vector<colored_rect> current_line_frame_rects;
        for (const current_line_primitive &primitive : framed_current_lines) {
            append_outline_pixel_rects(current_line_frame_rects, primitive.rect, primitive.color, window);
        }
        sync_rectangle_nodes(
            window,
            m_current_line_group,
            m_current_line_frame_nodes,
            static_cast<qsizetype>(current_line_frame_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect( current_line_frame_rects[i].rect);
                node->setColor(current_line_frame_rects[i].color);
            });

        // Selection fills from frame
        sync_rectangle_nodes(
            window,
            m_selection_group,
            m_selection_nodes,
            static_cast<qsizetype>(frame.selection_primitives.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(frame.selection_primitives[i].rect);
                node->setColor(frame.selection_primitives[i].color);
            });

        {
            SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.update_from_frame.text");

            // Body text from frame visual lines (key-based reuse)
            sync_text_nodes_by_key(
                window,
                m_text_group,
                m_text_nodes,
                frame.visual_lines,
                frame.text_rect);

            // Gutter text from frame margin text primitives (key-based reuse)
            sync_margin_text_nodes_by_key(
                window,
                m_gutter_group,
                m_gutter_nodes,
                frame.margin_text_primitives,
                frame.margin_rect);
        }

        // Indicator geometry from frame metadata (index-based to avoid copies).
        std::vector<size_t> under_fill_idx, under_geo_idx, over_fill_idx, over_geo_idx;
        std::vector<colored_rect> under_stroke_rects, over_stroke_rects;
        under_fill_idx.reserve(frame.indicator_primitives.size());
        under_geo_idx.reserve(frame.indicator_primitives.size());
        over_fill_idx.reserve(frame.indicator_primitives.size());
        over_geo_idx.reserve(frame.indicator_primitives.size());
        for (size_t j = 0; j < frame.indicator_primitives.size(); ++j) {
            const indicator_primitive &primitive = frame.indicator_primitives[j];
            const bool is_rectangle              = primitive.indicator_style == static_cast<int>(IndicatorStyle::Plain);
            const bool uses_stroke_rects =
                primitive.indicator_style == static_cast<int>(IndicatorStyle::Squiggle) ||
                primitive.indicator_style == static_cast<int>(IndicatorStyle::SquigglePixmap) ||
                primitive.indicator_style == static_cast<int>(IndicatorStyle::SquiggleLow) ||
                primitive.indicator_style == static_cast<int>(IndicatorStyle::Box);
            if (primitive.under_text) {
                if (is_rectangle) {
                    under_fill_idx.push_back(j);
                }
                else
                if (uses_stroke_rects) {
                    if (primitive.indicator_style == static_cast<int>(IndicatorStyle::Box)) {
                        append_indicator_box_rects(under_stroke_rects, primitive, window);
                    }
                    else {
                        append_indicator_squiggle_rects(
                            under_stroke_rects,
                            primitive,
                            window);
                    }
                }
                else {
                    under_geo_idx.push_back(j);
                }
            }
            else {
                if (is_rectangle) {
                    over_fill_idx.push_back(j);
                }
                else
                if (uses_stroke_rects) {
                    if (primitive.indicator_style == static_cast<int>(IndicatorStyle::Box)) {
                        append_indicator_box_rects(over_stroke_rects, primitive, window);
                    }
                    else {
                        append_indicator_squiggle_rects(
                            over_stroke_rects,
                            primitive,
                            window);
                    }
                }
                else {
                    over_geo_idx.push_back(j);
                }
            }
        }

        sync_rectangle_nodes(
            window,
            m_indicator_under_group,
            m_indicator_under_fill_nodes,
            static_cast<qsizetype>(under_fill_idx.size()),
            [&](QSGRectangleNode *node, size_t i) {
                const indicator_primitive &p = frame.indicator_primitives[under_fill_idx[i]];
                node->setRect(p.rect);
                node->setColor(p.color);
            });

        sync_rectangle_nodes(
            window,
            m_indicator_under_group,
            m_indicator_under_stroke_nodes,
            static_cast<qsizetype>(under_stroke_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(under_stroke_rects[i].rect);
                node->setColor(under_stroke_rects[i].color);
            });

        sync_geometry_nodes(
            window,
            m_indicator_under_group,
            m_indicator_under_nodes,
            static_cast<qsizetype>(under_geo_idx.size()),
            [&](QSGGeometryNode *&node, size_t i) {
                const indicator_primitive &primitive = frame.indicator_primitives[under_geo_idx[i]];
                const QRectF rect                    = primitive.rect;
                std::vector<QPointF> points;
                QSGGeometry::DrawingMode mode = QSGGeometry::DrawLines;

                switch (primitive.indicator_style) {
                    case static_cast<int>(IndicatorStyle::Hidden):
                    case static_cast<int>(IndicatorStyle::TextFore):
                    case static_cast<int>(IndicatorStyle::Plain):
                        break;
                    case static_cast<int>(IndicatorStyle::Squiggle):
                    case static_cast<int>(IndicatorStyle::SquigglePixmap):
                        points = make_indicator_squiggle_triangles(rect, false, window);
                        mode   = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(IndicatorStyle::SquiggleLow):
                        points = make_indicator_squiggle_triangles(rect, true, window);
                        mode   = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(IndicatorStyle::TT):
                        points = make_tt_points(rect);
                        break;
                    case static_cast<int>(IndicatorStyle::Diagonal):
                        points = make_diagonal_points(rect);
                        break;
                    case static_cast<int>(IndicatorStyle::Strike):
                        points = make_line_points(rect, rect.center().y());
                        break;
                    case static_cast<int>(IndicatorStyle::Box):
                        points = make_indicator_box_triangles(rect, window);
                        mode   = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(IndicatorStyle::RoundBox):
                        points = line_strip_to_lines(make_rounded_rect_outline_points(rect.adjusted(0.5, 0.5, -0.5, -0.5)));
                        mode   = QSGGeometry::DrawLines;
                        break;
                    case static_cast<int>(IndicatorStyle::StraightBox):
                        points = make_rect_outline_as_lines(rect.adjusted(0.0, 0.0, -physical_pixel_size(window), -physical_pixel_size(window)));
                        mode   = QSGGeometry::DrawLines;
                        break;
                    case static_cast<int>(IndicatorStyle::FullBox):
                        points = make_filled_rect_points(rect);
                        mode   = QSGGeometry::DrawTriangleFan;
                        break;
                    case static_cast<int>(IndicatorStyle::Gradient):
                        points = make_dashed_points(rect, rect.top() + 1.0, std::max<qreal>(2.0, rect.width() / 3.0), std::max<qreal>(1.0, rect.width() / 6.0));
                        break;
                    case static_cast<int>(IndicatorStyle::GradientCentre):
                        points = make_dashed_points(rect, rect.center().y(), std::max<qreal>(2.0, rect.width() / 3.0), std::max<qreal>(1.0, rect.width() / 6.0));
                        break;
                    case static_cast<int>(IndicatorStyle::DotBox):
                        points = make_dotted_box_points(rect);
                        break;
                    case static_cast<int>(IndicatorStyle::Dash):
                        points = make_dashed_points(rect, rect.center().y(), std::max<qreal>(2.0, rect.width() / 8.0), std::max<qreal>(2.0, rect.width() / 10.0));
                        break;
                    case static_cast<int>(IndicatorStyle::Dots):
                        points = make_dashed_points(rect, rect.center().y(), std::max<qreal>(1.0, rect.width() / 14.0), std::max<qreal>(1.0, rect.width() / 14.0));
                        break;
                    case static_cast<int>(IndicatorStyle::CompositionThick): {
                        const QRectF strip(
                            rect.left() + 1.0,
                            std::max(rect.top(), rect.bottom() - 3.0),
                            std::max<qreal>(1.0, rect.width() - 2.0),
                            2.0);
                        points = make_filled_rect_points(strip);
                        mode   = QSGGeometry::DrawTriangleFan;
                        break;
                    }
                    case static_cast<int>(IndicatorStyle::CompositionThin): {
                        const qreal y = rect.bottom() - 1.0;
                        points        = make_line_points(rect, y);
                        break;
                    }
                    case static_cast<int>(IndicatorStyle::Point):
                    case static_cast<int>(IndicatorStyle::PointCharacter):
                        points = make_triangle_points(
                            QPointF(rect.left()  + rect.width() * 0.2, rect.bottom() - 1.0),
                            QPointF(rect.right() - rect.width() * 0.2, rect.bottom() - 1.0),
                            QPointF(rect.center().x(),                 rect.top()    + 1.0));
                        mode = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(IndicatorStyle::PointTop):
                        points = make_triangle_points(
                            QPointF(rect.left()  + rect.width() * 0.2, rect.top()    + 1.0),
                            QPointF(rect.right() - rect.width() * 0.2, rect.top()    + 1.0),
                            QPointF(rect.center().x(),                 rect.bottom() - 1.0));
                        mode = QSGGeometry::DrawTriangles;
                        break;
                    default:
                        points = make_line_points(rect, rect.bottom() - 1.0);
                        break;
                }
                update_geometry_node(window, m_indicator_under_group, node, points, mode, primitive.color);
            });

        sync_rectangle_nodes(
            window,
            m_indicator_over_group,
            m_indicator_over_fill_nodes,
            static_cast<qsizetype>(over_fill_idx.size()),
            [&](QSGRectangleNode *node, size_t i) {
                const indicator_primitive& p = frame.indicator_primitives[over_fill_idx[i]];
                node->setRect( p.rect);
                node->setColor(p.color);
            });

        sync_rectangle_nodes(
            window,
            m_indicator_over_group,
            m_indicator_over_stroke_nodes,
            static_cast<qsizetype>(over_stroke_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(over_stroke_rects[i].rect);
                node->setColor(over_stroke_rects[i].color);
            });

        sync_geometry_nodes(
            window,
            m_indicator_over_group,
            m_indicator_over_nodes,
            static_cast<qsizetype>(over_geo_idx.size()),
            [&](QSGGeometryNode *&node, size_t i) {
                const indicator_primitive &primitive = frame.indicator_primitives[over_geo_idx[i]];
                const QRectF rect                    = primitive.rect;
                std::vector<QPointF> points;
                QSGGeometry::DrawingMode mode = QSGGeometry::DrawLines;

                switch (primitive.indicator_style) {
                    case static_cast<int>(IndicatorStyle::Hidden):
                    case static_cast<int>(IndicatorStyle::TextFore):
                    case static_cast<int>(IndicatorStyle::Plain):
                        break;
                    case static_cast<int>(IndicatorStyle::Squiggle):
                    case static_cast<int>(IndicatorStyle::SquigglePixmap):
                        points = make_indicator_squiggle_triangles(rect, false, window);
                        mode   = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(IndicatorStyle::SquiggleLow):
                        points = make_indicator_squiggle_triangles(rect, true, window);
                        mode   = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(IndicatorStyle::TT):
                        points = make_tt_points(rect);
                        break;
                    case static_cast<int>(IndicatorStyle::Diagonal):
                        points = make_diagonal_points(rect);
                        break;
                    case static_cast<int>(IndicatorStyle::Strike):
                        points = make_line_points(rect, rect.center().y());
                        break;
                    case static_cast<int>(IndicatorStyle::Box):
                        points = make_indicator_box_triangles(rect, window);
                        mode   = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(IndicatorStyle::RoundBox):
                        points = line_strip_to_lines(make_rounded_rect_outline_points(rect.adjusted(0.5, 0.5, -0.5, -0.5)));
                        mode   = QSGGeometry::DrawLines;
                        break;
                    case static_cast<int>(IndicatorStyle::StraightBox):
                        points = make_rect_outline_as_lines(rect.adjusted(0.0, 0.0, -physical_pixel_size(window), -physical_pixel_size(window)));
                        mode   = QSGGeometry::DrawLines;
                        break;
                    case static_cast<int>(IndicatorStyle::FullBox):
                        points = make_filled_rect_points(rect);
                        mode   = QSGGeometry::DrawTriangleFan;
                        break;
                    case static_cast<int>(IndicatorStyle::Gradient):
                        points = make_dashed_points(rect, rect.top() + 1.0, std::max<qreal>(2.0, rect.width() / 3.0), std::max<qreal>(1.0, rect.width() / 6.0));
                        break;
                    case static_cast<int>(IndicatorStyle::GradientCentre):
                        points = make_dashed_points(rect, rect.center().y(), std::max<qreal>(2.0, rect.width() / 3.0), std::max<qreal>(1.0, rect.width() / 6.0));
                        break;
                    case static_cast<int>(IndicatorStyle::DotBox):
                        points = make_dotted_box_points(rect);
                        break;
                    case static_cast<int>(IndicatorStyle::Dash):
                        points = make_dashed_points(rect, rect.center().y(), std::max<qreal>(2.0, rect.width() / 8.0), std::max<qreal>(2.0, rect.width() / 10.0));
                        break;
                    case static_cast<int>(IndicatorStyle::Dots):
                        points = make_dashed_points(rect, rect.center().y(), std::max<qreal>(1.0, rect.width() / 14.0), std::max<qreal>(1.0, rect.width() / 14.0));
                        break;
                    case static_cast<int>(IndicatorStyle::CompositionThick): {
                        const QRectF strip(
                            rect.left() + 1.0,
                            std::max(rect.top(), rect.bottom() - 3.0),
                            std::max<qreal>(1.0, rect.width()  - 2.0),
                            2.0);
                        points = make_filled_rect_points(strip);
                        mode   = QSGGeometry::DrawTriangleFan;
                        break;
                    }
                    case static_cast<int>(IndicatorStyle::CompositionThin): {
                        const qreal y = rect.bottom() - 1.0;
                        points        = make_line_points(rect, y);
                        break;
                    }
                    case static_cast<int>(IndicatorStyle::Point):
                    case static_cast<int>(IndicatorStyle::PointCharacter):
                        points = make_triangle_points(
                            QPointF(rect.left() + rect.width()  * 0.2, rect.bottom() - 1.0),
                            QPointF(rect.right() - rect.width() * 0.2, rect.bottom() - 1.0),
                            QPointF(rect.center().x(),                 rect.top()    + 1.0));
                        mode = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(IndicatorStyle::PointTop):
                        points = make_triangle_points(
                            QPointF(rect.left()  + rect.width() * 0.2, rect.top()    + 1.0),
                            QPointF(rect.right() - rect.width() * 0.2, rect.top()    + 1.0),
                            QPointF(rect.center().x(),                 rect.bottom() - 1.0));
                        mode = QSGGeometry::DrawTriangles;
                        break;
                    default:
                        points = make_line_points(rect, rect.bottom() - 1.0);
                        break;
                }

                update_geometry_node(window, m_indicator_over_group, node, points, mode, primitive.color);
            });

        sync_geometry_nodes(
            window,
            m_marker_group,
            m_marker_connector_nodes,
            static_cast<qsizetype>(frame.marker_primitives.size()),
            [&](QSGGeometryNode *&node, size_t i) {
                const marker_primitive &primitive = frame.marker_primitives[i];
                const std::vector<QPointF> points =
                    (is_software_backend(window) && is_fold_marker_symbol(primitive.marker_type))
                        ? std::vector<QPointF>{}
                        : make_marker_connector_points(primitive);
                update_geometry_node(
                    window,
                    m_marker_group,
                    node,
                    points,
                    QSGGeometry::DrawLines,
                    fold_connector_color(primitive));
            });

        std::vector<colored_rect> marker_raster_rects;
        if (is_software_backend(window)) {
            for (const marker_primitive &primitive : frame.marker_primitives) {
                if (append_rasterized_fold_marker_rects(marker_raster_rects, primitive, window)) {
                    continue;
                }
                if (primitive.marker_type == static_cast<int>(MarkerSymbol::Circle)) {
                    append_rasterized_circle_marker_rects(marker_raster_rects, primitive, window);
                }
            }
        }
        sync_rectangle_nodes(
            window,
            m_marker_group,
            m_marker_raster_nodes,
            static_cast<qsizetype>(marker_raster_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(marker_raster_rects[i].rect);
                node->setColor(marker_raster_rects[i].color);
            });

        // Marker geometry from frame metadata.
        sync_geometry_nodes(
            window,
            m_marker_group,
            m_marker_nodes,
            static_cast<qsizetype>(frame.marker_primitives.size()),
            [&](QSGGeometryNode *&node, size_t i) {
                const marker_primitive &primitive = frame.marker_primitives[i];
                if (is_software_backend(window) && is_fold_marker_symbol(primitive.marker_type)) {
                    update_geometry_node(
                        window,
                        m_marker_group,
                        node,
                        {},
                        QSGGeometry::DrawLines,
                        QColor(0, 0, 0, 0));
                    return;
                }
                const QRectF rect = primitive.rect;
                std::vector<QPointF> points;
                QSGGeometry::DrawingMode mode = QSGGeometry::DrawTriangleFan;

                switch (primitive.marker_type) {
                    case static_cast<int>(MarkerSymbol::Empty):
                    case static_cast<int>(MarkerSymbol::Background):
                    case static_cast<int>(MarkerSymbol::Underline):
                    case static_cast<int>(MarkerSymbol::Available):
                        break;
                    case static_cast<int>(MarkerSymbol::Circle):
                        if (!is_software_backend(window)) {
                            points = make_circle_fill_triangles(make_scintilla_circle_marker_rect(rect));
                            mode   = QSGGeometry::DrawTriangles;
                        }
                        break;
                    case static_cast<int>(MarkerSymbol::RoundRect):
                        points = make_rounded_rect_outline_points(rect.adjusted(1.0, 1.0, -1.0, -1.0));
                        mode   = QSGGeometry::DrawLineStrip;
                        break;
                    case static_cast<int>(MarkerSymbol::Arrow):
                        points = make_arrow_points(rect, false);
                        mode   = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(MarkerSymbol::ArrowDown):
                        points = make_arrow_points(rect, true);
                        mode   = QSGGeometry::DrawTriangles;
                        break;
                    case static_cast<int>(MarkerSymbol::SmallRect):
                        points = make_filled_rect_points(rect.adjusted(1.0, 2.0, -1.0, -2.0));
                        break;
                    case static_cast<int>(MarkerSymbol::Minus):
                        points = make_line_points(rect, rect.center().y());
                        mode   = QSGGeometry::DrawLines;
                        break;
                    case static_cast<int>(MarkerSymbol::Plus):
                        points = make_plus_points(rect);
                        mode   = QSGGeometry::DrawLines;
                        break;
                    case static_cast<int>(MarkerSymbol::DotDotDot):
                        points = make_dotdotdot_points(rect);
                        mode   = QSGGeometry::DrawLines;
                        break;
                    case static_cast<int>(MarkerSymbol::Arrows): {
                        const qreal center_y = rect.center().y();
                        const qreal step     = std::max<qreal>(3.0, rect.width() / 4.0);
                        const qreal left     = rect.left() + rect.width() * 0.15;
                        for (int arrow = 0; arrow < 3; ++arrow) {
                            const qreal offset = left + static_cast<qreal>(arrow) * step;
                            points.emplace_back(offset, center_y);
                            points.emplace_back(offset + step * 0.55, center_y - step * 0.35);
                            points.emplace_back(offset, center_y);
                            points.emplace_back(offset + step * 0.55, center_y + step * 0.35);
                        }
                        mode = QSGGeometry::DrawLines;
                        break;
                    }
                    case static_cast<int>(MarkerSymbol::ShortArrow):
                        points = make_short_arrow_points(rect);
                        mode   = QSGGeometry::DrawLineStrip;
                        break;
                    case static_cast<int>(MarkerSymbol::FullRect):
                        points = make_filled_rect_points(rect);
                        break;
                    case static_cast<int>(MarkerSymbol::LeftRect): {
                        const QRectF leftRect(rect.left(), rect.top(), std::max<qreal>(2.0, rect.width() / 3.0), rect.height());
                        points = make_filled_rect_points(leftRect);
                        break;
                    }
                    case static_cast<int>(MarkerSymbol::VLine):
                        break;
                    case static_cast<int>(MarkerSymbol::LCorner):
                    case static_cast<int>(MarkerSymbol::LCornerCurve): {
                        break;
                    }
                    case static_cast<int>(MarkerSymbol::TCorner):
                    case static_cast<int>(MarkerSymbol::TCornerCurve): {
                        break;
                    }
                    case static_cast<int>(MarkerSymbol::BoxPlus):
                    case static_cast<int>(MarkerSymbol::CirclePlus): {
                        // Collapsed fold head (not connected): box/circle with +
                        const qreal inset    = std::max<qreal>(2.0, rect.width() / 4.0);
                        const QRectF inner   = rect.adjusted(inset, inset, -inset, -inset);
                        const bool is_circle = (primitive.marker_type == static_cast<int>(MarkerSymbol::CirclePlus));
                        if (is_circle) {
                            points = make_circle_outline_as_lines(inner, 16);
                        }
                        else {
                            points = make_rect_outline_as_lines(inner);
                        }
                        // + cross: horizontal and vertical through center
                        const qreal cx  = inner.center().x();
                        const qreal cy  = inner.center().y();
                        const qreal arm = std::max<qreal>(1.0, inner.width() / 4.0);
                        points.insert(points.end(), {
                            QPointF(cx - arm, cy), QPointF(cx + arm, cy),
                            QPointF(cx, cy - arm), QPointF(cx, cy + arm),
                        });
                        mode = QSGGeometry::DrawLines;
                        break;
                    }
                    case static_cast<int>(MarkerSymbol::BoxMinus):
                    case static_cast<int>(MarkerSymbol::CircleMinus): {
                        // Expanded fold head (not connected): box/circle with -
                        const qreal inset    = std::max<qreal>(2.0, rect.width() / 4.0);
                        const QRectF inner   = rect.adjusted(inset, inset, -inset, -inset);
                        const bool is_circle = (primitive.marker_type == static_cast<int>(MarkerSymbol::CircleMinus));
                        if (is_circle) {
                            points = make_circle_outline_as_lines(inner, 16);
                        }
                        else {
                            points = make_rect_outline_as_lines(inner);
                        }
                        // - horizontal through center
                        const qreal cx  = inner.center().x();
                        const qreal cy  = inner.center().y();
                        const qreal arm = std::max<qreal>(1.0, inner.width() / 4.0);
                        points.insert(points.end(), {
                            QPointF(cx - arm, cy), QPointF(cx + arm, cy),
                        });
                        mode = QSGGeometry::DrawLines;
                        break;
                    }
                    case static_cast<int>(MarkerSymbol::BoxPlusConnected):
                    case static_cast<int>(MarkerSymbol::CirclePlusConnected): {
                        const qreal inset    = std::max<qreal>(2.0, rect.width() / 4.0);
                        const QRectF inner   = rect.adjusted(inset, inset, -inset, -inset);
                        const bool is_circle = (primitive.marker_type == static_cast<int>(MarkerSymbol::CirclePlusConnected));
                        const auto symbol_lines = is_circle
                            ? make_circle_outline_as_lines(inner, 16)
                            : make_rect_outline_as_lines(inner);
                        points.insert(points.end(), symbol_lines.begin(), symbol_lines.end());
                        // + cross
                        const qreal cx  = inner.center().x();
                        const qreal cy  = inner.center().y();
                        const qreal arm = std::max<qreal>(1.0, inner.width() / 4.0);
                        points.insert(points.end(), {
                            QPointF(cx - arm, cy), QPointF(cx + arm, cy),
                            QPointF(cx, cy - arm), QPointF(cx, cy + arm),
                        });
                        mode = QSGGeometry::DrawLines;
                        break;
                    }
                    case static_cast<int>(MarkerSymbol::BoxMinusConnected):
                    case static_cast<int>(MarkerSymbol::CircleMinusConnected): {
                        const qreal inset    = std::max<qreal>(2.0, rect.width() / 4.0);
                        const QRectF inner   = rect.adjusted(inset, inset, -inset, -inset);
                        const bool is_circle = (primitive.marker_type == static_cast<int>(MarkerSymbol::CircleMinusConnected));
                        const auto symbol_lines = is_circle
                            ? make_circle_outline_as_lines(inner, 16)
                            : make_rect_outline_as_lines(inner);
                        points.insert(points.end(), symbol_lines.begin(), symbol_lines.end());
                        // - horizontal
                        const qreal cx  = inner.center().x();
                        const qreal cy  = inner.center().y();
                        const qreal arm = std::max<qreal>(1.0, inner.width() / 4.0);
                        points.insert(points.end(), {
                            QPointF(cx - arm, cy), QPointF(cx + arm, cy),
                        });
                        mode = QSGGeometry::DrawLines;
                        break;
                    }
                    case static_cast<int>(MarkerSymbol::Bar):
                        points = make_marker_bar_points(rect);
                        break;
                    case static_cast<int>(MarkerSymbol::Bookmark):
                        points = make_bookmark_points(rect, false);
                        mode   = QSGGeometry::DrawLineStrip;
                        break;
                    case static_cast<int>(MarkerSymbol::VerticalBookmark):
                        points = make_bookmark_points(rect, true);
                        mode   = QSGGeometry::DrawLineStrip;
                        break;
                    default:
                        if (primitive.marker_type >= static_cast<int>(MarkerSymbol::Character)) {
                            points = make_diamond_points(rect);
                            mode   = QSGGeometry::DrawLineStrip;
                        }
                        else {
                            points = make_filled_rect_points(rect);
                        }
                        break;
                }

                const QColor color =
                    (mode == QSGGeometry::DrawLineStrip || mode == QSGGeometry::DrawLines)
                        ? marker_stroke_color(primitive)
                        : marker_fill_color(primitive);
                update_geometry_node(window, m_marker_group, node, points, mode, color);
            });

        sync_geometry_nodes(
            window,
            m_marker_group,
            m_marker_outline_nodes,
            static_cast<qsizetype>(frame.marker_primitives.size()),
            [&](QSGGeometryNode *&node, size_t i) {
                const marker_primitive &primitive = frame.marker_primitives[i];
                std::vector<QPointF> points;

                if (!is_software_backend(window) &&
                    primitive.marker_type == static_cast<int>(MarkerSymbol::Circle)) {
                    points = make_circle_outline_as_lines(make_scintilla_circle_marker_rect(primitive.rect), 24);
                }

                update_geometry_node(
                    window,
                    m_marker_group,
                    node,
                    points,
                    QSGGeometry::DrawLines,
                    marker_stroke_color(primitive));
            });

        // Fold display text: backgrounds first (behind), then text on top
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_fold_display_bg_nodes,
            static_cast<qsizetype>(frame.fold_display_texts.size()),
            [&](QSGRectangleNode *node, size_t i) {
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
            [&](Scene_graph_frame_text_node *node, size_t i) {
                const fold_display_text_primitive &fold = frame.fold_display_texts[i];
                margin_text_primitive as_margin;
                as_margin.text          = fold.text;
                as_margin.position      = fold.position;
                as_margin.baseline_y    = fold.baseline_y;
                as_margin.foreground    = fold.foreground;
                as_margin.font          = fold.font;
                as_margin.clip_rect     = fold.rect;
                as_margin.document_line = fold.document_line;
                as_margin.style_id      = fold.style_id;
                node->update_from_margin_text(window, as_margin, frame.text_rect);
            });

        // Fold display text: boxed outlines (index-based to avoid copies)
        std::vector<size_t> boxed_fold_idx;
        for (size_t j = 0; j < frame.fold_display_texts.size(); ++j) {
            if (frame.fold_display_texts[j].boxed) {
                boxed_fold_idx.push_back(j);
            }
        }
        std::vector<colored_rect> fold_display_box_rects;
        for (size_t i = 0; i < boxed_fold_idx.size(); ++i) {
            const auto &fold = frame.fold_display_texts[boxed_fold_idx[i]];
            append_outline_pixel_rects(
                fold_display_box_rects,
                fold.rect.adjusted(0.0, 0.0, -physical_pixel_size(window), 0.0),
                fold.foreground,
                window);
        }
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_fold_display_box_nodes,
            static_cast<qsizetype>(fold_display_box_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(fold_display_box_rects[i].rect);
                node->setColor(fold_display_box_rects[i].color);
            });

        // EOL annotations: backgrounds first, then text
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_eol_annotation_bg_nodes,
            static_cast<qsizetype>(frame.eol_annotations.size()),
            [&](QSGRectangleNode *node, size_t i) {
                QRectF rect = frame.eol_annotations[i].rect;
                if (frame.eol_annotations[i].visible_style ==
                    static_cast<int>(EOLAnnotationVisible::Boxed)) {
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
            [&](Scene_graph_frame_text_node *node, size_t i) {
                const eol_annotation_primitive &eol = frame.eol_annotations[i];
                margin_text_primitive as_margin;
                as_margin.text          = eol.text;
                as_margin.position      = eol.position;
                as_margin.baseline_y    = eol.baseline_y;
                as_margin.foreground    = eol.foreground;
                as_margin.font          = eol.font;
                as_margin.clip_rect     = eol.rect;
                as_margin.document_line = eol.document_line;
                as_margin.style_id      = eol.style_id;
                node->update_from_margin_text(window, as_margin, frame.text_rect);
            });

        // EOL annotation boxed outlines (index-based)
        std::vector<size_t> boxed_eol_idx;
        for (size_t j = 0; j < frame.eol_annotations.size(); ++j) {
            if (frame.eol_annotations[j].visible_style == static_cast<int>(EOLAnnotationVisible::Boxed)) {
                boxed_eol_idx.push_back(j);
            }
        }
        std::vector<colored_rect> eol_annotation_box_rects;
        for (size_t i = 0; i < boxed_eol_idx.size(); ++i) {
            const auto &eol = frame.eol_annotations[boxed_eol_idx[i]];
            append_outline_pixel_rects(
                eol_annotation_box_rects,
                eol.rect.adjusted(0.0, 0.0, -physical_pixel_size(window), 0.0),
                eol.foreground,
                window);
        }
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_eol_annotation_box_nodes,
            static_cast<qsizetype>(eol_annotation_box_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(eol_annotation_box_rects[i].rect);
                node->setColor(eol_annotation_box_rects[i].color);
            });

        // Annotations: backgrounds first, then text
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_annotation_bg_nodes,
            static_cast<qsizetype>(frame.annotations.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(frame.annotations[i].rect);
                node->setColor(frame.annotations[i].background);
            });

        sync_frame_text_nodes(
            window,
            m_annotation_text_group,
            m_annotation_nodes,
            static_cast<qsizetype>(frame.annotations.size()),
            [&](Scene_graph_frame_text_node *node, size_t i) {
                const annotation_primitive &annot = frame.annotations[i];
                margin_text_primitive as_margin;
                as_margin.text          = annot.text;
                as_margin.position      = annot.position;
                as_margin.baseline_y    = annot.baseline_y;
                as_margin.foreground    = annot.foreground;
                as_margin.font          = annot.font;
                as_margin.clip_rect     = annot.rect;
                as_margin.document_line = annot.document_line;
                as_margin.style_id      = annot.style_id;
                node->update_from_margin_text(window, as_margin, frame.text_rect);
            });

        // Annotation boxed outlines (index-based)
        std::vector<size_t> boxed_annot_idx;
        for (size_t j = 0; j < frame.annotations.size(); ++j) {
            if (frame.annotations[j].boxed) {
                boxed_annot_idx.push_back(j);
            }
        }
        std::vector<colored_rect> annotation_box_rects;
        for (size_t i = 0; i < boxed_annot_idx.size(); ++i) {
            const auto &annot = frame.annotations[boxed_annot_idx[i]];
            append_outline_pixel_rects(annotation_box_rects, annot.rect, annot.foreground, window);
        }
        sync_rectangle_nodes(
            window,
            m_annotation_background_group,
            m_annotation_box_nodes,
            static_cast<qsizetype>(annotation_box_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(annotation_box_rects[i].rect);
                node->setColor(annotation_box_rects[i].color);
            });

        // Whitespace marks (index-based split: dots vs tabs)
        std::vector<size_t> ws_dot_idx, ws_tab_idx;
        ws_dot_idx.reserve(frame.whitespace_marks.size());
        ws_tab_idx.reserve(frame.whitespace_marks.size());
        for (size_t j = 0; j < frame.whitespace_marks.size(); ++j) {
            if (frame.whitespace_marks[j].kind == whitespace_mark_kind::space_dot) {
                ws_dot_idx.push_back(j);
            }
            else {
                ws_tab_idx.push_back(j);
            }
        }

        sync_rectangle_nodes(
            window,
            m_whitespace_group,
            m_whitespace_dot_nodes,
            static_cast<qsizetype>(ws_dot_idx.size()),
            [&](QSGRectangleNode *node, size_t i) {
                const auto &ws = frame.whitespace_marks[ws_dot_idx[i]];
                node->setRect(ws.rect);
                node->setColor(ws.color);
            });

        std::vector<colored_rect> whitespace_tab_rects;
        for (size_t i = 0; i < ws_tab_idx.size(); ++i) {
            append_rasterized_tab_arrow_rects(
                whitespace_tab_rects,
                frame.whitespace_marks[ws_tab_idx[i]],
                window);
        }
        sync_rectangle_nodes(
            window,
            m_whitespace_group,
            m_whitespace_tab_nodes,
            static_cast<qsizetype>(whitespace_tab_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(whitespace_tab_rects[i].rect);
                node->setColor(whitespace_tab_rects[i].color);
            });

        // Decoration underlines (hotspot + style underlines as rectangles)
        std::vector<colored_rect> decoration_underline_rects;
        for (const decoration_underline_primitive &underline : frame.decoration_underlines) {
            append_horizontal_pixel_rects(
                decoration_underline_rects,
                underline.rect,
                underline.color,
                window);
        }

        sync_rectangle_nodes(
            window,
            m_decoration_group,
            m_decoration_underline_nodes,
            static_cast<qsizetype>(decoration_underline_rects.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(decoration_underline_rects[i].rect);
                node->setColor(decoration_underline_rects[i].color);
            });

        // Indent guides as subtle semi-transparent dots, closer to Notepad++.
        struct Indent_guide_dot {
            QRectF rect;
            QColor color;
        };
        std::vector<Indent_guide_dot> indent_guide_dots;
        const qreal dpr            = std::max<qreal>(1.0, window->effectiveDevicePixelRatio());
        const qreal physical_pixel = 1.0 / dpr;
        const qreal dot_size       = physical_pixel;
        const qreal dot_step       = physical_pixel * 4.0;
        const auto snap_to_device_pixel = [dpr](qreal value) {
            return std::round(value * dpr) / dpr;
        };
        for (const indent_guide_primitive &guide : frame.indent_guides) {
            QColor color = guide.color.isValid() ? guide.color
                : (guide.highlight ? QColor(192, 192, 192) : QColor(128, 128, 128));
            const int target_alpha = guide.highlight ? 80 : 42;
            color.setAlpha(std::min(color.alpha() > 0 ? color.alpha() : 255, target_alpha));

            const qreal dot_x = snap_to_device_pixel(guide.x);
            for (qreal y = snap_to_device_pixel(guide.top + physical_pixel); y < guide.bottom; y += dot_step) {
                indent_guide_dots.push_back({
                    QRectF(dot_x, snap_to_device_pixel(y), dot_size, dot_size),
                    color,
                });
            }
        }

        sync_rectangle_nodes(
            window,
            m_indent_guide_group,
            m_indent_guide_nodes,
            static_cast<qsizetype>(indent_guide_dots.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(indent_guide_dots[i].rect);
                node->setColor(indent_guide_dots[i].color);
            });

        // Caret rectangles from frame
        sync_rectangle_nodes(
            window,
            m_overlay_group,
            m_caret_nodes,
            static_cast<qsizetype>(frame.caret_primitives.size()),
            [&](QSGRectangleNode *node, size_t i) {
                node->setRect(frame.caret_primitives[i].rect);
                node->setColor(frame.caret_primitives[i].color);
            });
    }

private:
    QSGNode *m_background_group            = nullptr;
    QSGNode *m_current_line_group          = nullptr;
    QSGNode *m_selection_group             = nullptr;
    QSGNode *m_gutter_group                = nullptr;
    QSGNode *m_marker_group                = nullptr;
    QSGNode *m_indicator_under_group       = nullptr;
    QSGClipNode *m_text_clip_node          = nullptr;
    QSGNode *m_representation_group        = nullptr;
    QSGNode *m_representation_text_group   = nullptr;
    QSGNode *m_text_group                  = nullptr;
    QSGNode *m_annotation_background_group = nullptr;
    QSGNode *m_annotation_text_group       = nullptr;
    QSGNode *m_whitespace_group            = nullptr;
    QSGNode *m_decoration_group            = nullptr;
    QSGNode *m_indent_guide_group          = nullptr;
    QSGNode *m_indicator_over_group        = nullptr;
    QSGNode *m_overlay_group               = nullptr;

    QSGRectangleNode *m_background_node = nullptr;
    std::vector<QSGRectangleNode *> m_gutter_background_nodes;

    std::vector<QSGRectangleNode *> m_current_line_fill_nodes;
    std::vector<QSGRectangleNode *> m_current_line_frame_nodes;
    std::vector<QSGRectangleNode *> m_selection_nodes;
    std::vector<QSGRectangleNode *> m_representation_blob_nodes;
    std::vector<Scene_graph_frame_text_node *> m_representation_text_nodes;
    std::vector<Scene_graph_frame_text_node *> m_text_nodes;
    std::vector<Scene_graph_frame_text_node *> m_gutter_nodes;
    std::vector<QSGRectangleNode *> m_indicator_under_fill_nodes;
    std::vector<QSGRectangleNode *> m_indicator_over_fill_nodes;
    std::vector<QSGRectangleNode *> m_indicator_under_stroke_nodes;
    std::vector<QSGRectangleNode *> m_indicator_over_stroke_nodes;
    std::vector<QSGGeometryNode  *> m_marker_connector_nodes;
    std::vector<QSGGeometryNode  *> m_marker_nodes;
    std::vector<QSGGeometryNode  *> m_marker_outline_nodes;
    std::vector<QSGRectangleNode *> m_marker_raster_nodes;
    std::vector<QSGGeometryNode  *> m_indicator_under_nodes;
    std::vector<QSGGeometryNode  *> m_indicator_over_nodes;
    std::vector<QSGRectangleNode *> m_caret_nodes;

    std::vector<Scene_graph_frame_text_node *> m_fold_display_text_nodes;
    std::vector<QSGRectangleNode *> m_fold_display_bg_nodes;
    std::vector<QSGRectangleNode *> m_fold_display_box_nodes;
    std::vector<Scene_graph_frame_text_node *> m_eol_annotation_nodes;
    std::vector<QSGRectangleNode *> m_eol_annotation_bg_nodes;
    std::vector<QSGRectangleNode *> m_eol_annotation_box_nodes;
    std::vector<Scene_graph_frame_text_node *> m_annotation_nodes;
    std::vector<QSGRectangleNode *> m_annotation_bg_nodes;
    std::vector<QSGRectangleNode *> m_annotation_box_nodes;
    std::vector<QSGRectangleNode *> m_whitespace_dot_nodes;
    std::vector<QSGRectangleNode *> m_whitespace_tab_nodes;
    std::vector<QSGRectangleNode *> m_decoration_underline_nodes;
    std::vector<QSGRectangleNode *> m_indent_guide_nodes;
};

}

QSGNode *scene_graph_renderer::update(
    QQuickWindow *window,
    QSGNode *old_node,
    const render_snapshot &snapshot,
    const render_frame &frame)
{
    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("renderer.update");

    if (!window) {
        delete old_node;
        return nullptr;
    }

    auto *root = dynamic_cast<Scene_graph_root_node *>(old_node);
    if (!root) {
        delete old_node;
        root = new Scene_graph_root_node();
    }

    root->update_from_frame(window, snapshot, frame);
    return root;
}

}
