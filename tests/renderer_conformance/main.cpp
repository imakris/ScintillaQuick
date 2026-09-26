// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "render_frame.h"
#include "scintillaquick_scene_graph_renderer.h"
#include "scintillaquick_platqt.h"

#include <scintillaquick/scintillaquick_item.h>
#include <Scintilla.h>
#include <Indicator.h>
#include <LineMarker.h>
#include <XPM.h>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QPointF>
#include <QPixmap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

namespace sci = Scintilla::Internal;
using Scintilla::IndicatorStyle;
using Scintilla::MarkerSymbol;

namespace
{

constexpr int k_width = 320;
constexpr int k_height = 128;
constexpr int k_channel_tolerance = 3;

class Frame_item final : public QQuickItem
{
public:
    explicit Frame_item(QQuickItem* parent)

    :
        QQuickItem(parent)
    {
        setFlag(ItemHasContents, true);
        setSize(QSizeF(k_width, k_height));
        m_snapshot.item_size = size();
        m_snapshot.background = Qt::white;
    }

    void set_frame(sci::Render_frame frame)
    {
        m_frame = std::move(frame);
        ++m_generation;
        update();
    }

    bool frame_rendered() const
    {
        return m_rendered_generation.load() == m_generation;
    }

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override
    {
        QSGNode* node = m_renderer.update(window(), old_node, m_snapshot, m_frame);
        m_rendered_generation.store(m_generation);
        return node;
    }

private:
    sci::Scene_graph_renderer m_renderer;
    sci::Render_snapshot m_snapshot;
    sci::Render_frame m_frame;
    unsigned int m_generation = 0;
    std::atomic_uint m_rendered_generation = 0;
};

sci::Render_frame empty_frame()
{
    sci::Render_frame frame;
    frame.text_rect = QRectF(64.0, 0.0, k_width - 64.0, k_height);
    frame.margin_rect = QRectF(0.0, 0.0, 64.0, k_height);
    return frame;
}

sci::ColourRGBA scintilla_color(const QColor& color)
{
    return sci::ColourRGBA(color.red(), color.green(), color.blue(), color.alpha());
}

class Conformance_runner
{
public:
    explicit Conformance_runner(QString backend, qreal requested_dpr,
        QPointF item_position = {}, qreal item_opacity = 1.0)

    :
        m_item(m_window.contentItem()),
        m_backend(std::move(backend)),
        m_requested_dpr(requested_dpr),
        m_item_position(item_position),
        m_item_opacity(item_opacity)
    {
        m_item.setPosition(item_position);
        m_item.setOpacity(item_opacity);
        m_window.setColor(Qt::white);
        m_window.resize(k_width, k_height);
        m_window.show();
    }

    bool run(
        const char* name,
        sci::Render_frame frame,
        const std::function<void(sci::Surface_impl&, QPainter&)>& draw_reference)
    {
        m_item.set_frame(std::move(frame));
        QElapsedTimer timer;
        timer.start();
        while ((!m_window.isExposed() || !m_item.frame_rendered()) && timer.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        if (!m_window.isExposed() || !m_item.frame_rendered()) {
            std::fprintf(stderr, "FAIL %s: window did not render the requested frame\n", name);
            return false;
        }

        const qreal dpr = m_window.effectiveDevicePixelRatio();
        const auto api = m_window.rendererInterface()->graphicsApi();
        const bool software = api == QSGRendererInterface::Software;
        if (!qFuzzyCompare(dpr, m_requested_dpr) || software != (m_backend == QStringLiteral("software")) ||
            api == QSGRendererInterface::Unknown)
        {
            std::fprintf(stderr, "FAIL %s: requested %s DPR %.2f, received API %d DPR %.2f\n",
                name, qPrintable(m_backend), m_requested_dpr, static_cast<int>(api), dpr);
            return false;
        }

        const QImage actual = m_window.grabWindow().convertToFormat(QImage::Format_ARGB32);
        if (actual.isNull()) {
            std::fprintf(stderr, "FAIL %s: grabWindow returned no image\n", name);
            return false;
        }
        // Match Qt's native raster surface format so text uses the same
        // subpixel antialiasing as the upstream QPainter drawing path.
        QImage expected(QSize(qRound(k_width * dpr), qRound(k_height * dpr)), QImage::Format_ARGB32_Premultiplied);
        expected.setDevicePixelRatio(dpr);
        expected.fill(Qt::white);
        {
            QPainter painter(&expected);
            painter.translate(m_item_position);
            painter.setOpacity(m_item_opacity);
            sci::Surface_impl surface;
            surface.Init(true, &painter);
            draw_reference(surface, painter);
        }
        if (actual.size() != expected.size()) {
            std::fprintf(stderr, "FAIL %s: capture size %dx%d, expected %dx%d\n", name,
                actual.width(), actual.height(), expected.width(), expected.height());
            return false;
        }

        int differences = 0;
        QPoint first_difference;
        for (int y = 0; y < expected.height(); ++y) {
            for (int x = 0; x < expected.width(); ++x) {
                const QRgb a = actual.pixel(x, y);
                const QRgb e = expected.pixel(x, y);
                if (std::abs(qRed(a) - qRed(e)) > k_channel_tolerance ||
                    std::abs(qGreen(a) - qGreen(e)) > k_channel_tolerance ||
                    std::abs(qBlue(a) - qBlue(e)) > k_channel_tolerance)
                {
                    if (differences++ == 0) {
                        first_difference = QPoint(x, y);
                    }
                }
            }
        }
        if (differences != 0) {
            const QString directory = QStringLiteral("renderer_conformance_artifacts/%1_%2")
                .arg(m_backend).arg(dpr);
            QDir().mkpath(directory);
            actual.save(directory + QLatin1Char('/') + QString::fromUtf8(name) + QStringLiteral("_actual.png"));
            expected.save(directory + QLatin1Char('/') + QString::fromUtf8(name) + QStringLiteral("_expected.png"));
            std::fprintf(stderr, "FAIL %s: %d differing pixels; first at (%d, %d)\n",
                name, differences, first_difference.x(), first_difference.y());
            return false;
        }
        std::printf("PASS %s (%s, DPR %.2f)\n", name, qPrintable(m_backend), dpr);
        return true;
    }

private:
    QQuickWindow m_window;
    Frame_item m_item;
    QString m_backend;
    qreal m_requested_dpr;
    QPointF m_item_position;
    qreal m_item_opacity;
};

bool check_indicator(
    Conformance_runner& runner,
    const char* name,
    IndicatorStyle style,
    QPointF translation = {},
    qreal stroke_width = 1.0)
{
    sci::Render_frame frame = empty_frame();
    sci::Indicator_primitive primitive;
    const bool point = style == IndicatorStyle::Point || style == IndicatorStyle::PointCharacter ||
        style == IndicatorStyle::PointTop;
    const qreal left = point ? 96.0 : 40.0;
    primitive.rect = QRectF(left, 32.0, 160.0, 8.0).translated(translation);
    primitive.line_rect = QRectF(left, 12.0, 160.0, 28.0).translated(translation);
    primitive.character_rect = QRectF(left, 12.0, 12.0, 28.0).translated(translation);
    primitive.color = QColor(220, 100, 30);
    primitive.indicator_style = static_cast<int>(style);
    primitive.fill_alpha = 65;
    primitive.outline_alpha = 173;
    primitive.stroke_width = stroke_width;
    frame.indicator_primitives.push_back(primitive);
    const QRectF text_rect = frame.text_rect;
    return runner.run(name, std::move(frame), [primitive, text_rect](sci::Surface_impl&, QPainter& painter) {
        // The outer scene-graph clip is independent of the temporary clips
        // installed by Indicator::Draw. Composite its complete drawing first.
        QImage shape(painter.device()->width(), painter.device()->height(), QImage::Format_ARGB32_Premultiplied);
        shape.setDevicePixelRatio(painter.device()->devicePixelRatioF());
        shape.fill(Qt::transparent);
        {
            QPainter shape_painter(&shape);
            sci::Surface_impl surface;
            surface.Init(true, &shape_painter);
            sci::Indicator indicator(static_cast<IndicatorStyle>(primitive.indicator_style),
                scintilla_color(primitive.color), false, primitive.fill_alpha, primitive.outline_alpha);
            indicator.strokeWidth = primitive.stroke_width;
            indicator.Draw(&surface, sci::PRectFromQRectF(primitive.rect), sci::PRectFromQRectF(primitive.line_rect),
                sci::PRectFromQRectF(primitive.character_rect), sci::Indicator::State::normal, 0);
        }
        painter.setClipRect(text_rect);
        painter.drawImage(QPointF(0.0, 0.0), shape);
    });
}

bool check_marker(
    Conformance_runner& runner,
    const char* name,
    MarkerSymbol symbol,
    Scintilla::MarginType margin_style = Scintilla::MarginType::Symbol,
    qreal stroke_width = 1.0,
    QRectF rect = QRectF(16.0, 16.0, 24.0, 30.0))
{
    sci::Render_frame frame = empty_frame();
    sci::Marker_primitive primitive;
    primitive.rect = rect;
    primitive.marker_type = static_cast<int>(symbol);
    primitive.foreground = QColor(20, 70, 140);
    primitive.background = QColor(90, 180, 100);
    primitive.background_selected = QColor(160, 80, 190);
    primitive.fold_part = static_cast<int>(sci::LineMarker::FoldPart::headWithTail);
    primitive.margin_style = static_cast<int>(margin_style);
    primitive.stroke_width = stroke_width;
    primitive.font = QFont(QStringLiteral("monospace"), 10);
    primitive.font.setStyleHint(QFont::TypeWriter);
    frame.marker_primitives.push_back(primitive);
    return runner.run(name, std::move(frame), [primitive](sci::Surface_impl&, QPainter& painter) {
        // Draw onto the same transparent native-format surface as the shape
        // texture, then composite it. Opaque-target subpixel text blending is
        // not equivalent, even when both paint devices use premultiplied ARGB.
        QImage shape(painter.device()->width(), painter.device()->height(), QImage::Format_ARGB32_Premultiplied);
        shape.setDevicePixelRatio(painter.device()->devicePixelRatioF());
        shape.fill(Qt::transparent);
        {
            QPainter shape_painter(&shape);
            sci::Surface_impl surface;
            surface.Init(true, &shape_painter);
            sci::LineMarker marker;
            marker.markType = static_cast<MarkerSymbol>(primitive.marker_type);
            marker.fore = scintilla_color(primitive.foreground);
            marker.back = scintilla_color(primitive.background);
            marker.backSelected = scintilla_color(primitive.background_selected);
            marker.strokeWidth = primitive.stroke_width;
            const std::shared_ptr<sci::Font> font = sci::font_from_qfont(primitive.font);
            marker.Draw(&surface, sci::PRectFromQRectF(primitive.rect), font.get(),
                static_cast<sci::LineMarker::FoldPart>(primitive.fold_part),
                static_cast<Scintilla::MarginType>(primitive.margin_style));
        }
        painter.drawImage(QPointF(0.0, 0.0), shape);
    });
}

bool check_thin_dot_box(Conformance_runner& runner, const char* name, int width, int height)
{
    sci::Render_frame frame = empty_frame();
    sci::Indicator_primitive primitive;
    primitive.rect = QRectF(96.0, 10.0 + height, width, 3.0);
    primitive.line_rect = QRectF(96.0, 12.0, width, height + 1.0);
    primitive.character_rect = primitive.line_rect;
    primitive.color = QColor(220, 100, 30);
    primitive.indicator_style = static_cast<int>(IndicatorStyle::DotBox);
    primitive.fill_alpha = 65;
    primitive.outline_alpha = 173;
    frame.indicator_primitives.push_back(primitive);
    return runner.run(name, std::move(frame), [primitive, width, height](sci::Surface_impl&, QPainter& painter) {
        // A one-row/column DotBox consists entirely of border pixels. Their
        // alternating alphas define the result independently of Draw's loops.
        QImage dots(width, height, QImage::Format_ARGB32);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                QColor color = primitive.color;
                color.setAlpha((x + y) % 2 ? primitive.outline_alpha : primitive.fill_alpha);
                dots.setPixelColor(x, y, color);
            }
        }
        painter.drawImage(QPointF(96.0, 13.0), dots);
    });
}

bool check_background_alignment(Conformance_runner& runner)
{
    sci::Render_frame frame = empty_frame();
    const QColor color(224, 255, 255);
    // EditView splits a logical line into text and remainder fills at glyph
    // advances, which need not be integral. Its FillRectangleAligned calls
    // must meet without a scene-graph antialiasing seam.
    frame.background_primitives.push_back({false, QRectF(64.0, 0.0, 51.25, 19.0), color});
    frame.background_primitives.push_back({false, QRectF(115.25, 0.0, 100.0, 19.0), color});
    const auto backgrounds = frame.background_primitives;
    return runner.run("captured_background_alignment", std::move(frame),
        [backgrounds](sci::Surface_impl& surface, QPainter&) {
            for (const auto& background : backgrounds) {
                surface.FillRectangleAligned(sci::PRectFromQRectF(background.rect),
                    scintilla_color(background.color));
            }
        });
}

bool check_caret_clip(Conformance_runner& runner)
{
    sci::Render_frame frame = empty_frame();
    frame.caret_primitives.push_back({QRectF(32.0, 16.0, 2.0, 28.0), Qt::black, true});
    frame.caret_primitives.push_back({QRectF(80.0, 16.0, 2.0, 28.0), Qt::black, false});
    const auto carets = frame.caret_primitives;
    const QRectF text_rect = frame.text_rect;
    return runner.run("caret_text_clip", std::move(frame), [carets, text_rect](sci::Surface_impl& surface, QPainter& painter) {
        painter.setClipRect(text_rect);
        for (const auto& caret : carets) {
            surface.FillRectangleAligned(sci::PRectFromQRectF(caret.rect), scintilla_color(caret.color));
        }
    });
}

class Editor_item final : public ScintillaQuick_item
{
public:
    using ScintillaQuick_item::ScintillaQuick_item;

    bool rendered() const { return m_rendered.load(); }

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override
    {
        QSGNode* node = ScintillaQuick_item::updatePaintNode(old_node, data);
        m_rendered.store(true);
        return node;
    }

private:
    std::atomic_bool m_rendered = false;
};

int message_color(const QColor& color)
{
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

bool check_editor_background(
    const char* name,
    const QString& backend,
    qreal dpr,
    const std::function<void(ScintillaQuick_item&)>& configure,
    QColor segment_color,
    QColor remainder_color,
    bool underline = false)
{
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(k_width, k_height);
    Editor_item editor(window.contentItem());
    editor.setSize(QSizeF(k_width, k_height));
    editor.send(SCI_SETCARETPERIOD, 0);
    editor.send(SCI_SETCARETWIDTH, 0);
    editor.send(SCI_SETCARETLINEVISIBLE, 0);
    for (int margin = 0; margin < 5; ++margin) {
        editor.send(SCI_SETMARGINWIDTHN, margin, 0);
        editor.send(SCI_SETMARGINMASKN, margin, 0);
    }
    editor.send(SCI_SETMARGINLEFT, 0, 0);
    editor.send(SCI_SETMARGINRIGHT, 0, 0);
    editor.send(SCI_STYLESETBACK, STYLE_DEFAULT, message_color(Qt::white));
    editor.send(SCI_STYLECLEARALL);
    editor.sends(SCI_SETTEXT, 0, "        \n");
    editor.send(SCI_SETSEL, 0, 0);
    configure(editor);
    window.show();
    QElapsedTimer timer;
    timer.start();
    while ((!window.isExposed() || !editor.rendered()) && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    const QImage actual = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
    if (!editor.rendered() || actual.isNull()) {
        std::fprintf(stderr, "FAIL %s: editor did not render\n", name);
        return false;
    }

    const int line_height = static_cast<int>(editor.send(SCI_TEXTHEIGHT, 0));
    const qreal sample_y = underline ? line_height - 1.0 : line_height / 2.0;
    const std::array<QPointF, 3> sample_points = {
        QPointF(8.0, sample_y), QPointF(k_width - 20.0, sample_y), QPointF(8.0, line_height + 3.0),
    };
    const std::array<QColor, 3> expected_colors = {segment_color, remainder_color, QColor(Qt::white)};
    bool matches = true;
    for (size_t i = 0; i < sample_points.size(); ++i) {
        const QPoint pixel(
            static_cast<int>(std::floor(sample_points[i].x() * dpr)),
            static_cast<int>(std::floor(sample_points[i].y() * dpr)));
        const QColor observed = actual.pixelColor(pixel);
        const QColor expected = expected_colors[i];
        if (std::abs(observed.red() - expected.red()) > k_channel_tolerance ||
            std::abs(observed.green() - expected.green()) > k_channel_tolerance ||
            std::abs(observed.blue() - expected.blue()) > k_channel_tolerance)
        {
            std::fprintf(stderr, "FAIL %s: pixel (%d,%d) is (%d,%d,%d), expected (%d,%d,%d)\n",
                name, pixel.x(), pixel.y(), observed.red(), observed.green(), observed.blue(),
                expected.red(), expected.green(), expected.blue());
            matches = false;
        }
    }
    if (!matches) {
        const QString directory = QStringLiteral("renderer_conformance_artifacts/%1_%2").arg(backend).arg(dpr);
        QDir().mkpath(directory);
        actual.save(directory + QLatin1Char('/') + QString::fromUtf8(name) + QStringLiteral("_actual.png"));
    }
    else {
        std::printf("PASS %s (%s, DPR %.2f)\n", name, qPrintable(backend), dpr);
    }
    return matches;
}

int check_background_capture(const QString& backend, qreal dpr)
{
    int failures = 0;
    const QColor style_background(40, 170, 220);
    for (const bool eol_filled : {false, true}) {
        const char* name = eol_filled ? "style_background_eol_filled" : "style_background_segment";
        failures += !check_editor_background(name, backend, dpr,
            [style_background, eol_filled](ScintillaQuick_item& editor) {
                editor.send(SCI_STYLESETBACK, 1, message_color(style_background));
                editor.send(SCI_STYLESETEOLFILLED, 1, eol_filled);
                editor.send(SCI_STARTSTYLING, 0);
                editor.send(SCI_SETSTYLING, 9, 1);
            }, style_background, eol_filled ? style_background : QColor(Qt::white));
    }

    const QColor marker_background(180, 50, 90);
    for (const auto layer : {Scintilla::Layer::Base, Scintilla::Layer::UnderText, Scintilla::Layer::OverText}) {
        // Upstream base-layer backgrounds are opaque (ViewStyle::Background /
        // DrawMarkUnderline); the other layers use LineMarker::BackWithAlpha.
        QImage swatch(1, 1, QImage::Format_ARGB32);
        swatch.fill(Qt::white);
        {
            QPainter painter(&swatch);
            QColor color = marker_background;
            color.setAlpha(layer == Scintilla::Layer::Base ? 255 : 96);
            painter.fillRect(swatch.rect(), color);
        }
        const QColor expected = swatch.pixelColor(0, 0);
        for (const auto symbol : {MarkerSymbol::Background, MarkerSymbol::Underline, MarkerSymbol::Arrow}) {
            const QByteArray name = QStringLiteral("body_marker_%1_layer_%2")
                .arg(static_cast<int>(symbol)).arg(static_cast<int>(layer)).toUtf8();
            failures += !check_editor_background(name.constData(), backend, dpr,
                [marker_background, symbol, layer](ScintillaQuick_item& editor) {
                    constexpr int marker = 7;
                    editor.send(SCI_SETMARGINMASKN, 0, 1 << marker);
                    editor.send(SCI_MARKERDEFINE, marker, static_cast<int>(symbol));
                    editor.send(SCI_MARKERSETBACK, marker, message_color(marker_background));
                    editor.send(SCI_MARKERSETALPHA, marker, 96);
                    editor.send(SCI_MARKERSETLAYER, marker, static_cast<int>(layer));
                    editor.send(SCI_MARKERADD, 0, marker);
                }, expected, expected, symbol == MarkerSymbol::Underline);
        }
    }

    const QColor selection_background(40, 90, 210);
    const auto select_spaces = [selection_background](ScintillaQuick_item& editor, Scintilla::Layer layer) {
        editor.send(SCI_SETSELECTIONLAYER, static_cast<int>(layer));
        for (const int element : {SC_ELEMENT_SELECTION_BACK, SC_ELEMENT_SELECTION_INACTIVE_BACK}) {
            editor.send(SCI_SETELEMENTCOLOUR, element, scintilla_color(selection_background).AsInteger());
        }
        editor.send(SCI_SETSEL, 0, 8);
    };
    failures += !check_editor_background("base_underline_over_base_selection", backend, dpr,
        [marker_background, select_spaces](ScintillaQuick_item& editor) {
            editor.send(SCI_SETMARGINMASKN, 0, 1 << 7);
            editor.send(SCI_MARKERDEFINE, 7, SC_MARK_UNDERLINE);
            editor.send(SCI_MARKERSETBACK, 7, message_color(marker_background));
            editor.send(SCI_MARKERADD, 0, 7);
            select_spaces(editor, Scintilla::Layer::Base);
        }, marker_background, marker_background, true);

    failures += !check_editor_background("base_underline_over_under_indicator", backend, dpr,
        [marker_background](ScintillaQuick_item& editor) {
            editor.send(SCI_SETMARGINMASKN, 0, 1 << 7);
            editor.send(SCI_MARKERDEFINE, 7, SC_MARK_UNDERLINE);
            editor.send(SCI_MARKERSETBACK, 7, message_color(marker_background));
            editor.send(SCI_MARKERADD, 0, 7);
            editor.send(SCI_INDICSETSTYLE, 12, INDIC_FULLBOX);
            editor.send(SCI_INDICSETFORE, 12, message_color(QColor(70, 180, 100)));
            editor.send(SCI_INDICSETALPHA, 12, 255);
            editor.send(SCI_INDICSETOUTLINEALPHA, 12, 255);
            editor.send(SCI_INDICSETUNDER, 12, 1);
            editor.send(SCI_SETINDICATORCURRENT, 12);
            editor.send(SCI_INDICATORFILLRANGE, 0, 8);
        }, marker_background, marker_background, true);

    failures += !check_editor_background("over_selection_over_under_marker", backend, dpr,
        [marker_background, select_spaces](ScintillaQuick_item& editor) {
            editor.send(SCI_SETMARGINMASKN, 0, 1 << 7);
            editor.send(SCI_MARKERDEFINE, 7, SC_MARK_BACKGROUND);
            editor.send(SCI_MARKERSETBACK, 7, message_color(marker_background));
            editor.send(SCI_MARKERSETALPHA, 7, 255);
            editor.send(SCI_MARKERSETLAYER, 7, SC_LAYER_UNDER_TEXT);
            editor.send(SCI_MARKERADD, 0, 7);
            select_spaces(editor, Scintilla::Layer::OverText);
        }, selection_background, marker_background);

    failures += !check_editor_background("over_selection_over_indicator", backend, dpr,
        [select_spaces](ScintillaQuick_item& editor) {
            editor.send(SCI_INDICSETSTYLE, 12, INDIC_FULLBOX);
            editor.send(SCI_INDICSETFORE, 12, message_color(QColor(70, 180, 100)));
            editor.send(SCI_INDICSETALPHA, 12, 255);
            editor.send(SCI_INDICSETOUTLINEALPHA, 12, 255);
            editor.send(SCI_SETINDICATORCURRENT, 12);
            editor.send(SCI_INDICATORFILLRANGE, 0, 8);
            select_spaces(editor, Scintilla::Layer::OverText);
        }, selection_background, Qt::white);
    return failures;
}

bool check_folding_margin(const QString& backend, qreal dpr)
{
    constexpr char name[] = "custom_folding_margin_pattern";
    constexpr int margin_left = 11;
    constexpr int margin_width = 24;
    const QColor base(180, 50, 90);
    const QColor stripes(40, 170, 220);
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(k_width, k_height);
    Editor_item editor(window.contentItem());
    editor.setSize(QSizeF(k_width, k_height));
    editor.send(SCI_SETCARETPERIOD, 0);
    editor.send(SCI_SETCARETWIDTH, 0);
    editor.send(SCI_SETCARETLINEVISIBLE, 0);
    for (int margin = 0; margin < 5; ++margin) {
        editor.send(SCI_SETMARGINWIDTHN, margin, 0);
        editor.send(SCI_SETMARGINMASKN, margin, 0);
    }
    editor.send(SCI_SETMARGINWIDTHN, 0, margin_left);
    editor.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_COLOUR);
    editor.send(SCI_SETMARGINBACKN, 0, message_color(Qt::white));
    editor.send(SCI_SETMARGINWIDTHN, 1, margin_width);
    editor.send(SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
    editor.send(SCI_SETMARGINMASKN, 1, SC_MASK_FOLDERS);
    editor.send(SCI_SETFOLDMARGINCOLOUR, 1, message_color(base));
    editor.send(SCI_SETFOLDMARGINHICOLOUR, 1, message_color(stripes));
    editor.sends(SCI_SETTEXT, 0, "\n");
    window.show();
    QElapsedTimer timer;
    timer.start();
    while ((!window.isExposed() || !editor.rendered()) && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    const QImage actual = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
    if (!editor.rendered() || actual.isNull()) {
        std::fprintf(stderr, "FAIL %s: editor did not render\n", name);
        return false;
    }

    // MarginView::RefreshPixMaps creates the offset-one pattern for visible
    // origin zero. Surface_impl tiles it from the individual margin's origin.
    QImage pattern(8, 8, QImage::Format_ARGB32);
    for (int y = 0; y < pattern.height(); ++y) {
        for (int x = 0; x < pattern.width(); ++x) {
            pattern.setPixelColor(x, y, (x + y) % 2 ? stripes : base);
        }
    }
    QImage expected(actual.size(), QImage::Format_ARGB32);
    expected.setDevicePixelRatio(dpr);
    expected.fill(Qt::white);
    {
        QPainter painter(&expected);
        painter.drawTiledPixmap(QRectF(margin_left, 0, margin_width, k_height), QPixmap::fromImage(pattern));
    }
    int differences = 0;
    const int first_pixel = static_cast<int>(std::ceil(margin_left * dpr));
    const int last_pixel = static_cast<int>(std::floor((margin_left + margin_width) * dpr));
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = first_pixel; x < last_pixel; ++x) {
            const QColor observed = actual.pixelColor(x, y);
            const QColor reference = expected.pixelColor(x, y);
            if (std::abs(observed.red() - reference.red()) > k_channel_tolerance ||
                std::abs(observed.green() - reference.green()) > k_channel_tolerance ||
                std::abs(observed.blue() - reference.blue()) > k_channel_tolerance)
            {
                ++differences;
            }
        }
    }
    if (differences != 0) {
        const QString directory = QStringLiteral("renderer_conformance_artifacts/%1_%2").arg(backend).arg(dpr);
        QDir().mkpath(directory);
        actual.save(directory + QLatin1Char('/') + name + QStringLiteral("_actual.png"));
        expected.save(directory + QLatin1Char('/') + name + QStringLiteral("_expected.png"));
        std::fprintf(stderr, "FAIL %s: %d differing pixels\n", name, differences);
        return false;
    }
    std::printf("PASS %s (%s, DPR %.2f)\n", name, qPrintable(backend), dpr);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3) {
        std::fprintf(stderr, "usage: renderer_conformance <software|rhi> <device-pixel-ratio>\n");
        return 2;
    }
    const QString backend = QString::fromLocal8Bit(argv[1]);
    const QByteArray dpr_text(argv[2]);
    bool dpr_valid = false;
    const qreal dpr = dpr_text.toDouble(&dpr_valid);
    if ((backend != QStringLiteral("software") && backend != QStringLiteral("rhi")) ||
        !dpr_valid || dpr <= 0.0)
    {
        return 2;
    }
    qputenv("QT_SCALE_FACTOR", dpr_text);
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    if (backend == QStringLiteral("software")) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }
    else {
#ifdef Q_OS_WIN
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
#elif defined(Q_OS_MACOS)
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#else
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
    }
    QGuiApplication application(argc, argv);
    Conformance_runner runner(backend, dpr);
    int failures = 0;
    failures += !check_caret_clip(runner);
    failures += !check_background_alignment(runner);
    struct indicator_case_t
    {
        const char* name;
        IndicatorStyle style;
    };
    constexpr std::array indicators = {
        indicator_case_t{"full_box_alpha_height_clip", IndicatorStyle::FullBox},
        indicator_case_t{"straight_box_alpha_height_clip", IndicatorStyle::StraightBox},
        indicator_case_t{"gradient_alpha_height_clip", IndicatorStyle::Gradient},
        indicator_case_t{"gradient_centre", IndicatorStyle::GradientCentre},
        indicator_case_t{"round_box", IndicatorStyle::RoundBox},
        indicator_case_t{"box", IndicatorStyle::Box},
        indicator_case_t{"dot_box", IndicatorStyle::DotBox},
        indicator_case_t{"plain", IndicatorStyle::Plain},
        indicator_case_t{"squiggle", IndicatorStyle::Squiggle},
        indicator_case_t{"squiggle_low", IndicatorStyle::SquiggleLow},
        indicator_case_t{"squiggle_pixmap", IndicatorStyle::SquigglePixmap},
        indicator_case_t{"tt", IndicatorStyle::TT},
        indicator_case_t{"diagonal", IndicatorStyle::Diagonal},
        indicator_case_t{"strike", IndicatorStyle::Strike},
        indicator_case_t{"dash", IndicatorStyle::Dash},
        indicator_case_t{"dots", IndicatorStyle::Dots},
        indicator_case_t{"composition_thick", IndicatorStyle::CompositionThick},
        indicator_case_t{"composition_thin", IndicatorStyle::CompositionThin},
        indicator_case_t{"point", IndicatorStyle::Point},
        indicator_case_t{"point_character", IndicatorStyle::PointCharacter},
        indicator_case_t{"point_top", IndicatorStyle::PointTop},
    };
    for (const auto& fixture : indicators) {
        failures += !check_indicator(runner, fixture.name, fixture.style);
    }
    // Reuse the same scene-graph nodes while changing position, raster phase,
    // style and stroke. The oracle paints in unchanged item coordinates.
    failures += !check_indicator(runner, "translated_full_box", IndicatorStyle::FullBox, QPointF(0.25, 0.5), 2.0);
    failures += !check_indicator(runner, "translated_dash", IndicatorStyle::Dash, QPointF(0.25, 0.5), 2.0);
    failures += !check_indicator(runner, "translated_point", IndicatorStyle::Point, QPointF(0.25, 0.5), 2.0);
    failures += !check_thin_dot_box(runner, "dot_box_one_column", 1, 17);
    failures += !check_thin_dot_box(runner, "dot_box_one_row", 17, 1);
    failures += !check_thin_dot_box(runner, "dot_box_one_pixel", 1, 1);

    struct marker_case_t
    {
        const char* name;
        MarkerSymbol symbol;
    };
    constexpr std::array markers = {
        marker_case_t{"full_rect_marker", MarkerSymbol::FullRect},
        marker_case_t{"circle_plus_marker", MarkerSymbol::CirclePlus},
        marker_case_t{"circle_minus_marker", MarkerSymbol::CircleMinus},
        marker_case_t{"circle_plus_connected_marker", MarkerSymbol::CirclePlusConnected},
        marker_case_t{"circle_minus_connected_marker", MarkerSymbol::CircleMinusConnected},
        marker_case_t{"circle_marker", MarkerSymbol::Circle},
        marker_case_t{"round_rect_marker", MarkerSymbol::RoundRect},
        marker_case_t{"arrow_marker", MarkerSymbol::Arrow},
        marker_case_t{"small_rect_marker", MarkerSymbol::SmallRect},
        marker_case_t{"short_arrow_marker", MarkerSymbol::ShortArrow},
        marker_case_t{"arrow_down_marker", MarkerSymbol::ArrowDown},
        marker_case_t{"minus_marker", MarkerSymbol::Minus},
        marker_case_t{"plus_marker", MarkerSymbol::Plus},
        marker_case_t{"vertical_line_marker", MarkerSymbol::VLine},
        marker_case_t{"l_corner_marker", MarkerSymbol::LCorner},
        marker_case_t{"t_corner_marker", MarkerSymbol::TCorner},
        marker_case_t{"l_corner_curve_marker", MarkerSymbol::LCornerCurve},
        marker_case_t{"t_corner_curve_marker", MarkerSymbol::TCornerCurve},
        marker_case_t{"box_plus_marker", MarkerSymbol::BoxPlus},
        marker_case_t{"box_minus_marker", MarkerSymbol::BoxMinus},
        marker_case_t{"box_plus_connected_marker", MarkerSymbol::BoxPlusConnected},
        marker_case_t{"box_minus_connected_marker", MarkerSymbol::BoxMinusConnected},
        marker_case_t{"dot_dot_dot_marker", MarkerSymbol::DotDotDot},
        marker_case_t{"arrows_marker", MarkerSymbol::Arrows},
        marker_case_t{"left_rect_marker", MarkerSymbol::LeftRect},
        marker_case_t{"bookmark_marker", MarkerSymbol::Bookmark},
        marker_case_t{"vertical_bookmark_marker", MarkerSymbol::VerticalBookmark},
        marker_case_t{"bar_marker", MarkerSymbol::Bar},
    };
    for (const auto& fixture : markers) {
        failures += !check_marker(runner, fixture.name, fixture.symbol);
    }
    failures += !check_marker(runner, "number_margin_circle", MarkerSymbol::Circle,
        Scintilla::MarginType::Number, 2.0, QRectF(6.25, 17.5, 46.0, 29.0));
    failures += !check_marker(runner, "character_marker",
        static_cast<MarkerSymbol>(static_cast<int>(MarkerSymbol::Character) + 'A'));
    // Exercise updates after the item has already rendered with inherited
    // translation and opacity. Both body and margin descendants must retain
    // that state when new shape nodes are attached under plain groups.
    {
        Conformance_runner transformed(backend, dpr, QPointF(16.0, 8.0), 0.5);
        failures += !check_indicator(transformed, "transformed_full_box", IndicatorStyle::FullBox);
        failures += !check_marker(transformed, "transformed_circle_marker", MarkerSymbol::Circle);
        failures += !check_indicator(transformed, "transformed_plain_indicator", IndicatorStyle::Plain);
        failures += !check_marker(transformed, "transformed_character_marker",
            static_cast<MarkerSymbol>(static_cast<int>(MarkerSymbol::Character) + 'A'));
    }
    failures += check_background_capture(backend, dpr);
    failures += !check_folding_margin(backend, dpr);
    return failures == 0 ? 0 : 1;
}
