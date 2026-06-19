// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file ScintillaQuick_item.cpp - Qt Quick item (QQuickItem) that wraps
// ScintillaQuick_core and drives events, input method handling and
// scene-graph rendering. This is NOT a QWidget.

#include <scintillaquick/scintillaquick_item.h>
#include "scintillaquick_core.h"
#include "scintillaquick_dispatch_table.h"
#include "scintillaquick_platqt.h"
#include "scintillaquick_scene_graph_renderer.h"

// Internal Scintilla headers that are not part of the public
// `scintillaquick_item.h` surface. They are required here for Platform::,
// Scintilla::Internal::Point, and PRectangle references in this TU.
#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"
#include "Position.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethod>
#include <QMetaMethod>
#include <QMetaType>
#include <QPalette>
#include <QQuickWindow>
#include <QSGNode>
#include <QTextFormat>
#include <QVarLengthArray>
#include <QTimer>
#include <qqml.h>
#include <QtGlobal>
#include <QPoint>
#include <QPair>
#include <QList>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

constexpr int k_indicator_input     = static_cast<int>(Scintilla::IndicatorNumbers::Ime);
constexpr int k_indicator_target    = k_indicator_input + 1;
constexpr int k_indicator_converted = k_indicator_input + 2;
constexpr int k_indicator_unknown   = k_indicator_input + 3;

// This translation unit is dominated by Scintilla and Scintilla::Internal
// interop. Keeping those upstream namespaces open avoids burying the local
// logic under qualifiers while repo-owned identifiers still follow the local
// style guide.
using namespace Scintilla;
using namespace Scintilla::Internal;

class ScintillaQuick_item::Render_data
{
public:
    Render_snapshot snapshot;
    Render_frame frame;
    Scene_graph_renderer renderer;
    bool snapshot_dirty                      = true;
    bool static_content_dirty                = true;
    bool style_sync_needed                   = true;
    bool scrolling_update                    = false;
    bool overlay_content_dirty               = true;
    bool content_modified_since_last_capture = true;
    bool update_pending                      = false;
    int capture_base_first_visible_line = -1;
    int previous_first_visible_line     = -1;
    int previous_x_offset               = -1;
    std::vector<Caret_primitive> captured_caret_primitives;
};

namespace
{

constexpr int k_margin_count = SC_MAX_MARGIN + 1;
constexpr int k_vertical_scroll_reuse_buffer_min_lines = 16;

// `scene_graph_update_request` and `scene_graph_update_request_info_t` live in
// src/core/scintillaquick_dispatch_table.h (included above) so that they can be
// covered by dedicated unit tests without spinning up a Qt Quick window. They
// are reachable here via the file-scope `using namespace Scintilla::Internal;`
// above.

void register_notification_metatypes()
{
    static const int snapshot_type_id =
        qRegisterMetaType<ScintillaQuick_notification>("ScintillaQuick_notification");
    static const int position_type_id =
        qRegisterMetaType<Scintilla::Position>("Scintilla::Position");
    static const int modification_flags_type_id =
        qRegisterMetaType<Scintilla::ModificationFlags>("Scintilla::ModificationFlags");
    static const int fold_level_type_id =
        qRegisterMetaType<Scintilla::FoldLevel>("Scintilla::FoldLevel");

    (void)snapshot_type_id;
    (void)position_type_id;
    (void)modification_flags_type_id;
    (void)fold_level_type_id;
}

bool copy_byte_count(uptr_t byte_count, qsizetype& result)
{
    if (byte_count > static_cast<uptr_t>(std::numeric_limits<qsizetype>::max())) {
        return false;
    }

    result = static_cast<qsizetype>(byte_count);
    return true;
}

bool copy_position_byte_count(Position byte_count, qsizetype& result)
{
    if (byte_count < 0) {
        return false;
    }

    return copy_byte_count(static_cast<uptr_t>(byte_count), result);
}

QByteArray copy_nul_terminated_bytes(const char* text)
{
    return text ? QByteArray(text) : QByteArray();
}

bool macro_lparam_contract_is_numeric(Message message)
{
    switch (message) {
        case Message::DeleteRange:
        case Message::SetSel:
        case Message::LineScroll:
        case Message::ScrollRange:
        case Message::SetTargetRange:
        case Message::CallTipSetHlt:
        case Message::ShowLines:
        case Message::HideLines:
        case Message::BraceHighlight:
        case Message::CopyRange:
        case Message::SetSelection:
        case Message::FindIndicatorShow:
        case Message::FindIndicatorFlash:
        case Message::Colourise:
            return true;

        default:
            return false;
    }
}

void copy_notification_text_payload(
    const NotificationData& scn,
    ScintillaQuick_notification& snapshot)
{
    switch (scn.nmhdr.code) {
        case Notification::Modified: {
            snapshot.textAvailable = scn.text != nullptr;
            qsizetype byte_count = 0;
            if (scn.text && scn.length > 0 && copy_position_byte_count(scn.length, byte_count)) {
                snapshot.text = QByteArray(scn.text, byte_count);
            }
            break;
        }

        case Notification::UserListSelection:
        case Notification::AutoCSelection:
        case Notification::AutoCCompleted:
        case Notification::AutoCSelectionChange:
        case Notification::URIDropped:
            snapshot.textAvailable = scn.text != nullptr;
            snapshot.text = copy_nul_terminated_bytes(scn.text);
            break;

        default:
            snapshot.textAvailable = false;
            snapshot.text.clear();
            break;
    }
}

void copy_macro_lparam_payload(
    const NotificationData& scn,
    ScintillaQuick_notification& snapshot)
{
    switch (scn.message) {
        case Message::AddText:
        case Message::AppendText: {
            snapshot.lParamKind = ScintillaQuick_lparam_kind::Text;
            if (scn.lParam == 0) {
                break;
            }
            const char* const payload = reinterpret_cast<const char*>(scn.lParam);
            qsizetype byte_count = 0;
            if (copy_byte_count(scn.wParam, byte_count)) {
                snapshot.lParamText = QByteArray(payload, byte_count);
                snapshot.lParamTextAvailable = true;
            }
            break;
        }

        case Message::ReplaceSel:
        case Message::InsertText:
        case Message::SearchNext:
        case Message::SearchPrev:
            snapshot.lParamKind = ScintillaQuick_lparam_kind::Text;
            if (scn.lParam == 0) {
                break;
            }
            {
                const char* const payload = reinterpret_cast<const char*>(scn.lParam);
                snapshot.lParamText = copy_nul_terminated_bytes(payload);
                snapshot.lParamTextAvailable = true;
            }
            break;

        default:
            if (macro_lparam_contract_is_numeric(scn.message)) {
                snapshot.lParamKind = ScintillaQuick_lparam_kind::Numeric;
                snapshot.lParamValue = scn.lParam;
            }
            break;
    }
}

void copy_notification_lparam_payload(
    const NotificationData& scn,
    ScintillaQuick_notification& snapshot)
{
    switch (scn.nmhdr.code) {
        case Notification::AutoCSelection:
        case Notification::AutoCCompleted:
        case Notification::AutoCSelectionChange:
        case Notification::UserListSelection:
            snapshot.lParamKind = ScintillaQuick_lparam_kind::Numeric;
            snapshot.lParamValue = scn.lParam;
            break;

        case Notification::MacroRecord:
            copy_macro_lparam_payload(scn, snapshot);
            break;

        default:
            break;
    }
}

ScintillaQuick_notification notification_snapshot_from(const NotificationData& scn)
{
    ScintillaQuick_notification snapshot;
    snapshot.hwndFrom = reinterpret_cast<uptr_t>(scn.nmhdr.hwndFrom);
    snapshot.idFrom = scn.nmhdr.idFrom;
    snapshot.code = scn.nmhdr.code;
    snapshot.position = scn.position;
    snapshot.ch = scn.ch;
    snapshot.modifiers = scn.modifiers;
    snapshot.modificationType = scn.modificationType;
    snapshot.length = scn.length;
    snapshot.linesAdded = scn.linesAdded;
    snapshot.message = scn.message;
    snapshot.wParam = scn.wParam;
    snapshot.line = scn.line;
    snapshot.foldLevelNow = scn.foldLevelNow;
    snapshot.foldLevelPrev = scn.foldLevelPrev;
    snapshot.margin = scn.margin;
    snapshot.listType = scn.listType;
    snapshot.x = scn.x;
    snapshot.y = scn.y;
    snapshot.token = scn.token;
    snapshot.annotationLinesAdded = scn.annotationLinesAdded;
    snapshot.updated = scn.updated;
    snapshot.listCompletionMethod = scn.listCompletionMethod;
    snapshot.characterSource = scn.characterSource;

    copy_notification_text_payload(scn, snapshot);
    copy_notification_lparam_payload(scn, snapshot);
    return snapshot;
}

void translate_rect(QRectF& rect, qreal dy)
{
    if (!rect.isNull()) {
        rect.translate(0.0, dy);
    }
}

void translate_point(QPointF& point, qreal dy)
{
    point.setY(point.y() + dy);
}

void translate_render_frame(Render_frame& frame, qreal dy)
{
    translate_rect(frame.text_rect, dy);
    translate_rect(frame.margin_rect, dy);

    for (Visual_line_frame& visual_line : frame.visual_lines) {
        translate_point(visual_line.origin, dy);
        visual_line.baseline_y += dy;
        translate_rect(visual_line.clip_rect, dy);
        for (Text_run& run : visual_line.text_runs) {
            translate_point(run.position, dy);
            run.top += dy;
            run.bottom += dy;
            translate_rect(run.blob_text_clip_rect, dy);
            translate_rect(run.blob_outer_rect, dy);
            translate_rect(run.blob_inner_rect, dy);
        }
    }

    for (Selection_primitive& selection : frame.selection_primitives) {
        translate_rect(selection.rect, dy);
    }
    for (Caret_primitive& caret : frame.caret_primitives) {
        translate_rect(caret.rect, dy);
    }
    for (Indicator_primitive& indicator : frame.indicator_primitives) {
        translate_rect(indicator.rect, dy);
        translate_rect(indicator.line_rect, dy);
        translate_rect(indicator.character_rect, dy);
    }
    for (Current_line_primitive& current_line : frame.current_line_primitives) {
        translate_rect(current_line.rect, dy);
    }
    for (Marker_primitive& marker : frame.marker_primitives) {
        translate_rect(marker.rect, dy);
    }
    for (Margin_text_primitive& margin : frame.margin_text_primitives) {
        translate_point(margin.position, dy);
        translate_rect(margin.clip_rect, dy);
        margin.baseline_y += dy;
    }
    for (Fold_display_text_primitive& fold : frame.fold_display_texts) {
        translate_point(fold.position, dy);
        translate_rect(fold.rect, dy);
        fold.baseline_y += dy;
    }
    for (Eol_annotation_primitive& eol : frame.eol_annotations) {
        translate_point(eol.position, dy);
        translate_rect(eol.rect, dy);
        eol.baseline_y += dy;
    }
    for (Annotation_primitive& annotation : frame.annotations) {
        translate_point(annotation.position, dy);
        translate_rect(annotation.rect, dy);
        annotation.baseline_y += dy;
    }
    for (Whitespace_mark_primitive& mark : frame.whitespace_marks) {
        translate_rect(mark.rect, dy);
        mark.mid_y += dy;
    }
    for (Decoration_underline_primitive& underline : frame.decoration_underlines) {
        translate_rect(underline.rect, dy);
    }
    for (Indent_guide_primitive& guide : frame.indent_guides) {
        guide.top += dy;
        guide.bottom += dy;
    }
}

template <typename Dispatch, typename Apply_update>
sptr_t dispatch_scintilla_message(
    unsigned int i_message,
    uptr_t       w_param,
    sptr_t       l_param,
    Dispatch&&   dispatch,
    Apply_update&& apply_update)
{
    const sptr_t result = std::forward<Dispatch>(dispatch)(
        static_cast<Message>(i_message), w_param, l_param);
    std::forward<Apply_update>(apply_update)(scene_graph_update_request(i_message));
    return result;
}

QString text_for_visual_line(const Visual_line_frame& line)
{
    QString text;
    for (const Text_run& run : line.text_runs) {
        text += run.text;
    }
    return text;
}

QColor color_from_scintilla(sptr_t value)
{
    return QColorFromColourRGBA(ColourRGBA::FromIpRGB(value));
}

struct Style_attributes
{
    QColor foreground;
    QColor background;
    QFont font;
};

using Style_cache = std::array<std::optional<Style_attributes>, STYLE_MAX + 1>;

QFont font_for_style(const ScintillaQuick_item* item, int style)
{
    char font_name[128] = {};
    item->sends(SCI_STYLEGETFONT, style, font_name);

    QFont font;
    font.setFamily(QString::fromUtf8(font_name));

    const sptr_t size_fractional = item->send(SCI_STYLEGETSIZEFRACTIONAL, style);
    if (size_fractional > 0) {
        font.setPointSizeF(static_cast<qreal>(size_fractional) / SC_FONT_SIZE_MULTIPLIER);
    }
    else {
        font.setPointSize(static_cast<int>(item->send(SCI_STYLEGETSIZE, style)));
    }

    const int weight = static_cast<int>(item->send(SCI_STYLEGETWEIGHT, style));
    if (weight > 0) {
        font.setWeight(static_cast<QFont::Weight>(weight));
    }
    font.setItalic(item->send(SCI_STYLEGETITALIC, style) != 0);
    font.setUnderline(item->send(SCI_STYLEGETUNDERLINE, style) != 0);
    return font;
}

const Style_attributes& style_attributes_for(
    const ScintillaQuick_item* item,
    Style_cache& Style_cache,
    int style)
{
    const int bounded_style = std::clamp(style, 0, STYLE_MAX);
    std::optional<Style_attributes>& cached_attributes = Style_cache[bounded_style];
    if (!cached_attributes.has_value()) {
        Style_attributes attributes;
        attributes.foreground = color_from_scintilla(item->send(SCI_STYLEGETFORE, bounded_style));
        attributes.background = color_from_scintilla(item->send(SCI_STYLEGETBACK, bounded_style));
        attributes.font       = font_for_style(item, bounded_style);
        cached_attributes     = std::move(attributes);
    }
    return *cached_attributes;
}

int total_margin_width(const ScintillaQuick_item* item)
{
    int total = 0;
    for (int margin = 0; margin < k_margin_count; ++margin) {
        total += static_cast<int>(item->send(SCI_GETMARGINWIDTHN, margin));
    }
    return total;
}

QColor margin_background_color_for(
    const ScintillaQuick_item* item,
    Style_cache& Style_cache,
    int margin)
{
    const int margin_type = static_cast<int>(item->send(SCI_GETMARGINTYPEN, margin));
    const int margin_mask = static_cast<int>(item->send(SCI_GETMARGINMASKN, margin));

    if ((margin_mask & SC_MASK_FOLDERS) != 0) {
        return QColorFromColourRGBA(Platform::ChromeHighlight());
    }

    switch (margin_type) {
        case SC_MARGIN_BACK:   return style_attributes_for(item, Style_cache, STYLE_DEFAULT).background;
        case SC_MARGIN_FORE:   return style_attributes_for(item, Style_cache, STYLE_DEFAULT).foreground;
        case SC_MARGIN_COLOUR: return color_from_scintilla(item->send(SCI_GETMARGINBACKN, margin));
        default:               return style_attributes_for(item, Style_cache, STYLE_LINENUMBER).background;
    }
}

}

ScintillaQuick_item::ScintillaQuick_item(QQuickItem* parent)
    : QQuickItem(parent)
    , m_updates_enabled(true)
    , m_logical_width(0)
    , m_logical_height(0)
    , m_input_method_hints(Qt::ImhNone)
    , m_last_touch_press_time(-1)
    , m_core(new ScintillaQuick_core(this))
    , m_preedit_pos(-1)
    , m_render_data(std::make_unique<Render_data>())
{
    register_notification_metatypes();

    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptTouchEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setFlag(QQuickItem::ItemIsFocusScope, true);
    setFlag(QQuickItem::ItemHasContents, true);
    setFlag(QQuickItem::ItemAcceptsDrops, true);
    setFlag(QQuickItem::ItemClipsChildrenToShape, true);
    setClip(true);

    m_elapsed_timer.start();

    // All IME indicators drawn in same colour, blue, with different patterns
    const ColourRGBA colourIME(0, 0, UCHAR_MAX);
    m_core->vs.indicators[k_indicator_unknown]   = Indicator(IndicatorStyle::Hidden, colourIME);
    m_core->vs.indicators[k_indicator_input]     = Indicator(IndicatorStyle::Dots, colourIME);
    m_core->vs.indicators[k_indicator_converted] = Indicator(IndicatorStyle::CompositionThick, colourIME);
    m_core->vs.indicators[k_indicator_target]    = Indicator(IndicatorStyle::StraightBox, colourIME);

    connect(m_core, SIGNAL(notifyParent(Scintilla::NotificationData)), this,
        SLOT(notifyParent(Scintilla::NotificationData)));

    // Connect pass-through signals.
    connect(m_core, SIGNAL(horizontalRangeChanged(int, int)), this, SIGNAL(horizontalRangeChanged(int, int)));
    connect(m_core, SIGNAL(verticalRangeChanged(  int, int)), this, SIGNAL(verticalRangeChanged(  int, int)));
    connect(m_core, SIGNAL(horizontalScrolled(int)), this, SIGNAL(horizontalScrolled(int)));
    connect(m_core, SIGNAL(verticalScrolled(  int)), this, SIGNAL(verticalScrolled(  int)));

    connect(m_core, SIGNAL(notifyChange()), this, SIGNAL(notifyChange()));

    connect(m_core, SIGNAL(command(Scintilla::uptr_t, Scintilla::sptr_t)), this,
        SLOT(event_command(Scintilla::uptr_t, Scintilla::sptr_t)));

    connect(m_core, SIGNAL(aboutToCopy(QMimeData*)), this, SIGNAL(aboutToCopy(QMimeData*)));

    connect(m_core, SIGNAL(cursorPositionChanged()), this,
        SIGNAL(cursorPositionChanged())); // needed to update markers on android platform

    m_caret_blink_timer.setSingleShot(false);
    QObject::connect(&m_caret_blink_timer, &QTimer::timeout, this, [this]() {
        m_caret_blink_visible = !m_caret_blink_visible;
        if (m_render_data && m_updates_enabled) {
            m_render_data->snapshot_dirty = true;
            polish();
            update();
        }
    });

    send(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE);
    send(SCI_SETSCROLLWIDTHTRACKING, 1);
}

ScintillaQuick_item::~ScintillaQuick_item()
{
    // Tell the core to stop all of its timers and disconnect from the
    // clipboard before the ScintillaQuick_item subobject finishes
    // destructing. `m_core` is a child QObject (parented to `this`) and
    // Qt will destroy it as part of ~QObject(), which runs AFTER this
    // derived destructor body. Without this step, a queued timerEvent
    // or clipboard SelectionChanged slot can reach `m_core` between the
    // derived teardown and Qt's child-deletion pass and dispatch into
    // a sliced-down QQuickItem.
    if (m_core) {
        m_core->prepare_for_owner_destruction();
    }
}

void ScintillaQuick_item::apply_scene_graph_update_request(
    bool scroll_width_reset,
    bool needed,
    bool static_content_dirty,
    bool needs_style_sync,
    bool scrolling) const
{
    ScintillaQuick_item* self = const_cast<ScintillaQuick_item*>(this);

    if (scroll_width_reset) {
        self->reset_tracked_scroll_width();
    }
    // Re-entry guard: `syncQuickViewProperties()` issues SCI_* queries through
    // `send()`. A missed read-only classification should not recurse forever.
    if (needed && !m_in_sync_quick_view_properties) {
        const bool needs_property_sync = static_content_dirty || needs_style_sync || scrolling;
        if (needs_property_sync) {
            self->syncQuickViewProperties();
            if (static_content_dirty && m_render_data) {
                m_render_data->content_modified_since_last_capture = true;
            }
        }
        self->request_scene_graph_update(static_content_dirty, needs_style_sync, scrolling);
    }
}

sptr_t ScintillaQuick_item::send(
    unsigned int i_message,
    uptr_t       w_param,
    sptr_t       l_param) const
{
    auto apply_update_request = [this](const scene_graph_update_request_info_t& request) {
        apply_scene_graph_update_request(
            request.scroll_width_reset,
            request.needed,
            request.static_content_dirty,
            request.needs_style_sync,
            request.scrolling);
    };

    return dispatch_scintilla_message(
        i_message,
        w_param,
        l_param,
        [this](Message message, uptr_t w, sptr_t l) {
            return m_core->WndProc(message, w, l);
        },
        apply_update_request);
}

sptr_t ScintillaQuick_item::sends(
    unsigned int i_message,
    uptr_t       w_param,
    const char*  s) const
{
    auto apply_update_request = [this](const scene_graph_update_request_info_t& request) {
        apply_scene_graph_update_request(
            request.scroll_width_reset,
            request.needed,
            request.static_content_dirty,
            request.needs_style_sync,
            request.scrolling);
    };

    return dispatch_scintilla_message(
        i_message,
        w_param,
        reinterpret_cast<sptr_t>(s),
        [this](Message message, uptr_t w, sptr_t l) {
            return m_core->WndProc(message, w, l);
        },
        apply_update_request);
}

void ScintillaQuick_item::request_scene_graph_update(
    bool static_content_dirty,
    bool needs_style_sync,
    bool scrolling)
{
    if (!m_render_data || !m_updates_enabled) {
        return;
    }

    m_render_data->snapshot_dirty        = true;
    m_render_data->static_content_dirty  = m_render_data->static_content_dirty || static_content_dirty;
    m_render_data->style_sync_needed     = m_render_data->style_sync_needed || needs_style_sync;
    m_render_data->scrolling_update      = m_render_data->scrolling_update || scrolling;
    m_render_data->overlay_content_dirty = true;
    if (m_render_data->update_pending) {
        return;
    }
    m_render_data->update_pending = true;
    polish();
    update();
}

void ScintillaQuick_item::scrollRow(int delta_lines)
{
    int current_line = m_core->TopLineOfMain();
    scrollVertical(current_line + delta_lines);
}

void ScintillaQuick_item::scrollColumn(int delta_columns)
{
    int current_column_in_pixel = send(SCI_GETXOFFSET);
    int new_value = current_column_in_pixel + delta_columns * getCharWidth();
    if (new_value < 0) {
        new_value = 0;
    }
    // Go through scrollHorizontal() rather than SCI_SETXOFFSET so the
    // horizontal scroll signal is emitted on the Qt side.
    scrollHorizontal(new_value);
}

void ScintillaQuick_item::cmdContextMenu(int menu_id)
{
    m_core->Command(menu_id);
}

void ScintillaQuick_item::enableUpdate(bool enable)
{
    m_updates_enabled = enable;
    if (m_updates_enabled) {
        request_scene_graph_update();
    }
}

void ScintillaQuick_item::scrollHorizontal(int value)
{
    m_core->HorizontalScrollTo(value);
    syncQuickViewProperties();
    request_scene_graph_update(true, false, false);
}

void ScintillaQuick_item::scrollVertical(int value)
{
    m_core->ScrollTo(value);
    request_scene_graph_update(true, false, true);
}

bool ScintillaQuick_item::event(QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        // Circumvent the tab focus convention.
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return event->isAccepted();
    }
    return QQuickItem::event(event);
}

namespace
{

bool isWheelEventHorizontal(QWheelEvent* event)
{
    return event->angleDelta().y() == 0;
}

int wheelEventYDelta(QWheelEvent* event)
{
    return event->angleDelta().y();
}

}

void ScintillaQuick_item::wheelEvent(QWheelEvent* event)
{
    if (isWheelEventHorizontal(event)) {
        QQuickItem::wheelEvent(event);
    }
    else
    if (event->modifiers() & Qt::ControlModifier) {
        // Zoom! We play with the font sizes in the styles.
        // Number of steps/line is ignored, we just care if sizing up or down
        if (wheelEventYDelta(event) > 0) {
            m_core->KeyCommand(Message::ZoomIn);
        }
        else {
            m_core->KeyCommand(Message::ZoomOut);
        }
        if (m_render_data) {
            m_render_data->content_modified_since_last_capture = true;
        }
        syncQuickViewProperties();
        request_scene_graph_update(true, true, false);
        event->accept();
    }
    else {
        // Scroll
        int lines_to_scroll = 3;
        if (event->angleDelta().y() > 0) {
            scrollVertical(m_core->topLine - lines_to_scroll);
        }
        else {
            scrollVertical(m_core->topLine + lines_to_scroll);
        }
        event->accept();
    }
}

void ScintillaQuick_item::focusInEvent(QFocusEvent * event)
{
    m_core->SetFocusState(true);

    QQuickItem::focusInEvent(event);
    syncCaretBlinkTimer(true);
    request_scene_graph_update(false, false, false);
}

void ScintillaQuick_item::focusOutEvent(QFocusEvent * event)
{
    m_core->SetFocusState(false);

    QQuickItem::focusOutEvent(event);
    syncCaretBlinkTimer(false);
    request_scene_graph_update(false, false, false);
}

void ScintillaQuick_item::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);

    // trigger resize handling only if the size of the control has changed
    // no update is needed for a position change
    if (newGeometry.width()  != oldGeometry.width() ||
        newGeometry.height() != oldGeometry.height())
    {
        m_core->ChangeSize();
        emit resized();

        if (m_render_data) {
            m_render_data->content_modified_since_last_capture = true;
        }
        request_scene_graph_update(true, true, false);
    }
}

void ScintillaQuick_item::keyPressEvent(QKeyEvent * event)
{
    bool view_changed = false;

    // All keystrokes containing the meta modifier are
    // assumed to be shortcuts not handled by scintilla.
    if (event->modifiers() & Qt::MetaModifier) {
        QQuickItem::keyPressEvent(event);
        emit keyPressed(event);
        return;
    }

    int key = 0;
    switch (event->key()) {
        case Qt::Key_Down:          key = SCK_DOWN;     break;
        case Qt::Key_Up:            key = SCK_UP;       break;
        case Qt::Key_Left:          key = SCK_LEFT;     break;
        case Qt::Key_Right:         key = SCK_RIGHT;    break;
        case Qt::Key_Home:          key = SCK_HOME;     break;
        case Qt::Key_End:           key = SCK_END;      break;
        case Qt::Key_PageUp:        key = SCK_PRIOR;    break;
        case Qt::Key_PageDown:      key = SCK_NEXT;     break;
        case Qt::Key_Delete:        key = SCK_DELETE;   break;
        case Qt::Key_Insert:        key = SCK_INSERT;   break;
        case Qt::Key_Escape:        key = SCK_ESCAPE;   break;
        case Qt::Key_Backspace:     key = SCK_BACK;     break;
        case Qt::Key_Plus:          key = SCK_ADD;      break;
        case Qt::Key_Minus:         key = SCK_SUBTRACT; break;
        case Qt::Key_Backtab:       // fall through
        case Qt::Key_Tab:           key = SCK_TAB;      break;
        case Qt::Key_Enter:         // fall through
        case Qt::Key_Return:        key = SCK_RETURN;   break;
        case Qt::Key_Control:       key = 0;            break;
        case Qt::Key_Alt:           key = 0;            break;
        case Qt::Key_Shift:         key = 0;            break;
        case Qt::Key_Meta:          key = 0;            break;
        default:                    key = event->key(); break;
    }

#ifdef Q_OS_ANDROID
    // do not use the current state, because a key event could also be triggered by the input context menu as meta
    // keyboard event ! --> needed for edit context menu on android platform
    bool shift = event->modifiers() & Qt::ShiftModifier;
    bool ctrl  = event->modifiers() & Qt::ControlModifier;
    bool alt   = event->modifiers() & Qt::AltModifier;
#else
    bool shift = event->modifiers() & Qt::ShiftModifier;
    bool ctrl  = event->modifiers() & Qt::ControlModifier;
    bool alt   = event->modifiers() & Qt::AltModifier;
#endif

    bool consumed = false;
    bool added = m_core->KeyDownWithModifiers(
        static_cast<Keys>(key),
        ModifierFlags(shift, ctrl, alt),
        &consumed) != 0;
    if (!consumed) {
        consumed = added;
    }
    view_changed = consumed;

    if (!consumed) {
        // Don't insert text if the control key was pressed unless
        // it was pressed in conjunction with alt for AltGr emulation.
        bool input = (!ctrl || alt);

        // Additionally, on non-mac platforms, don't insert text
        // if the alt key was pressed unless control is also present.
        // On mac alt can be used to insert special characters.
#ifndef Q_OS_MAC
        input &= (!alt || ctrl);
#endif

        QString text = event->text();
        if (input && !text.isEmpty() && text[0].isPrint()) {
            QByteArray utext = m_core->BytesForDocument(text);
            m_core->InsertCharacter(std::string_view(utext.data(), utext.size()), CharacterSource::DirectInput);
            view_changed = true;
        }
        else {
            event->ignore();
        }
    }

    if (view_changed) {
        cursorChangedUpdateMarker();
        request_scene_graph_update(false, false, false);
    }

    emit keyPressed(event);
}

#if !defined(Q_OS_MAC) && !defined(Q_OS_WIN)
static int modifierTranslated(int sciModifier)
{
    switch (sciModifier) {
        case SCMOD_SHIFT: return Qt::ShiftModifier;
        case SCMOD_CTRL:  return Qt::ControlModifier;
        case SCMOD_ALT:   return Qt::AltModifier;
        case SCMOD_SUPER: return Qt::MetaModifier;
        default:          return 0;
    }
}
#endif

void ScintillaQuick_item::mousePressEvent(QMouseEvent * event)
{
    Point pos = PointFromQPoint(event->pos());

    emit buttonPressed(event);

    auto finish_consumed_press = [this, event]() {
        forceActiveFocus();
        emit enableScrollViewInteraction(false);
        event->setAccepted(true);
    };

    if (event->button() == Qt::MiddleButton &&
        QGuiApplication::clipboard()->supportsSelection())
    {
        SelectionPosition selPos = m_core->SPositionFromLocation(
            pos, false, false, m_core->UserVirtualSpace());
        m_core->sel.Clear();
        m_core->SetSelection(selPos, selPos);
        m_core->PasteFromMode(QClipboard::Selection);
        cursorChangedUpdateMarker();
        request_scene_graph_update(true, true, false);
        finish_consumed_press();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_core->ButtonDownWithModifiers(pos, m_elapsed_timer.elapsed(), ModifiersOfMouse());

        cursorChangedUpdateMarker();
        request_scene_graph_update(false, false, false);
    }

    if (event->button() == Qt::RightButton) {
        m_core->RightButtonDownWithModifiers(pos, m_elapsed_timer.elapsed(), ModifiersOfKeyboard());

        Point ctx_global = PointFromQPoint(event->globalPos());
        Point ctx_pt     = PointFromQPoint(event->pos());
        if (!m_core->PointInSelection(ctx_pt)) {
            m_core->SetEmptySelection(m_core->PositionFromLocation(ctx_pt));
        }
        // TODO: call context menu callback if set otherwise use default context menu...
        if (m_core->ShouldDisplayPopup(ctx_pt)) {
            m_core->ContextMenu(ctx_global);
        }
        request_scene_graph_update(false, false, false);
    }

    finish_consumed_press();
}

void ProcessScintillaContextMenu(
    Scintilla::Internal::Point                     pt,
    const Scintilla::Internal::Window&             w,
    const QList<QPair<QString, QPair<int, bool>>>& menu)
{
    ScintillaQuick_item* qt_object = static_cast<ScintillaQuick_item*>(w.GetID());

    emit qt_object->clearContextMenu();
    for (const QPair<QString, QPair<int, bool>>& item : menu) {
        if (item.first.size() > 0) {
            emit qt_object->addToContextMenu(item.second.first, item.first, item.second.second);
        }
    }

    QPoint point(pt.x, pt.y);
    emit qt_object->showContextMenu(point);
}

void ScintillaQuick_item::mouseReleaseEvent(QMouseEvent * event)
{
    const QPoint point = event->pos();
    if (event->button() == Qt::LeftButton) {
        m_core->ButtonUpWithModifiers(PointFromQPoint(point), m_elapsed_timer.elapsed(), ModifiersOfKeyboard());
    }

    const sptr_t pos  = send(SCI_POSITIONFROMPOINT, point.x(), point.y());
    const sptr_t line = send(SCI_LINEFROMPOSITION, pos);
    int modifiers = QGuiApplication::keyboardModifiers();

    emit textAreaClicked(line, modifiers);
    emit buttonReleased(event);

    emit enableScrollViewInteraction(true);
    request_scene_graph_update(false, false, false);

    event->setAccepted(true);
}

void ScintillaQuick_item::mouseDoubleClickEvent(QMouseEvent * event)
{
    // Scintilla does its own double-click detection.
    Q_UNUSED(event);
}

void ScintillaQuick_item::mouseMoveEvent(QMouseEvent * event)
{
    Point pos  = PointFromQPoint(event->pos());

    const int previous_first_visible_line = static_cast<int>(send(SCI_GETFIRSTVISIBLELINE));
    const int previous_x_offset           = static_cast<int>(send(SCI_GETXOFFSET));

    m_core->ButtonMoveWithModifiers(pos, m_elapsed_timer.elapsed(), ModifiersOfMouse());

    cursorChangedUpdateMarker();
    const int current_first_visible_line = static_cast<int>(send(SCI_GETFIRSTVISIBLELINE));
    const int current_x_offset           = static_cast<int>(send(SCI_GETXOFFSET));
    const bool vertical_scroll_changed   = current_first_visible_line != previous_first_visible_line;
    const bool horizontal_scroll_changed = current_x_offset != previous_x_offset;
    if (vertical_scroll_changed || horizontal_scroll_changed) {
        syncQuickViewProperties();
        request_scene_graph_update(true, false, vertical_scroll_changed);
    }
    else {
        request_scene_graph_update();
    }

    event->setAccepted(true);
}

void ScintillaQuick_item::dragEnterEvent(QDragEnterEvent * event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
    else
    if (event->mimeData()->hasText()) {
        event->acceptProposedAction();

        Point point = PointFromQPoint(event->pos());
        m_core->DragEnter(point);
    }
    else {
        event->ignore();
    }
}

void ScintillaQuick_item::dragLeaveEvent(QDragLeaveEvent* /* event */)
{
    m_core->DragLeave();
}

void ScintillaQuick_item::dragMoveEvent(QDragMoveEvent * event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
    else
    if (event->mimeData()->hasText()) {
        event->acceptProposedAction();

        Point point = PointFromQPoint(event->pos());
        m_core->DragMove(point);
    }
    else {
        event->ignore();
    }
}

void ScintillaQuick_item::dropEvent(QDropEvent * event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        m_core->DropUrls(event->mimeData());
    }
    else
    if (event->mimeData()->hasText()) {
        event->acceptProposedAction();

        Point point = PointFromQPoint(event->pos());
        bool move = (event->source() == this && event->proposedAction() == Qt::MoveAction);
        m_core->Drop(point, event->mimeData(), move);
    }
    else {
        event->ignore();
    }
}

bool ScintillaQuick_item::IsHangul(const QChar qchar)
{
    unsigned int unicode = qchar.unicode();
    // Korean character ranges used for preedit chars.
    // http://www.programminginkorean.com/programming/hangul-in-unicode/
    const bool hangul_jamo            = (0x1100 <= unicode && unicode <= 0x11FF);
    const bool hangul_compatible_jamo = (0x3130 <= unicode && unicode <= 0x318F);
    const bool hangul_jamo_extended_a = (0xA960 <= unicode && unicode <= 0xA97F);
    const bool hangul_jamo_extended_b = (0xD7B0 <= unicode && unicode <= 0xD7FF);
    const bool hangul_syllable        = (0xAC00 <= unicode && unicode <= 0xD7A3);
    return
        hangul_jamo            ||
        hangul_compatible_jamo ||
        hangul_syllable        ||
        hangul_jamo_extended_a ||
        hangul_jamo_extended_b;
}

void ScintillaQuick_item::MoveImeCarets(Scintilla::Position offset)
{
    // Move carets relatively by bytes
    for (size_t r = 0; r < m_core->sel.Count(); r++) {
        const Sci::Position positionInsert = m_core->sel.Range(r).Start().Position();
        m_core->sel.Range(r).caret.SetPosition(positionInsert + offset);
        m_core->sel.Range(r).anchor.SetPosition(positionInsert + offset);
    }
}

void ScintillaQuick_item::DrawImeIndicator(int indicator, int len)
{
    // Emulate the visual style of IME characters with indicators.
    // Draw an indicator on the character before caret by the character bytes of len
    // so it should be called after InsertCharacter().
    // It does not affect caret positions.
    if (indicator < INDICATOR_CONTAINER || indicator > INDICATOR_MAX) {
        return;
    }
    m_core->pdoc->DecorationSetCurrentIndicator(indicator);
    for (size_t r = 0; r < m_core->sel.Count(); r++) {
        const Sci::Position positionInsert = m_core->sel.Range(r).Start().Position();
        m_core->pdoc->DecorationFillRange(positionInsert - len, 1, len);
    }
}

static int bounded_ime_length(Sci::Position length)
{
    if (length <= 0) {
        return 0;
    }
    const Sci::Position max_int = static_cast<Sci::Position>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(length, max_int));
}

static std::pair<int, int> clamped_ime_attribute_range(int start, int length, int limit)
{
    const long long bounded_limit = std::max(0, limit);
    long long begin = static_cast<long long>(start);
    long long end = begin + static_cast<long long>(length);
    if (end < begin) {
        std::swap(begin, end);
    }

    begin = std::clamp(begin, 0LL, bounded_limit);
    end = std::clamp(end, 0LL, bounded_limit);
    if (end < begin) {
        end = begin;
    }

    return {static_cast<int>(begin), static_cast<int>(end - begin)};
}

static int clamped_ime_position(int position, int limit)
{
    return static_cast<int>(std::clamp(
        static_cast<long long>(position),
        0LL,
        static_cast<long long>(std::max(0, limit))));
}

static int GetImeCaretPos(QInputMethodEvent * event, int preedit_length)
{
    foreach (QInputMethodEvent::Attribute attr, event->attributes()) {
        if (attr.type == QInputMethodEvent::Cursor) {
            return clamped_ime_position(attr.start, preedit_length);
        }
    }
    return 0;
}

static std::vector<int> MapImeIndicators(QInputMethodEvent * event)
{
    const int preedit_length = event->preeditString().size();
    std::vector<int> ime_indicator(preedit_length, k_indicator_unknown);
    foreach (QInputMethodEvent::Attribute attr, event->attributes()) {
        if (attr.type == QInputMethodEvent::TextFormat) {
            QTextFormat format = attr.value.value<QTextFormat>();
            QTextCharFormat char_format = format.toCharFormat();

            int indicator = k_indicator_unknown;
            switch (char_format.underlineStyle()) {
                case QTextCharFormat::NoUnderline: // win32, linux
                    indicator = k_indicator_target;
                    break;
                case QTextCharFormat::SingleUnderline: // osx
                case QTextCharFormat::DashUnderline:   // win32, linux
                    indicator = k_indicator_input;
                    break;
                case QTextCharFormat::DotLine:
                case QTextCharFormat::DashDotLine:
                case QTextCharFormat::WaveUnderline:
                case QTextCharFormat::SpellCheckUnderline:
                    indicator = k_indicator_converted;
                    break;

                default:
                    indicator = k_indicator_unknown;
            }

            if (format.hasProperty(QTextFormat::BackgroundBrush)) // win32, linux
                indicator = k_indicator_target;

#ifdef Q_OS_OSX
            if (char_format.underlineStyle() == QTextCharFormat::SingleUnderline) {
                QColor uc = char_format.underlineColor();
                if (uc.lightness() < 2) { // osx
                    indicator = k_indicator_target;
                }
            }
#endif

            const auto [range_start, range_length] =
                clamped_ime_attribute_range(attr.start, attr.length, preedit_length);
            const int range_end = range_start + range_length;
            for (int i = range_start; i < range_end; i++) {
                ime_indicator[i] = indicator;
            }
        }
    }
    return ime_indicator;
}

void ScintillaQuick_item::inputMethodEvent(QInputMethodEvent * event)
{
    // Copy & paste by johnsonj with a lot of helps of Neil
    // Great thanks for my forerunners, jiniya and BLUEnLIVE

    if (m_core->pdoc->IsReadOnly() || m_core->SelectionContainsProtected()) {
        m_core->ShowCaretAtCurrentPosition();
        cursorChangedUpdateMarker();
        request_scene_graph_update(false, false, false);
        event->accept();
        return;
    }

    // Follows the shape of QQuickTextControlPrivate::inputMethodEvent().
    // Only Selection attributes are actioned here; Cursor attributes
    // from the IME are intentionally ignored because the caret is
    // driven by the ScintillaQuick selection model after Scintilla
    // applies the composition, and there is no public Scintilla hook
    // to override the cursor visibility while a composition is in
    // progress. The loop still inspects every attribute so that new
    // Qt attribute types do not silently cause us to skip a Selection
    // that follows them.
    for (int i = 0; i < event->attributes().size(); ++i) {
        const QInputMethodEvent::Attribute& a = event->attributes().at(i);
        if (a.type == QInputMethodEvent::Selection) {
            const Sci::Position cur_pos = m_core->CurrentPosition();
            const int para_start        = m_core->pdoc->ParaUp(cur_pos);
            const int para_end          = m_core->pdoc->ParaDown(cur_pos);
            const int para_length       = bounded_ime_length(para_end - para_start);
            const auto [selection_start, selection_length] =
                clamped_ime_attribute_range(a.start, a.length, para_length);

            SelectionPosition new_start(para_start + selection_start);
            SelectionPosition new_end(para_start + selection_start + selection_length);
            if (new_start > new_end) {
                m_core->SetSelection(new_end, new_start);
            }
            else {
                m_core->SetSelection(new_start, new_end);
            }

            // update markers by triggering QtAndroidInputContext::updateSelectionHandles()
            cursorChangedUpdateMarker();
        }
    }

    bool initial_compose = false;
    if (m_core->pdoc->TentativeActive()) {
        m_core->pdoc->TentativeUndo();
    }
    else {
        // No tentative undo means start of this composition so
        // Fill in any virtual spaces.
        initial_compose = true;
    }

    m_core->view.imeCaretBlockOverride = false;

    if (!event->commitString().isEmpty()) {
        const QString& commit_str = event->commitString();
        const int commit_str_len  = commit_str.length();

        for (int i = 0; i < commit_str_len;) {
            const int uc_width           = commit_str.at(i).isHighSurrogate() ? 2 : 1;
            const QString one_char_utf16 = commit_str.mid(i, uc_width);
            const QByteArray one_char    = m_core->BytesForDocument(one_char_utf16);

            m_core->InsertCharacter(
                std::string_view(one_char.data(), one_char.length()), CharacterSource::DirectInput);
            i += uc_width;
        }
    }
    else
    if (!event->preeditString().isEmpty()) {
        const QString preedit_str = event->preeditString();
        const int preedit_str_len = preedit_str.length();

        if (initial_compose) {
            m_core->ClearBeforeTentativeStart();
        }
        m_core->pdoc->TentativeStart(); // TentativeActive() from now on.

        std::vector<int> ime_indicator = MapImeIndicators(event);

        for (int i = 0; i < preedit_str_len;) {
            const int uc_width           = preedit_str.at(i).isHighSurrogate() ? 2 : 1;
            const QString one_char_utf16 = preedit_str.mid(i, uc_width);
            const QByteArray one_char    = m_core->BytesForDocument(one_char_utf16);
            const int one_char_len       = one_char.length();

            m_core->InsertCharacter(
                std::string_view(one_char.data(), one_char_len), CharacterSource::TentativeInput);

            DrawImeIndicator(ime_indicator[i], one_char_len);
            i += uc_width;
        }

        // Move IME carets.
        int ime_caret_pos            = GetImeCaretPos(event, preedit_str_len);
        int ime_end_to_ime_caret_u16 = ime_caret_pos - preedit_str_len;
        const Sci::Position ime_caret_pos_doc =
            m_core->pdoc->GetRelativePositionUTF16(m_core->CurrentPosition(), ime_end_to_ime_caret_u16);

        MoveImeCarets(-m_core->CurrentPosition() + ime_caret_pos_doc);

        if (IsHangul(preedit_str.at(0))) {
#ifndef Q_OS_WIN
            if (ime_caret_pos > 0) {
                int one_char_before = m_core->pdoc->GetRelativePosition(m_core->CurrentPosition(), -1);
                MoveImeCarets(-m_core->CurrentPosition() + one_char_before);
            }
#endif
            m_core->view.imeCaretBlockOverride = true;
        }

        // Set candidate box position for Qt::ImMicroFocus.
        m_preedit_pos = m_core->CurrentPosition();
        m_core->EnsureCaretVisible();
    }
    m_core->ShowCaretAtCurrentPosition();
    cursorChangedUpdateMarker();
    syncQuickViewProperties();
    if (m_render_data) {
        m_render_data->content_modified_since_last_capture = true;
    }
    request_scene_graph_update(true, true, false);
    event->accept();
}

QVariant ScintillaQuick_item::inputMethodQuery(Qt::InputMethodQuery property, QVariant argument) const
{
    // see: QQuickTextEdit::inputMethodQuery(...)
    const PRectangle text_rect = m_core ? m_core->GetTextRectangle() : PRectangle();
    const QPointF text_offset(text_rect.left, text_rect.top);

    if (property == Qt::ImCursorPosition && !argument.isNull()) {
        argument = QVariant(argument.toPointF() - text_offset);
        const QPointF pt = argument.toPointF();
        if (!pt.isNull()) {
            Point scintilla_point   = PointFromQPointF(pt);
            Sci::Position point_pos = m_core->PositionFromLocation(scintilla_point);
            int pos                 = send(SCI_GETCURRENTPOS);
            int para_start          = m_core->pdoc->ParaUp(pos);
            return QVariant((int)point_pos - para_start);
        }
        return inputMethodQuery(property);
    }

    auto v = inputMethodQuery(property);
    if (property == Qt::ImCursorRectangle || property == Qt::ImAnchorRectangle) {
        v = QVariant(v.toRectF().translated(text_offset));
    }

    return v;
}

QVariant ScintillaQuick_item::inputMethodQuery(Qt::InputMethodQuery query) const
{
    const Scintilla::Position pos  = send(SCI_GETCURRENTPOS);
    const Scintilla::Position line = send(SCI_LINEFROMPOSITION, pos);

    switch (query) {
        case Qt::ImEnabled: {
            return QVariant((bool)(flags() & ItemAcceptsInputMethod));
        }
        case Qt::ImHints:
            return QVariant((int)inputMethodHints());
        case Qt::ImInputItemClipRectangle:
            return QQuickItem::inputMethodQuery(query);
        // see: QQuickTextControl::inputMethodQuery()
        case Qt::ImMaximumTextLength:
            return QVariant(); // No limit.
        case Qt::ImAnchorRectangle: {
            SelectionPosition sel_end   = m_core->SelectionEnd();
            Point pt_end                = m_core->LocationFromPosition(sel_end);

            int width  = send(SCI_GETCARETWIDTH);
            int height = send(SCI_TEXTHEIGHT, line);
            return QRect(pt_end.x, pt_end.y, width, height);
        }
        // selection == Position <--> AnchorPosition
        case Qt::ImAnchorPosition: {
            SelectionPosition sel_end   = m_core->SelectionEnd();

            int para_start = m_core->pdoc->ParaUp(pos);
            return (int)sel_end.Position() - para_start;
        }
        case Qt::ImAbsolutePosition: {
            return QVariant((int)pos);
        }
        case Qt::ImTextAfterCursor: {
            // from Qt::ImSurroundingText:
            int para_start = m_core->pdoc->ParaUp(pos);
            int para_end   = m_core->pdoc->ParaDown(pos);
            QVarLengthArray<char, 1024> buffer(para_end - para_start + 1);

            Sci_CharacterRange charRange;
            charRange.cpMin = pos;
            charRange.cpMax = para_end;

            Sci_TextRange textRange;
            textRange.chrg      = charRange;
            textRange.lpstrText = buffer.data();

            send(SCI_GETTEXTRANGE, 0, (sptr_t)&textRange);

            return m_core->StringFromDocument(buffer.constData());
        }
        case Qt::ImTextBeforeCursor: {
            // from Qt::ImSurroundingText:
            int para_start = m_core->pdoc->ParaUp(pos);
            int para_end   = m_core->pdoc->ParaDown(pos);
            QVarLengthArray<char, 1024> buffer(para_end - para_start + 1);

            Sci_CharacterRange charRange;
            charRange.cpMin = para_start;
            charRange.cpMax = pos;

            Sci_TextRange textRange;
            textRange.chrg      = charRange;
            textRange.lpstrText = buffer.data();

            send(SCI_GETTEXTRANGE, 0, (sptr_t)&textRange);

            return m_core->StringFromDocument(buffer.constData());
        }
        case Qt::ImCursorRectangle: {
            const Scintilla::Position start_pos = (m_preedit_pos >= 0) ? m_preedit_pos : pos;
            const Point pt   = m_core->LocationFromPosition(start_pos);
            const int width  = static_cast<int>(send(SCI_GETCARETWIDTH));
            const int height = static_cast<int>(send(SCI_TEXTHEIGHT, line));
            return QRectF(pt.x, pt.y, width, height).toRect();
        }

        case Qt::ImFont: {
            const sptr_t style = send(SCI_GETSTYLEAT, pos);
            return font_for_style(this, static_cast<int>(style));
        }

        case Qt::ImCursorPosition: {
            const Scintilla::Position para_start = m_core->pdoc->ParaUp(pos);
            return QVariant(static_cast<int>(pos - para_start));
        }

        case Qt::ImSurroundingText: {
            const Scintilla::Position para_start = m_core->pdoc->ParaUp(pos);
            const Scintilla::Position para_end   = m_core->pdoc->ParaDown(pos);
            const std::string buffer             = m_core->RangeText(para_start, para_end);
            return m_core->StringFromDocument(buffer.c_str());
        }

        case Qt::ImCurrentSelection: {
            QVarLengthArray<char, 1024> buffer(send(SCI_GETSELTEXT));
            sends(SCI_GETSELTEXT, 0, buffer.data());

            return m_core->StringFromDocument(buffer.constData());
        }

        default:
            return QVariant();
    }
}

void ScintillaQuick_item::touchEvent(QTouchEvent * event)
{
    if (m_core->pdoc->IsReadOnly()) {
        return;
    }

    forceActiveFocus();

    if (event->touchPointStates() == Qt::TouchPointPressed && event->touchPoints().count() > 0) {
        m_last_touch_press_time = m_elapsed_timer.elapsed();
        cursorChangedUpdateMarker();
    }
    else
    if (event->touchPointStates() == Qt::TouchPointReleased && event->touchPoints().count() > 0) {
        // is ths a short touch (m_elapsed_timer between press and release < 100ms) ?
        if (m_last_touch_press_time >= 0 && (m_elapsed_timer.elapsed() - m_last_touch_press_time) < 100) {
            QTouchEvent::TouchPoint point = event->touchPoints().first();
            QPoint mouse_pressed_point = point.pos().toPoint();
            Point scintilla_point      = PointFromQPoint(mouse_pressed_point);

            Sci::Position pos = m_core->PositionFromLocation(scintilla_point);
            m_core->MovePositionTo(pos);

#ifdef Q_OS_ANDROID
            // Android: trigger software keyboard for inputs:
            // https://stackoverflow.com/questions/39436518/how-to-get-the-android-keyboard-to-appear-when-using-qt-for-android
            // https://stackoverflow.com/questions/5724811/how-to-show-the-keyboard-on-qt

            // Check if not in readonly modus --> pdoc->IsReadOnly()
            if (hasActiveFocus() && !m_core->pdoc->IsReadOnly()) {
                QInputMethod* keyboard = qGuiApp->inputMethod();
                if (!keyboard->isVisible()) {
                    keyboard->show();
                }
            }
#endif

            cursorChangedUpdateMarker();
        }
    }
    else {
        QQuickItem::touchEvent(event);
        return;
    }

    event->accept();
}

void ScintillaQuick_item::updatePolish()
{
    if (m_render_data) {
        m_render_data->update_pending = false;
    }
    if (m_render_data && m_render_data->snapshot_dirty) {
        build_render_snapshot();
    }
}

QSGNode* ScintillaQuick_item::updatePaintNode(QSGNode * old_node, UpdatePaintNodeData * update_paint_node_data)
{
    Q_UNUSED(update_paint_node_data);

    if (!m_render_data) {
        delete old_node;
        return nullptr;
    }

    return m_render_data->renderer.update(window(), old_node, m_render_data->snapshot, m_render_data->frame);
}

void ScintillaQuick_item::build_render_snapshot()
{
    if (!m_render_data) {
        return;
    }

    if (!m_render_data->static_content_dirty && !m_render_data->overlay_content_dirty &&
        !m_render_data->captured_caret_primitives.empty())
    {
        const int caret_width = static_cast<int>(send(SCI_GETCARETWIDTH));
        if (hasActiveFocus() && m_core && m_core->caret.active && m_caret_blink_visible && caret_width > 0) {
            m_render_data->frame.caret_primitives = m_render_data->captured_caret_primitives;
        }
        else {
            m_render_data->frame.caret_primitives.clear();
        }
        m_render_data->snapshot_dirty = false;
        return;
    }

    Render_snapshot snapshot;
    Style_cache cache;
    snapshot.item_size  = QSizeF(width(), height());
    snapshot.background = style_attributes_for(this, cache, STYLE_DEFAULT).background;
    snapshot.gutter_bands.reserve(k_margin_count);
    qreal margin_left = 0.0;
    for (int margin = 0; margin < k_margin_count; ++margin) {
        const int margin_width = static_cast<int>(send(SCI_GETMARGINWIDTHN, margin));
        if (margin_width <= 0) {
            continue;
        }

        snapshot.gutter_bands.push_back({
            QRectF(margin_left, 0.0, static_cast<qreal>(margin_width), height()),
            margin_background_color_for(this, cache, margin),
        });
        margin_left += margin_width;
    }

    const int current_first_visible_line = m_core ? static_cast<int>(send(SCI_GETFIRSTVISIBLELINE)) : -1;
    const int current_x_offset           = m_core ? static_cast<int>(send(SCI_GETXOFFSET))          : -1;
    const int previous_scroll_width      = m_core ? static_cast<int>(send(SCI_GETSCROLLWIDTH))      :  0;
    const int scroll_delta_lines =
        (m_render_data->previous_first_visible_line >= 0 && current_first_visible_line >= 0)
            ? (current_first_visible_line - m_render_data->previous_first_visible_line)
            : 0;
    const bool scroll_position_changed =
        m_render_data->previous_first_visible_line < 0                           ||
        m_render_data->previous_x_offset           < 0                           ||
        current_first_visible_line != m_render_data->previous_first_visible_line ||
        current_x_offset           != m_render_data->previous_x_offset;
    const bool static_content_dirty =
        scroll_position_changed ||
        (m_render_data->static_content_dirty && m_render_data->content_modified_since_last_capture);
    const bool overlay_only_capture = !static_content_dirty && m_render_data->overlay_content_dirty;
    Render_frame frame;
    const int line_height = (m_core && m_core->vs.lineHeight > 0) ? m_core->vs.lineHeight : 1;
    const int capture_buffer_lines = std::max(
        k_vertical_scroll_reuse_buffer_min_lines,
        std::max(1, static_cast<int>(height() / line_height) / 3 + 2));
    const int capture_base_first_visible_line = m_render_data->capture_base_first_visible_line;
    const int capture_max_first_visible_line =
        capture_base_first_visible_line >= 0 ? capture_base_first_visible_line + capture_buffer_lines : -1;
    const bool can_reuse_vertical_scroll =
        m_render_data->static_content_dirty                           &&
        m_render_data->scrolling_update                               &&
        !m_render_data->content_modified_since_last_capture           &&
        current_x_offset >= 0                                         &&
        current_x_offset == m_render_data->previous_x_offset          &&
        current_first_visible_line >= 0                               &&
        scroll_delta_lines != 0                                       &&
        std::abs(scroll_delta_lines) <= capture_buffer_lines          &&
        capture_base_first_visible_line >= 0                          &&
        current_first_visible_line >= capture_base_first_visible_line &&
        current_first_visible_line <= capture_max_first_visible_line;

    if (can_reuse_vertical_scroll || !static_content_dirty) {
        snapshot = m_render_data->snapshot;
    }

    if (can_reuse_vertical_scroll) {
        frame = m_render_data->frame;
        translate_render_frame(frame, -static_cast<qreal>(scroll_delta_lines * line_height));
    }
    else
    if (overlay_only_capture && m_core) {
        frame = m_core->current_render_frame(false, false, false, 0);
    }
    else
    if (m_core) {
        frame = m_core->current_render_frame(
            static_content_dirty,
            m_render_data->style_sync_needed && static_content_dirty,
            m_render_data->scrolling_update,
            capture_buffer_lines);
    }

    if (can_reuse_vertical_scroll || !static_content_dirty) {
        frame.text_rect   = m_render_data->frame.text_rect;
        frame.margin_rect = m_render_data->frame.margin_rect;
        if (!can_reuse_vertical_scroll) {
            frame.visual_lines           = std::move(m_render_data->frame.visual_lines);
            frame.indicator_primitives   = std::move(m_render_data->frame.indicator_primitives);
            frame.marker_primitives      = std::move(m_render_data->frame.marker_primitives);
            frame.margin_text_primitives = std::move(m_render_data->frame.margin_text_primitives);
            frame.fold_display_texts     = std::move(m_render_data->frame.fold_display_texts);
            frame.eol_annotations        = std::move(m_render_data->frame.eol_annotations);
            frame.annotations            = std::move(m_render_data->frame.annotations);
            frame.whitespace_marks       = std::move(m_render_data->frame.whitespace_marks);
            frame.decoration_underlines  = std::move(m_render_data->frame.decoration_underlines);
            frame.indent_guides          = std::move(m_render_data->frame.indent_guides);
        }
    }

    m_render_data->captured_caret_primitives = frame.caret_primitives;

    const int caret_width = static_cast<int>(send(SCI_GETCARETWIDTH));
    if (!(hasActiveFocus() && m_core && m_core->caret.active && m_caret_blink_visible && caret_width > 0)) {
        frame.caret_primitives.clear();
    }

    m_render_data->snapshot                            = std::move(snapshot);
    m_render_data->frame                               = std::move(frame);
    m_render_data->snapshot_dirty                      = false;
    m_render_data->static_content_dirty                = false;
    m_render_data->style_sync_needed                   = false;
    m_render_data->scrolling_update                    = false;
    m_render_data->overlay_content_dirty               = false;
    m_render_data->content_modified_since_last_capture = false;
    if (!can_reuse_vertical_scroll) {
        m_render_data->capture_base_first_visible_line = current_first_visible_line;
    }
    m_render_data->previous_first_visible_line = current_first_visible_line;
    m_render_data->previous_x_offset           = current_x_offset;

    if (m_core) {
        const int current_scroll_width = static_cast<int>(send(SCI_GETSCROLLWIDTH));
        if (current_scroll_width != previous_scroll_width) {
            syncQuickViewProperties();
        }
    }
}

std::vector<Displayed_row_for_test> ScintillaQuick_item::displayed_rows_for_test() const
{
    if (!m_render_data) {
        return {};
    }

    std::vector<Displayed_row_for_test> rows;
    rows.reserve(m_render_data->frame.visual_lines.size());
    for (const Visual_line_frame& line : m_render_data->frame.visual_lines) {
        Displayed_row_for_test row;
        row.document_line = line.key.document_line;
        row.subline_index = line.key.subline_index;
        row.top           = line.clip_rect.top();
        row.bottom        = line.clip_rect.bottom();
        row.text          = text_for_visual_line(line);
        rows.push_back(std::move(row));
    }
    return rows;
}

const Render_frame& ScintillaQuick_item::rendered_frame_for_test() const
{
    static const Render_frame empty_frame;
    return m_render_data ? m_render_data->frame : empty_frame;
}

void ScintillaQuick_item::notifyParent(NotificationData scn)
{
    static const QMetaMethod notification_received_signal =
        QMetaMethod::fromSignal(&ScintillaQuick_item::notificationReceived);
    const bool safe_notification_connected = isSignalConnected(notification_received_signal);
    const char* const pre_notify_text = scn.text;
    const Position pre_notify_length = scn.length;
    const ScintillaQuick_notification snapshot =
        safe_notification_connected ? notification_snapshot_from(scn) : ScintillaQuick_notification();

    emit notify(&scn);
    if (safe_notification_connected) {
        emit notificationReceived(snapshot);
    }

    switch (scn.nmhdr.code) {
        case Notification::StyleNeeded:
            emit styleNeeded(scn.position);
            if (m_render_data) {
                m_render_data->content_modified_since_last_capture = true;
            }
            request_scene_graph_update(true, true, false);
            break;

        case Notification::CharAdded:        emit charAdded(scn.ch);       break;
        case Notification::SavePointReached: emit savePointChanged(false); break;
        case Notification::SavePointLeft:    emit savePointChanged(true);  break;
        case Notification::ModifyAttemptRO:  emit modifyAttemptReadOnly(); break;
        case Notification::Key:              emit key(scn.ch);             break;

        case Notification::DoubleClick:
            emit doubleClick(scn.position, scn.line);
            break;

        case Notification::UpdateUI:
            updateQuickView(scn.updated);
            emit updateUi(scn.updated);
            break;

        case Notification::Modified: {
            const bool added = FlagSet(scn.modificationType, ModificationFlags::InsertText);
            const bool deleted = FlagSet(scn.modificationType, ModificationFlags::DeleteText);

            const Scintilla::Position length = send(SCI_GETTEXTLENGTH);
            bool first_line_added = (added && length == 1) || (deleted && length == 0);

            if (scn.linesAdded != 0) {
                emit linesAdded(scn.linesAdded);
            }
            else
            if (first_line_added) {
                emit linesAdded(added ? 1 : -1);
            }

            const QByteArray bytes = [&scn, safe_notification_connected, &snapshot,
                pre_notify_text, pre_notify_length]() {
                if (safe_notification_connected &&
                    snapshot.code == Notification::Modified &&
                    scn.text == pre_notify_text &&
                    scn.length == pre_notify_length) {
                    return snapshot.text;
                }

                qsizetype byte_count = 0;
                return scn.text && scn.length > 0 && copy_position_byte_count(scn.length, byte_count)
                    ? QByteArray(scn.text, byte_count)
                    : QByteArray();
            }();
            emit modified(scn.modificationType, scn.position, scn.length,
                scn.linesAdded, bytes, scn.line, scn.foldLevelNow, scn.foldLevelPrev);
            if (m_render_data) {
                m_render_data->content_modified_since_last_capture = true;
            }
            if (deleted) {
                reset_tracked_scroll_width();
            }
            request_scene_graph_update(true, true, false);
            break;
        }

        case Notification::MacroRecord:
            emit macroRecord(scn.message, scn.wParam, scn.lParam);
            break;

        case Notification::MarginClick:
            emit marginClicked(scn.position, scn.modifiers, scn.margin);
            break;

        case Notification::NeedShown:
            emit needShown(scn.position, scn.length);
            break;

        case Notification::Painted:
            emit painted();
            break;

        case Notification::UserListSelection:
            emit userListSelection();
            break;

        case Notification::URIDropped:
            emit uriDropped(QString::fromUtf8(scn.text));
            break;

        case Notification::DwellStart:
            emit dwellStart(scn.x, scn.y);
            break;

        case Notification::DwellEnd:
            emit dwellEnd(scn.x, scn.y);
            break;

        case Notification::Zoom:
            if (m_render_data) {
                m_render_data->content_modified_since_last_capture = true;
            }
            reset_tracked_scroll_width();
            syncQuickViewProperties();
            request_scene_graph_update(true, true, false);
            emit zoom(send(SCI_GETZOOM));
            break;

        case Notification::HotSpotClick:
            emit hotSpotClick(scn.position, scn.modifiers);
            break;

        case Notification::HotSpotDoubleClick:
            emit hotSpotDoubleClick(scn.position, scn.modifiers);
            break;

        case Notification::CallTipClick:
            emit callTipClick();
            break;

        case Notification::AutoCSelection:
            emit autoCompleteSelection(scn.lParam, QString::fromUtf8(scn.text));
            break;

        case Notification::AutoCCancelled:
            emit autoCompleteCancelled();
            break;

        case Notification::FocusIn:
            emit focusChanged(true);
            break;

        case Notification::FocusOut:
            emit focusChanged(false);
            break;

        default:
            return;
    }
}

void ScintillaQuick_item::event_command(uptr_t w_param, sptr_t l_param)
{
    emit command(w_param, l_param);
}

KeyMod ScintillaQuick_item::ModifiersOfKeyboard()
{
    const bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
    const bool ctrl  = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
    const bool alt   = QGuiApplication::keyboardModifiers() & Qt::AltModifier;

    return ModifierFlags(shift, ctrl, alt);
}

KeyMod ScintillaQuick_item::ModifiersOfMouse() const
{
    const auto kbd_modifiers = QGuiApplication::keyboardModifiers();
    const bool shift = kbd_modifiers & Qt::ShiftModifier;
    const bool ctrl  = kbd_modifiers & Qt::ControlModifier;
#if !defined(Q_OS_MAC) && !defined(Q_OS_WIN)
    // On X allow choice of rectangular modifier since most window
    // managers grab alt + click for moving windows.
    const bool alt = kbd_modifiers & modifierTranslated(m_core->m_rectangular_selection_modifier);
#else
    const bool alt = kbd_modifiers & Qt::AltModifier;
#endif
    return ModifierFlags(shift, ctrl, alt);
}

QString ScintillaQuick_item::getText() const
{
    const int text_length = static_cast<int>(send(SCI_GETTEXTLENGTH));
    QByteArray buffer(text_length + 1, Qt::Uninitialized);
    send(SCI_GETTEXT, text_length + 1, reinterpret_cast<sptr_t>(buffer.data()));
    return QString::fromUtf8(buffer.constData());
}

void ScintillaQuick_item::setText(const QString& txt)
{
    const QByteArray utf8 = txt.toUtf8();
    send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(utf8.constData()));
    send(SCI_EMPTYUNDOBUFFER);
    send(SCI_COLOURISE, 0, -1);
    setFocus(true);
    syncQuickViewProperties();
    emit textChanged();
}

void ScintillaQuick_item::setFont(const QFont& newFont)
{
    // Intentionally scoped to this editor item only - we deliberately
    // do NOT touch QGuiApplication::setFont(), because the editor
    // font should not bleed into unrelated Quick items in the same
    // application.
    if (newFont == m_font) {
        return;
    }

    m_font = newFont;

    // Set the font for a style.
    setStylesFont(newFont, 0);
    syncQuickViewProperties();
    emit fontChanged();
    if (m_render_data) {
        m_render_data->content_modified_since_last_capture = true;
    }
    request_scene_graph_update(true, true, false);
}

int ScintillaQuick_item::getLogicalWidth() const
{
    return m_logical_width;
}

int ScintillaQuick_item::getLogicalHeight() const
{
    return m_logical_height;
}

int ScintillaQuick_item::getCharHeight() const
{
    return static_cast<int>(send(SCI_TEXTHEIGHT));
}

int ScintillaQuick_item::getCharWidth() const
{
    const char buf[] = "X";
    return static_cast<int>(send(SCI_TEXTWIDTH, 0, reinterpret_cast<sptr_t>(buf)));
}

int ScintillaQuick_item::getFirstVisibleLine() const
{
    return static_cast<int>(send(SCI_GETFIRSTVISIBLELINE));
}

void ScintillaQuick_item::setFirstVisibleLine(int line_no)
{
    send(SCI_SETFIRSTVISIBLELINE, line_no);
    syncQuickViewProperties();
}

int ScintillaQuick_item::getTotalLines() const
{
    return static_cast<int>(send(SCI_GETLINECOUNT));
}

int ScintillaQuick_item::getFirstVisibleColumn() const
{
    const int char_width = getCharWidth();
    if (char_width <= 0) {
        return 0;
    }
    return static_cast<int>(send(SCI_GETXOFFSET)) / char_width;
}

int ScintillaQuick_item::getTotalColumns() const
{
    const int char_width = getCharWidth();
    if (char_width <= 0) {
        return 0;
    }
    return static_cast<int>(send(SCI_GETSCROLLWIDTH)) / char_width;
}

int ScintillaQuick_item::getVisibleLines() const
{
    return static_cast<int>(send(SCI_LINESONSCREEN));
}

int ScintillaQuick_item::getVisibleColumns() const
{
    const int char_width = getCharWidth();
    if (char_width <= 0) {
        return 0;
    }

    const PRectangle text_rect = m_core->GetTextRectangle();
    const int visible_width    = std::max(0, static_cast<int>(text_rect.Width()));
    return visible_width / char_width;
}

Qt::InputMethodHints ScintillaQuick_item::inputMethodHints() const
{
    return m_input_method_hints | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText | Qt::ImhMultiLine;
}

void ScintillaQuick_item::setInputMethodHints(Qt::InputMethodHints hints)
{
    if (hints == m_input_method_hints) {
        return;
    }

    m_input_method_hints = hints;
    updateInputMethod(Qt::ImHints);
    emit inputMethodHintsChanged();
}

bool ScintillaQuick_item::getReadonly() const
{
    bool flag = (bool)send(SCI_GETREADONLY);
    return flag;
}

void ScintillaQuick_item::setReadonly(bool value)
{
    if (value != getReadonly()) {
        send(SCI_SETREADONLY, value);

        syncQuickViewProperties();
        emit readonlyChanged();
    }
}

void ScintillaQuick_item::updateQuickView(Update updated)
{
    const bool needs_property_sync =
        FlagSet(updated, Update::Content) ||
        FlagSet(updated, Update::VScroll) ||
        FlagSet(updated, Update::HScroll);
    if (needs_property_sync) {
        syncQuickViewProperties();
    }

    cursorChangedUpdateMarker();
    request_scene_graph_update(
        FlagSet(updated, Update::Content) || FlagSet(updated, Update::VScroll) || FlagSet(updated, Update::HScroll),
        FlagSet(updated, Update::Content) || FlagSet(updated, Update::VScroll),
        FlagSet(updated, Update::VScroll));
}

void ScintillaQuick_item::syncQuickViewProperties()
{
    if (!m_core) {
        return;
    }

    // Re-entry guard: the SCI_* queries issued below go back through
    // `send()`, which in turn consults the scene-graph update dispatch.
    // If any of those queries is not in the read-only allow-list, the
    // dispatch's conservative default would call back into this
    // function, causing unbounded recursion and a stack overflow.
    // Setting the flag here makes the `send()` re-entry guard short-
    // circuit the nested dispatch.
    if (m_in_sync_quick_view_properties) {
        return;
    }
    m_in_sync_quick_view_properties = true;
    struct Scope_guard
    {
        bool& flag;
        ~Scope_guard()
        {
            flag = false;
        }
    } scope_guard{m_in_sync_quick_view_properties};

    const int char_height          = getCharHeight();
    const int char_width           = getCharWidth();
    const int line_count           = send(SCI_GETLINECOUNT);
    const int text_width           = send(SCI_GETSCROLLWIDTH);
    const int text_height          = line_count * char_height;
    const int total_columns        = (char_width > 0) ? (text_width / char_width) : 0;
    const int visible_lines        = send(SCI_LINESONSCREEN);
    const int visible_columns      = (char_width > 0) ? getVisibleColumns() : 0;
    const int first_visible_line   = send(SCI_GETFIRSTVISIBLELINE);
    const int first_visible_column = (char_width > 0) ? getFirstVisibleColumn() : 0;

    using Item = ScintillaQuick_item;
    auto emit_if_changed = [this](int& cached_value, int current_value, void (Item::*signal)()) {
        if (cached_value != current_value) {
            cached_value = current_value;
            (this->*signal)();
        }
    };

    emit_if_changed(m_last_emitted_char_height,          char_height,          &Item::charHeightChanged);
    emit_if_changed(m_last_emitted_char_width,           char_width,           &Item::charWidthChanged);
    emit_if_changed(m_last_emitted_total_lines,          line_count,           &Item::totalLinesChanged);
    emit_if_changed(m_last_emitted_total_columns,        total_columns,        &Item::totalColumnsChanged);
    emit_if_changed(m_last_emitted_visible_lines,        visible_lines,        &Item::visibleLinesChanged);
    emit_if_changed(m_last_emitted_visible_columns,      visible_columns,      &Item::visibleColumnsChanged);
    emit_if_changed(m_last_emitted_first_visible_line,   first_visible_line,   &Item::firstVisibleLineChanged);
    emit_if_changed(m_last_emitted_first_visible_column, first_visible_column, &Item::firstVisibleColumnChanged);

    if (m_logical_width != text_width) {
        m_logical_width = text_width;
        emit logicalWidthChanged();
    }
    if (m_logical_height != text_height) {
        m_logical_height = text_height;
        emit logicalHeightChanged();
    }
}

void ScintillaQuick_item::reset_tracked_scroll_width()
{
    if (!m_core) {
        return;
    }

    if (!m_core->WndProc(Message::GetScrollWidthTracking, 0, 0)) {
        return;
    }

    m_core->reset_tracked_scroll_width_to_viewport();
}

// taken from QScintilla
void ScintillaQuick_item::setStylesFont(const QFont& f, int style)
{
    const QByteArray family = f.family().toLatin1();
    send(SCI_STYLESETFONT,           style, reinterpret_cast<sptr_t>(family.constData()));
    send(SCI_STYLESETSIZEFRACTIONAL, style, long(f.pointSizeF() * SC_FONT_SIZE_MULTIPLIER));

    // Pass the Qt weight via the back door.
    send(SCI_STYLESETWEIGHT, style, -f.weight());

    send(SCI_STYLESETITALIC, style, f.italic());
    send(SCI_STYLESETUNDERLINE, style, f.underline());

    // Tie the font settings of the default style to that of style 0 (the style
    // conventionally used for whitespace by lexers).  This is needed so that
    // fold marks, indentations, edge columns etc are set properly.
    if (style == 0) {
        setStylesFont(f, STYLE_DEFAULT);
    }
}

void ScintillaQuick_item::cursorChangedUpdateMarker()
{
    syncCaretBlinkTimer(true);
    emit qGuiApp->inputMethod()->cursorRectangleChanged(); // IMPORTANT: this moves the handle !!! see:
                                                           // QQuickTextControl::updateCursorRectangle()
    emit qGuiApp->inputMethod()->anchorRectangleChanged();
    emit cursorPositionChanged();
}

void ScintillaQuick_item::syncCaretBlinkTimer(bool resetPhase)
{
    const bool caret_should_blink = m_core && hasActiveFocus() && m_core->caret.active;
    const int caret_period        = m_core ? static_cast<int>(send(SCI_GETCARETPERIOD)) : 0;

    if (!caret_should_blink || caret_period <= 0) {
        m_caret_blink_timer.stop();
        m_caret_blink_visible = true;
        return;
    }

    if (resetPhase) {
        m_caret_blink_visible = true;
        m_caret_blink_timer.start(caret_period);
        return;
    }

    if (m_caret_blink_timer.interval() != caret_period) {
        m_caret_blink_timer.setInterval(caret_period);
    }

    if (!m_caret_blink_timer.isActive()) {
        m_caret_blink_timer.start();
    }
}

void register_scintilla_type()
{
    register_notification_metatypes();
    qmlRegisterType<ScintillaQuick_item>("ScintillaQuick", 1, 0, "ScintillaQuick_item");
}
