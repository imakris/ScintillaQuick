//
//          Copyright (c) 1990-2011, Scientific Toolworks, Inc.
//
// The License.txt file describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//
// Additions Copyright (c) 2011 Archaeopteryx Software, Inc. d/b/a Wingware
// @file ScintillaQuickCore.cpp - Qt specific subclass of ScintillaBase
//
// Additions Copyright (c) 2020 Michael Neuroth
// Scintilla platform layer for Qt QML/Quick

#include "ScintillaQuickCore.h"
#include "scintillaquick_hierarchical_profiler.h"
#include "scintillaquick_platqt.h"
#include <scintillaquick/ScintillaQuickItem.h>

#include <QAction>
#include <QDrag>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QPainter>
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

using namespace Scintilla;
using namespace Scintilla::Internal;

namespace {

QRectF rect_from_capture(double left, double top, double right, double bottom)
{
    return QRectF(QPointF(left, top), QPointF(right, bottom)).normalized();
}

text_direction_t direction_from_capture(Capture_text_direction direction)
{
    switch (direction) {
    case Capture_text_direction::left_to_right:
        return text_direction_t::left_to_right;
    case Capture_text_direction::right_to_left:
        return text_direction_t::right_to_left;
    case Capture_text_direction::mixed:
        return text_direction_t::mixed;
    }

    return text_direction_t::left_to_right;
}

class Capture_frame_builder final : public Render_collector
{
public:
    Capture_frame_builder(Capture_frame &frame, bool capture_static_content)
    :
        m_frame(frame),
        m_capture_static_content(capture_static_content)
    {
    }

    bool wants_static_content() const override
    {
        return m_capture_static_content;
    }

    void begin_visual_line(const Captured_visual_line &line) override
    {
        capture_visual_line_t visual_line;
        visual_line.document_line = line.document_line;
        visual_line.subline_index = line.subline_index;
        visual_line.visual_order = line.visual_order;
        visual_line.left = line.left;
        visual_line.top = line.top;
        visual_line.right = line.right;
        visual_line.bottom = line.bottom;
        visual_line.baseline_y = line.baseline_y;
        m_frame.visual_lines.push_back(std::move(visual_line));
        m_current_visual_line = &m_frame.visual_lines.back();
    }

    void add_text_run(const Captured_text_run &run) override
    {
        if (!m_current_visual_line) {
            return;
        }

        capture_text_run_t text_run;
        text_run.text = run.utf8_text;
        text_run.foreground = QColorFromColourRGBA(ColourRGBA(static_cast<int>(run.foreground_rgba)));
        text_run.x = run.x;
        text_run.width = run.width;
        text_run.top = run.top;
        text_run.bottom = run.bottom;
        text_run.blob_text_left = run.blob_text_left;
        text_run.blob_text_top = run.blob_text_top;
        text_run.blob_text_right = run.blob_text_right;
        text_run.blob_text_bottom = run.blob_text_bottom;
        text_run.blob_outer_left = run.blob_outer_left;
        text_run.blob_outer_top = run.blob_outer_top;
        text_run.blob_outer_right = run.blob_outer_right;
        text_run.blob_outer_bottom = run.blob_outer_bottom;
        text_run.blob_inner_left = run.blob_inner_left;
        text_run.blob_inner_top = run.blob_inner_top;
        text_run.blob_inner_right = run.blob_inner_right;
        text_run.blob_inner_bottom = run.blob_inner_bottom;
        text_run.baseline_y = run.baseline_y;
        text_run.style_id = run.style_id;
        text_run.blob_outer = QColorFromColourRGBA(ColourRGBA(static_cast<int>(run.blob_outer_rgba)));
        text_run.blob_inner = QColorFromColourRGBA(ColourRGBA(static_cast<int>(run.blob_inner_rgba)));
        text_run.direction = direction_from_capture(run.direction);
        text_run.is_represented_text = run.is_represented_text;
        text_run.represented_as_blob = run.represented_as_blob;

        m_current_visual_line->text_runs.push_back(std::move(text_run));
    }

    void add_selection_rect(const Captured_selection_rect &rect) override
    {
        capture_selection_primitive_t selection;
        selection.left = rect.left;
        selection.top = rect.top;
        selection.right = rect.right;
        selection.bottom = rect.bottom;
        selection.rgba = rect.rgba;
        selection.is_main = rect.is_main;
        m_frame.selection_primitives.push_back(std::move(selection));
    }

    void add_caret_rect(const Captured_caret_rect &rect) override
    {
        capture_caret_primitive_t caret;
        caret.left = rect.left;
        caret.top = rect.top;
        caret.right = rect.right;
        caret.bottom = rect.bottom;
        caret.rgba = rect.rgba;
        caret.is_main = rect.is_main;
        m_frame.caret_primitives.push_back(std::move(caret));
    }

    void add_indicator_primitive(const Captured_indicator &indicator) override
    {
        capture_indicator_primitive_t primitive;
        primitive.left = indicator.left;
        primitive.top = indicator.top;
        primitive.right = indicator.right;
        primitive.bottom = indicator.bottom;
        primitive.line_top = indicator.line_top;
        primitive.line_bottom = indicator.line_bottom;
        primitive.character_left = indicator.character_left;
        primitive.character_top = indicator.character_top;
        primitive.character_right = indicator.character_right;
        primitive.character_bottom = indicator.character_bottom;
        primitive.stroke_width = indicator.stroke_width;
        primitive.fill_alpha = indicator.fill_alpha;
        primitive.outline_alpha = indicator.outline_alpha;
        primitive.rgba = indicator.rgba;
        primitive.indicator_number = indicator.indicator_number;
        primitive.indicator_style = indicator.indicator_style;
        primitive.under_text = indicator.under_text;
        primitive.is_main = indicator.is_main;
        m_frame.indicator_primitives.push_back(std::move(primitive));
    }

    void add_current_line_highlight(const Captured_current_line_highlight &highlight) override
    {
        capture_current_line_primitive_t primitive;
        primitive.left = highlight.left;
        primitive.top = highlight.top;
        primitive.right = highlight.right;
        primitive.bottom = highlight.bottom;
        primitive.rgba = highlight.rgba;
        primitive.framed = highlight.framed;
        m_frame.current_line_primitives.push_back(std::move(primitive));
    }

    void add_marker_symbol(const Captured_marker_symbol &marker) override
    {
        capture_marker_primitive_t primitive;
        primitive.left = marker.left;
        primitive.top = marker.top;
        primitive.right = marker.right;
        primitive.bottom = marker.bottom;
        primitive.marker_number = marker.marker_number;
        primitive.marker_type = marker.marker_type;
        primitive.fore_rgba = marker.fore_rgba;
        primitive.back_rgba = marker.back_rgba;
        primitive.back_rgba_selected = marker.back_rgba_selected;
        primitive.document_line = marker.document_line;
        primitive.fold_part = marker.fold_part;
        m_frame.marker_primitives.push_back(std::move(primitive));
    }

    void add_margin_text(const Captured_margin_text &text) override
    {
        capture_margin_text_primitive_t primitive;
        primitive.text = text.utf8_text;
        primitive.x = text.x;
        primitive.y = text.y;
        primitive.left = text.left;
        primitive.top = text.top;
        primitive.right = text.right;
        primitive.bottom = text.bottom;
        primitive.baseline_y = text.baseline_y;
        primitive.document_line = text.document_line;
        primitive.subline_index = text.subline_index;
        primitive.style_id = text.style_id;
        m_frame.margin_text_primitives.push_back(std::move(primitive));
    }

    void add_fold_display_text(const Captured_fold_display_text &text) override
    {
        capture_fold_display_text_t primitive;
        primitive.text = text.utf8_text;
        primitive.left = text.left;
        primitive.top = text.top;
        primitive.right = text.right;
        primitive.bottom = text.bottom;
        primitive.baseline_y = text.baseline_y;
        primitive.style_id = text.style_id;
        primitive.fore_rgba = text.fore_rgba;
        primitive.back_rgba = text.back_rgba;
        primitive.document_line = text.document_line;
        primitive.boxed = text.boxed;
        m_frame.fold_display_texts.push_back(std::move(primitive));
    }

    void add_eol_annotation(const Captured_eol_annotation &annotation) override
    {
        capture_eol_annotation_t primitive;
        primitive.text = annotation.utf8_text;
        primitive.left = annotation.left;
        primitive.top = annotation.top;
        primitive.right = annotation.right;
        primitive.bottom = annotation.bottom;
        primitive.text_left = annotation.text_left;
        primitive.baseline_y = annotation.baseline_y;
        primitive.style_id = annotation.style_id;
        primitive.fore_rgba = annotation.fore_rgba;
        primitive.back_rgba = annotation.back_rgba;
        primitive.document_line = annotation.document_line;
        primitive.visible_style = annotation.visible_style;
        m_frame.eol_annotations.push_back(std::move(primitive));
    }

    void add_annotation(const Captured_annotation &annotation) override
    {
        capture_annotation_t primitive;
        primitive.text = annotation.utf8_text;
        primitive.left = annotation.left;
        primitive.top = annotation.top;
        primitive.right = annotation.right;
        primitive.bottom = annotation.bottom;
        primitive.text_left = annotation.text_left;
        primitive.baseline_y = annotation.baseline_y;
        primitive.style_id = annotation.style_id;
        primitive.fore_rgba = annotation.fore_rgba;
        primitive.back_rgba = annotation.back_rgba;
        primitive.document_line = annotation.document_line;
        primitive.annotation_line = annotation.annotation_line;
        primitive.boxed = annotation.boxed;
        m_frame.annotations.push_back(std::move(primitive));
    }

    void add_whitespace_mark(const Captured_whitespace_mark &mark) override
    {
        capture_whitespace_mark_t primitive;
        primitive.left = mark.left;
        primitive.top = mark.top;
        primitive.right = mark.right;
        primitive.bottom = mark.bottom;
        primitive.mid_y = mark.mid_y;
        primitive.rgba = mark.rgba;
        primitive.kind = (mark.kind == Whitespace_mark_kind::tab_arrow)
            ? whitespace_mark_kind_t::tab_arrow
            : whitespace_mark_kind_t::space_dot;
        m_frame.whitespace_marks.push_back(std::move(primitive));
    }

    void add_decoration_underline(const Captured_decoration_underline &underline) override
    {
        capture_decoration_underline_t primitive;
        primitive.left = underline.left;
        primitive.top = underline.top;
        primitive.right = underline.right;
        primitive.bottom = underline.bottom;
        primitive.rgba = underline.rgba;
        primitive.kind = (underline.kind == Decoration_kind::hotspot)
            ? decoration_kind_t::hotspot
            : decoration_kind_t::style_underline;
        m_frame.decoration_underlines.push_back(std::move(primitive));
    }

    void add_indent_guide(const Captured_indent_guide &guide) override
    {
        capture_indent_guide_t primitive;
        primitive.x = guide.x;
        primitive.top = guide.top;
        primitive.bottom = guide.bottom;
        primitive.rgba = guide.rgba;
        primitive.highlight = guide.highlight;
        m_frame.indent_guides.push_back(std::move(primitive));
    }

    void end_visual_line() override
    {
        m_current_visual_line = nullptr;
    }

private:
    Capture_frame &m_frame;
    bool m_capture_static_content = true;
    capture_visual_line_t *m_current_visual_line = nullptr;
};

}

ScintillaQuickCore::ScintillaQuickCore(::ScintillaQuickItem *parent)
: QObject(parent), m_owner(parent), vMax(0),  hMax(0), vPage(0), hPage(0),
 haveMouseCapture(false), dragWasDropped(false),
 rectangularSelectionModifier(SCMOD_ALT),
 currentPainter(nullptr)
{
	wMain = static_cast<QQuickItem *>(m_owner); // Scintilla wMain stores the platform window handle.

	imeInteraction = IMEInteraction::Inline;

	// On macOS drawing text into a pixmap moves it around 1 pixel to
	// the right compared to drawing it directly onto a window.
	// Buffered drawing turned off by default to avoid this.
	view.bufferedDraw = false;

	Init();

	std::fill(timers, std::end(timers), 0);
}

void ScintillaQuickCore::UpdateInfos(int winId)
{
	SetCtrlID(winId);
}

void ScintillaQuickCore::ensure_visible_range_styled(bool scrolling)
{
    StyleAreaBounded(GetClientDrawingRectangle(), scrolling);
}

void ScintillaQuickCore::selectCurrentWord()
{
	auto pos = CurrentPosition();
	const auto max = pdoc->Length();
	if (max <= 0) {
		return;
	}

	if (pos < 0) {
		pos = 0;
	} else if (pos >= max) {
		pos = max - 1;
	}

	if (pos > 0 &&
		!std::isalnum(static_cast<unsigned char>(pdoc->CharAt(pos))) &&
		std::isalnum(static_cast<unsigned char>(pdoc->CharAt(pos - 1)))) {
		pos--;
	}

	auto startPos = pos;
	while (startPos > 0 && std::isalnum(static_cast<unsigned char>(pdoc->CharAt(startPos - 1)))) {
		startPos--;
	}

	auto endPos = pos + 1;
	while (endPos < max && std::isalnum(static_cast<unsigned char>(pdoc->CharAt(endPos)))) {
		endPos++;
	}

	if (startPos == endPos) {
		return;
	}

	SetSelection(startPos, endPos);

	emit cursorPositionChanged();
}

struct ScintillaQuickCore::style_attributes_t
{
    QColor foreground;
    QColor background;
    QFont font;
};

ScintillaQuickCore::style_attributes_t ScintillaQuickCore::style_attributes_for(int style) const
{
    style_attributes_t attributes;
    const int bounded_style = std::clamp(style, 0, STYLE_MAX);
    const Style &scintilla_style = vs.styles[static_cast<size_t>(bounded_style)];
    const Style &default_style = vs.styles[StyleDefault];

    attributes.foreground = QColorFromColourRGBA(scintilla_style.fore);
    attributes.background = QColorFromColourRGBA(scintilla_style.back);

    const char *font_name = scintilla_style.fontName ? scintilla_style.fontName : default_style.fontName;
    if (font_name) {
        attributes.font.setFamily(QString::fromUtf8(font_name));
    }
    const int size_zoomed = scintilla_style.sizeZoomed > 0
        ? scintilla_style.sizeZoomed
        : default_style.sizeZoomed;
    if (size_zoomed > 0) {
        attributes.font.setPointSizeF(
            static_cast<qreal>(size_zoomed) / SC_FONT_SIZE_MULTIPLIER);
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

Render_frame ScintillaQuickCore::render_frame_from_capture(const Capture_frame &capture_frame) const
{
    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("core.render_frame_from_capture");

    Render_frame frame;
    std::array<std::optional<style_attributes_t>, STYLE_MAX + 1> style_cache;
    const auto attributes_for = [&](int style) -> const style_attributes_t & {
        const int bounded_style = std::clamp(style, 0, STYLE_MAX);
        std::optional<style_attributes_t> &cached = style_cache[bounded_style];
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
    for (const capture_visual_line_t &capture_line : capture_frame.visual_lines) {
        visual_line_frame_t visual_line;
        visual_line.key.document_line = capture_line.document_line;
        visual_line.key.subline_index = capture_line.subline_index;
        visual_line.visual_order = capture_line.visual_order;
        visual_line.origin = QPointF(capture_line.left, capture_line.top);
        visual_line.baseline_y = capture_line.baseline_y;
        visual_line.clip_rect = rect_from_capture(
            capture_line.left,
            capture_line.top,
            capture_line.right,
            capture_line.bottom);

        visual_line.text_runs.reserve(capture_line.text_runs.size());
        for (const capture_text_run_t &capture_run : capture_line.text_runs) {
            const style_attributes_t &attributes = attributes_for(capture_run.style_id);

            text_run_t run;
            run.text = QString::fromUtf8(
                capture_run.text.data(),
                static_cast<int>(capture_run.text.size()));
            run.position = QPointF(capture_run.x, capture_run.baseline_y);
            run.width = capture_run.width;
            run.top = capture_run.top;
            run.bottom = capture_run.bottom;
            run.blob_text_clip_rect = rect_from_capture(
                capture_run.blob_text_left,
                capture_run.blob_text_top,
                capture_run.blob_text_right,
                capture_run.blob_text_bottom);
            run.blob_outer_rect = rect_from_capture(
                capture_run.blob_outer_left,
                capture_run.blob_outer_top,
                capture_run.blob_outer_right,
                capture_run.blob_outer_bottom);
            run.blob_inner_rect = rect_from_capture(
                capture_run.blob_inner_left,
                capture_run.blob_inner_top,
                capture_run.blob_inner_right,
                capture_run.blob_inner_bottom);
            run.foreground = capture_run.foreground.isValid() ? capture_run.foreground : attributes.foreground;
            run.blob_outer = capture_run.blob_outer;
            run.blob_inner = capture_run.blob_inner;
            run.font = attributes.font;
            run.style_id = capture_run.style_id;
            run.direction = capture_run.direction;
            run.is_represented_text = capture_run.is_represented_text;
            run.represented_as_blob = capture_run.represented_as_blob;
            visual_line.text_runs.push_back(std::move(run));
        }

        frame.visual_lines.push_back(std::move(visual_line));
    }

    frame.selection_primitives.reserve(capture_frame.selection_primitives.size());
    for (const capture_selection_primitive_t &capture_selection : capture_frame.selection_primitives) {
        Selection_primitive selection;
        selection.rect = rect_from_capture(
            capture_selection.left,
            capture_selection.top,
            capture_selection.right,
            capture_selection.bottom);
        selection.color = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_selection.rgba)));
        selection.is_main = capture_selection.is_main;
        frame.selection_primitives.push_back(std::move(selection));
    }

    frame.indicator_primitives.reserve(capture_frame.indicator_primitives.size());
    for (const capture_indicator_primitive_t &capture_indicator : capture_frame.indicator_primitives) {
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
        indicator.color = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_indicator.rgba)));
        indicator.stroke_width = capture_indicator.stroke_width;
        indicator.fill_alpha = capture_indicator.fill_alpha;
        indicator.outline_alpha = capture_indicator.outline_alpha;
        indicator.indicator_number = capture_indicator.indicator_number;
        indicator.indicator_style = capture_indicator.indicator_style;
        indicator.under_text = capture_indicator.under_text;
        indicator.is_main = capture_indicator.is_main;
        frame.indicator_primitives.push_back(std::move(indicator));
    }

    frame.current_line_primitives.reserve(capture_frame.current_line_primitives.size());
    for (const capture_current_line_primitive_t &capture_cl : capture_frame.current_line_primitives) {
        Current_line_primitive cl;
        cl.rect = rect_from_capture(
            capture_cl.left,
            capture_cl.top,
            capture_cl.right,
            capture_cl.bottom);
        cl.color = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_cl.rgba)));
        cl.framed = capture_cl.framed;
        frame.current_line_primitives.push_back(std::move(cl));
    }

    frame.marker_primitives.reserve(capture_frame.marker_primitives.size());
    for (const capture_marker_primitive_t &capture_marker : capture_frame.marker_primitives) {
        Marker_primitive marker;
        marker.rect = rect_from_capture(
            capture_marker.left,
            capture_marker.top,
            capture_marker.right,
            capture_marker.bottom);
        marker.marker_number = capture_marker.marker_number;
        marker.marker_type = capture_marker.marker_type;
        marker.foreground = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_marker.fore_rgba)));
        marker.background = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_marker.back_rgba)));
        marker.background_selected = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_marker.back_rgba_selected)));
        marker.document_line = capture_marker.document_line;
        marker.fold_part = capture_marker.fold_part;
        frame.marker_primitives.push_back(std::move(marker));
    }

    frame.caret_primitives.reserve(capture_frame.caret_primitives.size());
    for (const capture_caret_primitive_t &capture_caret : capture_frame.caret_primitives) {
        caret_primitive_t caret;
        caret.rect = rect_from_capture(
            capture_caret.left,
            capture_caret.top,
            capture_caret.right,
            capture_caret.bottom);
        caret.color = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_caret.rgba)));
        caret.is_main = capture_caret.is_main;
        frame.caret_primitives.push_back(std::move(caret));
    }

    frame.margin_text_primitives.reserve(capture_frame.margin_text_primitives.size());
    for (const capture_margin_text_primitive_t &capture_margin_text : capture_frame.margin_text_primitives) {
        const style_attributes_t &attributes = attributes_for(capture_margin_text.style_id);

        Margin_text_primitive margin_text;
        margin_text.text = QString::fromUtf8(
            capture_margin_text.text.data(),
            static_cast<int>(capture_margin_text.text.size()));
        margin_text.position = QPointF(capture_margin_text.x, capture_margin_text.top);
        margin_text.baseline_y = capture_margin_text.baseline_y;
        margin_text.clip_rect = rect_from_capture(
            capture_margin_text.left,
            capture_margin_text.top,
            capture_margin_text.right,
            capture_margin_text.bottom);
        margin_text.foreground = attributes.foreground;
        margin_text.font = attributes.font;
        margin_text.document_line = capture_margin_text.document_line;
        margin_text.subline_index = capture_margin_text.subline_index;
        margin_text.style_id = capture_margin_text.style_id;
        frame.margin_text_primitives.push_back(std::move(margin_text));
    }

    frame.fold_display_texts.reserve(capture_frame.fold_display_texts.size());
    for (const capture_fold_display_text_t &capture_fold : capture_frame.fold_display_texts) {
        const style_attributes_t &attributes = attributes_for(capture_fold.style_id);

        Fold_display_text_primitive fold;
        fold.text = QString::fromUtf8(
            capture_fold.text.data(),
            static_cast<int>(capture_fold.text.size()));
        fold.position = QPointF(capture_fold.left, capture_fold.top);
        fold.baseline_y = capture_fold.baseline_y;
        fold.rect = rect_from_capture(
            capture_fold.left, capture_fold.top,
            capture_fold.right, capture_fold.bottom);
        fold.foreground = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_fold.fore_rgba)));
        fold.background = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_fold.back_rgba)));
        fold.font = attributes.font;
        fold.document_line = capture_fold.document_line;
        fold.style_id = capture_fold.style_id;
        fold.boxed = capture_fold.boxed;
        frame.fold_display_texts.push_back(std::move(fold));
    }

    frame.eol_annotations.reserve(capture_frame.eol_annotations.size());
    for (const capture_eol_annotation_t &capture_eol : capture_frame.eol_annotations) {
        const style_attributes_t &attributes = attributes_for(capture_eol.style_id);

        Eol_annotation_primitive eol;
        eol.text = QString::fromUtf8(
            capture_eol.text.data(),
            static_cast<int>(capture_eol.text.size()));
        eol.position = QPointF(capture_eol.text_left, capture_eol.top);
        eol.baseline_y = capture_eol.baseline_y;
        eol.rect = rect_from_capture(
            capture_eol.left, capture_eol.top,
            capture_eol.right, capture_eol.bottom);
        eol.foreground = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_eol.fore_rgba)));
        eol.background = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_eol.back_rgba)));
        eol.font = attributes.font;
        eol.document_line = capture_eol.document_line;
        eol.style_id = capture_eol.style_id;
        eol.visible_style = capture_eol.visible_style;
        frame.eol_annotations.push_back(std::move(eol));
    }

    frame.annotations.reserve(capture_frame.annotations.size());
    for (const capture_annotation_t &capture_annot : capture_frame.annotations) {
        const style_attributes_t &attributes = attributes_for(capture_annot.style_id);

        Annotation_primitive annot;
        annot.text = QString::fromUtf8(
            capture_annot.text.data(),
            static_cast<int>(capture_annot.text.size()));
        annot.position = QPointF(capture_annot.text_left, capture_annot.top);
        annot.baseline_y = capture_annot.baseline_y;
        annot.rect = rect_from_capture(
            capture_annot.left, capture_annot.top,
            capture_annot.right, capture_annot.bottom);
        annot.foreground = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_annot.fore_rgba)));
        annot.background = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_annot.back_rgba)));
        annot.font = attributes.font;
        annot.document_line = capture_annot.document_line;
        annot.annotation_line = capture_annot.annotation_line;
        annot.style_id = capture_annot.style_id;
        annot.boxed = capture_annot.boxed;
        frame.annotations.push_back(std::move(annot));
    }

    frame.whitespace_marks.reserve(capture_frame.whitespace_marks.size());
    for (const capture_whitespace_mark_t &capture_ws : capture_frame.whitespace_marks) {
        Whitespace_mark_primitive ws;
        ws.rect = rect_from_capture(
            capture_ws.left, capture_ws.top,
            capture_ws.right, capture_ws.bottom);
        ws.mid_y = capture_ws.mid_y;
        ws.color = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_ws.rgba)));
        ws.kind = capture_ws.kind;
        frame.whitespace_marks.push_back(std::move(ws));
    }

    frame.decoration_underlines.reserve(capture_frame.decoration_underlines.size());
    for (const capture_decoration_underline_t &capture_ul : capture_frame.decoration_underlines) {
        Decoration_underline_primitive ul;
        ul.rect = rect_from_capture(
            capture_ul.left, capture_ul.top,
            capture_ul.right, capture_ul.bottom);
        ul.color = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_ul.rgba)));
        ul.kind = capture_ul.kind;
        frame.decoration_underlines.push_back(std::move(ul));
    }

    frame.indent_guides.reserve(capture_frame.indent_guides.size());
    for (const capture_indent_guide_t &capture_ig : capture_frame.indent_guides) {
        Indent_guide_primitive ig;
        ig.x = capture_ig.x;
        ig.top = capture_ig.top;
        ig.bottom = capture_ig.bottom;
        ig.color = QColorFromColourRGBA(ColourRGBA(static_cast<int>(capture_ig.rgba)));
        ig.highlight = capture_ig.highlight;
        frame.indent_guides.push_back(std::move(ig));
    }

    return frame;
}

Capture_frame ScintillaQuickCore::capture_current_frame(
    bool static_content_dirty,
    bool ensure_styled,
    bool scrolling,
    int extra_capture_lines)
{
    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("core.capture_current_frame");

    Capture_frame frame;

    if (!m_owner) {
        return frame;
    }

    const PRectangle client_rect = GetClientRectangle();
    const PRectangle text_rect = GetTextRectangle();

    frame.viewport_width = client_rect.Width();
    frame.viewport_height = client_rect.Height();
    frame.text_left = text_rect.left;
    frame.text_top = text_rect.top;
    frame.text_width = text_rect.Width();
    frame.text_height = text_rect.Height();
    frame.margin_left = client_rect.left;
    frame.margin_top = client_rect.top;
    frame.margin_width = std::max<XYPOSITION>(0.0, text_rect.left - client_rect.left);
    frame.margin_height = client_rect.Height();

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
    if (auto *surface_impl = dynamic_cast<SurfaceImpl *>(static_cast<Surface *>(surface))) {
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

Render_frame ScintillaQuickCore::current_render_frame(
    const Capture_frame *capture_frame,
    bool static_content_dirty,
    bool ensure_styled,
    bool scrolling,
    int extra_capture_lines)
{
    SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE("core.current_render_frame");

    if (capture_frame) {
        return render_frame_from_capture(*capture_frame);
    }

    Capture_frame captured = capture_current_frame(static_content_dirty, ensure_styled, scrolling, extra_capture_lines);
    return render_frame_from_capture(captured);
}

ScintillaQuickCore::~ScintillaQuickCore()
{
	CancelTimers();
	ChangeIdle(false);
}

void ScintillaQuickCore::execCommand(QAction *action)
{
	const int commandNum = action->data().toInt();
	Command(commandNum);
}

#if defined(Q_OS_WIN)
static const QString sMSDEVColumnSelect("MSDEVColumnSelect");
static const QString sWrappedMSDEVColumnSelect("application/x-qt-windows-mime;value=\"MSDEVColumnSelect\"");
static const QString sVSEditorLineCutCopy("VisualStudioEditorOperationsLineCutCopyClipboardTag");
static const QString sWrappedVSEditorLineCutCopy("application/x-qt-windows-mime;value=\"VisualStudioEditorOperationsLineCutCopyClipboardTag\"");
#elif defined(Q_OS_MAC)
static const QString sScintillaRecPboardType("com.scintilla.utf16-plain-text.rectangular");
static const QString sScintillaRecMimeType("text/x-scintilla.utf16-plain-text.rectangular");
#else
// Linux
static const QString sMimeRectangularMarker("text/x-rectangular-marker");
#endif

void ScintillaQuickCore::Init()
{
	rectangularSelectionModifier = SCMOD_ALT;

	connect(QGuiApplication::clipboard(), SIGNAL(selectionChanged()),
		this, SLOT(SelectionChanged()));
}

void ScintillaQuickCore::Finalise()
{
	CancelTimers();
	ScintillaBase::Finalise();
}

void ScintillaQuickCore::SelectionChanged()
{
	bool nowPrimary = QGuiApplication::clipboard()->ownsSelection();
	if (nowPrimary != primarySelection) {
		primarySelection = nowPrimary;
		Redraw();
	}
}

bool ScintillaQuickCore::DragThreshold(Point ptStart, Point ptNow)
{
	int xMove = std::abs(ptStart.x - ptNow.x);
	int yMove = std::abs(ptStart.y - ptNow.y);
	return (xMove > QGuiApplication::styleHints()->startDragDistance()) ||
		(yMove > QGuiApplication::styleHints()->startDragDistance());
}

static QString StringFromSelectedText(const SelectionText &selectedText)
{
	Q_UNUSED(selectedText.characterSet);
	return QString::fromUtf8(selectedText.Data(), static_cast<int>(selectedText.Length()));
}

static void AddRectangularToMime(QMimeData *mimeData, [[maybe_unused]] const QString &su)
{
	Q_UNUSED(su);
#if defined(Q_OS_WIN)
	// Add an empty marker
	mimeData->setData(sMSDEVColumnSelect, QByteArray());
#elif defined(Q_OS_MAC)
	// macOS gets marker + data to work with other implementations.
	// Don't understand how this works but it does - the
	// clipboard format is supposed to be UTF-16, not UTF-8.
	mimeData->setData(sScintillaRecMimeType, su.toUtf8());
#else
	// Linux
	// Add an empty marker
	mimeData->setData(sMimeRectangularMarker, QByteArray());
#endif
}

static void AddLineCutCopyToMime([[maybe_unused]] QMimeData *mimeData)
{
	Q_UNUSED(mimeData);
#if defined(Q_OS_WIN)
	// Add an empty marker
	mimeData->setData(sVSEditorLineCutCopy, QByteArray());
#endif
}

static bool IsRectangularInMime(const QMimeData *mimeData)
{
	QStringList formats = mimeData->formats();
	for (int i = 0; i < formats.size(); ++i) {
#if defined(Q_OS_WIN)
		// Windows rectangular markers
		// If rectangular copies made by this application, see base name.
		if (formats[i] == sMSDEVColumnSelect)
			return true;
		// Otherwise see wrapped name.
		if (formats[i] == sWrappedMSDEVColumnSelect)
			return true;
#elif defined(Q_OS_MAC)
		if (formats[i] == sScintillaRecMimeType)
			return true;
#else
		// Linux
		if (formats[i] == sMimeRectangularMarker)
			return true;
#endif
	}
	return false;
}

static bool IsLineCutCopyInMime(const QMimeData *mimeData)
{
	QStringList formats = mimeData->formats();
	for (int i = 0; i < formats.size(); ++i) {
#if defined(Q_OS_WIN)
		// Visual Studio Line Cut/Copy markers
		// If line cut/copy made by this application, see base name.
		if (formats[i] == sVSEditorLineCutCopy)
			return true;
		// Otherwise see wrapped name.
		if (formats[i] == sWrappedVSEditorLineCutCopy)
			return true;
#endif
	}
	return false;
}

bool ScintillaQuickCore::ValidCodePage(int codePage) const
{
	return codePage == SC_CP_UTF8;
}

std::string ScintillaQuickCore::UTF8FromEncoded(std::string_view encoded) const {
	return std::string(encoded);
}

std::string ScintillaQuickCore::EncodedFromUTF8(std::string_view utf8) const {
	return std::string(utf8);
}

void ScintillaQuickCore::SetVerticalScrollPos()
{
	emit verticalScrolled(topLine);
}

void ScintillaQuickCore::SetHorizontalScrollPos()
{
	emit horizontalScrolled(xOffset);
}

void ScintillaQuickCore::reset_tracked_scroll_width_to_viewport()
{
    const int viewport_width = std::max(1, static_cast<int>(GetTextRectangle().Width()));
    WndProc(Message::SetScrollWidth, static_cast<uptr_t>(viewport_width), 0);
    if (xOffset > hMax) {
        xOffset = hMax;
        SetHorizontalScrollPos();
    }
}

bool ScintillaQuickCore::ModifyScrollBars(Sci::Line nMax, Sci::Line nPage)
{
	bool modified = false;

	int vNewPage = nPage;
	int vNewMax = nMax - vNewPage + 1;
	if (vMax != vNewMax || vPage != vNewPage) {
		vMax = vNewMax;
		vPage = vNewPage;
		modified = true;
		emit verticalRangeChanged(vMax, vPage);
	}

	int hNewPage = GetTextRectangle().Width();
	int hNewMax = (scrollWidth > hNewPage) ? scrollWidth - hNewPage : 0;
	if (hMax != hNewMax || hPage != hNewPage) {
		hMax = hNewMax;
		hPage = hNewPage;
		modified = true;
		emit horizontalRangeChanged(hMax, hPage);
	}

	return modified;
}

void ScintillaQuickCore::CopyToModeClipboard(const SelectionText &selectedText, QClipboard::Mode clipboardMode_)
{
	QClipboard *clipboard = QGuiApplication::clipboard();
	QString su = StringFromSelectedText(selectedText);
	QMimeData *mimeData = new QMimeData();
	mimeData->setText(su);
	if (selectedText.rectangular) {
		AddRectangularToMime(mimeData, su);
	}

	if (selectedText.lineCopy) {
		AddLineCutCopyToMime(mimeData);
	}

	// Allow client code to add additional data (e.g rich text).
	emit aboutToCopy(mimeData);

	clipboard->setMimeData(mimeData, clipboardMode_);
}

void ScintillaQuickCore::Copy()
{
	if (!sel.Empty()) {
		SelectionText st;
		CopySelectionRange(&st);
		CopyToClipboard(st);
	}
}

void ScintillaQuickCore::CopyToClipboard(const SelectionText &selectedText)
{
	CopyToModeClipboard(selectedText, QClipboard::Clipboard);
}

void ScintillaQuickCore::PasteFromMode(QClipboard::Mode clipboardMode_)
{
	QClipboard *clipboard = QGuiApplication::clipboard();
	const QMimeData *mimeData = clipboard->mimeData(clipboardMode_);
	bool isRectangular = IsRectangularInMime(mimeData);
	bool isLine = SelectionEmpty() && IsLineCutCopyInMime(mimeData);
	QString text = clipboard->text(clipboardMode_);
	QByteArray utext = BytesForDocument(text);
	std::string dest(utext.constData(), utext.length());
	SelectionText selText;
	selText.Copy(dest, pdoc->dbcsCodePage, CharacterSetOfDocument(), isRectangular, false);

	UndoGroup ug(pdoc);
	ClearSelection(multiPasteMode == MultiPaste::Each);
	InsertPasteShape(selText.Data(), selText.Length(),
		isRectangular ? PasteShape::rectangular : (isLine ? PasteShape::line : PasteShape::stream));
	EnsureCaretVisible();
}

void ScintillaQuickCore::Paste()
{
	PasteFromMode(QClipboard::Clipboard);
}

void ScintillaQuickCore::ClaimSelection()
{
	if (QGuiApplication::clipboard()->supportsSelection()) {
		// X Windows has a 'primary selection' as well as the clipboard.
		// Whenever the user selects some text, we become the primary selection
		if (!sel.Empty()) {
			primarySelection = true;
			SelectionText st;
			CopySelectionRange(&st);
			CopyToModeClipboard(st, QClipboard::Selection);
		} else {
			primarySelection = false;
		}
	}
}

void ScintillaQuickCore::NotifyChange()
{
	emit notifyChange();
	emit command(
			Platform::LongFromTwoShorts(GetCtrlID(), SCEN_CHANGE),
			reinterpret_cast<sptr_t>(wMain.GetID()));
}

void ScintillaQuickCore::NotifyFocus(bool focus)
{
	if (commandEvents) {
		emit command(
				Platform::LongFromTwoShorts
						(GetCtrlID(), focus ? SCEN_SETFOCUS : SCEN_KILLFOCUS),
				reinterpret_cast<sptr_t>(wMain.GetID()));
	}

	Editor::NotifyFocus(focus);
}

void ScintillaQuickCore::NotifyParent(NotificationData scn)
{
	scn.nmhdr.hwndFrom = wMain.GetID();
	scn.nmhdr.idFrom = GetCtrlID();
	emit notifyParent(scn);
}

void ScintillaQuickCore::NotifyURIDropped(const char *uri)
{
	NotificationData scn = {};
	scn.nmhdr.code = Notification::URIDropped;
	scn.text = uri;

	NotifyParent(scn);
}

bool ScintillaQuickCore::FineTickerRunning(TickReason reason)
{
	return timers[static_cast<size_t>(reason)] != 0;
}

void ScintillaQuickCore::FineTickerStart(TickReason reason, int millis, int /* tolerance */)
{
	FineTickerCancel(reason);
	timers[static_cast<size_t>(reason)] = startTimer(millis);
}

// CancelTimers cleans up all fine-ticker timers and is non-virtual to avoid warnings when
// called during destruction.
void ScintillaQuickCore::CancelTimers()
{
	for (size_t tr = static_cast<size_t>(TickReason::caret); tr <= static_cast<size_t>(TickReason::dwell); tr++) {
		if (timers[tr]) {
			killTimer(timers[tr]);
			timers[tr] = 0;
		}
	}
}

void ScintillaQuickCore::FineTickerCancel(TickReason reason)
{
	const size_t reasonIndex = static_cast<size_t>(reason);
	if (timers[reasonIndex]) {
		killTimer(timers[reasonIndex]);
		timers[reasonIndex] = 0;
	}
}

void ScintillaQuickCore::onIdle()
{
	bool continueIdling = Idle();
	if (!continueIdling) {
		SetIdle(false);
	}
}

bool ScintillaQuickCore::ChangeIdle(bool on)
{
	if (on) {
		// Start idler, if it's not running.
		if (!idler.state) {
			idler.state = true;
			QTimer *qIdle = new QTimer;
			connect(qIdle, SIGNAL(timeout()), this, SLOT(onIdle()));
			qIdle->start(0);
			idler.idlerID = qIdle;
		}
	} else {
		// Stop idler, if it's running
		if (idler.state) {
			idler.state = false;
			QTimer *qIdle = static_cast<QTimer *>(idler.idlerID);
			qIdle->stop();
			disconnect(qIdle, SIGNAL(timeout()), nullptr, nullptr);
			delete qIdle;
			idler.idlerID = {};
		}
	}
	return true;
}

bool ScintillaQuickCore::SetIdle(bool on)
{
	return ChangeIdle(on);
}

CharacterSet ScintillaQuickCore::CharacterSetOfDocument() const
{
	return vs.styles[STYLE_DEFAULT].characterSet;
}

QString ScintillaQuickCore::StringFromDocument(const char *s) const
{
	return QString::fromUtf8(s);
}

QByteArray ScintillaQuickCore::BytesForDocument(const QString &text) const
{
	return text.toUtf8();
}

std::unique_ptr<CaseFolder> ScintillaQuickCore::CaseFolderForEncoding()
{
	return std::make_unique<CaseFolderUnicode>();
}

std::string ScintillaQuickCore::CaseMapString(const std::string &s, CaseMapping caseMapping)
{
	if (s.empty() || (caseMapping == CaseMapping::same))
		return s;

	std::string retMapped(s.length() * maxExpansionCaseConversion, 0);
	const size_t lenMapped = CaseConvertString(&retMapped[0], retMapped.length(), s.c_str(), s.length(),
		(caseMapping == CaseMapping::upper) ? CaseConversion::upper : CaseConversion::lower);
	retMapped.resize(lenMapped);
	return retMapped;
}

void ScintillaQuickCore::SetMouseCapture(bool on)
{
	// This is handled automatically by Qt
	if (mouseDownCaptures) {
		haveMouseCapture = on;
	}
}

bool ScintillaQuickCore::HaveMouseCapture()
{
	return haveMouseCapture;
}

void ScintillaQuickCore::StartDrag()
{
	inDragDrop = DragDrop::dragging;
	dropWentOutside = true;
	if (drag.Length()) {
		QMimeData *mimeData = new QMimeData;
		QString sText = StringFromSelectedText(drag);
		mimeData->setText(sText);
		if (drag.rectangular) {
			AddRectangularToMime(mimeData, sText);
		}
		// This QDrag is not freed as that causes a crash on Linux
		QDrag *dragon = new QDrag(m_owner);
		dragon->setMimeData(mimeData);

		Qt::DropAction dropAction = dragon->exec(static_cast<Qt::DropActions>(Qt::CopyAction|Qt::MoveAction));
		if ((dropAction == Qt::MoveAction) && dropWentOutside) {
			// Remove dragged out text
			ClearSelection();
		}
	}
	inDragDrop = DragDrop::none;
	SetDragPosition(SelectionPosition(Sci::invalidPosition));
}

class CallTipItem : public QQuickItem {
public:
	explicit CallTipItem(CallTip *pct_, QQuickItem *parent)
		: QQuickItem(parent),
		  pct(pct_)
	{
		setAcceptedMouseButtons(Qt::NoButton);
		setAcceptHoverEvents(false);
		setFlag(QQuickItem::ItemHasContents, true);
		setVisible(false);
		setZ(1000.0);
	}

	QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override
	{
		QQuickWindow *quickWindow = window();
		if (!quickWindow || !pct || !pct->inCallTipMode || width() <= 0.0 || height() <= 0.0) {
			delete oldNode;
			return nullptr;
		}

		const QSize imageSize(std::max(1, static_cast<int>(std::ceil(width()))),
			std::max(1, static_cast<int>(std::ceil(height()))));
		QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);

		QPainter painter(&image);
		std::unique_ptr<Surface> surfaceWindow = Surface::Allocate(Technology::Default);
		surfaceWindow->Init(false, &painter);
		surfaceWindow->SetMode(SurfaceMode(pct->codePage, false));
		pct->PaintCT(surfaceWindow.get());

		auto *imageNode = dynamic_cast<QSGImageNode *>(oldNode);
		if (!imageNode) {
			delete oldNode;
			imageNode = quickWindow->createImageNode();
		}
		QSGTexture *texture = quickWindow->createTextureFromImage(image);
		imageNode->setTexture(texture);
		imageNode->setOwnsTexture(true);
		imageNode->setRect(QRectF(QPointF(0.0, 0.0), QSizeF(imageSize)));
		imageNode->setSourceRect(QRectF(QPointF(0.0, 0.0), QSizeF(imageSize)));
		imageNode->setFiltering(QSGTexture::Linear);
		return imageNode;
	}

private:
	CallTip *pct;
};

void ScintillaQuickCore::CreateCallTipWindow(PRectangle rc)
{

	if (!ct.wCallTip.Created()) {
		QQuickItem *parentItem = m_owner->window() ? m_owner->window()->contentItem() : static_cast<QQuickItem *>(m_owner);
		QQuickItem *pCallTip = new CallTipItem(&ct, parentItem);
		ct.wCallTip = pCallTip;
		pCallTip->setPosition(QPointF(rc.left, rc.top));
		pCallTip->setSize(QSizeF(rc.Width(), rc.Height()));
		pCallTip->update();
	}
}

void ScintillaQuickCore::AddToPopUp(const char *label,
                             int cmd,
                             bool enabled)
{
	QList<QPair<QString, QPair<int, bool>>> *menu = static_cast<QList<QPair<QString, QPair<int, bool>>> *>(popup.GetID());

	QPair<QString, QPair<int, bool>> item(label, QPair<int, bool>(cmd, enabled));
	menu->append(item);
}

sptr_t ScintillaQuickCore::WndProc(Message iMessage, uptr_t wParam, sptr_t lParam)
{
	try {
		switch (iMessage) {

		case Message::SetBidirectional:
			bidirectional = static_cast<Scintilla::Bidirectional>(wParam);
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
            rectangularSelectionModifier = static_cast<int>(wParam);
            break;

        case Message::GetRectangularSelectionModifier:
            return rectangularSelectionModifier;

        default:
			return ScintillaBase::WndProc(iMessage, wParam, lParam);
		}
	} catch (std::bad_alloc &) {
		errorStatus = Status::BadAlloc;
	} catch (...) {
		errorStatus = Status::Failure;
	}
	return 0;
}

sptr_t ScintillaQuickCore::DefWndProc(Message, uptr_t, sptr_t)
{
	return 0;
}

sptr_t ScintillaQuickCore::DirectFunction(
    sptr_t ptr, unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
	ScintillaQuickCore *sci = reinterpret_cast<ScintillaQuickCore *>(ptr);
	return sci->WndProc(static_cast<Message>(iMessage), wParam, lParam);
}

sptr_t ScintillaQuickCore::DirectStatusFunction(
    sptr_t ptr, unsigned int iMessage, uptr_t wParam, sptr_t lParam, int *pStatus)
{
	ScintillaQuickCore *sci = reinterpret_cast<ScintillaQuickCore *>(ptr);
	const sptr_t returnValue = sci->WndProc(static_cast<Message>(iMessage), wParam, lParam);
	*pStatus = static_cast<int>(sci->errorStatus);
	return returnValue;
}

// Additions to merge in Scientific Toolworks widget structure

void ScintillaQuickCore::PartialPaint(const PRectangle &rect)
{
	PartialPaintQml(rect, nullptr);
}

void ScintillaQuickCore::PartialPaintQml(const PRectangle & rect, QPainter *painter)
{
	currentPainter = painter;
	rcPaint = rect;
    paintState = PaintState::painting;
	PRectangle rcClient = GetClientRectangle();
// TODO: analyze repaint problem when LineEnd should be marked...
    paintingAllText = rcPaint.Contains(rcClient);

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
	currentPainter = nullptr;
}
	
void ScintillaQuickCore::DragEnter(const Point &point)
{
	SetDragPosition(SPositionFromLocation(point,
					      false, false, UserVirtualSpace()));
}

void ScintillaQuickCore::DragMove(const Point &point)
{
	DragEnter(point);
}

void ScintillaQuickCore::DragLeave()
{
	SetDragPosition(SelectionPosition(Sci::invalidPosition));
}

void ScintillaQuickCore::Drop(const Point &point, const QMimeData *data, bool move)
{
	QString text = data->text();
	bool rectangular = IsRectangularInMime(data);
	QByteArray bytes = BytesForDocument(text);
	int len = bytes.length();

	SelectionPosition movePos = SPositionFromLocation(point,
				false, false, UserVirtualSpace());

	DropAt(movePos, bytes, len, move, rectangular);
}

void ScintillaQuickCore::DropUrls(const QMimeData *data)
{
	foreach(const QUrl &url, data->urls()) {
		NotifyURIDropped(url.toString().toUtf8().constData());
	}
}

void ScintillaQuickCore::timerEvent(QTimerEvent *event)
{
	for (size_t tr=static_cast<size_t>(TickReason::caret); tr<=static_cast<size_t>(TickReason::dwell); tr++) {
		if (timers[tr] == event->timerId()) {
            const Sci::Line previous_top_line = topLine;
            const int previous_x_offset = xOffset;
			TickFor(static_cast<TickReason>(tr));
            if (m_owner) {
                const bool vertical_scroll_changed = topLine != previous_top_line;
                const bool horizontal_scroll_changed = xOffset != previous_x_offset;
                if (vertical_scroll_changed || horizontal_scroll_changed) {
                    m_owner->request_scene_graph_update(true, false, vertical_scroll_changed);
                } else {
                    m_owner->request_scene_graph_update();
                }
            }
		}
	}
}
