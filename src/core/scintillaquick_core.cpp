// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file ScintillaQuick_core.cpp - Qt specific subclass of ScintillaBase

#include "scintillaquick_core.h"
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
#include <QScopeGuard>
#include <QSGImageNode>
#include <QStyleHints>
#include <QTimer>
#include <QTimerEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

#include "CaseConvert.h"
#include "RenderCapture.h"

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

// Convert a raw Scintilla rgba value (as carried by Captured_* primitives)
// to a QColor at the capture/render boundary.
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

Whitespace_mark_kind_t whitespace_kind_from_capture(Whitespace_mark_kind kind)
{
    return kind == Whitespace_mark_kind::tab_arrow
        ? Whitespace_mark_kind_t::tab_arrow
        : Whitespace_mark_kind_t::space_dot;
}

Decoration_kind_t decoration_kind_from_capture(Decoration_kind kind)
{
    return kind == Decoration_kind::hotspot
        ? Decoration_kind_t::hotspot
        : Decoration_kind_t::style_underline;
}

// Sole consumer of Scintilla's capture callbacks. Builds a Render_frame
// directly with Qt types and per-style attributes resolved from the core's
// view-style cache, avoiding an intermediate Captured_frame translation pass.
class Render_frame_builder final : public Render_collector
{
public:
    Render_frame_builder(
        Render_frame&              frame,
        bool                       capture_static_content,
        const ScintillaQuick_core& core)
    :
        m_frame(frame),
        m_capture_static_content(capture_static_content),
        m_core(core)
    {}

    bool wants_static_content() const override
    {
        return m_capture_static_content;
    }

    void begin_visual_line(const Captured_visual_line& line) override
    {
        Visual_line_frame visual_line;
        visual_line.key.document_line = line.document_line;
        visual_line.key.subline_index = line.subline_index;
        visual_line.visual_order      = line.visual_order;
        visual_line.origin            = QPointF(line.left, line.top);
        visual_line.baseline_y        = line.baseline_y;
        visual_line.clip_rect = rect_from_capture(line.left, line.top, line.right, line.bottom);
        m_frame.visual_lines.push_back(std::move(visual_line));
        m_current_visual_line = &m_frame.visual_lines.back();
    }

    void add_text_run(const Captured_text_run& run) override
    {
        if (!m_current_visual_line) {
            return;
        }
        const ScintillaQuick_core::Style_attributes& attributes = attributes_for(run.style_id);

        Text_run text_run;
        text_run.text = QString::fromUtf8(run.utf8_text.data(), static_cast<int>(run.utf8_text.size()));
        text_run.position            = QPointF(run.x, run.baseline_y);
        text_run.width               = run.width;
        text_run.top                 = run.top;
        text_run.bottom              = run.bottom;
        text_run.blob_text_clip_rect = rect_from_capture(
            run.blob_text_left, run.blob_text_top, run.blob_text_right, run.blob_text_bottom);
        text_run.blob_outer_rect = rect_from_capture(
            run.blob_outer_left, run.blob_outer_top, run.blob_outer_right, run.blob_outer_bottom);
        text_run.blob_inner_rect = rect_from_capture(
            run.blob_inner_left, run.blob_inner_top, run.blob_inner_right, run.blob_inner_bottom);
        const QColor captured_fore   = qcolor_from_rgba(run.foreground_rgba);
        text_run.foreground          = captured_fore.isValid() ? captured_fore : attributes.foreground;
        text_run.blob_outer          = qcolor_from_rgba(run.blob_outer_rgba);
        text_run.blob_inner          = qcolor_from_rgba(run.blob_inner_rgba);
        text_run.font                = attributes.font;
        text_run.style_id            = run.style_id;
        text_run.direction           = direction_from_capture(run.direction);
        text_run.is_represented_text = run.is_represented_text;
        text_run.represented_as_blob = run.represented_as_blob;

        m_current_visual_line->text_runs.push_back(std::move(text_run));
    }

    void add_selection_rect(const Captured_selection_rect& rect) override
    {
        Selection_primitive selection;
        selection.rect    = rect_from_capture(rect.left, rect.top, rect.right, rect.bottom);
        selection.color   = qcolor_from_rgba(rect.rgba);
        selection.is_main = rect.is_main;
        m_frame.selection_primitives.push_back(std::move(selection));
    }

    void add_caret_rect(const Captured_caret_rect& rect) override
    {
        Caret_primitive caret;
        caret.rect    = rect_from_capture(rect.left, rect.top, rect.right, rect.bottom);
        caret.color   = qcolor_from_rgba(rect.rgba);
        caret.is_main = rect.is_main;
        m_frame.caret_primitives.push_back(std::move(caret));
    }

    void add_indicator_primitive(const Captured_indicator& indicator) override
    {
        Indicator_primitive primitive;
        primitive.rect = rect_from_capture(
            indicator.left, indicator.top, indicator.right, indicator.bottom);
        primitive.line_rect = rect_from_capture(
            indicator.left, indicator.line_top, indicator.right, indicator.line_bottom);
        primitive.character_rect = rect_from_capture(
            indicator.character_left, indicator.character_top,
            indicator.character_right, indicator.character_bottom);
        primitive.color            = qcolor_from_rgba(indicator.rgba);
        primitive.stroke_width     = indicator.stroke_width;
        primitive.fill_alpha       = indicator.fill_alpha;
        primitive.outline_alpha    = indicator.outline_alpha;
        primitive.indicator_number = indicator.indicator_number;
        primitive.indicator_style  = indicator.indicator_style;
        primitive.under_text       = indicator.under_text;
        primitive.is_main          = indicator.is_main;
        m_frame.indicator_primitives.push_back(std::move(primitive));
    }

    void add_current_line_highlight(const Captured_current_line_highlight& highlight) override
    {
        Current_line_primitive primitive;
        primitive.rect   = rect_from_capture(
            highlight.left, highlight.top, highlight.right, highlight.bottom);
        primitive.color  = qcolor_from_rgba(highlight.rgba);
        primitive.framed = highlight.framed;
        m_frame.current_line_primitives.push_back(std::move(primitive));
    }

    void add_marker_symbol(const Captured_marker_symbol& marker) override
    {
        Marker_primitive primitive;
        primitive.rect = rect_from_capture(
            marker.left, marker.top, marker.right, marker.bottom);
        primitive.marker_number       = marker.marker_number;
        primitive.marker_type         = marker.marker_type;
        primitive.foreground          = qcolor_from_rgba(marker.fore_rgba);
        primitive.background          = qcolor_from_rgba(marker.back_rgba);
        primitive.background_selected = qcolor_from_rgba(marker.back_rgba_selected);
        primitive.document_line       = marker.document_line;
        primitive.fold_part           = marker.fold_part;
        m_frame.marker_primitives.push_back(std::move(primitive));
    }

    void add_margin_text(const Captured_margin_text& text) override
    {
        const ScintillaQuick_core::Style_attributes& attributes = attributes_for(text.style_id);

        Margin_text_primitive primitive;
        primitive.text = QString::fromUtf8(text.utf8_text.data(), static_cast<int>(text.utf8_text.size()));
        primitive.position      = QPointF(text.x, text.top);
        primitive.baseline_y    = text.baseline_y;
        primitive.clip_rect     = rect_from_capture(text.left, text.top, text.right, text.bottom);
        primitive.foreground    = attributes.foreground;
        primitive.font          = attributes.font;
        primitive.document_line = text.document_line;
        primitive.subline_index = text.subline_index;
        primitive.style_id      = text.style_id;
        m_frame.margin_text_primitives.push_back(std::move(primitive));
    }

    void add_fold_display_text(const Captured_fold_display_text& text) override
    {
        const ScintillaQuick_core::Style_attributes& attributes = attributes_for(text.style_id);

        Fold_display_text_primitive primitive;
        primitive.text = QString::fromUtf8(text.utf8_text.data(), static_cast<int>(text.utf8_text.size()));
        primitive.position      = QPointF(text.left, text.top);
        primitive.baseline_y    = text.baseline_y;
        primitive.rect          = rect_from_capture(text.left, text.top, text.right, text.bottom);
        primitive.foreground    = qcolor_from_rgba(text.fore_rgba);
        primitive.background    = qcolor_from_rgba(text.back_rgba);
        primitive.font          = attributes.font;
        primitive.document_line = text.document_line;
        primitive.style_id      = text.style_id;
        primitive.boxed         = text.boxed;
        m_frame.fold_display_texts.push_back(std::move(primitive));
    }

    void add_eol_annotation(const Captured_eol_annotation& annotation) override
    {
        const ScintillaQuick_core::Style_attributes& attributes = attributes_for(annotation.style_id);

        Eol_annotation_primitive primitive;
        primitive.text = QString::fromUtf8(
            annotation.utf8_text.data(), static_cast<int>(annotation.utf8_text.size()));
        primitive.position      = QPointF(annotation.text_left, annotation.top);
        primitive.baseline_y    = annotation.baseline_y;
        primitive.rect          = rect_from_capture(
            annotation.left, annotation.top, annotation.right, annotation.bottom);
        primitive.foreground    = qcolor_from_rgba(annotation.fore_rgba);
        primitive.background    = qcolor_from_rgba(annotation.back_rgba);
        primitive.font          = attributes.font;
        primitive.document_line = annotation.document_line;
        primitive.style_id      = annotation.style_id;
        primitive.visible_style = annotation.visible_style;
        m_frame.eol_annotations.push_back(std::move(primitive));
    }

    void add_annotation(const Captured_annotation& annotation) override
    {
        const ScintillaQuick_core::Style_attributes& attributes = attributes_for(annotation.style_id);

        Annotation_primitive primitive;
        primitive.text = QString::fromUtf8(
            annotation.utf8_text.data(), static_cast<int>(annotation.utf8_text.size()));
        primitive.position        = QPointF(annotation.text_left, annotation.top);
        primitive.baseline_y      = annotation.baseline_y;
        primitive.rect            = rect_from_capture(
            annotation.left, annotation.top, annotation.right, annotation.bottom);
        primitive.foreground      = qcolor_from_rgba(annotation.fore_rgba);
        primitive.background      = qcolor_from_rgba(annotation.back_rgba);
        primitive.font            = attributes.font;
        primitive.document_line   = annotation.document_line;
        primitive.annotation_line = annotation.annotation_line;
        primitive.style_id        = annotation.style_id;
        primitive.boxed           = annotation.boxed;
        m_frame.annotations.push_back(std::move(primitive));
    }

    void add_whitespace_mark(const Captured_whitespace_mark& mark) override
    {
        Whitespace_mark_primitive primitive;
        primitive.rect  = rect_from_capture(mark.left, mark.top, mark.right, mark.bottom);
        primitive.mid_y = mark.mid_y;
        primitive.color = qcolor_from_rgba(mark.rgba);
        primitive.kind  = whitespace_kind_from_capture(mark.kind);
        m_frame.whitespace_marks.push_back(std::move(primitive));
    }

    void add_decoration_underline(const Captured_decoration_underline& underline) override
    {
        Decoration_underline_primitive primitive;
        primitive.rect  = rect_from_capture(underline.left, underline.top, underline.right, underline.bottom);
        primitive.color = qcolor_from_rgba(underline.rgba);
        primitive.kind  = decoration_kind_from_capture(underline.kind);
        m_frame.decoration_underlines.push_back(std::move(primitive));
    }

    void add_indent_guide(const Captured_indent_guide& guide) override
    {
        Indent_guide_primitive primitive;
        primitive.x         = guide.x;
        primitive.top       = guide.top;
        primitive.bottom    = guide.bottom;
        primitive.color     = qcolor_from_rgba(guide.rgba);
        primitive.highlight = guide.highlight;
        m_frame.indent_guides.push_back(std::move(primitive));
    }

    void end_visual_line() override
    {
        m_current_visual_line = nullptr;
    }

private:
    const ScintillaQuick_core::Style_attributes& attributes_for(int style_id)
    {
        const int bounded_style = std::clamp(style_id, 0, STYLE_MAX);
        std::optional<ScintillaQuick_core::Style_attributes>& cached = m_style_cache[bounded_style];
        if (!cached.has_value()) {
            cached = m_core.style_attributes_for(bounded_style);
        }
        return *cached;
    }

    Render_frame& m_frame;
    bool m_capture_static_content = true;
    Visual_line_frame* m_current_visual_line = nullptr;
    const ScintillaQuick_core& m_core;
    std::array<std::optional<ScintillaQuick_core::Style_attributes>, STYLE_MAX + 1> m_style_cache;
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

void ScintillaQuick_core::ensure_visible_range_styled(bool scrolling)
{
    StyleAreaBounded(GetClientDrawingRectangle(), scrolling);
}

void ScintillaQuick_core::selectCurrentWord()
{
    auto pos = CurrentPosition();
    const auto length = pdoc->Length();
    if (length <= 0) {
        return;
    }

    if (pos < 0) {
        pos = 0;
    }
    else
    if (pos > length) {
        pos = length;
    }

    const auto start_pos = pdoc->ExtendWordSelect(pos, -1, true);
    const auto end_pos   = pdoc->ExtendWordSelect(start_pos, 1, true);

    if (start_pos >= end_pos) {
        return;
    }

    SetSelection(start_pos, end_pos);

    emit cursorPositionChanged();
}

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

Render_frame ScintillaQuick_core::current_render_frame(
    bool static_content_dirty,
    bool ensure_styled,
    bool scrolling)
{
    Render_frame frame;

    if (!m_owner) {
        return frame;
    }

    const PRectangle client_rect = GetClientRectangle();
    const PRectangle text_rect   = GetTextRectangle();

    if (client_rect.Width() <= 0.0 || client_rect.Height() <= 0.0) {
        return frame;
    }

    frame.text_rect = QRectF(
        text_rect.left, text_rect.top, text_rect.Width(), text_rect.Height());
    frame.margin_rect = QRectF(
        client_rect.left,
        client_rect.top,
        std::max<XYPOSITION>(0.0, text_rect.left - client_rect.left),
        client_rect.Height());

    const int estimated_line_height = std::max(1, vs.lineHeight);
    const int estimated_lines = std::max<int>(
        1,
        static_cast<int>(client_rect.Height() / estimated_line_height) + 2);
    frame.visual_lines.reserve(estimated_lines);
    frame.selection_primitives.reserve(4);
    frame.caret_primitives.reserve(4);
    frame.margin_text_primitives.reserve(estimated_lines);

    if (ensure_styled) {
        ensure_visible_range_styled(scrolling);
    }
    if (static_content_dirty) {
        RefreshStyleData();
        WrapLines(WrapScope::wsVisible);
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

    Render_frame_builder collector(frame, static_content_dirty, *this);
    PRectangle capture_rect = client_rect;

    const bool buffered_draw_before_capture = view.bufferedDraw;
    view.bufferedDraw = false;
    const auto restore_buffered_draw =
        qScopeGuard([&] { view.bufferedDraw = buffered_draw_before_capture; });

    view.PaintText(surface, *this, capture_rect, client_rect, vs, &collector);

    PRectangle margin_rect = capture_rect;
    margin_rect.Move(0.0, -GetVisibleOriginInMain().y);
    margin_rect.left = 0.0;
    margin_rect.right = static_cast<XYPOSITION>(vs.fixedColumnWidth);
    if (capture_rect.Intersects(margin_rect)) {
        marginView.PaintMargin(surface, topLine, capture_rect, margin_rect, *this, vs, &collector);
    }

    if (horizontalScrollBarVisible && trackLineWidth && (view.lineWidthMaxSeen > scrollWidth)) {
        scrollWidth = view.lineWidthMaxSeen;
        SetScrollBars();
    }

    return frame;
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
    if (!mime_data) {
        return;
    }
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
    if (!mime_data) {
        return;
    }
#if defined(Q_OS_WIN)
    // Add an empty marker
    mime_data->setData(sVSEditorLineCutCopy, QByteArray());
#endif
}

static bool is_rectangular_in_mime(const QMimeData* mime_data)
{
    if (!mime_data) {
        return false;
    }
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
    if (!mime_data) {
        return false;
    }
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
        register_owned_window(ct.wCallTip, call_tip_item, Platform_owned_window_kind::CallTip);
    }

    if (QQuickItem* call_tip_item = resolve_window_item(ct.wCallTip.GetID())) {
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
    const int current_wnd_proc_depth = ++m_wnd_proc_depth;
    const auto leave_wnd_proc =
        qScopeGuard([this] { --m_wnd_proc_depth; });

    sptr_t result = 0;
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
                result = reinterpret_cast<sptr_t>(DirectFunction);
                break;

            case Message::GetDirectStatusFunction:
                result = reinterpret_cast<sptr_t>(DirectStatusFunction);
                break;

            case Message::GetDirectPointer:
                result = reinterpret_cast<sptr_t>(this);
                break;

            case Message::SetRectangularSelectionModifier:
                m_rectangular_selection_modifier = static_cast<int>(w_param);
                break;

            case Message::GetRectangularSelectionModifier:
                result = m_rectangular_selection_modifier;
                break;

            default:
                result = ScintillaBase::WndProc(i_message, w_param, l_param);
                break;
        }
    }
    catch (std::bad_alloc&) {
        errorStatus = Status::BadAlloc;
    }
    catch (...) {
        errorStatus = Status::Failure;
    }

    for (Direct_status_capture& capture : m_direct_status_captures) {
        if (!capture.captured && capture.wnd_proc_depth == current_wnd_proc_depth) {
            capture.status   = errorStatus;
            capture.captured = true;
        }
    }
    return result;
}

sptr_t ScintillaQuick_core::DefWndProc(Message, uptr_t, sptr_t)
{
    return 0;
}

sptr_t ScintillaQuick_core::DispatchDirectMessage(unsigned int i_message, uptr_t w_param, sptr_t l_param)
{
    if (!m_owner) {
        return 0;
    }
    return m_owner->ScintillaQuick_item::send(i_message, w_param, l_param);
}

sptr_t ScintillaQuick_core::DispatchDirectStatusMessage(
    unsigned int i_message,
    uptr_t       w_param,
    sptr_t       l_param,
    Status&      status_after_dispatch)
{
    if (!m_owner) {
        status_after_dispatch = Status::Failure;
        return 0;
    }

    const std::size_t capture_index = m_direct_status_captures.size();
    m_direct_status_captures.push_back(
        Direct_status_capture{m_wnd_proc_depth + 1, Status::Failure, false});
    const auto discard_capture = qScopeGuard([this, capture_index] {
        if (capture_index < m_direct_status_captures.size()) {
            m_direct_status_captures.erase(
                m_direct_status_captures.begin()
                + static_cast<std::ptrdiff_t>(capture_index));
        }
    });

    const sptr_t return_value =
        m_owner->ScintillaQuick_item::send(i_message, w_param, l_param);

    Direct_status_capture& capture = m_direct_status_captures[capture_index];
    if (!capture.captured) {
        // If a future direct path returns before entering WndProc, report
        // the current editor status rather than leaking capture state into
        // the next unrelated message.
        capture.status   = errorStatus;
        capture.captured = true;
    }

    status_after_dispatch = capture.status;
    return return_value;
}

sptr_t ScintillaQuick_core::DirectFunction(sptr_t ptr, unsigned int i_message, uptr_t w_param, sptr_t l_param)
{
    ScintillaQuick_core* sci = reinterpret_cast<ScintillaQuick_core*>(ptr);
    if (!sci) {
        return 0;
    }
    return sci->DispatchDirectMessage(i_message, w_param, l_param);
}

sptr_t ScintillaQuick_core::DirectStatusFunction(
    sptr_t       ptr,
    unsigned int i_message,
    uptr_t       w_param,
    sptr_t       l_param,
    int*         p_status)
{
    ScintillaQuick_core* sci  = reinterpret_cast<ScintillaQuick_core*>(ptr);
    if (!sci) {
        if (p_status) {
            *p_status = static_cast<int>(Status::Failure);
        }
        return 0;
    }
    Status status = Status::Failure;
    const sptr_t return_value =
        sci->DispatchDirectStatusMessage(i_message, w_param, l_param, status);
    if (p_status) {
        *p_status = static_cast<int>(status);
    }
    return return_value;
}

void ScintillaQuick_core::PartialPaint(const PRectangle& rect)
{
    PartialPaintQml(rect, nullptr);
}

// Raster reference path used by tests through ScintillaQuick_validation_access.
// The production Qt Quick path builds Render_frame snapshots and renders those
// with scene graph nodes.
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
    if (!data) {
        return;
    }
    QString text     = data->text();
    bool rectangular = is_rectangular_in_mime(data);
    QByteArray bytes = BytesForDocument(text);
    int len          = bytes.length();

    SelectionPosition move_pos = SPositionFromLocation(point, false, false, UserVirtualSpace());

    DropAt(move_pos, bytes, len, move, rectangular);
}

void ScintillaQuick_core::DropUrls(const QMimeData* data)
{
    if (!data) {
        return;
    }
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
