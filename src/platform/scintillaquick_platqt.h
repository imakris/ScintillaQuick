// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file scintillaquick_platqt.h
// Scintilla platform layer for Qt QML/Quick

#ifndef SCINTILLAQUICK_PLATQT_H
#define SCINTILLAQUICK_PLATQT_H

#include <algorithm>
#include <cstddef>

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <memory>

#include "Debugging.h"
#include "Geometry.h"
#include "ScintillaTypes.h"
#include "ScintillaMessages.h"
#include "Platform.h"
#include <QUrl>
#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QPaintDevice>
#include <QPainter>
#include <QHash>

class QQuickItem;

namespace Scintilla::Internal
{

struct Utf8_measure_step
{
    size_t byte_count = 1;
    int code_units = 1;
};

constexpr size_t utf8_nominal_byte_count(unsigned char lead) noexcept
{
    if (lead < 0xc0) {
        return 1;
    }
    if (lead < 0xe0) {
        return 2;
    }
    if (lead < 0xf0) {
        return 3;
    }
    if (lead < 0xf5) {
        return 4;
    }
    return 1;
}

constexpr int utf16_code_units_from_utf8_byte_count(size_t byte_count) noexcept
{
    return byte_count < 4 ? 1 : 2;
}

inline Utf8_measure_step utf8_measure_step(std::string_view text, size_t byte_index, int remaining_code_units) noexcept
{
    if (byte_index >= text.length() || remaining_code_units <= 0) {
        return {};
    }

    const size_t remaining_bytes = text.length() - byte_index;
    if (remaining_bytes == 0) {
        return {};
    }

    const unsigned char lead = static_cast<unsigned char>(text[byte_index]);
    const size_t nominal_byte_count = utf8_nominal_byte_count(lead);
    const size_t byte_count = std::clamp<size_t>(nominal_byte_count, 1, remaining_bytes);
    const int code_units = std::clamp(
        utf16_code_units_from_utf8_byte_count(byte_count),
        1,
        remaining_code_units);

    return {byte_count, code_units};
}

template <typename CursorToX>
void fill_utf8_cursor_positions_from_cursor(
    std::string_view text,
    int utf16_length,
    XYPOSITION* positions,
    CursorToX cursor_to_x)
{
    if (!positions) {
        return;
    }

    int ui = 0;
    size_t i = 0;
    while (i < text.length() && ui < utf16_length) {
        const Utf8_measure_step step = utf8_measure_step(text, i, utf16_length - ui);
        const size_t next_i = std::min(text.length(), i + step.byte_count);
        const XYPOSITION x_position = static_cast<XYPOSITION>(cursor_to_x(ui + step.code_units));
        while (i < next_i) {
            positions[i++] = x_position;
        }
        ui += step.code_units;
    }

    XYPOSITION lastPos = 0;
    if (i > 0) {
        lastPos = positions[i - 1];
    }
    while (i < text.length()) {
        positions[i++] = lastPos;
    }
}

inline QColor QColorFromColourRGBA(ColourRGBA ca)
{
    return QColor(ca.GetRed(), ca.GetGreen(), ca.GetBlue(), ca.GetAlpha());
}

inline QRect QRectFromPRect(PRectangle pr)
{
    return QRect(pr.left, pr.top, pr.Width(), pr.Height());
}

inline QRectF QRectFFromPRect(PRectangle pr)
{
    return QRectF(pr.left, pr.top, pr.Width(), pr.Height());
}

inline PRectangle PRectFromQRect(QRect qr)
{
    return PRectangle(qr.x(), qr.y(), qr.x() + qr.width(), qr.y() + qr.height());
}

inline PRectangle PRectFromQRectF(QRectF qr)
{
    return PRectangle(qr.x(), qr.y(), qr.x() + qr.width(), qr.y() + qr.height());
}

enum class Platform_owned_window_kind
{
    CallTip,
    ListBox,
};

void register_owned_window(
    Window& owner,
    QQuickItem* item,
    Platform_owned_window_kind kind) noexcept;
QQuickItem* resolve_window_item(WindowID wid) noexcept;

inline Point PointFromQPoint(QPoint qp)
{
    return Point(qp.x(), qp.y());
}

inline QPointF QPointFFromPoint(Point qp)
{
    return QPointF(qp.x, qp.y);
}

constexpr PRectangle RectangleInset(PRectangle rc, XYPOSITION delta) noexcept
{
    return PRectangle(rc.left + delta, rc.top + delta, rc.right - delta, rc.bottom - delta);
}

inline Point PointFromQPointF(QPointF qp)
{
    return Point(qp.x(), qp.y());
}

class Surface_impl : public Surface
{
private:
    QPaintDevice* m_device        = nullptr;
    QPainter*     m_painter       = nullptr;
    bool          m_device_owned  = false;
    bool          m_painter_owned = false;
    bool          m_capture_only  = false;
    SurfaceMode   m_mode;

    void Clear();

public:
    Surface_impl();
    Surface_impl(int width, int height, SurfaceMode surface_mode);
    virtual ~Surface_impl();

    void Init(bool signature_flag, Scintilla::Internal::PainterID pid) override;
    void Init(WindowID wid) override;
    void Init(SurfaceID sid, WindowID wid) override;
    std::unique_ptr<Surface> AllocatePixMap(int width, int height) override;

    void SetMode(SurfaceMode mode) override;

    void Release() noexcept override;
    int SupportsFeature(Scintilla::Supports feature) noexcept override;
    bool Initialised() override;
    void PenColour(ColourRGBA fore);
    void PenColourWidth(ColourRGBA fore, XYPOSITION strokeWidth);
    int LogPixelsY() override;
    int PixelDivisions() override;
    int DeviceHeightFont(int points) override;
    void LineDraw(Point start, Point end, Stroke stroke) override;
    void PolyLine(const Point* pts, size_t npts, Stroke stroke) override;
    void Polygon(const Point* pts, size_t npts, FillStroke fillStroke) override;
    void RectangleDraw(PRectangle rc, FillStroke fillStroke) override;
    void RectangleFrame(PRectangle rc, Stroke stroke) override;
    void FillRectangle(PRectangle rc, Fill fill) override;
    void FillRectangleAligned(PRectangle rc, Fill fill) override;
    void FillRectangle(PRectangle rc, Surface& surfacePattern) override;
    void RoundedRectangle(PRectangle rc, FillStroke fillStroke) override;
    void AlphaRectangle(PRectangle rc, XYPOSITION cornerSize, FillStroke fillStroke) override;
    void GradientRectangle(
        PRectangle                     rc,
        const std::vector<ColourStop>& stops,
        GradientOptions                options) override;
    void DrawRGBAImage(
        PRectangle           rc,
        int                  width,
        int                  height,
        const unsigned char* pixels_image) override;
    void Ellipse(PRectangle rc, FillStroke fillStroke) override;
    void Stadium(PRectangle rc, FillStroke fillStroke, Ends ends) override;
    void Copy(PRectangle rc, Point from, Surface& surfaceSource) override;

    std::unique_ptr<IScreenLineLayout> Layout(const IScreenLine* screenLine) override;

    void DrawTextNoClip(
        PRectangle       rc,
        const Font*      font,
        XYPOSITION       y_base,
        std::string_view text,
        ColourRGBA       fore,
        ColourRGBA       back) override;
    void DrawTextClipped(
        PRectangle       rc,
        const Font*      font,
        XYPOSITION       y_base,
        std::string_view text,
        ColourRGBA       fore,
        ColourRGBA       back) override;
    void DrawTextTransparent(
        PRectangle       rc,
        const Font*      font,
        XYPOSITION       y_base,
        std::string_view text,
        ColourRGBA       fore) override;
    void MeasureWidths(
        const Font*      font,
        std::string_view text,
        XYPOSITION*      positions) override;
    XYPOSITION WidthText(const Font* font, std::string_view text) override;

    void DrawTextNoClipUTF8(
        PRectangle       rc,
        const Font*      font,
        XYPOSITION       y_base,
        std::string_view text,
        ColourRGBA       fore,
        ColourRGBA       back) override;
    void DrawTextClippedUTF8(
        PRectangle       rc,
        const Font*      font,
        XYPOSITION       y_base,
        std::string_view text,
        ColourRGBA       fore,
        ColourRGBA       back) override;
    void DrawTextTransparentUTF8(
        PRectangle       rc,
        const Font*      font,
        XYPOSITION       y_base,
        std::string_view text,
        ColourRGBA       fore) override;
    void MeasureWidthsUTF8(
        const Font*      font,
        std::string_view text,
        XYPOSITION*      positions) override;
    XYPOSITION WidthTextUTF8(const Font* font, std::string_view text) override;

    XYPOSITION Ascent(const Font* font) override;
    XYPOSITION Descent(const Font* font) override;
    XYPOSITION InternalLeading(const Font* font) override;
    XYPOSITION Height(const Font* font) override;
    XYPOSITION AverageCharWidth(const Font* font) override;

    void SetClip(PRectangle rc) override;
    void PopClip() override;
    void FlushCachedState() override;
    void FlushDrawing() override;

    void BrushColour(ColourRGBA back);
    void SetFont(const Font* font);

    QPaintDevice* GetPaintDevice();
    void SetPainter(QPainter* painter);
    QPainter* GetPainter();
    void SetCaptureOnly(bool value);
};

} // namespace Scintilla::Internal

#endif /* SCINTILLAQUICK_PLATQT_H */
