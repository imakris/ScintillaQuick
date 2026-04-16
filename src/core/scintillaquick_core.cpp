// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file ScintillaQuick_core.cpp - Qt specific subclass of ScintillaBase

#include "scintillaquick_core.h"
#include "scintillaquick_hierarchical_profiler.h"
#include "scintillaquick_platqt.h"
#include <scintillaquick/scintillaquick_item.h>

#include <QAction>
#include <QDrag>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPixmap>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QStyleHints>
#include <QTimer>
#include <QTimerEvent>

#include <array>
#include <cmath>

#include "RenderCapture.h"

#include <utility>

// This translation unit is almost entirely upstream Scintilla interop.
// Keeping the upstream namespaces open here avoids overwhelming the file
// with qualifiers while repo-owned identifiers still follow the local
// style guide.
using namespace Scintilla;
using namespace Scintilla::Internal;

namespace
{

QRectF rect_from_capture(double left, double top, double right, double bottom)
{
    return QRectF(QPointF(left, top), QPointF(right, bottom)).normalized();
}

// Convert a raw Scintilla capture rgba value (as stored in Captured_*
// structs) to a QColor. Wraps the recurrent
// `qcolor_from_rgba(X)` triple used
// across every capture-to-frame conversion below.
template <typename T>
QColor qcolor_from_rgba(T rgba)
{
    return QColorFromColourRGBA(ColourRGBA(static_cast<int>(rgba)));
}

Text_direction direction_from_capture(Capture_text_direction direction)
{
    switch (direction) {
        case Capture_text_direction::left_to_right: return Text_direction::left_to_right;
        case Capture_text_direction::right_to_left: return Text_direction::right_to_left;
        case Capture_text_direction::mixed:         return Text_direction::mixed;
        default:                                    return Text_direction::left_to_right;
    }
}

class Capture_frame_builder final : public Render_collector
{
public:
    Capture_frame_builder(Captured_frame& frame, bool capture_static_content)
    :
        m_frame(frame),
        m_capture_static_content(capture_static_content)
    {}

    bool wants_static_content() const override
    {
        return m_capture_static_content;
    }

    void begin_visual_line(const Captured_visual_line& line) override
    {
        Capture_visual_line visual_line;
        visual_line.document_line = line.document_line;
        visual_line.subline_index = line.subline_index;
        visual_line.visual_order  = line.visual_order;
        visual_line.left          = line.left;
        visual_line.top           = line.top;
        visual_line.right         = line.right;
        visual_line.bottom        = line.bottom;
        visual_line.baseline_y    = line.baseline_y;
        m_frame.visual_lines.push_back(std::move(visual_line));
        m_current_visual_line = &m_frame.visual_lines.back();
    }

    void add_text_run(const Captured_text_run& run) override
    {
        if (!m_current_visual_line) {
            return;
        }

        Capture_text_run Text_run;
        Text_run.text                = run.utf8_text;
        Text_run.foreground          = qcolor_from_rgba(run.foreground_rgba);
        Text_run.x                   = run.x;
        Text_run.width               = run.width;
        Text_run.top                 = run.top;
        Text_run.bottom              = run.bottom;
        Text_run.blob_text_left      = run.blob_text_left;
        Text_run.blob_text_top       = run.blob_text_top;
        Text_run.blob_text_right     = run.blob_text_right;
        Text_run.blob_text_bottom    = run.blob_text_bottom;
        Text_run.blob_outer_left     = run.blob_outer_left;
        Text_run.blob_outer_top      = run.blob_outer_top;
        Text_run.blob_outer_right    = run.blob_outer_right;
        Text_run.blob_outer_bottom   = run.blob_outer_bottom;
        Text_run.blob_inner_left     = run.blob_inner_left;
        Text_run.blob_inner_top      = run.blob_inner_top;
        Text_run.blob_inner_right    = run.blob_inner_right;
        Text_run.blob_inner_bottom   = run.blob_inner_bottom;
        Text_run.baseline_y          = run.baseline_y;
        Text_run.style_id            = run.style_id;
        Text_run.blob_outer          = qcolor_from_rgba(run.blob_outer_rgba);
        Text_run.blob_inner          = qcolor_from_rgba(run.blob_inner_rgba);
        Text_run.direction           = direction_from_capture(run.direction);
        Text_run.is_represented_text = run.is_represented_text;
        Text_run.represented_as_blob = run.represented_as_blob;

        m_current_visual_line->text_runs.push_back(std::move(Text_run));
    }

    void add_selection_rect(const Captured_selection_rect& rect) override
    {
        Capture_selection_primitive selection;
        selection.left    = rect.left;
        selection.top     = rect.top;
        selection.right   = rect.right;
        selection.bottom  = rect.bottom;
        selection.rgba    = rect.rgba;
        selection.is_main = rect.is_main;
        m_frame.selection_primitives.push_back(std::move(selection));
    }

    void add_caret_rect(const Captured_caret_rect& rect) override
    {
        Capture_caret_primitive caret;
        caret.left    = rect.left;
        caret.top     = rect.top;
        caret.right   = rect.right;
        caret.bottom  = rect.bottom;
        caret.rgba    = rect.rgba;
        caret.is_main = rect.is_main;
        m_frame.caret_primitives.push_back(std::move(caret));
    }

    void add_indicator_primitive(const Captured_indicator& indicator) override
    {
        Capture_indicator_primitive primitive;
        primitive.left             = indicator.left;
        primitive.top              = indicator.top;
        primitive.right            = indicator.right;
        primitive.bottom           = indicator.bottom;
        primitive.line_top         = indicator.line_top;
        primitive.line_bottom      = indicator.line_bottom;
        primitive.character_left   = indicator.character_left;
        primitive.character_top    = indicator.character_top;
        primitive.character_right  = indicator.character_right;
        primitive.character_bottom = indicator.character_bottom;
        primitive.stroke_width     = indicator.stroke_width;
        primitive.fill_alpha       = indicator.fill_alpha;
        primitive.outline_alpha    = indicator.outline_alpha;
        primitive.rgba             = indicator.rgba;
        primitive.indicator_number = indicator.indicator_number;
        primitive.indicator_style  = indicator.indicator_style;
        primitive.under_text       = indicator.under_text;
        primitive.is_main          = indicator.is_main;
        m_frame.indicator_primitives.push_back(std::move(primitive));
    }

    void add_current_line_highlight(const Captured_current_line_highlight& highlight) override
    {
        Capture_current_line_primitive primitive;
        primitive.left   = highlight.left;
        primitive.top    = highlight.top;
        primitive.right  = highlight.right;
        primitive.bottom = highlight.bottom;
        primitive.rgba   = highlight.rgba;
        primitive.framed = highlight.framed;
        m_frame.current_line_primitives.push_back(std::move(primitive));
    }

    void add_marker_symbol(const Captured_marker_symbol& marker) override
    {
        Capture_marker_primitive primitive;
        primitive.left               = marker.left;
        primitive.top                = marker.top;
        primitive.right              = marker.right;
        primitive.bottom             = marker.bottom;
        primitive.marker_number      = marker.marker_number;
        primitive.marker_type        = marker.marker_type;
        primitive.fore_rgba          = marker.fore_rgba;
        primitive.back_rgba          = marker.back_rgba;
        primitive.back_rgba_selected = marker.back_rgba_selected;
        primitive.document_line      = marker.document_line;
        primitive.fold_part          = marker.fold_part;
        m_frame.marker_primitives.push_back(std::move(primitive));
    }

    void add_margin_text(const Captured_margin_text& text) override
    {
        Capture_margin_text_primitive primitive;
        primitive.text          = text.utf8_text;
        primitive.x             = text.x;
        primitive.y             = text.y;
        primitive.left          = text.left;
        primitive.top           = text.top;
        primitive.right         = text.right;
        primitive.bottom        = text.bottom;
        primitive.baseline_y    = text.baseline_y;
        primitive.document_line = text.document_line;
        primitive.subline_index = text.subline_index;
        primitive.style_id      = text.style_id;
        m_frame.margin_text_primitives.push_back(std::move(primitive));
    }

    void add_fold_display_text(const Captured_fold_display_text& text) override
    {
        Capture_fold_display_text primitive;
        primitive.text          = text.utf8_text;
        primitive.left          = text.left;
        primitive.top           = text.top;
        primitive.right         = text.right;
        primitive.bottom        = text.bottom;
        primitive.baseline_y    = text.baseline_y;
        primitive.style_id      = text.style_id;
        primitive.fore_rgba     = text.fore_rgba;
        primitive.back_rgba     = text.back_rgba;
        primitive.document_line = text.document_line;
        primitive.boxed         = text.boxed;
        m_frame.fold_display_texts.push_back(std::move(primitive));
    }

    void add_eol_annotation(const Captured_eol_annotation& annotation) override
    {
        Capture_eol_annotation primitive;
        primitive.text          = annotation.utf8_text;
        primitive.left          = annotation.left;
        primitive.top           = annotation.top;
        primitive.right         = annotation.right;
        primitive.bottom        = annotation.bottom;
        primitive.text_left     = annotation.text_left;
        primitive.baseline_y    = annotation.baseline_y;
        primitive.style_id      = annotation.style_id;
        primitive.fore_rgba     = annotation.fore_rgba;
        primitive.back_rgba     = annotation.back_rgba;
        primitive.document_line = annotation.document_line;
        primitive.visible_style = annotation.visible_style;
        m_frame.eol_annotations.push_back(std::move(primitive));
    }

    void add_annotation(const Captured_annotation& annotation) override
    {
        Capture_annotation primitive;
        primitive.text            = annotation.utf8_text;
        primitive.left            = annotation.left;
        primitive.top             = annotation.top;
        primitive.right           = annotation.right;
        primitive.bottom          = annotation.bottom;
        primitive.text_left       = annotation.text_left;
        primitive.baseline_y      = annotation.baseline_y;
        primitive.style_id        = annotation.style_id;
        primitive.fore_rgba       = annotation.fore_rgba;
        primitive.back_rgba       = annotation.back_rgba;
        primitive.document_line   = annotation.document_line;
        primitive.annotation_line = annotation.annotation_line;
        primitive.boxed           = annotation.boxed;
        m_frame.annotations.push_back(std::move(primitive));
    }

    void add_whitespace_mark(const Captured_whitespace_mark& mark) override
    {
        Capture_whitespace_mark primitive;
        primitive.left   = mark.left;
        primitive.top    = mark.top;
        primitive.right  = mark.right;
        primitive.bottom = mark.bottom;
        primitive.mid_y  = mark.mid_y;
        primitive.rgba   = mark.rgba;
        primitive.kind =
            (mark.kind == Whitespace_mark_kind::tab_arrow)
                ? Whitespace_mark_kind_t::tab_arrow
                : Whitespace_mark_kind_t::space_dot;
        m_frame.whitespace_marks.push_back(std::move(primitive));
    }

    void add_decoration_underline(const Captured_decoration_underline& underline) override
    {
        Capture_decoration_underline primitive;
        primitive.left   = underline.left;
        primitive.top    = underline.top;
        primitive.right  = underline.right;
        primitive.bottom = underline.bottom;
        primitive.rgba   = underline.rgba;
        primitive.kind =
            (underline.kind == Decoration_kind::hotspot)
                ? Decoration_kind_t::hotspot
                : Decoration_kind_t::style_underline;
        m_frame.decoration_underlines.push_back(std::move(primitive));
    }

    void add_indent_guide(const Captured_indent_guide& guide) override
    {
        Capture_indent_guide primitive;
        primitive.x         = guide.x;
        primitive.top       = guide.top;
        primitive.bottom    = guide.bottom;
        primitive.rgba      = guide.rgba;
        primitive.highlight = guide.highlight;
        m_frame.indent_guides.push_back(std::move(primitive));
    }

    void end_visual_line() override
    {
        m_current_visual_line = nullptr;
    }

private:
    Captured_frame& m_frame;
    bool m_capture_static_content = true;
    Capture_visual_line* m_current_visual_line = nullptr;
};

} // namespace

ScintillaQuick_core::ScintillaQuick_core(::ScintillaQuick_item* parent)
:
    QObject(parent),
    m_owner(parent),
    m_v_max(0),
    m_h_max(0),
    m_v_page(0),
    m_h_page(0),
    m_have_mouse_capture(false),
    m_drag_was_dropped(false),
    m_rectangular_selection_modifier(SCMOD_ALT),
    m_current_painter(nullptr)
{
    wMain = static_cast<QQuickItem*>(m_owner); // Scintilla wMain stores the platform window handle.

    imeInteraction = IMEInteraction::Inline;

    // On macOS drawing text into a pixmap moves it around 1 pixel to
    // the right compared to drawing it directly onto a window.
    // Buffered drawing turned off by default to avoid this.
    view.bufferedDraw = false;

    Init();

    std::fill(timers, std::end(timers), 0);
}

void ScintillaQuick_core::UpdateInfos(int winId)
{
    SetCtrlID(winId);
}

void ScintillaQuick_core::ensure_visible_range_styled(bool scrolling)
{
    StyleAreaBounded(GetClientDrawingRectangle(), scrolling);
}

void ScintillaQuick_core::selectCurrentWord()
{
    auto pos = CurrentPosition();
    const auto max = pdoc->Length();
    if (max <= 0) {
        return;
    }

    if (pos < 0) {
        pos = 0;
    }
    else
    if (pos >= max) {
        pos = max - 1;
    }

    if (pos > 0 &&
        !std::isalnum(static_cast<unsigned char>(pdoc->CharAt(pos))) &&
        std::isalnum(static_cast<unsigned char>(pdoc->CharAt(pos - 1))))
    {
        pos--;
    }

    auto start_pos = pos;
    while (start_pos > 0 && std::isalnum(static_cast<unsigned char>(pdoc->CharAt(start_pos - 1)))) {
        start_pos--;
    }

    auto end_pos = pos + 1;
    while (end_pos < max && std::isalnum(static_cast<unsigned char>(pdoc->CharAt(end_pos)))) {
        end_pos++;
    }

    if (start_pos == end_pos) {
        return;
    }

    SetSelection(start_pos, end_pos);

    emit cursorPositionChanged();
}

struct ScintillaQuick_core::Style_attributes
{
    QColor foreground;
    QColor background;
    QFont font;
};

ScintillaQuick_core::Style_attributes ScintillaQuick_core::style_attributes_for(int style) const
{
    Style_attributes attributes;
    const int bounded_style      = std::clamp(style, 0, STYLE_MAX);
    const Style& scintilla_style = vs.styles[static_cast<size_t>(bounded_style)];
    const Style& default_style   = vs.styles[StyleDefault];

    attributes.foreground = QColorFromColourRGBA(scintilla_style.fore);
    attributes.background = QColorFromColourRGBA(scintilla_style.back);

    const char* font_name =
        scintilla_style.fontName ? scintilla_style.fontName : default_style.fontName;
    if (font_name) {
        attributes.font.setFamily(QString::fromUtf8(font_name));
    }
    const int size_zoomed =
        scintilla_style.sizeZoomed > 0 ? scintilla_style.sizeZoomed : default_style.sizeZoomed;
    if (size_zoomed > 0) {
        attributes.font.setPointSizeF(static_cast<qreal>(size_zoomed) / SC_FONT_SIZE_MULTIPLIER);
    }
    const int weight = static_cast<int>(scintilla_style.weight) > 0
        ? static_cast<int>(scintilla_style.weight)
        : static_cast<int>(default_style.weight);
    if (weight > 0) {
        attributes.font.setWeight(static_cast<QFont::Weight>(weight));
    }
    attributes.font.setItalic(scintilla_style.italic);
    // Underlines are rendered from Scintilla's captured decoration primitives.
    attributes.font.setUnderline(false);

    return attributes;
}

Render_frame ScintillaQuick_core::render_frame_from_capture(const Captured_frame& capture_frame) const
{
    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("core.render_frame_from_capture");

    Render_frame frame;
    std::array<std::optional<Style_attributes>, STYLE_MAX + 1> Style_cache;
    const auto attributes_for = [&](int style) -> const Style_attributes& {
        const int bounded_style                 = std::clamp(style, 0, STYLE_MAX);
        std::optional<Style_attributes>& cached = Style_cache[bounded_style];
        if (!cached.has_value()) {
            cached = style_attributes_for(bounded_style);
        }
        return *cached;
    };

    frame.text_rect = QRectF(
        capture_frame.text_left,
        capture_frame.text_top,
        capture_frame.text_width,
        capture_frame.text_height);
    frame.margin_rect = QRectF(
        capture_frame.margin_left,
        capture_frame.margin_top,
        capture_frame.margin_width,
        capture_frame.margin_height);

    frame.visual_lines.reserve(capture_frame.visual_lines.size());
    for (const Capture_visual_line& capture_line : capture_frame.visual_lines) {
        Visual_line_frame visual_line;
        visual_line.key.document_line = capture_line.document_line;
        visual_line.key.subline_index = capture_line.subline_index;
        visual_line.visual_order      = capture_line.visual_order;
        visual_line.origin            = QPointF(capture_line.left, capture_line.top);
        visual_line.baseline_y        = capture_line.baseline_y;
        visual_line.clip_rect = rect_from_capture(
            capture_line.left,
            capture_line.top,
            capture_line.right,
            capture_line.bottom);

        visual_line.text_runs.reserve(capture_line.text_runs.size());
        for (const Capture_text_run& capture_run : capture_line.text_runs) {
            const Style_attributes& attributes = attributes_for(capture_run.style_id);

            Text_run run;
            run.text     = QString::fromUtf8(capture_run.text.data(), static_cast<int>(capture_run.text.size()));
            run.position = QPointF(capture_run.x, capture_run.baseline_y);
            run.width    = capture_run.width;
            run.top      = capture_run.top;
            run.bottom   = capture_run.bottom;
            run.blob_text_clip_rect = rect_from_capture(
                capture_run.blob_text_left,
                capture_run.blob_text_top,
                capture_run.blob_text_right,
                capture_run.blob_text_bottom);
            run.blob_outer_rect     = rect_from_capture(
                capture_run.blob_outer_left,
                capture_run.blob_outer_top,
                capture_run.blob_outer_right,
                capture_run.blob_outer_bottom);
            run.blob_inner_rect     = rect_from_capture(
                capture_run.blob_inner_left,
                capture_run.blob_inner_top,
                capture_run.blob_inner_right,
                capture_run.blob_inner_bottom);
            run.foreground          = capture_run.foreground.isValid()
                ? capture_run.foreground
                : attributes.foreground;
            run.blob_outer          = capture_run.blob_outer;
            run.blob_inner          = capture_run.blob_inner;
            run.font                = attributes.font;
            run.style_id            = capture_run.style_id;
            run.direction           = capture_run.direction;
            run.is_represented_text = capture_run.is_represented_text;
            run.represented_as_blob = capture_run.represented_as_blob;
            visual_line.text_runs.push_back(std::move(run));
        }

        frame.visual_lines.push_back(std::move(visual_line));
    }

    frame.selection_primitives.reserve(capture_frame.selection_primitives.size());
    for (const Capture_selection_primitive& capture_selection : capture_frame.selection_primitives) {
        Selection_primitive selection;
        selection.rect = rect_from_capture(
            capture_selection.left,
            capture_selection.top,
            capture_selection.right,
            capture_selection.bottom);
        selection.color   = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_selection.rgba)));
        selection.is_main = capture_selection.is_main;
        frame.selection_primitives.push_back(std::move(selection));
    }

    frame.indicator_primitives.reserve(capture_frame.indicator_primitives.size());
    for (const Capture_indicator_primitive& capture_indicator : capture_frame.indicator_primitives) {
        Indicator_primitive indicator;
        indicator.rect = rect_from_capture(
            capture_indicator.left,
            capture_indicator.top,
            capture_indicator.right,
            capture_indicator.bottom);
        indicator.line_rect = rect_from_capture(
            capture_indicator.left,
            capture_indicator.line_top,
            capture_indicator.right,
            capture_indicator.line_bottom);
        indicator.character_rect = rect_from_capture(
            capture_indicator.character_left,
            capture_indicator.character_top,
            capture_indicator.character_right,
            capture_indicator.character_bottom);
        indicator.color            = qcolor_from_rgba(capture_indicator.rgba);
        indicator.stroke_width     = capture_indicator.stroke_width;
        indicator.fill_alpha       = capture_indicator.fill_alpha;
        indicator.outline_alpha    = capture_indicator.outline_alpha;
        indicator.indicator_number = capture_indicator.indicator_number;
        indicator.indicator_style  = capture_indicator.indicator_style;
        indicator.under_text       = capture_indicator.under_text;
        indicator.is_main          = capture_indicator.is_main;
        frame.indicator_primitives.push_back(std::move(indicator));
    }

    frame.current_line_primitives.reserve(capture_frame.current_line_primitives.size());
    for (const Capture_current_line_primitive& capture_cl : capture_frame.current_line_primitives) {
        Current_line_primitive cl;
        cl.rect   = rect_from_capture(capture_cl.left, capture_cl.top, capture_cl.right, capture_cl.bottom);
        cl.color  = qcolor_from_rgba(capture_cl.rgba);
        cl.framed = capture_cl.framed;
        frame.current_line_primitives.push_back(std::move(cl));
    }

    frame.marker_primitives.reserve(capture_frame.marker_primitives.size());
    for (const Capture_marker_primitive& capture_marker : capture_frame.marker_primitives) {
        Marker_primitive marker;
        marker.rect = rect_from_capture(
            capture_marker.left,
            capture_marker.top,
            capture_marker.right,
            capture_marker.bottom);
        marker.marker_number       = capture_marker.marker_number;
        marker.marker_type         = capture_marker.marker_type;
        marker.foreground          = qcolor_from_rgba(capture_marker.fore_rgba);
        marker.background          = qcolor_from_rgba(capture_marker.back_rgba);
        marker.background_selected = qcolor_from_rgba(capture_marker.back_rgba_selected);
        marker.document_line       = capture_marker.document_line;
        marker.fold_part           = capture_marker.fold_part;
        frame.marker_primitives.push_back(std::move(marker));
    }

    frame.caret_primitives.reserve(capture_frame.caret_primitives.size());
    for (const Capture_caret_primitive& capture_caret : capture_frame.caret_primitives) {
        Caret_primitive caret;
        caret.rect = rect_from_capture(
            capture_caret.left,
            capture_caret.top,
            capture_caret.right,
            capture_caret.bottom);
        caret.color   = qcolor_from_rgba(capture_caret.rgba);
        caret.is_main = capture_caret.is_main;
        frame.caret_primitives.push_back(std::move(caret));
    }

    frame.margin_text_primitives.reserve(capture_frame.margin_text_primitives.size());
    for (const Capture_margin_text_primitive& capture_margin_text : capture_frame.margin_text_primitives) {
        const Style_attributes& attributes = attributes_for(capture_margin_text.style_id);
        const auto& text_bytes = capture_margin_text.text;

        Margin_text_primitive margin_text;
        margin_text.text       = QString::fromUtf8(text_bytes.data(), static_cast<int>(text_bytes.size()));
        margin_text.position   = QPointF(capture_margin_text.x, capture_margin_text.top);
        margin_text.baseline_y = capture_margin_text.baseline_y;
        margin_text.clip_rect  = rect_from_capture(
            capture_margin_text.left,
            capture_margin_text.top,
            capture_margin_text.right,
            capture_margin_text.bottom);
        margin_text.foreground    = attributes.foreground;
        margin_text.font          = attributes.font;
        margin_text.document_line = capture_margin_text.document_line;
        margin_text.subline_index = capture_margin_text.subline_index;
        margin_text.style_id      = capture_margin_text.style_id;
        frame.margin_text_primitives.push_back(std::move(margin_text));
    }

    frame.fold_display_texts.reserve(capture_frame.fold_display_texts.size());
    for (const Capture_fold_display_text& capture_fold : capture_frame.fold_display_texts) {
        const Style_attributes& attributes = attributes_for(capture_fold.style_id);

        Fold_display_text_primitive fold;
        fold.text       = QString::fromUtf8(capture_fold.text.data(), static_cast<int>(capture_fold.text.size()));
        fold.position   = QPointF(capture_fold.left, capture_fold.top);
        fold.baseline_y = capture_fold.baseline_y;
        fold.rect = rect_from_capture(
            capture_fold.left, capture_fold.top, capture_fold.right, capture_fold.bottom);
        fold.foreground    = qcolor_from_rgba(capture_fold.fore_rgba);
        fold.background    = qcolor_from_rgba(capture_fold.back_rgba);
        fold.font          = attributes.font;
        fold.document_line = capture_fold.document_line;
        fold.style_id      = capture_fold.style_id;
        fold.boxed         = capture_fold.boxed;
        frame.fold_display_texts.push_back(std::move(fold));
    }

    frame.eol_annotations.reserve(capture_frame.eol_annotations.size());
    for (const Capture_eol_annotation& capture_eol : capture_frame.eol_annotations) {
        const Style_attributes& attributes = attributes_for(capture_eol.style_id);
        const auto& text_bytes = capture_eol.text;

        Eol_annotation_primitive eol;
        eol.text          = QString::fromUtf8(text_bytes.data(), static_cast<int>(text_bytes.size()));
        eol.position      = QPointF(capture_eol.text_left, capture_eol.top);
        eol.baseline_y    = capture_eol.baseline_y;
        eol.rect          = rect_from_capture(
            capture_eol.left,
            capture_eol.top,
            capture_eol.right,
            capture_eol.bottom);
        eol.foreground    = qcolor_from_rgba(capture_eol.fore_rgba);
        eol.background    = qcolor_from_rgba(capture_eol.back_rgba);
        eol.font          = attributes.font;
        eol.document_line = capture_eol.document_line;
        eol.style_id      = capture_eol.style_id;
        eol.visible_style = capture_eol.visible_style;
        frame.eol_annotations.push_back(std::move(eol));
    }

    frame.annotations.reserve(capture_frame.annotations.size());
    for (const Capture_annotation& capture_annot : capture_frame.annotations) {
        const Style_attributes& attributes = attributes_for(capture_annot.style_id);
        const auto& text_bytes = capture_annot.text;

        Annotation_primitive annot;
        annot.text            = QString::fromUtf8(text_bytes.data(), static_cast<int>(text_bytes.size()));
        annot.position        = QPointF(capture_annot.text_left, capture_annot.top);
        annot.baseline_y      = capture_annot.baseline_y;
        annot.rect            = rect_from_capture(
            capture_annot.left,
            capture_annot.top,
            capture_annot.right,
            capture_annot.bottom);
        annot.foreground      = qcolor_from_rgba(capture_annot.fore_rgba);
        annot.background      = qcolor_from_rgba(capture_annot.back_rgba);
        annot.font            = attributes.font;
        annot.document_line   = capture_annot.document_line;
        annot.annotation_line = capture_annot.annotation_line;
        annot.style_id        = capture_annot.style_id;
        annot.boxed           = capture_annot.boxed;
        frame.annotations.push_back(std::move(annot));
    }

    frame.whitespace_marks.reserve(capture_frame.whitespace_marks.size());
    for (const Capture_whitespace_mark& capture_ws : capture_frame.whitespace_marks) {
        Whitespace_mark_primitive ws;
        ws.rect  = rect_from_capture(capture_ws.left, capture_ws.top, capture_ws.right, capture_ws.bottom);
        ws.mid_y = capture_ws.mid_y;
        ws.color = qcolor_from_rgba(capture_ws.rgba);
        ws.kind  = capture_ws.kind;
        frame.whitespace_marks.push_back(std::move(ws));
    }

    frame.decoration_underlines.reserve(capture_frame.decoration_underlines.size());
    for (const Capture_decoration_underline& capture_ul : capture_frame.decoration_underlines) {
        Decoration_underline_primitive ul;
        ul.rect  = rect_from_capture(capture_ul.left, capture_ul.top, capture_ul.right, capture_ul.bottom);
        ul.color = qcolor_from_rgba(capture_ul.rgba);
        ul.kind  = capture_ul.kind;
        frame.decoration_underlines.push_back(std::move(ul));
    }

    frame.indent_guides.reserve(capture_frame.indent_guides.size());
    for (const Capture_indent_guide& capture_ig : capture_frame.indent_guides) {
        Indent_guide_primitive ig;
        ig.x         = capture_ig.x;
        ig.top       = capture_ig.top;
        ig.bottom    = capture_ig.bottom;
        ig.color     = qcolor_from_rgba(capture_ig.rgba);
        ig.highlight = capture_ig.highlight;
        frame.indent_guides.push_back(std::move(ig));
    }

    return frame;
}

Captured_frame ScintillaQuick_core::capture_current_frame(
    bool static_content_dirty,
    bool ensure_styled,
    bool scrolling,
    int extra_capture_lines)
{
    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("core.capture_current_frame");

    Captured_frame frame;

    if (!m_owner) {
        return frame;
    }

    const PRectangle client_rect = GetClientRectangle();
    const PRectangle text_rect   = GetTextRectangle();

    frame.viewport_width  = client_rect.Width();
    frame.viewport_height = client_rect.Height();
    frame.text_left       = text_rect.left;
    frame.text_top        = text_rect.top;
    frame.text_width      = text_rect.Width();
    frame.text_height     = text_rect.Height();
    frame.margin_left     = client_rect.left;
    frame.margin_top      = client_rect.top;
    frame.margin_width    = std::max<XYPOSITION>(0.0, text_rect.left - client_rect.left);
    frame.margin_height   = client_rect.Height();

    if (frame.viewport_width <= 0.0 || frame.viewport_height <= 0.0) {
        return frame;
    }

    const int capture_buffer_lines = std::max(0, extra_capture_lines);
    const int estimated_lines = std::max<int>(
        1,
        static_cast<int>(client_rect.Height() / vs.lineHeight) + (capture_buffer_lines * 2) + 2);
    frame.visual_lines.reserve(estimated_lines);
    frame.selection_primitives.reserve(4);
    frame.caret_primitives.reserve(4);
    frame.margin_text_primitives.reserve(estimated_lines);

    if (ensure_styled) {
        ensure_visible_range_styled(scrolling);
    }

    const QSize capture_surface_size(1, 1);
    QImage capture_surface(capture_surface_size, QImage::Format_ARGB32_Premultiplied);
    capture_surface.fill(Qt::transparent);

    QPainter painter(&capture_surface);
    AutoSurface surface(this, &painter);
    surface->SetMode(CurrentSurfaceMode());
    if (auto* surface_impl = dynamic_cast<Surface_impl*>(static_cast<Surface*>(surface))) {
        surface_impl->SetCaptureOnly(true);
    }

    Capture_frame_builder collector(frame, static_content_dirty);
    PRectangle capture_rect = client_rect;
    if (capture_buffer_lines > 0 && vs.lineHeight > 0) {
        capture_rect.top -= static_cast<XYPOSITION>(capture_buffer_lines * vs.lineHeight);
        capture_rect.bottom += static_cast<XYPOSITION>(capture_buffer_lines * vs.lineHeight);
    }

    const bool buffered_draw_before_capture = view.bufferedDraw;
    view.bufferedDraw = false;

    {
        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("core.capture_current_frame.paint_text");
        view.PaintText(surface, *this, capture_rect, client_rect, vs, &collector);
    }

    PRectangle margin_rect = capture_rect;
    margin_rect.Move(0.0, -GetVisibleOriginInMain().y);
    margin_rect.left = 0.0;
    margin_rect.right = static_cast<XYPOSITION>(vs.fixedColumnWidth);
    if (capture_rect.Intersects(margin_rect)) {
        SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("core.capture_current_frame.paint_margin");
        marginView.PaintMargin(surface, topLine, capture_rect, margin_rect, *this, vs, &collector);
    }

    if (horizontalScrollBarVisible && trackLineWidth && (view.lineWidthMaxSeen > scrollWidth)) {
        scrollWidth = view.lineWidthMaxSeen;
        SetScrollBars();
    }

    view.bufferedDraw = buffered_draw_before_capture;

    return frame;
}

Render_frame ScintillaQuick_core::current_render_frame(
    const Captured_frame* capture_frame,
    bool static_content_dirty,
    bool ensure_styled,
    bool scrolling,
    int extra_capture_lines)
{
    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("core.current_render_frame");

    if (capture_frame) {
        return render_frame_from_capture(*capture_frame);
    }

    Captured_frame captured =
        capture_current_frame(static_content_dirty, ensure_styled, scrolling, extra_capture_lines);
    return render_frame_from_capture(captured);
}

ScintillaQuick_core::~ScintillaQuick_core()
{
    // Belt-and-braces: prepare_for_owner_destruction() is the correct
    // path and runs from ~ScintillaQuick_item() before the derived
    // subobject dies. If, for some reason, that path was skipped (for
    // instance, a direct user-managed ScintillaQuick_core deletion),
    // still try to clean up here.
    CancelTimers();
    ChangeIdle(false);
}

void ScintillaQuick_core::prepare_for_owner_destruction()
{
    // Break the clipboard connection first so a pending SelectionChanged
    // delivery cannot land between here and the final teardown.
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        disconnect(clipboard, nullptr, this, nullptr);
    }

    CancelTimers();
    ChangeIdle(false);

    // After this point, timerEvent / onIdle / any queued slot that
    // checks `m_owner` will short-circuit rather than dereference a
    // sliced-down QQuickItem.
    m_owner = nullptr;
}

void ScintillaQuick_core::execCommand(QAction* action)
{
    const int command_num = action->data().toInt();
    Command(command_num);
}

#if defined(Q_OS_WIN)
static const QString sMSDEVColumnSelect("MSDEVColumnSelect");
static const QString sWrappedMSDEVColumnSelect("application/x-qt-windows-mime;value=\"MSDEVColumnSelect\"");
static const QString sVSEditorLineCutCopy("VisualStudioEditorOperationsLineCutCopyClipboardTag");
static const QString sWrappedVSEditorLineCutCopy(
    "application/x-qt-windows-mime;value=\"VisualStudioEditorOperationsLineCutCopyClipboardTag\"");
#elif defined(Q_OS_MAC)
static const QString sScintillaRecPboardType("com.scintilla.utf16-plain-text.rectangular");
static const QString sScintillaRecMimeType("text/x-scintilla.utf16-plain-text.rectangular");
#else
// Linux
static const QString sMimeRectangularMarker("text/x-rectangular-marker");
#endif

void ScintillaQuick_core::Init()
{
    m_rectangular_selection_modifier = SCMOD_ALT;

    connect(QGuiApplication::clipboard(), SIGNAL(selectionChanged()), this, SLOT(SelectionChanged()));
}

void ScintillaQuick_core::Finalise()
{
    CancelTimers();
    ScintillaBase::Finalise();
}

void ScintillaQuick_core::SelectionChanged()
{
    bool now_primary = QGuiApplication::clipboard()->ownsSelection();
    if (now_primary != primarySelection) {
        primarySelection = now_primary;
        Redraw();
    }
}

bool ScintillaQuick_core::DragThreshold(Point pt_start, Point pt_now)
{
    int x_move = std::abs(pt_start.x - pt_now.x);
    int y_move = std::abs(pt_start.y - pt_now.y);
    return
        (x_move > QGuiApplication::styleHints()->startDragDistance()) ||
        (y_move > QGuiApplication::styleHints()->startDragDistance());
}

static QString string_from_selected_text(const SelectionText& selected_text)
{
    Q_UNUSED(selected_text.characterSet);
    return QString::fromUtf8(selected_text.Data(), static_cast<int>(selected_text.Length()));
}

static void add_rectangular_to_mime(QMimeData* mime_data, [[maybe_unused]] const QString& su)
{
    Q_UNUSED(su);
#if defined(Q_OS_WIN)
    // Add an empty marker
    mime_data->setData(sMSDEVColumnSelect, QByteArray());
#elif defined(Q_OS_MAC)
    // macOS gets marker + data to work with other implementations.
    // Don't understand how this works but it does - the
    // clipboard format is supposed to be UTF-16, not UTF-8.
    mime_data->setData(sScintillaRecMimeType, su.toUtf8());
#else
    // Linux
    // Add an empty marker
    mime_data->setData(sMimeRectangularMarker, QByteArray());
#endif
}

static void add_line_cut_copy_to_mime([[maybe_unused]] QMimeData* mime_data)
{
    Q_UNUSED(mime_data);
#if defined(Q_OS_WIN)
    // Add an empty marker
    mime_data->setData(sVSEditorLineCutCopy, QByteArray());
#endif
}

static bool is_rectangular_in_mime(const QMimeData* mime_data)
{
    QStringList formats = mime_data->formats();
    for (int i = 0; i < formats.size(); ++i) {
#if defined(Q_OS_WIN)
        // Windows rectangular markers
        // If rectangular copies made by this application, see base name.
        if (formats[i] == sMSDEVColumnSelect) {
            return true;
        }
        // Otherwise see wrapped name.
        if (formats[i] == sWrappedMSDEVColumnSelect) {
            return true;
        }
#elif defined(Q_OS_MAC)
        if (formats[i] == sScintillaRecMimeType) {
            return true;
        }
#else
        // Linux
        if (formats[i] == sMimeRectangularMarker) {
            return true;
        }
#endif
    }
    return false;
}

static bool is_line_cut_copy_in_mime(const QMimeData* mime_data)
{
    QStringList formats = mime_data->formats();
    for (int i = 0; i < formats.size(); ++i) {
#if defined(Q_OS_WIN)
        // Visual Studio Line Cut/Copy markers
        // If line cut/copy made by this application, see base name.
        if (formats[i] == sVSEditorLineCutCopy) {
            return true;
        }
        // Otherwise see wrapped name.
        if (formats[i] == sWrappedVSEditorLineCutCopy) {
            return true;
        }
#endif
    }
    return false;
}

bool ScintillaQuick_core::ValidCodePage(int code_page) const
{
    return code_page == SC_CP_UTF8;
}

std::string ScintillaQuick_core::UTF8FromEncoded(std::string_view encoded) const
{
    return std::string(encoded);
}

std::string ScintillaQuick_core::EncodedFromUTF8(std::string_view utf8) const
{
    return std::string(utf8);
}

void ScintillaQuick_core::SetVerticalScrollPos()
{
    emit verticalScrolled(topLine);
}

void ScintillaQuick_core::SetHorizontalScrollPos()
{
    emit horizontalScrolled(xOffset);
}

void ScintillaQuick_core::reset_tracked_scroll_width_to_viewport()
{
    const int viewport_width = std::max(1, static_cast<int>(GetTextRectangle().Width()));
    WndProc(Message::SetScrollWidth, static_cast<uptr_t>(viewport_width), 0);
    if (xOffset > m_h_max) {
        xOffset = m_h_max;
        SetHorizontalScrollPos();
    }
}

bool ScintillaQuick_core::ModifyScrollBars(Sci::Line nMax, Sci::Line nPage)
{
    bool modified = false;

    int v_new_page = nPage;
    int v_new_max  = nMax - v_new_page + 1;
    if (m_v_max != v_new_max || m_v_page != v_new_page) {
        m_v_max  = v_new_max;
        m_v_page = v_new_page;
        modified = true;
        emit verticalRangeChanged(m_v_max, m_v_page);
    }

    int h_new_page = GetTextRectangle().Width();
    int h_new_max  = (scrollWidth > h_new_page) ? scrollWidth - h_new_page : 0;
    if (m_h_max != h_new_max || m_h_page != h_new_page) {
        m_h_max  = h_new_max;
        m_h_page = h_new_page;
        modified = true;
        emit horizontalRangeChanged(m_h_max, m_h_page);
    }

    return modified;
}

void ScintillaQuick_core::CopyToModeClipboard(const SelectionText& selected_text, QClipboard::Mode clipboard_mode)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    QString su            = string_from_selected_text(selected_text);

    // Owned by this function until the final hand-off to
    // QClipboard::setMimeData (which takes ownership of the raw
    // pointer). If any of the helper calls below throws - unlikely
    // with pure Qt data types, but reachable through the
    // `aboutToCopy` signal, which QML/C++ client code may connect to -
    // the unique_ptr will free the mime data instead of leaking it.
    std::unique_ptr<QMimeData> mime_data = std::make_unique<QMimeData>();
    mime_data->setText(su);
    if (selected_text.rectangular) {
        add_rectangular_to_mime(mime_data.get(), su);
    }

    if (selected_text.lineCopy) {
        add_line_cut_copy_to_mime(mime_data.get());
    }

    // Allow client code to add additional data (e.g rich text).
    emit aboutToCopy(mime_data.get());

    clipboard->setMimeData(mime_data.release(), clipboard_mode);
}

void ScintillaQuick_core::Copy()
{
    if (!sel.Empty()) {
        SelectionText sel_text;
        CopySelectionRange(&sel_text);
        CopyToClipboard(sel_text);
    }
}

void ScintillaQuick_core::CopyToClipboard(const SelectionText& selected_text)
{
    CopyToModeClipboard(selected_text, QClipboard::Clipboard);
}

void ScintillaQuick_core::PasteFromMode(QClipboard::Mode clipboard_mode)
{
    QClipboard* clipboard      = QGuiApplication::clipboard();
    const QMimeData* mime_data = clipboard->mimeData(clipboard_mode);
    bool is_rectangular        = is_rectangular_in_mime(mime_data);
    bool is_line               = SelectionEmpty() && is_line_cut_copy_in_mime(mime_data);
    QString text               = clipboard->text(clipboard_mode);
    QByteArray utext           = BytesForDocument(text);
    std::string dest(utext.constData(), utext.length());
    SelectionText sel_text;
    sel_text.Copy(dest, pdoc->dbcsCodePage, CharacterSetOfDocument(), is_rectangular, false);

    UndoGroup ug(pdoc);
    ClearSelection(multiPasteMode == MultiPaste::Each);
    InsertPasteShape(sel_text.Data(), sel_text.Length(),
        is_rectangular ? PasteShape::rectangular : (is_line ? PasteShape::line : PasteShape::stream));
    EnsureCaretVisible();
}

void ScintillaQuick_core::Paste()
{
    PasteFromMode(QClipboard::Clipboard);
}

void ScintillaQuick_core::ClaimSelection()
{
    if (QGuiApplication::clipboard()->supportsSelection()) {
        // X Windows has a 'primary selection' as well as the clipboard.
        // Whenever the user selects some text, we become the primary selection
        if (!sel.Empty()) {
            primarySelection = true;
            SelectionText sel_text;
            CopySelectionRange(&sel_text);
            CopyToModeClipboard(sel_text, QClipboard::Selection);
        }
        else {
            primarySelection = false;
        }
    }
}

void ScintillaQuick_core::NotifyChange()
{
    emit notifyChange();
    emit command(
        Platform::LongFromTwoShorts(GetCtrlID(), SCEN_CHANGE),
        reinterpret_cast<sptr_t>(wMain.GetID()));
}

void ScintillaQuick_core::NotifyFocus(bool focus)
{
    if (commandEvents) {
        emit command(
            Platform::LongFromTwoShorts(
                GetCtrlID(),
                focus ? SCEN_SETFOCUS : SCEN_KILLFOCUS),
            reinterpret_cast<sptr_t>(wMain.GetID()));
    }

    Editor::NotifyFocus(focus);
}

void ScintillaQuick_core::NotifyParent(NotificationData scn)
{
    scn.nmhdr.hwndFrom = wMain.GetID();
    scn.nmhdr.idFrom   = GetCtrlID();
    emit notifyParent(scn);
}

void ScintillaQuick_core::NotifyURIDropped(const char* uri)
{
    NotificationData scn = {};
    scn.nmhdr.code       = Notification::URIDropped;
    scn.text             = uri;

    NotifyParent(scn);
}

bool ScintillaQuick_core::FineTickerRunning(TickReason reason)
{
    return timers[static_cast<size_t>(reason)] != 0;
}

void ScintillaQuick_core::FineTickerStart(TickReason reason, int millis, int /* tolerance */)
{
    FineTickerCancel(reason);
    timers[static_cast<size_t>(reason)] = startTimer(millis);
}

// CancelTimers cleans up all fine-ticker timers and is non-virtual to avoid warnings when
// called during destruction.
void ScintillaQuick_core::CancelTimers()
{
    for (size_t tr = static_cast<size_t>(TickReason::caret); tr <= static_cast<size_t>(TickReason::dwell); tr++) {
        if (timers[tr]) {
            killTimer(timers[tr]);
            timers[tr] = 0;
        }
    }
}

void ScintillaQuick_core::FineTickerCancel(TickReason reason)
{
    const size_t reason_index = static_cast<size_t>(reason);
    if (timers[reason_index]) {
        killTimer(timers[reason_index]);
        timers[reason_index] = 0;
    }
}

void ScintillaQuick_core::onIdle()
{
    // Guard against onIdle() being invoked after
    // prepare_for_owner_destruction() has nulled m_owner but before
    // the idle timer has actually stopped (queued signals).
    if (!m_owner) {
        return;
    }
    const bool continue_idling = Idle();
    if (!continue_idling) {
        SetIdle(false);
    }
}

bool ScintillaQuick_core::ChangeIdle(bool on)
{
    if (on) {
        // Start idler, if it's not running.
        if (!idler.state) {
            idler.state = true;
            // QTimer is parented to `this` and tracked via unique_ptr
            // so it is cleaned up on destruction even if
            // ChangeIdle(false) is never called. The timer fires on
            // the GUI thread; onIdle()'s return value may decide to
            // stop the timer from inside its own timeout slot via
            // SetIdle(false) -> ChangeIdle(false), which is why the
            // stop path below uses deleteLater() rather than
            // destroying the object directly.
            QTimer* timer = new QTimer(this);
            m_idle_timer.reset(timer);
            connect(timer, &QTimer::timeout, this, &ScintillaQuick_core::onIdle);
            timer->start(0);
            idler.idlerID = timer;
        }
    }
    else {
        // Stop idler, if it's running.
        if (idler.state) {
            idler.state = false;
            if (m_idle_timer) {
                QTimer* timer = m_idle_timer.release();
                timer->stop();
                // deleteLater() defers actual destruction to the
                // next event-loop iteration so we do not destroy
                // the QTimer from inside its own timeout signal
                // handler (which is undefined behaviour for a
                // direct-connection slot).
                timer->deleteLater();
            }
            idler.idlerID = {};
        }
    }
    return true;
}

bool ScintillaQuick_core::SetIdle(bool on)
{
    return ChangeIdle(on);
}

CharacterSet ScintillaQuick_core::CharacterSetOfDocument() const
{
    return vs.styles[STYLE_DEFAULT].characterSet;
}

QString ScintillaQuick_core::StringFromDocument(const char* s) const
{
    return QString::fromUtf8(s);
}

QByteArray ScintillaQuick_core::BytesForDocument(const QString& text) const
{
    return text.toUtf8();
}

std::unique_ptr<CaseFolder> ScintillaQuick_core::CaseFolderForEncoding()
{
    return std::make_unique<CaseFolderUnicode>();
}

std::string ScintillaQuick_core::CaseMapString(const std::string& s, CaseMapping caseMapping)
{
    if (s.empty() || (caseMapping == CaseMapping::same)) {
        return s;
    }

    std::string ret_mapped(s.length() * maxExpansionCaseConversion, 0);
    const size_t len_mapped = CaseConvertString(
        &ret_mapped[0],
        ret_mapped.length(),
        s.c_str(),
        s.length(),
        (caseMapping == CaseMapping::upper) ? CaseConversion::upper : CaseConversion::lower);
    ret_mapped.resize(len_mapped);
    return ret_mapped;
}

void ScintillaQuick_core::SetMouseCapture(bool on)
{
    // This is handled automatically by Qt
    if (mouseDownCaptures) {
        m_have_mouse_capture = on;
    }
}

bool ScintillaQuick_core::HaveMouseCapture()
{
    return m_have_mouse_capture;
}

void ScintillaQuick_core::StartDrag()
{
    inDragDrop = DragDrop::dragging;
    dropWentOutside = true;
    if (drag.Length() && m_owner) {
        // Build the mime data under unique_ptr ownership so an
        // exception thrown from string_from_selected_text/setText or
        // from the rectangular-marker helper does not leak it.
        std::unique_ptr<QMimeData> mime_data = std::make_unique<QMimeData>();
        const QString s_text = string_from_selected_text(drag);
        mime_data->setText(s_text);
        if (drag.rectangular) {
            add_rectangular_to_mime(mime_data.get(), s_text);
        }

        // QDrag is parented to the owning QQuickItem so Qt's object
        // tree handles the worst-case cleanup if anything below
        // throws. `deleteLater()` schedules destruction after the
        // current event-loop tick so any platform drag helpers have
        // safely finished before the object goes away. `setMimeData`
        // takes ownership of the
        // released raw pointer; `exec()` is synchronous and returns
        // once the drop has been processed.
        QPointer<QDrag> dragon = new QDrag(m_owner);
        dragon->setMimeData(mime_data.release());

        const Qt::DropAction drop_action = dragon->exec(
            static_cast<Qt::DropActions>(Qt::CopyAction | Qt::MoveAction));
        if (dragon) {
            dragon->deleteLater();
        }

        if ((drop_action == Qt::MoveAction) && dropWentOutside) {
            // Remove dragged out text
            ClearSelection();
        }
    }
    inDragDrop = DragDrop::none;
    SetDragPosition(SelectionPosition(Sci::invalidPosition));
}

class Call_tip_item : public QQuickItem
{
public:
    explicit Call_tip_item(CallTip* call_tip, QQuickItem* parent)
    :
        QQuickItem(parent),
        pct(call_tip)
    {
        setAcceptedMouseButtons(Qt::NoButton);
        setAcceptHoverEvents(false);
        setFlag(QQuickItem::ItemHasContents, true);
        setVisible(false);
        setZ(1000.0);
    }

    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override
    {
        QQuickWindow* quick_window = window();
        if (!quick_window || !pct || !pct->inCallTipMode || width() <= 0.0 || height() <= 0.0) {
            delete old_node;
            return nullptr;
        }

        const QSize image_size(
            std::max(1, static_cast<int>(std::ceil(width()))), std::max(1, static_cast<int>(std::ceil(height()))));
        QImage image(image_size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);

        QPainter painter(&image);
        std::unique_ptr<Surface> surface_window = Surface::Allocate(Technology::Default);
        surface_window->Init(false, &painter);
        surface_window->SetMode(SurfaceMode(pct->codePage, false));
        pct->PaintCT(surface_window.get());

        auto* image_node = dynamic_cast<QSGImageNode*>(old_node);
        if (!image_node) {
            delete old_node;
            image_node = quick_window->createImageNode();
        }
        QSGTexture* texture = quick_window->createTextureFromImage(image);
        image_node->setTexture(texture);
        image_node->setOwnsTexture(true);
        image_node->setRect(QRectF(QPointF(0.0, 0.0), QSizeF(image_size)));
        image_node->setSourceRect(QRectF(QPointF(0.0, 0.0), QSizeF(image_size)));
        image_node->setFiltering(QSGTexture::Linear);
        return image_node;
    }

private:
    CallTip* pct;
};

void ScintillaQuick_core::CreateCallTipWindow(PRectangle rc)
{

    if (!ct.wCallTip.Created()) {
        QQuickItem* parent_item =
            m_owner->window()
                ? m_owner->window()->contentItem()
                : static_cast<QQuickItem*>(m_owner);
        QQuickItem* call_tip_item = new Call_tip_item(&ct, parent_item);
        ct.wCallTip = call_tip_item;
        call_tip_item->setPosition(QPointF(rc.left, rc.top));
        call_tip_item->setSize(QSizeF(rc.Width(), rc.Height()));
        call_tip_item->update();
    }
}

void ScintillaQuick_core::AddToPopUp(const char* label, int cmd, bool enabled)
{
    QList<QPair<QString, QPair<int, bool>>>* menu =
        static_cast<QList<QPair<QString, QPair<int, bool>>>*>(popup.GetID());

    QPair<QString, QPair<int, bool>> item(label, QPair<int, bool>(cmd, enabled));
    menu->append(item);
}

sptr_t ScintillaQuick_core::WndProc(Message i_message, uptr_t w_param, sptr_t l_param)
{
    try {
        switch (i_message) {

            case Message::SetBidirectional:
                bidirectional = static_cast<Scintilla::Bidirectional>(w_param);
                InvalidateStyleData();
                break;

            case Message::SetIMEInteraction:
                // Only inline IME supported on Qt
                break;

            case Message::GrabFocus:
                m_owner->setFocus(true);
                m_owner->forceActiveFocus(Qt::OtherFocusReason);
                break;

            case Message::GetDirectFunction:
                return reinterpret_cast<sptr_t>(DirectFunction);

            case Message::GetDirectStatusFunction:
                return reinterpret_cast<sptr_t>(DirectStatusFunction);

            case Message::GetDirectPointer:
                return reinterpret_cast<sptr_t>(this);

            case Message::SetRectangularSelectionModifier:
                m_rectangular_selection_modifier = static_cast<int>(w_param);
                break;

            case Message::GetRectangularSelectionModifier:
                return m_rectangular_selection_modifier;

            default:
                return ScintillaBase::WndProc(i_message, w_param, l_param);
        }
    }
    catch (std::bad_alloc&) {
        errorStatus = Status::BadAlloc;
    }
    catch (...) {
        errorStatus = Status::Failure;
    }
    return 0;
}

sptr_t ScintillaQuick_core::DefWndProc(Message, uptr_t, sptr_t)
{
    return 0;
}

sptr_t ScintillaQuick_core::DirectFunction(sptr_t ptr, unsigned int i_message, uptr_t w_param, sptr_t l_param)
{
    ScintillaQuick_core* sci = reinterpret_cast<ScintillaQuick_core*>(ptr);
    return sci->WndProc(static_cast<Message>(i_message), w_param, l_param);
}

sptr_t ScintillaQuick_core::DirectStatusFunction(
    sptr_t       ptr,
    unsigned int i_message,
    uptr_t       w_param,
    sptr_t       l_param,
    int*         p_status)
{
    ScintillaQuick_core* sci  = reinterpret_cast<ScintillaQuick_core*>(ptr);
    const sptr_t return_value = sci->WndProc(static_cast<Message>(i_message), w_param, l_param);
    *p_status                 = static_cast<int>(sci->errorStatus);
    return return_value;
}

// Additions to merge in Scientific Toolworks widget structure

void ScintillaQuick_core::PartialPaint(const PRectangle& rect)
{
    PartialPaintQml(rect, nullptr);
}

void ScintillaQuick_core::PartialPaintQml(const PRectangle& rect, QPainter* painter)
{
    m_current_painter   = painter;
    rcPaint             = rect;
    paintState          = PaintState::painting;
    PRectangle rcClient = GetClientRectangle();
    // TODO: analyze repaint problem when LineEnd should be marked...
    paintingAllText     = rcPaint.Contains(rcClient);

    AutoSurface surfacePaint(this, painter);
    Paint(surfacePaint, rcPaint);
    surfacePaint->Release();

    if (paintState == PaintState::abandoned) {
        // FIXME: Failure to paint the requested rectangle in each
        // paint event causes flicker on some platforms (Mac?)
        // Paint rect immediately.
        paintState = PaintState::painting;
        paintingAllText = true;

        AutoSurface surface(this, painter);
        Paint(surface, rcPaint);
        surface->Release();

        // Queue a full repaint.
        m_owner->update();
    }

    paintState = PaintState::notPainting;
    m_current_painter = nullptr;
}

void ScintillaQuick_core::DragEnter(const Point& point)
{
    SetDragPosition(SPositionFromLocation(point, false, false, UserVirtualSpace()));
}

void ScintillaQuick_core::DragMove(const Point& point)
{
    DragEnter(point);
}

void ScintillaQuick_core::DragLeave()
{
    SetDragPosition(SelectionPosition(Sci::invalidPosition));
}

void ScintillaQuick_core::Drop(const Point& point, const QMimeData* data, bool move)
{
    QString text     = data->text();
    bool rectangular = is_rectangular_in_mime(data);
    QByteArray bytes = BytesForDocument(text);
    int len          = bytes.length();

    SelectionPosition move_pos = SPositionFromLocation(point, false, false, UserVirtualSpace());

    DropAt(move_pos, bytes, len, move, rectangular);
}

void ScintillaQuick_core::DropUrls(const QMimeData* data)
{
    foreach (const QUrl& url, data->urls()) {
        NotifyURIDropped(url.toString().toUtf8().constData());
    }
}

void ScintillaQuick_core::timerEvent(QTimerEvent* event)
{
    // If the owning item is already destructing, do not dispatch any
    // ticks. prepare_for_owner_destruction() cancels all timers and
    // nulls m_owner before the derived ScintillaQuick_item subobject
    // dies, but a timer event that was already queued before the
    // killTimer() call can still be delivered here.
    if (!m_owner) {
        return;
    }
    for (size_t tr = static_cast<size_t>(TickReason::caret); tr <= static_cast<size_t>(TickReason::dwell); tr++) {
        if (timers[tr] == event->timerId()) {
            const Sci::Line previous_top_line = topLine;
            const int previous_x_offset       = xOffset;
            TickFor(static_cast<TickReason>(tr));
            if (m_owner) {
                const bool vertical_scroll_changed   = topLine != previous_top_line;
                const bool horizontal_scroll_changed = xOffset != previous_x_offset;
                if (vertical_scroll_changed || horizontal_scroll_changed) {
                    m_owner->request_scene_graph_update(true, false, vertical_scroll_changed);
                }
                else {
                    m_owner->request_scene_graph_update();
                }
            }
        }
    }
}
