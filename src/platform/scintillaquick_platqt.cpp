// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file PlatQt.cpp
// Scintilla platform layer for Qt QML/Quick

#include <cstdio>
#include <cstring>
#include <algorithm>

#include <scintillaquick/ScintillaQuickItem.h>
#include "../core/scintillaquick_hierarchical_profiler.h"
#include "scintillaquick_platqt.h"
#include "scintillaquick_fonts.h"
#include "Scintilla.h"
#include "XPM.h"
#include "UniConversion.h"
#include "DBCS.h"

#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>
#include <QColor>
#include <QCursor>
#include <QList>
#include <QImage>
#include <QMouseEvent>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPair>
#include <QRect>
#include <QScreen>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QStyleHints>
#include <QGlyphRun>
#include <QRawFont>
#include <QTextOption>
#include <QTextLayout>
#include <QTextLine>
#include <QFontMetricsF>
#include <QWheelEvent>
#include <QVarLengthArray>
#include <QLibrary>
#include <QWindow>

extern void ProcessScintillaContextMenu(Scintilla::Internal::Point pt, const Scintilla::Internal::Window & w, const QList<QPair<QString, QPair<int, bool>>> & menu);

namespace Scintilla::Internal {

//----------------------------------------------------------------------

bool is_ascii_text(std::string_view text) {
    return std::all_of(text.cbegin(), text.cend(), [](char ch) {
        return static_cast<unsigned char>(ch) < 0x80;
    });
}

bool is_simple_printable_ascii(std::string_view text) {
    return std::all_of(text.cbegin(), text.cend(), [](char ch) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        return uch >= 0x20 && uch < 0x7f;
    });
}

bool fill_simple_glyph_positions(const QTextLine &line, std::string_view text, XYPOSITION *positions)
{
    if (!line.isValid() || !positions || text.empty()) {
        return false;
    }

    const auto glyph_runs = line.glyphRuns(
        0,
        line.textLength(),
        QTextLayout::RetrieveGlyphIndexes |
        QTextLayout::RetrieveGlyphPositions |
        QTextLayout::RetrieveStringIndexes);

    size_t filled = 0;
    for (const QGlyphRun &glyph_run : glyph_runs) {
        if (glyph_run.isRightToLeft()) {
            return false;
        }

        const QList<quint32> glyph_indexes    = glyph_run.glyphIndexes();
        const QList<QPointF> glyph_positions  = glyph_run.positions();
        const QList<qsizetype> string_indexes = glyph_run.stringIndexes();
        if (glyph_indexes.size() != glyph_positions.size() ||
            glyph_indexes.size() != string_indexes.size()) {
            return false;
        }

        const QList<QPointF> advances = glyph_run.rawFont().advancesForGlyphIndexes(glyph_indexes);
        if (advances.size() != glyph_indexes.size()) {
            return false;
        }

        for (qsizetype i = 0; i < glyph_indexes.size(); ++i) {
            if (filled >= text.size()) {
                return false;
            }
            if (string_indexes[i] != static_cast<qsizetype>(filled)) {
                return false;
            }
            positions[filled] = glyph_positions[i].x() + advances[i].x();
            ++filled;
        }
    }

    return filled == text.size();
}

QString unicode_from_text(std::string_view text) {
    if (is_ascii_text(text)) {
        return QString::fromLatin1(text.data(), static_cast<int>(text.length()));
    }
    return QString::fromUtf8(text.data(), static_cast<int>(text.length()));
}

static QFont::StyleStrategy choose_strategy(FontQuality eff)
{
    switch (eff) {
        case FontQuality::QualityDefault:         return QFont::PreferDefault;
        case FontQuality::QualityNonAntialiased: return QFont::NoAntialias;
        case FontQuality::QualityAntialiased:     return QFont::PreferAntialias;
        case FontQuality::QualityLcdOptimized:   return QFont::PreferAntialias;
        default:                             return QFont::PreferDefault;
    }
}

class Font_and_character_set : public Font {
public:
    CharacterSet m_character_set = CharacterSet::Ansi;
    std::unique_ptr<QFont> m_font;

    // Lazy per-font advance cache used by the ASCII fast path in
    // MeasureWidths. For fixed-pitch fonts (the common case in code
    // editors) every printable-ASCII character has the same advance, so
    // the single `m_fixed_advance` is enough to compute cumulative
    // positions with one multiply per character, bypassing both
    // QTextLayout construction and glyph enumeration. The cache is
    // populated on the first successful slow-path measurement so the
    // cached advance matches what shaping produces for that font.
    //
    // m_advance_cache_state values:
    //   0 = uninitialized (take the slow path and try to populate)
    //   1 = fixed-pitch (fast path uses m_fixed_advance)
    //   2 = known not fixed-pitch (skip the fast path permanently)
    //
    // Access is serialised by the fact that MeasureWidths is only ever
    // called from the GUI thread.
    mutable int m_advance_cache_state = 0;
    mutable double m_fixed_advance = 0.0;

    explicit Font_and_character_set(const FontParameters &fp) : m_character_set(fp.characterSet) {
        m_font = std::make_unique<QFont>();
        m_font->setStyleStrategy(choose_strategy(fp.extraFontFlag));
        m_font->setFamily(QString::fromUtf8(fp.faceName));
        m_font->setPointSizeF(fp.size);
        m_font->setBold(static_cast<int>(fp.weight) > 500);
        m_font->setItalic(fp.italic);
    }

    // Attempts to fill `positions` for printable-ASCII `text` using the
    // cached fixed-pitch advance. Returns false if the cache is not yet
    // ready or the font is known not to be fixed-pitch, in which case
    // the caller must fall through to the slow QTextLayout-based path.
    bool try_fill_ascii_positions_from_cache(std::string_view text, XYPOSITION *positions) const
    {
        if (m_advance_cache_state != 1) {
            return false;
        }
        const double w = m_fixed_advance;
        for (size_t i = 0; i < text.size(); ++i) {
            positions[i] = static_cast<XYPOSITION>(static_cast<double>(i + 1) * w);
        }
        return true;
    }

    // Populate the ASCII advance cache from a concrete measurement that
    // has already been performed through the slow path. If the observed
    // advances match and QFontInfo agrees the font is fixed-pitch, the
    // cache switches to the fast path for all subsequent calls. If the
    // font is known not to be fixed-pitch the state is frozen so we do
    // not keep re-running the detection.
    void populate_ascii_cache_from_measurement(std::string_view text, const XYPOSITION *positions) const
    {
        if (m_advance_cache_state != 0) {
            return;
        }
        if (text.empty()) {
            return;
        }

        // Extract per-character advances and verify they are uniform
        // across observed printable-ASCII bytes.
        double first_adv = -1.0;
        double prev = 0.0;
        bool uniform = true;
        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char uch = static_cast<unsigned char>(text[i]);
            if (uch < 0x20 || uch >= 0x7F) {
                uniform = false;
                break;
            }
            const double cur = positions[i];
            const double adv = cur - prev;
            prev = cur;
            if (first_adv < 0.0) {
                first_adv = adv;
            }
            else
            if (std::abs(adv - first_adv) > 0.01) {
                uniform = false;
                break;
            }
        }

        if (uniform && first_adv > 0.0 && QFontInfo(*m_font).fixedPitch()) {
            m_fixed_advance = first_adv;
            m_advance_cache_state = 1;
        }
        else {
            m_advance_cache_state = 2;
        }
    }
};

namespace {

const Supports k_supports_qt[] = {
    Supports::LineDrawsFinal,
    Supports::FractionalStrokeWidth,
    Supports::TranslucentStroke,
    Supports::PixelModification,
};

QSGTextNode::RenderType map_text_render_type()
{
    switch (QQuickWindow::textRenderType()) {
        case QQuickWindow::QtTextRendering:
            return QSGTextNode::QtRendering;
        case QQuickWindow::NativeTextRendering:
            return QSGTextNode::NativeRendering;
        case QQuickWindow::CurveTextRendering:
            return QSGTextNode::CurveRendering;
    }

    return QSGTextNode::QtRendering;
}

const Font_and_character_set *as_font_and_character_set(const Font *f) {
    return dynamic_cast<const Font_and_character_set *>(f);
}

QFont *font_pointer(const Font *f)
{
    return as_font_and_character_set(f)->m_font.get();
}

Surface_impl *as_surface_impl(Surface &surface) noexcept
{
    return dynamic_cast<Surface_impl *>(&surface);
}

const QScreen *screen_for_log_pixels() noexcept
{
    const QWindow *focusWindow = QGuiApplication::focusWindow();
    if (focusWindow) {
        const QScreen *screen = focusWindow->screen();
        if (screen) {
            return screen;
        }
    }
    return QGuiApplication::primaryScreen();
}

}

std::shared_ptr<Font> Font::Allocate(const FontParameters &fp)
{
    return std::make_shared<Font_and_character_set>(fp);
}

Surface_impl::Surface_impl() = default;

Surface_impl::Surface_impl(int width, int height, SurfaceMode surface_mode)
{
    if (width < 1) width   = 1;
    if (height < 1) height = 1;
    deviceOwned            = true;
    device                 = new QPixmap(width, height);
    mode                   = surface_mode;
}

Surface_impl::~Surface_impl()
{
    Clear();
}

void Surface_impl::Clear()
{
    if (painterOwned && painter) {
        delete painter;
    }

    if (deviceOwned && device) {
        delete device;
    }
    device       = nullptr;
    painter      = nullptr;
    deviceOwned  = false;
    painterOwned = false;
    capture_only = false;
}

void Surface_impl::Init(bool signature_flag, PainterID pid)
{
    Q_UNUSED(signature_flag);
    Release();
    painter = static_cast<QPainter *>(pid);
    device  = painter ? painter->device() : nullptr;
}

void Surface_impl::Init(WindowID wid)
{
    Release();
    Q_UNUSED(wid);
    Q_ASSERT(false);
}

void Surface_impl::Init(SurfaceID sid, WindowID /*wid*/)
{
    Release();
    device = static_cast<QPaintDevice *>(sid);
}

std::unique_ptr<Surface> Surface_impl::AllocatePixMap(int width, int height)
{
    return std::make_unique<Surface_impl>(width, height, mode);
}

void Surface_impl::SetMode(SurfaceMode surface_mode)
{
    mode = surface_mode;
}

void Surface_impl::Release() noexcept
{
    Clear();
}

int Surface_impl::SupportsFeature(Supports feature) noexcept
{
    for (const Supports f : k_supports_qt) {
        if (f == feature)
            return 1;
    }
    return 0;
}

bool Surface_impl::Initialised()
{
    return painter != nullptr;
}

void Surface_impl::PenColour(ColourRGBA fore)
{
    if (capture_only) {
        return;
    }
    QPen penOutline(QColorFromColourRGBA(fore));
    penOutline.setCapStyle(Qt::FlatCap);
    GetPainter()->setPen(penOutline);
}

void Surface_impl::PenColourWidth(ColourRGBA fore, XYPOSITION strokeWidth) {
    if (capture_only) {
        return;
    }
    QPen penOutline(QColorFromColourRGBA(fore));
    penOutline.setCapStyle(Qt::FlatCap);
    penOutline.setJoinStyle(Qt::MiterJoin);
    penOutline.setWidthF(strokeWidth);
    GetPainter()->setPen(penOutline);
}

void Surface_impl::BrushColour(ColourRGBA back)
{
    if (capture_only) {
        return;
    }
    GetPainter()->setBrush(QBrush(QColorFromColourRGBA(back)));
}

void Surface_impl::SetFont(const Font *font)
{
    if (capture_only) {
        return;
    }
    const Font_and_character_set *pfacs = as_font_and_character_set(font);
    if (pfacs && pfacs->m_font) {
        GetPainter()->setFont(*(pfacs->m_font));
    }
}

int Surface_impl::LogPixelsY()
{
    const QScreen *screen = screen_for_log_pixels();
    return screen ? screen->logicalDotsPerInchY() : 96;
}

int Surface_impl::PixelDivisions()
{
    // Qt uses device pixels.
    return 1;
}

int Surface_impl::DeviceHeightFont(int points)
{
    return points;
}

void Surface_impl::LineDraw(Point start, Point end, Stroke stroke)
{
    if (capture_only) {
        return;
    }
    PenColourWidth(stroke.colour, stroke.width);
    QLineF line(start.x, start.y, end.x, end.y);
    GetPainter()->drawLine(line);
}

void Surface_impl::PolyLine(const Point *pts, size_t npts, Stroke stroke)
{
    if (capture_only) {
        return;
    }
    // TODO: set line joins and caps
    PenColourWidth(stroke.colour, stroke.width);
    std::vector<QPointF> qpts;
    std::transform(pts, pts + npts, std::back_inserter(qpts), QPointFFromPoint);
    GetPainter()->drawPolyline(&qpts[0], static_cast<int>(npts));
}

void Surface_impl::Polygon(const Point *pts, size_t npts, FillStroke fillStroke)
{
    if (capture_only) {
        return;
    }
    PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
    BrushColour(fillStroke.fill.colour);

    std::vector<QPointF> qpts;
    std::transform(pts, pts + npts, std::back_inserter(qpts), QPointFFromPoint);

    GetPainter()->drawPolygon(&qpts[0], static_cast<int>(npts));
}

void Surface_impl::RectangleDraw(PRectangle rc, FillStroke fillStroke)
{
    if (capture_only) {
        return;
    }
    PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
    BrushColour(fillStroke.fill.colour);
    const QRectF rect = QRectFFromPRect(rc.Inset(fillStroke.stroke.width / 2));
    GetPainter()->drawRect(rect);
}

void Surface_impl::RectangleFrame(PRectangle rc, Stroke stroke) {
    if (capture_only) {
        return;
    }
    PenColourWidth(stroke.colour, stroke.width);
    // Default QBrush is Qt::NoBrush so does not fill
    GetPainter()->setBrush(QBrush());
    const QRectF rect = QRectFFromPRect(rc.Inset(stroke.width / 2));
    GetPainter()->drawRect(rect);
}

void Surface_impl::FillRectangle(PRectangle rc, Fill fill)
{
    if (capture_only) {
        return;
    }
    GetPainter()->fillRect(QRectFFromPRect(rc), QColorFromColourRGBA(fill.colour));
}

void Surface_impl::FillRectangleAligned(PRectangle rc, Fill fill)
{
    FillRectangle(PixelAlign(rc, 1), fill);
}

void Surface_impl::FillRectangle(PRectangle rc, Surface &surfacePattern)
{
    if (capture_only) {
        return;
    }
    // Tile pattern over rectangle
    Surface_impl *surface = as_surface_impl(surfacePattern);
    if (!surface) {
        return;
    }
    const QPixmap *pixmap = static_cast<QPixmap *>(surface->GetPaintDevice());
    if (!pixmap) {
        return;
    }
    GetPainter()->drawTiledPixmap(QRectFromPRect(rc), *pixmap);
}

void Surface_impl::RoundedRectangle(PRectangle rc, FillStroke fillStroke)
{
    if (capture_only) {
        return;
    }
    PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
    BrushColour(fillStroke.fill.colour);
    GetPainter()->drawRoundedRect(QRectFFromPRect(rc), 3.0f, 3.0f);
}

void Surface_impl::AlphaRectangle(PRectangle rc, XYPOSITION cornerSize, FillStroke fillStroke)
{
    if (capture_only) {
        return;
    }
    QColor qFill = QColorFromColourRGBA(fillStroke.fill.colour);
    QBrush brushFill(qFill);
    GetPainter()->setBrush(brushFill);
    if (fillStroke.fill.colour == fillStroke.stroke.colour) {
        painter->setPen(Qt::NoPen);
        QRectF rect = QRectFFromPRect(rc);
        if (cornerSize > 0.0f) {
            // A radius of 1 shows no curve so add 1
            qreal radius = cornerSize+1;
            GetPainter()->drawRoundedRect(rect, radius, radius);
        }
        else {
            GetPainter()->fillRect(rect, brushFill);
        }
    }
    else {
        QColor qOutline = QColorFromColourRGBA(fillStroke.stroke.colour);
        QPen penOutline(qOutline);
        penOutline.setWidthF(fillStroke.stroke.width);
        GetPainter()->setPen(penOutline);

        QRectF rect = QRectFFromPRect(rc.Inset(fillStroke.stroke.width / 2));
        if (cornerSize > 0.0f) {
            // A radius of 1 shows no curve so add 1
            qreal radius = cornerSize+1;
            GetPainter()->drawRoundedRect(rect, radius, radius);
        }
        else {
            GetPainter()->drawRect(rect);
        }
    }
}

void Surface_impl::GradientRectangle(PRectangle rc, const std::vector<ColourStop> &stops, GradientOptions m_options) {
    if (capture_only) {
        return;
    }
    QRectF rect = QRectFFromPRect(rc);
    QLinearGradient linearGradient;
    switch (m_options) {
        case GradientOptions::leftToRight:
            linearGradient = QLinearGradient(rc.left, rc.top, rc.right, rc.top);
            break;
        case GradientOptions::topToBottom:
        default:
            linearGradient = QLinearGradient(rc.left, rc.top, rc.left, rc.bottom);
            break;
    }
    linearGradient.setSpread(QGradient::RepeatSpread);
    for (const ColourStop &stop : stops) {
        linearGradient.setColorAt(stop.position, QColorFromColourRGBA(stop.colour));
    }
    QBrush brush = QBrush(linearGradient);
    GetPainter()->fillRect(rect, brush);
}

static std::vector<unsigned char> ImageByteSwapped(int width, int height, const unsigned char *pixels_image)
{
    // Input is RGBA, but Format_ARGB32 is BGRA, so swap the red bytes and blue bytes
    size_t bytes = width * height * 4;
    std::vector<unsigned char> image_bytes(pixels_image, pixels_image + bytes);
    for (size_t i=0; i<bytes; i+=4)
        std::swap(image_bytes[i], image_bytes[i+2]);
    return image_bytes;
}

void Surface_impl::DrawRGBAImage(PRectangle rc, int width, int height, const unsigned char *pixels_image)
{
    if (capture_only) {
        return;
    }
    std::vector<unsigned char> image_bytes = ImageByteSwapped(width, height, pixels_image);
    QImage image(&image_bytes[0], width, height, QImage::Format_ARGB32);
    QPoint pt(rc.left, rc.top);
    GetPainter()->drawImage(pt, image);
}

void Surface_impl::Ellipse(PRectangle rc, FillStroke fillStroke)
{
    if (capture_only) {
        return;
    }
    PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
    BrushColour(fillStroke.fill.colour);
    const QRectF rect = QRectFFromPRect(rc.Inset(fillStroke.stroke.width / 2));
    GetPainter()->drawEllipse(rect);
}

void Surface_impl::Stadium(PRectangle rc, FillStroke fillStroke, Ends ends) {
    if (capture_only) {
        return;
    }
    const XYPOSITION halfStroke = fillStroke.stroke.width / 2.0f;
    const XYPOSITION radius     = rc.Height() / 2.0f - halfStroke;
    PRectangle rcInner          = rc;
    rcInner.left += radius;
    rcInner.right -= radius;
    const XYPOSITION arc_height  = rc.Height() - fillStroke.stroke.width;

    PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
    BrushColour(fillStroke.fill.colour);

    QPainterPath path;

    const Ends leftSide  = static_cast<Ends>(static_cast<unsigned int>(ends) & 0xfu);
    const Ends rightSide = static_cast<Ends>(static_cast<unsigned int>(ends) & 0xf0u);
    switch (leftSide) {
        case Ends::leftFlat:
            path.moveTo(rc.left + halfStroke, rc.top + halfStroke);
            path.lineTo(rc.left + halfStroke, rc.bottom - halfStroke);
            break;
        case Ends::leftAngle:
            path.moveTo(rcInner.left + halfStroke, rc.top + halfStroke);
            path.lineTo(rc.left + halfStroke, rc.Centre().y);
            path.lineTo(rcInner.left + halfStroke, rc.bottom - halfStroke);
            break;
        case Ends::semiCircles:
        default:
            path.moveTo(rcInner.left + halfStroke, rc.top + halfStroke);
            QRectF rectangleArc(rc.left + halfStroke, rc.top + halfStroke,
                        arc_height, arc_height);
            path.arcTo(rectangleArc, 90, 180);
            break;
    }

    switch (rightSide) {
        case Ends::rightFlat:
            path.lineTo(rc.right - halfStroke, rc.bottom - halfStroke);
            path.lineTo(rc.right - halfStroke, rc.top + halfStroke);
            break;
        case Ends::rightAngle:
            path.lineTo(rcInner.right - halfStroke, rc.bottom - halfStroke);
            path.lineTo(rc.right - halfStroke, rc.Centre().y);
            path.lineTo(rcInner.right - halfStroke, rc.top + halfStroke);
            break;
        case Ends::semiCircles:
        default:
            path.lineTo(rcInner.right - halfStroke, rc.bottom - halfStroke);
            QRectF rectangleArc(rc.right - arc_height - halfStroke, rc.top + halfStroke,
                        arc_height, arc_height);
            path.arcTo(rectangleArc, 270, 180);
            break;
    }

    // Close the path to enclose it for stroking and for filling, then draw it
    path.closeSubpath();
    GetPainter()->drawPath(path);
}

void Surface_impl::Copy(PRectangle rc, Point from, Surface &surfaceSource)
{
    if (capture_only) {
        return;
    }
    Surface_impl *source = as_surface_impl(surfaceSource);
    Q_ASSERT(source);
    if (!source) {
        return;
    }
    QPixmap *pixmap = static_cast<QPixmap *>(source->GetPaintDevice());
    if (!pixmap) {
        return;
    }

    GetPainter()->drawPixmap(rc.left, rc.top, *pixmap, from.x, from.y, -1, -1);
}

std::unique_ptr<IScreenLineLayout> Surface_impl::Layout(const IScreenLine *)
{
    return {};
}

void Surface_impl::DrawTextNoClip(PRectangle rc,
                 const Font *font,
                                 XYPOSITION y_base,
                 std::string_view text,
                 ColourRGBA fore,
                 ColourRGBA back)
{
    if (capture_only) {
        return;
    }
    SetFont(font);
    PenColour(fore);

    GetPainter()->setBackground(QColorFromColourRGBA(back));
    GetPainter()->setBackgroundMode(Qt::OpaqueMode);
    QString su = unicode_from_text(text);
    GetPainter()->drawText(QPointF(rc.left, y_base), su);
}

void Surface_impl::DrawTextClipped(PRectangle rc,
                  const Font *font,
                                  XYPOSITION y_base,
                  std::string_view text,
                  ColourRGBA fore,
                  ColourRGBA back)
{
    SetClip(rc);
    DrawTextNoClip(rc, font, y_base, text, fore, back);
    PopClip();
}

void Surface_impl::DrawTextTransparent(PRectangle rc,
                      const Font *font,
                                      XYPOSITION y_base,
                      std::string_view text,
    ColourRGBA fore)
{
    if (capture_only) {
        return;
    }
    SetFont(font);
    PenColour(fore);

    GetPainter()->setBackgroundMode(Qt::TransparentMode);
    QString su = unicode_from_text(text);
    GetPainter()->drawText(QPointF(rc.left, y_base), su);
}

void Surface_impl::SetClip(PRectangle rc)
{
    if (capture_only) {
        return;
    }
    GetPainter()->save();
    GetPainter()->setClipRect(QRectFFromPRect(rc));
}

void Surface_impl::PopClip()
{
    if (capture_only) {
        return;
    }
    GetPainter()->restore();
}

void Surface_impl::MeasureWidths(const Font *font,
                std::string_view text,
                                XYPOSITION *positions)
{
    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("platform.measure_widths");
    if (!font)
        return;

    // Fast path: for printable-ASCII text against a font whose advance
    // cache is already populated, just sum the cached advances. This
    // avoids constructing a QTextLayout and enumerating glyph runs for
    // what is by far the common case (code editing in a fixed-pitch
    // font). No profile scope here: this path is called tens of
    // thousands of times per benchmark run and the profiler's mutex
    // lock would add more overhead than the fast path itself costs.
    const Font_and_character_set *font_wrapper = as_font_and_character_set(font);
    const bool ascii_text = is_simple_printable_ascii(text);
    if (ascii_text && font_wrapper &&
        font_wrapper->try_fill_ascii_positions_from_cache(text, positions)) {
        return;
    }

    QString su;
    {
        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("platform.measure_widths.to_qstring");
        su = unicode_from_text(text);
    }
    QTextLayout tlay(su, *font_pointer(font), GetPaintDevice());
    QTextLine tl;
    {
        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("platform.measure_widths.layout_text");
        tlay.beginLayout();
        tl = tlay.createLine();
        tlay.endLayout();
    }
    {
        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("platform.measure_widths.cursor_positions");
        if (is_simple_printable_ascii(text)) {
            SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("platform.measure_widths.cursor_positions.simple_glyph_path");
            if (fill_simple_glyph_positions(tl, text, positions)) {
                // Opportunistically populate the advance cache so that
                // subsequent calls on the same font can take the fast
                // path above. The populated values come from the same
                // slow path the benchmark previously exercised, so this
                // is not an independent source of truth.
                if (font_wrapper) {
                    font_wrapper->populate_ascii_cache_from_measurement(text, positions);
                }
                return;
            }
        }

        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("platform.measure_widths.cursor_positions.cursor_walk");
        if (mode.codePage == SC_CP_UTF8) {
            int fit  = su.size();
            int ui   = 0;
            size_t i = 0;
            while (ui<fit) {
                const unsigned char uch      = text[i];
                const unsigned int byteCount = UTF8BytesOfLead[uch];
                const int codeUnits          = UTF16LengthFromUTF8ByteCount(byteCount);
                qreal x_position              = tl.cursorToX(ui+codeUnits);
                for (size_t bytePos=0; (bytePos<byteCount) && (i<text.length()); bytePos++) {
                    positions[i++] = x_position;
                }
                ui += codeUnits;
            }
            XYPOSITION lastPos = 0;
            if (i > 0)
                lastPos = positions[i-1];
            while (i<text.length()) {
                positions[i++] = lastPos;
            }
        }
        else
        if (mode.codePage) {
            // DBCS
            int ui = 0;
            for (size_t i=0; i<text.length();) {
                size_t lenChar  = DBCSIsLeadByte(mode.codePage, text[i]) ? 2 : 1;
                qreal x_position = tl.cursorToX(ui+1);
                for (unsigned int bytePos=0; (bytePos<lenChar) && (i<text.length()); bytePos++) {
                    positions[i++] = x_position;
                }
                ui++;
            }
        }
        else {
            // Single byte encoding
            for (int i=0; i<static_cast<int>(text.length()); i++) {
                positions[i] = tl.cursorToX(i+1);
            }
        }
    }
}

XYPOSITION Surface_impl::WidthText(const Font *font, std::string_view text)
{
    QFontMetricsF metrics(*font_pointer(font), device);
    QString su = unicode_from_text(text);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return metrics.horizontalAdvance(su);
#else
    return metrics.width(su);
#endif
}

void Surface_impl::DrawTextNoClipUTF8(PRectangle rc,
                 const Font *font,
                 XYPOSITION y_base,
                 std::string_view text,
                 ColourRGBA fore,
                 ColourRGBA back)
{
    DrawTextNoClip(rc, font, y_base, text, fore, back);
}

void Surface_impl::DrawTextClippedUTF8(PRectangle rc,
                  const Font *font,
                  XYPOSITION y_base,
                  std::string_view text,
                  ColourRGBA fore,
                  ColourRGBA back)
{
    DrawTextClipped(rc, font, y_base, text, fore, back);
}

void Surface_impl::DrawTextTransparentUTF8(PRectangle rc,
                      const Font *font,
                      XYPOSITION y_base,
                      std::string_view text,
    ColourRGBA fore)
{
    DrawTextTransparent(rc, font, y_base, text, fore);
}

void Surface_impl::MeasureWidthsUTF8(const Font *font,
                std::string_view text,
                XYPOSITION *positions)
{
    if (!font) {
        return;
    }
    QString su = QString::fromUtf8(text.data(), static_cast<int>(text.length()));
    QTextLayout tlay(su, *font_pointer(font), GetPaintDevice());
    tlay.beginLayout();
    QTextLine tl = tlay.createLine();
    tlay.endLayout();
    int fit  = su.size();
    int ui   = 0;
    size_t i = 0;
    while (ui<fit) {
        const unsigned char uch      = text[i];
        const unsigned int byteCount = UTF8BytesOfLead[uch];
        const int codeUnits          = UTF16LengthFromUTF8ByteCount(byteCount);
        qreal x_position              = tl.cursorToX(ui+codeUnits);
        for (size_t bytePos=0; (bytePos<byteCount) && (i<text.length()); bytePos++) {
            positions[i++] = x_position;
        }
        ui += codeUnits;
    }
    XYPOSITION lastPos = 0;
    if (i > 0) {
        lastPos = positions[i-1];
    }
    while (i<text.length()) {
        positions[i++] = lastPos;
    }
}

XYPOSITION Surface_impl::WidthTextUTF8(const Font *font, std::string_view text)
{
    QFontMetricsF metrics(*font_pointer(font), device);
    QString su = QString::fromUtf8(text.data(), static_cast<int>(text.length()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return metrics.horizontalAdvance(su);
#else
    return metrics.width(su);
#endif
}

XYPOSITION Surface_impl::Ascent(const Font *font)
{
    QFontMetricsF metrics(*font_pointer(font), device);
    return metrics.ascent();
}

XYPOSITION Surface_impl::Descent(const Font *font)
{
    QFontMetricsF metrics(*font_pointer(font), device);
    // Qt returns 1 less than true descent
    // See: QFontEngineWin::descent which says:
    // ### we subtract 1 to even out the historical +1 in QFontMetrics's
    // ### height=asc+desc+1 equation. Fix in Qt5.
    return metrics.descent() + 1;
}

XYPOSITION Surface_impl::InternalLeading(const Font * /* font */)
{
    return 0;
}

XYPOSITION Surface_impl::Height(const Font *font)
{
    QFontMetricsF metrics(*font_pointer(font), device);
    return metrics.height();
}

XYPOSITION Surface_impl::AverageCharWidth(const Font *font)
{
    QFontMetricsF metrics(*font_pointer(font), device);
    return metrics.averageCharWidth();
}

void Surface_impl::FlushCachedState()
{
    if (device->paintingActive()) {
        GetPainter()->setPen(QPen());
        GetPainter()->setBrush(QBrush());
    }
}

void Surface_impl::FlushDrawing()
{
}

QPaintDevice *Surface_impl::GetPaintDevice()
{
    return device ? device : (painter ? painter->device() : nullptr);
}

QPainter *Surface_impl::GetPainter()
{
    if (!painter) {
        if (device->paintingActive()) {
            painter = device->paintEngine()->painter();
        }
        else {
            painterOwned = true;
            painter      = new QPainter(device);
        }

        // Set text antialiasing unconditionally.
        // The font's style strategy will override.
        painter->setRenderHint(QPainter::TextAntialiasing, true);
        painter->setRenderHint(QPainter::Antialiasing, false);
    }

    return painter;
}

void Surface_impl::SetCaptureOnly(bool value)
{
    capture_only = value;
}

std::unique_ptr<Surface> Surface::Allocate(Technology)
{
    return std::make_unique<Surface_impl>();
}


//----------------------------------------------------------------------

namespace {

QQuickItem *window(WindowID wid) noexcept
{
    return static_cast<QQuickItem *>(wid);
}

QRect ScreenRectangleForPoint(QPoint posGlobal)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const QScreen *screen = QGuiApplication::screenAt(posGlobal);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
#else
    const QScreen *screen = QGuiApplication::primaryScreen();
#endif
    return screen ? screen->availableGeometry() : QRect();
}

}

Window::~Window() noexcept = default;

void Window::Destroy() noexcept
{
    if (wid) {
        delete window(wid);
    }
    wid = nullptr;
}
PRectangle Window::GetPosition() const
{
    // Before any size allocated pretend its 1000 wide so not scrolled
    return wid ? PRectangle(window(wid)->x(), window(wid)->y(),
                window(wid)->x() + window(wid)->width(),
                window(wid)->y() + window(wid)->height()) : PRectangle(0, 0, 1000, 1000);
}

void Window::SetPosition(PRectangle rc)
{
    if (wid) {
        QRect a_rect = QRectFromPRect(rc);
        window(wid)->setPosition(QPointF(a_rect.x(),a_rect.y()));
        window(wid)->setSize(QSizeF(a_rect.width(),a_rect.height()));
    }
}

void Window::SetPositionRelative(PRectangle rc, const Window *relativeTo)
{
    QPointF origin = window(relativeTo->wid)->mapToGlobal(QPointF(0,0));
    int ox         = origin.x();
    int oy         = origin.y();
    ox += rc.left;
    oy += rc.top;

    const QRect rect_desk = ScreenRectangleForPoint(QPoint(ox, oy));
    /* do some corrections to fit into screen */
    int sizex       = rc.right - rc.left;
    int sizey       = rc.bottom - rc.top;
    int screen_width = rect_desk.width();
    if (ox < rect_desk.x())
        ox = rect_desk.x();
    if (sizex > screen_width)
        ox = rect_desk.x(); /* the best we can do */
    else if (ox + sizex > rect_desk.right())
        ox = rect_desk.right() - sizex;
    if (oy + sizey > rect_desk.bottom())
        oy = rect_desk.bottom() - sizey;
    if (oy < rect_desk.top())
        oy = rect_desk.top();

    Q_ASSERT(wid);
    QQuickItem *target           = window(wid);
    QQuickItem *target_parent    = target->parentItem();
    const QPointF local_position = target_parent ? target_parent->mapFromGlobal(QPointF(ox, oy)) : QPointF(ox, oy);
    target->setPosition(local_position);
    window(wid)->setSize(QSizeF(sizex, sizey));
}

PRectangle Window::GetClientPosition() const
{
    // The client position is the window position
    return GetPosition();
}

void Window::Show(bool show)
{
    if (wid)
        window(wid)->setVisible(show);
}

void Window::InvalidateAll()
{
    if (wid) {
        if (auto *item = qobject_cast<ScintillaQuick_item *>(window(wid))) {
            item->request_scene_graph_update();
        }
        else {
            window(wid)->update();
        }
    }
}

void Window::InvalidateRectangle(PRectangle rc)
{
    Q_UNUSED(rc);
    if (wid) {
        if (auto *item = qobject_cast<ScintillaQuick_item *>(window(wid))) {
            item->request_scene_graph_update();
        }
        else {
            window(wid)->update();
        }
    }
}

void Window::SetCursor(Cursor curs)
{
    if (wid) {
        Qt::CursorShape shape;

        switch (curs) {
            case Cursor::text:       shape = Qt::IBeamCursor;        break;
            case Cursor::arrow:      shape = Qt::ArrowCursor;        break;
            case Cursor::up:         shape = Qt::UpArrowCursor;      break;
            case Cursor::wait:       shape = Qt::WaitCursor;         break;
            case Cursor::horizontal: shape = Qt::SizeHorCursor;      break;
            case Cursor::vertical:   shape = Qt::SizeVerCursor;      break;
            case Cursor::hand:       shape = Qt::PointingHandCursor; break;
            default:                 shape = Qt::ArrowCursor;        break;
        }

        QCursor cursor = QCursor(shape);

        if (curs != cursorLast) {
            window(wid)->setCursor(cursor);
            cursorLast = curs;
        }
    }
}

/* Returns rectangle of monitor pt is on, both rect and pt are in Window's
   window coordinates */
PRectangle Window::GetMonitorRect(Point pt)
{
    const QPointF originGlobal = window(wid)->mapToGlobal(QPoint(0, 0));
    const QPointF posGlobal    = window(wid)->mapToGlobal(QPoint(pt.x, pt.y));
    QRect rectScreen           = ScreenRectangleForPoint(QPoint(posGlobal.x(), posGlobal.y()));
    rectScreen.translate(-originGlobal.x(), -originGlobal.y());
    return PRectFromQRect(rectScreen);
}

//----------------------------------------------------------------------
struct Quick_list_entry {
    QString text;
    int type = -1;
};

class Quick_list_box_item final : public QQuickItem {
public:
    explicit Quick_list_box_item(QQuickItem *parent)
        : QQuickItem(parent)
    {
        setAcceptedMouseButtons(Qt::LeftButton);
        setAcceptHoverEvents(false);
        setClip(true);
        setFlag(QQuickItem::ItemHasContents, true);
        setVisible(false);
        setZ(1000.0);
    }

    void setListFont(const QFont &font)
    {
        m_font = font;
        update();
    }

    void setVisibleRowCount(int rows)
    {
        m_visible_rows = std::max(1, rows);
        clampState();
        update();
    }

    int visibleRowCount() const noexcept
    {
        return m_visible_rows;
    }

    void clearItems() noexcept
    {
        m_entries.clear();
        m_current_row = -1;
        m_top_row     = 0;
        update();
    }

    void appendItem(QString text, int type)
    {
        m_entries.push_back({std::move(text), type});
        update();
    }

    int count() const noexcept
    {
        return static_cast<int>(m_entries.size());
    }

    void setSelection(int row, bool notify_delegate)
    {
        if (m_entries.empty()) {
            m_current_row = -1;
            m_top_row     = 0;
            update();
            return;
        }

        const int bounded_row = std::clamp(row, 0, count() - 1);
        const bool changed    = bounded_row != m_current_row;
        m_current_row         = bounded_row;
        ensureVisible(m_current_row);
        update();

        if (changed && notify_delegate && m_delegate) {
            ListBoxEvent event(ListBoxEvent::EventType::selectionChange);
            m_delegate->ListNotify(&event);
        }
    }

    int currentSelection() const noexcept
    {
        return m_current_row;
    }

    int findPrefix(const QString &prefix) const noexcept
    {
        for (int index = 0; index < count(); ++index) {
            if (m_entries[static_cast<size_t>(index)].text.startsWith(prefix)) {
                return index;
            }
        }
        return -1;
    }

    QString valueAt(int row) const
    {
        if (row < 0 || row >= count()) {
            return {};
        }
        return m_entries[static_cast<size_t>(row)].text;
    }

    void registerImage(int type, const QPixmap &pixmap)
    {
        m_images[type] = pixmap;
        update();
    }

    void clearImages() noexcept
    {
        m_images.clear();
        update();
    }

    void setDelegate(IListBoxDelegate *delegate) noexcept
    {
        m_delegate = delegate;
    }

    void setOptions(const ListOptions &options)
    {
        m_options = options;
        update();
    }

    int caretFromEdge() const
    {
        return maxIconWidth() + frameWidth() * 2 + horizontalPadding() * 2 + (maxIconWidth() > 0 ? iconSpacing() : 0);
    }

    PRectangle desiredRect() const
    {
        const int rows = std::max(1, std::min(count(), m_visible_rows));
        return PRectangle(0, 0, desiredWidth(), rows * rowHeight() + frameWidth() * 2);
    }

    QSGNode *updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *) override
    {
        QQuickWindow *window = this->window();
        if (!window || width() <= 0.0 || height() <= 0.0) {
            delete old_node;
            return nullptr;
        }

        delete old_node;
        m_layouts.clear();

        auto *root             = new QSGNode();
        const QPalette palette = QGuiApplication::palette();
        const QColor base_color = m_options.back
            ? QColorFromColourRGBA(*m_options.back)
            : palette.base().color();
        const QColor text_color = m_options.fore
            ? QColorFromColourRGBA(*m_options.fore)
            : palette.text().color();
        const QColor selected_back_color = m_options.backSelected
            ? QColorFromColourRGBA(*m_options.backSelected)
            : palette.highlight().color();
        const QColor selected_text_color = m_options.foreSelected
            ? QColorFromColourRGBA(*m_options.foreSelected)
            : palette.highlightedText().color();
        const QRectF item_rect = boundingRect();
        const QFont font       = resolvedFont();
        const QFontMetricsF metrics(font);
        const int rows = std::max(0, std::min(m_visible_rows, count() - m_top_row));

        if (auto *background = window->createRectangleNode()) {
            background->setRect(item_rect);
            background->setColor(base_color);
            root->appendChildNode(background);
        }

        const QColor border_color = palette.mid().color();
        if (auto *top_border = window->createRectangleNode()) {
            top_border->setRect(QRectF(item_rect.left(), item_rect.top(), item_rect.width(), 1.0));
            top_border->setColor(border_color);
            root->appendChildNode(top_border);
        }
        if (auto *bottom_border = window->createRectangleNode()) {
            bottom_border->setRect(QRectF(item_rect.left(), item_rect.bottom() - 1.0, item_rect.width(), 1.0));
            bottom_border->setColor(border_color);
            root->appendChildNode(bottom_border);
        }
        if (auto *left_border = window->createRectangleNode()) {
            left_border->setRect(QRectF(item_rect.left(), item_rect.top(), 1.0, item_rect.height()));
            left_border->setColor(border_color);
            root->appendChildNode(left_border);
        }
        if (auto *right_border = window->createRectangleNode()) {
            right_border->setRect(QRectF(item_rect.right() - 1.0, item_rect.top(), 1.0, item_rect.height()));
            right_border->setColor(border_color);
            root->appendChildNode(right_border);
        }

        for (int row = 0; row < rows; ++row) {
            const int index = m_top_row + row;
            const QRectF row_rect(
                frameWidth(),
                frameWidth() + row * rowHeight(),
                width() - frameWidth() * 2,
                rowHeight());
            const bool selected           = index == m_current_row;
            const QColor row_background   = selected ? selected_back_color : base_color;
            const QColor row_foreground   = selected ? selected_text_color : text_color;
            const Quick_list_entry &entry = m_entries[static_cast<size_t>(index)];

            if (auto *row_background_node = window->createRectangleNode()) {
                row_background_node->setRect(row_rect);
                row_background_node->setColor(row_background);
                root->appendChildNode(row_background_node);
            }

            qreal x            = row_rect.left() + horizontalPadding();
            const QPixmap icon = iconFor(index);
            if (!icon.isNull()) {
                const qreal icon_y = row_rect.top() + std::max(0.0, (row_rect.height() - icon.height()) * 0.5);
                if (auto *icon_node = window->createImageNode()) {
                    const QImage icon_image = icon.toImage();
                    QSGTexture *texture     = window->createTextureFromImage(icon_image);
                    icon_node->setTexture(texture);
                    icon_node->setOwnsTexture(true);
                    icon_node->setRect(QRectF(x, icon_y, icon.width(), icon.height()));
                    icon_node->setSourceRect(QRectF(0.0, 0.0, icon_image.width(), icon_image.height()));
                    icon_node->setFiltering(QSGTexture::Linear);
                    root->appendChildNode(icon_node);
                }
                x += icon.width() + iconSpacing();
            }

            auto layout = std::make_unique<QTextLayout>(entry.text, font);
            QTextOption option;
            option.setWrapMode(QTextOption::NoWrap);
            layout->setTextOption(option);
            layout->beginLayout();
            QTextLine line = layout->createLine();
            if (line.isValid()) {
                line.setLineWidth(1000000.0);
                line.setPosition(QPointF(0.0, 0.0));
            }
            layout->endLayout();

            if (auto *text_node = window->createTextNode()) {
                text_node->setRenderType(map_text_render_type());
                text_node->setColor(row_foreground);
                text_node->setViewport(item_rect);
                text_node->clear();
                text_node->addTextLayout(
                    QPointF(x, row_rect.top() + (row_rect.height() - metrics.height()) * 0.5),
                    layout.get());
                root->appendChildNode(text_node);
            }
            m_layouts.push_back(std::move(layout));
        }

        return root;
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        const int row = rowAt(event->position().y());
        if (row >= 0) {
            setSelection(row, true);
            event->accept();
            return;
        }
        QQuickItem::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const int row = rowAt(event->position().y());
        if (row >= 0) {
            setSelection(row, true);
            if (m_delegate) {
                ListBoxEvent notify_event(ListBoxEvent::EventType::doubleClick);
                m_delegate->ListNotify(&notify_event);
            }
            event->accept();
            return;
        }
        QQuickItem::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (count() <= m_visible_rows) {
            event->ignore();
            return;
        }

        const int delta = event->angleDelta().y();
        if (delta == 0) {
            event->ignore();
            return;
        }

        m_top_row = std::clamp(
            m_top_row + (delta > 0 ? -1 : 1),
            0,
            std::max(0, count() - m_visible_rows));
        update();
        event->accept();
    }

private:
    static constexpr int frameWidth() noexcept { return 1; }
    static constexpr int horizontalPadding() noexcept { return 6; }
    static constexpr int iconSpacing() noexcept { return 4; }

    QFont resolvedFont() const
    {
        if (!m_font.family().isEmpty()) {
            return m_font;
        }

        QFont font(bundled_fixed_font_family());
        if (font.family().compare(bundled_fixed_font_family(), Qt::CaseInsensitive) == 0) {
            return font;
        }

        return QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }

    int rowHeight() const
    {
        return std::max(maxIconHeight() + 4, QFontMetrics(resolvedFont()).height() + 4);
    }

    int desiredWidth() const
    {
        const QFontMetrics metrics(resolvedFont());
        int width = frameWidth() * 2 + horizontalPadding() * 2 + maxIconWidth();
        if (maxIconWidth() > 0) {
            width += iconSpacing();
        }
        for (const Quick_list_entry &entry : m_entries) {
            width = std::max(width, frameWidth() * 2 + horizontalPadding() * 2 + maxIconWidth() + (maxIconWidth() > 0 ? iconSpacing() : 0) + metrics.horizontalAdvance(entry.text));
        }
        return width;
    }

    int maxIconWidth() const
    {
        int width = 0;
        for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
            width = std::max(width, it.value().width());
        }
        return width;
    }

    int maxIconHeight() const
    {
        int height = 0;
        for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
            height = std::max(height, it.value().height());
        }
        return height;
    }

    QPixmap iconFor(int index) const
    {
        if (index < 0 || index >= count()) {
            return {};
        }
        return m_images.value(m_entries[static_cast<size_t>(index)].type);
    }

    int rowAt(qreal y) const
    {
        const int row   = static_cast<int>((y - frameWidth()) / rowHeight());
        const int index = m_top_row + row;
        return row >= 0 && index >= 0 && index < count() ? index : -1;
    }

    void ensureVisible(int row)
    {
        if (row < m_top_row) {
            m_top_row = row;
        }
        else
        if (row >= m_top_row + m_visible_rows) {
            m_top_row = row - m_visible_rows + 1;
        }
        clampState();
    }

    void clampState()
    {
        m_top_row = std::clamp(m_top_row, 0, std::max(0, count() - m_visible_rows));
        if (m_current_row >= count()) {
            m_current_row = count() - 1;
        }
    }

    std::vector<Quick_list_entry> m_entries;
    QMap<int, QPixmap> m_images;
    QFont m_font;
    IListBoxDelegate *m_delegate = nullptr;
    int m_visible_rows           =  5;
    int m_current_row            = -1;
    int m_top_row                =  0;
    std::vector<std::unique_ptr<QTextLayout>> m_layouts;
    ListOptions m_options;
};

class List_box_impl : public ListBox {
public:
    List_box_impl() noexcept;

    void SetFont(const Font *font) override;
    void Create(
        Window &parent,
        int ctrlID,
        Point location,
        int lineHeight,
        bool unicode_mode,
        Technology technology) override;
    void SetAverageCharWidth(int width) override;
    void SetVisibleRows(int rows) override;
    int GetVisibleRows() const override;
    PRectangle GetDesiredRect() override;
    int CaretFromEdge() override;
    void Clear() noexcept override;
    void Append(char *s, int type) override;
    int Length() override;
    void Select(int n) override;
    int GetSelection() override;
    int Find(const char *prefix) override;
    std::string GetValue(int n) override;
    void RegisterImage(int type, const char *xpmData) override;
    void RegisterRGBAImage(
        int type,
        int width,
        int height,
        const unsigned char *pixels_image) override;
    virtual void RegisterQPixmapImage(int type, const QPixmap &pm);
    void ClearRegisteredImages() override;
    void SetDelegate(IListBoxDelegate *delegate) override;
    void SetList(const char *list, char separator, char typesep) override;
    void SetOptions(ListOptions options) override;

    [[nodiscard]] Quick_list_box_item *GetWidget() const noexcept;
private:
    bool m_unicode_mode{false};
    int m_visible_rows{5};
    QMap<int, QPixmap> m_images;
    QFont m_font;
    bool m_font_set{false};
    IListBoxDelegate *m_delegate{nullptr};
    ListOptions m_options;
};
List_box_impl::List_box_impl() noexcept = default;

void List_box_impl::Create(Window &parent,
    int /*ctrlID*/,
    Point location,
    int /*lineHeight*/,
    bool unicode_mode,
    Technology)
{
    m_unicode_mode             = unicode_mode;
    QQuickItem *qparent        = window(parent.GetID());
    QQuickItem *overlay_parent = qparent && qparent->window() ? qparent->window()->contentItem() : qparent;
    Quick_list_box_item *list  = new Quick_list_box_item(overlay_parent);
    list->setPosition(QPointF(location.x, location.y));
    list->setVisibleRowCount(m_visible_rows);
    if (m_font_set) {
        list->setListFont(m_font);
    }
    if (m_delegate) {
        list->setDelegate(m_delegate);
    }
    for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
        list->registerImage(it.key(), it.value());
    }
    wid = list;
}
void List_box_impl::SetFont(const Font *font)
{
    const Font_and_character_set *pfacs = as_font_and_character_set(font);
    if (pfacs && pfacs->m_font) {
        m_font     = *(pfacs->m_font);
        m_font_set = true;
    }
    Quick_list_box_item *list = GetWidget();
    if (list && m_font_set) {
        list->setListFont(m_font);
    }
}
void List_box_impl::SetAverageCharWidth(int /*width*/) {}

void List_box_impl::SetVisibleRows(int rows)
{
    m_visible_rows = rows;
    if (Quick_list_box_item *list = GetWidget()) {
        list->setVisibleRowCount(rows);
    }
}

int List_box_impl::GetVisibleRows() const
{
    return m_visible_rows;
}

PRectangle List_box_impl::GetDesiredRect()
{
    Quick_list_box_item *list = GetWidget();
    return list ? list->desiredRect() : PRectangle(0, 0, 0, 0);
}

int List_box_impl::CaretFromEdge()
{
    Quick_list_box_item *list = GetWidget();
    return list ? list->caretFromEdge() : 0;
}

void List_box_impl::Clear() noexcept
{
    if (Quick_list_box_item *list = GetWidget()) {
        list->clearItems();
    }
}

void List_box_impl::Append(char *s, int type)
{
    if (Quick_list_box_item *list = GetWidget()) {
        list->appendItem(m_unicode_mode ? QString::fromUtf8(s) : QString::fromLocal8Bit(s), type);
    }
}

int List_box_impl::Length()
{
    Quick_list_box_item *list = GetWidget();
    return list ? list->count() : 0;
}

void List_box_impl::Select(int n)
{
    if (Quick_list_box_item *list = GetWidget()) {
        list->setSelection(n, true);
    }
}

int List_box_impl::GetSelection()
{
    Quick_list_box_item *list = GetWidget();
    return list ? list->currentSelection() : -1;
}
int List_box_impl::Find(const char *prefix)
{
    Quick_list_box_item *list = GetWidget();
    return list ? list->findPrefix(m_unicode_mode ? QString::fromUtf8(prefix) : QString::fromLocal8Bit(prefix)) : -1;
}
std::string List_box_impl::GetValue(int n)
{
    Quick_list_box_item *list = GetWidget();
    QString str               = list ? list->valueAt(n) : QString();
    QByteArray bytes          = m_unicode_mode ? str.toUtf8() : str.toLocal8Bit();
    return std::string(bytes.constData());
}

void List_box_impl::RegisterQPixmapImage(int type, const QPixmap &pm)
{
    m_images[type]            = pm;
    Quick_list_box_item *list = GetWidget();
    if (list) {
        list->registerImage(type, pm);
    }
}

void List_box_impl::RegisterImage(int type, const char *xpmData)
{
    XPM xpmImage(xpmData);
    RGBAImage rgbaImage(xpmImage);
    RegisterRGBAImage(type, rgbaImage.GetWidth(), rgbaImage.GetHeight(), rgbaImage.Pixels());
}

void List_box_impl::RegisterRGBAImage(int type, int width, int height, const unsigned char *pixels_image)
{
    std::vector<unsigned char> image_bytes = ImageByteSwapped(width, height, pixels_image);
    QImage image(&image_bytes[0], width, height, QImage::Format_ARGB32);
    RegisterQPixmapImage(type, QPixmap::fromImage(image));
}

void List_box_impl::ClearRegisteredImages()
{
    m_images.clear();
    if (Quick_list_box_item *list = GetWidget()) {
        list->clearImages();
    }
}

void List_box_impl::SetDelegate(IListBoxDelegate *delegate)
{
    m_delegate = delegate;
    if (Quick_list_box_item *list = GetWidget()) {
        list->setDelegate(delegate);
    }
}

void List_box_impl::SetList(const char *list, char separator, char typesep)
{
    // This method is *not* platform dependent.
    // It is borrowed from the GTK implementation.
    Clear();
    size_t count = strlen(list) + 1;
    std::vector<char> words(list, list+count);
    char *startword = &words[0];
    char *numword   = nullptr;
    int i           = 0;
    for (; words[i]; i++) {
        if (words[i] == separator) {
            words[i] = '\0';
            if (numword)
                *numword = '\0';
            Append(startword, numword?atoi(numword + 1):-1);
            startword = &words[0] + i + 1;
            numword   = nullptr;
        }
        else
        if (words[i] == typesep) {
            numword = &words[0] + i;
        }
    }
    if (startword) {
        if (numword)
            *numword = '\0';
        Append(startword, numword?atoi(numword + 1):-1);
    }
}

void List_box_impl::SetOptions(ListOptions options)
{
    m_options = options;
    if (Quick_list_box_item *list = GetWidget()) {
        list->setOptions(m_options);
    }
}

Quick_list_box_item *List_box_impl::GetWidget() const noexcept
{
    return static_cast<Quick_list_box_item *>(wid);
}

ListBox::ListBox()  noexcept = default;
ListBox::~ListBox() noexcept = default;

std::unique_ptr<ListBox> ListBox::Allocate()
{
    return std::make_unique<List_box_impl>();
}

//----------------------------------------------------------------------
Menu::Menu() noexcept : mid(nullptr) {}
void Menu::CreatePopUp()
{
    Destroy();
    mid = new QList<QPair<QString, QPair<int, bool>>>();    // text, menuId, enabled
}

void Menu::Destroy() noexcept
{
    if (mid) {
        QList<QPair<QString, QPair<int, bool>>> *menu = static_cast<QList<QPair<QString, QPair<int, bool>>> *>(mid);
        delete menu;
    }
    mid = nullptr;
}

void Menu::Show(Point pt, const Window & w)
{
    QList<QPair<QString, QPair<int, bool>>> *menu = static_cast<QList<QPair<QString, QPair<int, bool>>> *>(mid);
    ProcessScintillaContextMenu(pt, w, *menu);
    Destroy();
}

//----------------------------------------------------------------------

ColourRGBA Platform::Chrome()
{
    QColor c(Qt::gray);
    return ColourRGBA(c.red(), c.green(), c.blue());
}

ColourRGBA Platform::ChromeHighlight()
{
    QColor c(Qt::lightGray);
    return ColourRGBA(c.red(), c.green(), c.blue());
}

const char *Platform::DefaultFont()
{
    static char fontNameDefault[200] = "";
    if (!fontNameDefault[0]) {
        const QString family = bundled_fixed_font_family();
        QFont font(family);
        if (font.family().compare(family, Qt::CaseInsensitive) != 0) {
            font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        }
        const auto fontName = font.family().toUtf8();
        std::snprintf(fontNameDefault, sizeof(fontNameDefault), "%s", fontName.constData());
    }
    return fontNameDefault;
}

int Platform::DefaultFontSize()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    return font.pointSize();
}

unsigned int Platform::DoubleClickTime()
{
    return QGuiApplication::styleHints()->mouseDoubleClickInterval();
}

void Platform::DebugDisplay(const char *s) noexcept
{
    qWarning("Scintilla: %s", s);
}

void Platform::DebugPrintf(const char *format, ...) noexcept
{
    char buffer[2000];
    va_list pArguments{};
    va_start(pArguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, pArguments);
    buffer[sizeof(buffer) - 1] = '\0';
    va_end(pArguments);
    Platform::DebugDisplay(buffer);
}

bool Platform::ShowAssertionPopUps(bool /*assertionPopUps*/) noexcept
{
    return false;
}

void Platform::Assert(const char *c, const char *file, int line) noexcept
{
    char buffer[2000];
    std::snprintf(buffer, sizeof(buffer), "Assertion [%s] failed at %s %d", c, file, line);
    std::strncat(buffer, "\n", sizeof(buffer) - std::strlen(buffer) - 1);
    Platform::DebugDisplay(buffer);
}

}

