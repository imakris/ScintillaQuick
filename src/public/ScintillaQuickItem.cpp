// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.
//
// @file ScintillaQuick_item.cpp - Qt Quick item (QQuickItem) that wraps
// ScintillaQuick_core and drives events, input method handling and
// scene-graph rendering. This is NOT a QWidget.

#include <scintillaquick/ScintillaQuickItem.h>
#include "../core/scintillaquick_hierarchical_profiler.h"
#include "ScintillaQuickCore.h"
#include "scintillaquick_dispatch_table.h"
#include "scintillaquick_fonts.h"
#include "scintillaquick_platqt.h"
#include "scintillaquick_scene_graph_renderer.h"

// Internal Scintilla headers that used to be pulled in transitively via the
// public ScintillaQuick_item.h. They are required here for Platform::,
// Scintilla::Internal::Point, and PRectangle references in this TU.
#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"
#include "Position.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QInputMethod>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSGNode>
#include <QStandardPaths>
#include <QTextFormat>
#include <QVarLengthArray>
#include <QTimer>
#include <qqml.h>
#ifdef PLAT_QT_QML
#include <QtGlobal>
#include <QPoint>
#include <QPair>
#include <QList>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <string_view>

constexpr int k_indicator_input     = static_cast<int>(Scintilla::IndicatorNumbers::Ime);
constexpr int k_indicator_target    = k_indicator_input + 1;
constexpr int k_indicator_converted = k_indicator_input + 2;
constexpr int k_indicator_unknown   = k_indicator_input + 3;

// Q_WS_MAC and Q_WS_X11 aren't defined in Qt6
#ifdef Q_OS_MAC
#define Q_WS_MAC 1
#endif

#if !defined(Q_OS_MAC) && !defined(Q_OS_WIN)
#define Q_WS_X11 1
#endif

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
    int capture_base_first_visible_line      = -1;
    int previous_first_visible_line          = -1;
    int previous_x_offset                    = -1;
    std::vector<Caret_primitive> captured_caret_primitives;
};

struct Profiling_metric
{
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> total_ns{0};
    std::atomic<uint64_t> max_ns{0};

    void clear()
    {
        count.store(0, std::memory_order_release);
        total_ns.store(0, std::memory_order_release);
        max_ns.store(0, std::memory_order_release);
    }
};

class ScintillaQuick_item::Profiling_state
{
public:
    explicit Profiling_state(ScintillaQuick_item *owner)
    :
        m_owner(owner)
    {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, owner, [owner]() {
            owner->stopProfilingSession();
        });
    }

    void reset()
    {
        update_requests.store(0, std::memory_order_release);
        snapshot_build_count.store(0, std::memory_order_release);
        snapshot_line_total.store(0, std::memory_order_release);
        snapshot_line_max.store(0, std::memory_order_release);
        wheel_event_count.store(0, std::memory_order_release);
        horizontal_scroll_command_count.store(0, std::memory_order_release);
        vertical_scroll_command_count.store(0, std::memory_order_release);
        blink_only_update_count.store(0, std::memory_order_release);
        overlay_only_update_count.store(0, std::memory_order_release);
        full_update_count.store(0, std::memory_order_release);

        update_polish.clear();
        build_render_snapshot.clear();
        update_paint_node.clear();
        update_quick_view.clear();
        wheel_event.clear();
        scroll_horizontal.clear();
        scroll_vertical.clear();
        hierarchical_profiler.reset();
    }

    Scintilla::Internal::Hierarchical_profiler *hierarchical_profiler_if_active()
    {
        return active.load(std::memory_order_acquire) ? &hierarchical_profiler : nullptr;
    }

    ScintillaQuick_item *m_owner = nullptr;
    QTimer timer;
    QElapsedTimer elapsed;
    QDateTime started_at_utc;
    QString output_directory;
    double requested_duration_seconds = 0.0;
    std::atomic<bool> active{false};
    std::atomic<uint64_t> update_requests{0};
    std::atomic<uint64_t> snapshot_build_count{0};
    std::atomic<uint64_t> snapshot_line_total{0};
    std::atomic<uint64_t> snapshot_line_max{0};
    std::atomic<uint64_t> wheel_event_count{0};
    std::atomic<uint64_t> horizontal_scroll_command_count{0};
    std::atomic<uint64_t> vertical_scroll_command_count{0};
    std::atomic<uint64_t> blink_only_update_count{0};
    std::atomic<uint64_t> overlay_only_update_count{0};
    std::atomic<uint64_t> full_update_count{0};
    Profiling_metric update_polish;
    Profiling_metric build_render_snapshot;
    Profiling_metric update_paint_node;
    Profiling_metric update_quick_view;
    Profiling_metric wheel_event;
    Profiling_metric scroll_horizontal;
    Profiling_metric scroll_vertical;
    Scintilla::Internal::Hierarchical_profiler hierarchical_profiler;
};

namespace {

constexpr int k_margin_count                           = SC_MAX_MARGIN + 1;
constexpr int k_vertical_scroll_reuse_buffer_min_lines = 16;

// `scene_graph_update_request`, `scene_graph_update_request_info_t` and
// `tracked_scroll_width_should_reset` now live in
// src/core/scintillaquick_dispatch_table.h (included above) so that they
// can be covered by dedicated unit tests without spinning up a Qt Quick
// window. They are reachable here via the file-scope
// `using namespace Scintilla::Internal;` above.

void translate_rect(QRectF &rect, qreal dy)
{
    if (!rect.isNull()) {
        rect.translate(0.0, dy);
    }
}

void translate_point(QPointF &point, qreal dy)
{
    point.setY(point.y() + dy);
}

void translate_render_frame(Render_frame &frame, qreal dy)
{
    translate_rect(frame.text_rect, dy);
    translate_rect(frame.margin_rect, dy);

    for (Visual_line_frame &visual_line : frame.visual_lines) {
        translate_point(visual_line.origin, dy);
        visual_line.baseline_y += dy;
        translate_rect(visual_line.clip_rect, dy);
        for (Text_run &run : visual_line.text_runs) {
            translate_point(run.position, dy);
        }
    }

    for (Selection_primitive &selection : frame.selection_primitives) {
        translate_rect(selection.rect, dy);
    }
    for (Caret_primitive &caret : frame.caret_primitives) {
        translate_rect(caret.rect, dy);
    }
    for (Indicator_primitive &indicator : frame.indicator_primitives) {
        translate_rect(indicator.rect, dy);
    }
    for (Current_line_primitive &current_line : frame.current_line_primitives) {
        translate_rect(current_line.rect, dy);
    }
    for (Marker_primitive &marker : frame.marker_primitives) {
        translate_rect(marker.rect, dy);
    }
    for (Margin_text_primitive &margin : frame.margin_text_primitives) {
        translate_point(margin.position, dy);
        translate_rect(margin.clip_rect, dy);
        margin.baseline_y += dy;
    }
    for (Fold_display_text_primitive &fold : frame.fold_display_texts) {
        translate_point(fold.position, dy);
        translate_rect(fold.rect, dy);
        fold.baseline_y += dy;
    }
    for (Eol_annotation_primitive &eol : frame.eol_annotations) {
        translate_point(eol.position, dy);
        translate_rect(eol.rect, dy);
        eol.baseline_y += dy;
    }
    for (Annotation_primitive &annotation : frame.annotations) {
        translate_point(annotation.position, dy);
        translate_rect(annotation.rect, dy);
        annotation.baseline_y += dy;
    }
    for (Whitespace_mark_primitive &mark : frame.whitespace_marks) {
        translate_rect(mark.rect, dy);
        mark.mid_y += dy;
    }
    for (Decoration_underline_primitive &underline : frame.decoration_underlines) {
        translate_rect(underline.rect, dy);
    }
    for (Indent_guide_primitive &guide : frame.indent_guides) {
        guide.top += dy;
        guide.bottom += dy;
    }
}

QString text_for_visual_line(const Visual_line_frame &line)
{
    QString text;
    for (const Text_run &run : line.text_runs) {
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

QFont font_for_style(const ScintillaQuick_item *item, int style)
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

const Style_attributes &style_attributes_for(
    const ScintillaQuick_item *item,
    Style_cache &Style_cache,
    int style)
{
    const int bounded_style                            = std::clamp(style, 0, STYLE_MAX);
    std::optional<Style_attributes> &cached_attributes = Style_cache[bounded_style];
    if (!cached_attributes.has_value()) {
        Style_attributes attributes;
        attributes.foreground = color_from_scintilla(item->send(SCI_STYLEGETFORE, bounded_style));
        attributes.background = color_from_scintilla(item->send(SCI_STYLEGETBACK, bounded_style));
        attributes.font       = font_for_style(item, bounded_style);
        cached_attributes     = std::move(attributes);
    }
    return *cached_attributes;
}

int total_margin_width(const ScintillaQuick_item *item)
{
    int total = 0;
    for (int margin = 0; margin < k_margin_count; ++margin) {
        total += static_cast<int>(item->send(SCI_GETMARGINWIDTHN, margin));
    }
    return total;
}

QColor margin_background_color_for(
    const ScintillaQuick_item *item,
    Style_cache &Style_cache,
    int margin)
{
    const int margin_type = static_cast<int>(item->send(SCI_GETMARGINTYPEN, margin));
    const int margin_mask = static_cast<int>(item->send(SCI_GETMARGINMASKN, margin));

    if ((margin_mask & SC_MASK_FOLDERS) != 0) {
        return QColorFromColourRGBA(Platform::ChromeHighlight());
    }

    switch (margin_type) {
        case SC_MARGIN_BACK:
            return style_attributes_for(item, Style_cache, STYLE_DEFAULT).background;
        case SC_MARGIN_FORE:
            return style_attributes_for(item, Style_cache, STYLE_DEFAULT).foreground;
        case SC_MARGIN_COLOUR:
            return color_from_scintilla(item->send(SCI_GETMARGINBACKN, margin));
        default:
            return style_attributes_for(item, Style_cache, STYLE_LINENUMBER).background;
    }
}

void record_profiling_metric(Profiling_metric &metric, uint64_t elapsed_ns)
{
    metric.count.fetch_add(1, std::memory_order_relaxed);
    metric.total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);

    uint64_t previous_max = metric.max_ns.load(std::memory_order_relaxed);
    while (elapsed_ns > previous_max &&
           !metric.max_ns.compare_exchange_weak(previous_max, elapsed_ns, std::memory_order_relaxed))
    {
    }
}

class Profiling_scope
{
public:
    explicit Profiling_scope(Profiling_metric *metric)
    :
        m_metric(metric)
    {
        if (m_metric) {
            m_timer.start();
        }
    }

    ~Profiling_scope()
    {
        if (m_metric) {
            record_profiling_metric(*m_metric, static_cast<uint64_t>(m_timer.nsecsElapsed()));
        }
    }

private:
    Profiling_metric *m_metric = nullptr;
    QElapsedTimer m_timer;
};

QJsonObject profiling_metric_to_json(const Profiling_metric &metric)
{
    const uint64_t count    = metric.count.load(std::memory_order_acquire);
    const uint64_t total_ns = metric.total_ns.load(std::memory_order_acquire);
    const uint64_t max_ns   = metric.max_ns.load(std::memory_order_acquire);
    const double total_ms   = static_cast<double>(total_ns) / 1000000.0;
    const double max_ms     = static_cast<double>(max_ns) / 1000000.0;
    const double average_ms = count > 0 ? total_ms / static_cast<double>(count) : 0.0;

    QJsonObject result;
    result.insert("count", static_cast<qint64>(count));
    result.insert("total_ms", total_ms);
    result.insert("average_ms", average_ms);
    result.insert("max_ms", max_ms);
    return result;
}

QString profiling_output_directory(QString requested_directory)
{
    if (!requested_directory.trimmed().isEmpty()) {
        return requested_directory;
    }

    QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (fallback.trimmed().isEmpty()) {
        fallback = QDir::tempPath();
    }
    return fallback;
}

QString profiling_report_path(const QString &directory_path)
{
    QDir directory(directory_path);
    directory.mkpath(".");
    const QString timestamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz");
    return directory.filePath(QStringLiteral("text_editor_profile_%1.json").arg(timestamp));
}

}

ScintillaQuick_item::ScintillaQuick_item(QQuickItem *parent)
#ifdef PLAT_QT_QML
: QQuickItem(parent)
#else
: QAbstractScrollArea(parent)
#endif
, m_updates_enabled(true), m_logical_width(0), m_logical_height(0)
#ifdef PLAT_QT_QML
, m_input_method_hints(Qt::ImhNone)
, m_last_touch_press_time(-1)
#endif
, m_core((ensure_bundled_fonts_loaded(), new ScintillaQuick_core(this))), m_preedit_pos(-1), m_render_data(std::make_unique<Render_data>())
{
#ifdef PLAT_QT_QML
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptTouchEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setFlag(QQuickItem::ItemIsFocusScope, true);
    setFlag(QQuickItem::ItemHasContents, true);
    setFlag(QQuickItem::ItemAcceptsDrops, true);
    setFlag(QQuickItem::ItemClipsChildrenToShape, true);
    setClip(true);
#endif

    m_elapsed_timer.start();

#ifndef PLAT_QT_QML
    // Set Qt defaults.
    setAcceptDrops(true);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setFrameStyle(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_StaticContents);
    viewport()->setAutoFillBackground(false);
    setAttribute(Qt::WA_KeyCompression);
    setAttribute(Qt::WA_InputMethodEnabled);
#endif

    // All IME indicators drawn in same colour, blue, with different patterns
    const ColourRGBA colourIME(0, 0, UCHAR_MAX);
    m_core->vs.indicators[k_indicator_unknown]   = Indicator(IndicatorStyle::Hidden, colourIME);
    m_core->vs.indicators[k_indicator_input]     = Indicator(IndicatorStyle::Dots, colourIME);
    m_core->vs.indicators[k_indicator_converted] = Indicator(IndicatorStyle::CompositionThick, colourIME);
    m_core->vs.indicators[k_indicator_target]    = Indicator(IndicatorStyle::StraightBox, colourIME);

    connect(m_core, SIGNAL(notifyParent(Scintilla::NotificationData)),
        this, SLOT(notifyParent(Scintilla::NotificationData)));

    // Connect scroll bars.
#ifndef PLAT_QT_QML
    // scrollbars are handled by the QML ScrollView outside this class
    connect(verticalScrollBar(), SIGNAL(valueChanged(int)),
            this,                SLOT(scrollVertical(int)));
    connect(horizontalScrollBar(), SIGNAL(valueChanged(int)),
            this,                  SLOT(scrollHorizontal(int)));
#endif

    // Connect pass-through signals.
    connect(m_core, SIGNAL(horizontalRangeChanged(int,int)),
            this,   SIGNAL(horizontalRangeChanged(int,int)));
    connect(m_core, SIGNAL(verticalRangeChanged(int,int)),
            this,   SIGNAL(verticalRangeChanged(int,int)));
    connect(m_core, SIGNAL(horizontalScrolled(int)),
            this,   SIGNAL(horizontalScrolled(int)));
    connect(m_core, SIGNAL(verticalScrolled(int)),
            this,   SIGNAL(verticalScrolled(int)));

    connect(m_core, SIGNAL(notifyChange()),
            this, SIGNAL(notifyChange()));

    connect(m_core, SIGNAL(command(Scintilla::uptr_t,Scintilla::sptr_t)),
            this,   SLOT(event_command(Scintilla::uptr_t,Scintilla::sptr_t)));

    connect(m_core, SIGNAL(aboutToCopy(QMimeData*)),
            this,   SIGNAL(aboutToCopy(QMimeData*)));

#ifdef PLAT_QT_QML
    connect(m_core, SIGNAL(cursorPositionChanged()), this, SIGNAL(cursorPositionChanged()));   // needed to update markers on android platform
#endif

    m_caret_blink_timer.setSingleShot(false);
    QObject::connect(&m_caret_blink_timer, &QTimer::timeout, this, [this]() {
        m_caret_blink_visible = !m_caret_blink_visible;
        if (m_render_data && m_updates_enabled) {
            m_render_data->snapshot_dirty = true;
            polish();
            update();
        }
    });

    m_profiling_state = std::make_unique<Profiling_state>(this);

    send(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE);
    send(SCI_SETSCROLLWIDTHTRACKING, 1);
}

ScintillaQuick_item::~ScintillaQuick_item()
{
    stopProfilingSession();

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

sptr_t ScintillaQuick_item::send(
    unsigned int i_message,
    uptr_t w_param,
    sptr_t l_param) const
{
    // NOTE: `send()` is declared `const` because Qt's Q_PROPERTY READ getters
    // in this class funnel through it for SCI_GET* queries and READ functions
    // must be const. A subset of messages (SCI_SET*, mutators) do observably
    // change editor state and will also schedule scene-graph updates here, so
    // this method is not logically const for those messages. We cast `this`
    // once, in a single, well-marked place.
    ScintillaQuick_item *self = const_cast<ScintillaQuick_item *>(this);

    const sptr_t result = m_core->WndProc(static_cast<Message>(i_message), w_param, l_param);
    if (tracked_scroll_width_should_reset(i_message)) {
        self->reset_tracked_scroll_width();
    }
    const scene_graph_update_request_info_t update_request = scene_graph_update_request(i_message);
    // Re-entry guard (defence-in-depth): `syncQuickViewProperties()`
    // itself issues SCI_* queries through `send()`. If any of those
    // queries is not in the read-only allow-list, the dispatch's
    // "unknown -> full resync" default would call
    // `syncQuickViewProperties()` again, recursing until the stack
    // overflows. The primary defence is the allow-list
    // (`scene_graph_message_is_known_read_only()` in
    // scintillaquick_dispatch_table.h); this guard ensures that a
    // future missed entry degrades into "no nested resync" rather than
    // a crash.
    if (update_request.needed && !m_in_sync_quick_view_properties) {
        self->syncQuickViewProperties();
        if (update_request.static_content_dirty && m_render_data) {
            m_render_data->content_modified_since_last_capture = true;
        }
        self->request_scene_graph_update(
            update_request.static_content_dirty,
            update_request.needs_style_sync,
            update_request.scrolling);
    }
    return result;
}

sptr_t ScintillaQuick_item::sends(
    unsigned int i_message,
    uptr_t w_param,
    const char *s) const
{
    return m_core->WndProc(static_cast<Message>(i_message), w_param, reinterpret_cast<sptr_t>(s));
}

bool ScintillaQuick_item::startProfilingSession(const QString &output_directory, double duration_seconds)
{
    if (!m_profiling_state) {
        return false;
    }

    if (duration_seconds <= 0.0 || !std::isfinite(duration_seconds)) {
        duration_seconds = 10.0;
    }

    if (m_profiling_state->active.exchange(true, std::memory_order_acq_rel)) {
        stopProfilingSession();
        m_profiling_state->active.store(true, std::memory_order_release);
    }

    m_profiling_state->reset();
    m_profiling_state->requested_duration_seconds = duration_seconds;
    m_profiling_state->output_directory           = profiling_output_directory(output_directory);
    m_profiling_state->started_at_utc             = QDateTime::currentDateTimeUtc();
    m_profiling_state->elapsed.start();
    m_profiling_state->timer.start(static_cast<int>(duration_seconds * 1000.0));
    qWarning().noquote()
        << "ScintillaQuick profiling started."
        << " output_dir=" << m_profiling_state->output_directory
        << " duration_seconds=" << duration_seconds;
    emit profilingActiveChanged();
    return true;
}

void ScintillaQuick_item::stopProfilingSession()
{
    if (!m_profiling_state) {
        return;
    }

    const bool was_active = m_profiling_state->active.exchange(false, std::memory_order_acq_rel);
    if (!was_active) {
        return;
    }

    m_profiling_state->timer.stop();

    QJsonObject metadata;
    metadata.insert("started_at_utc", m_profiling_state->started_at_utc.toString(Qt::ISODateWithMs));
    metadata.insert("finished_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    metadata.insert("requested_duration_seconds", m_profiling_state->requested_duration_seconds);
    metadata.insert("actual_duration_ms", m_profiling_state->elapsed.elapsed());
    metadata.insert("item_width", width());
    metadata.insert("item_height", height());
    metadata.insert("logical_width", m_logical_width);
    metadata.insert("logical_height", m_logical_height);
    metadata.insert("char_width", getCharWidth());
    metadata.insert("char_height", getCharHeight());
    metadata.insert("document_length_bytes", static_cast<qint64>(send(SCI_GETTEXTLENGTH)));
    metadata.insert("total_lines", getTotalLines());
    metadata.insert("visible_lines", getVisibleLines());
    metadata.insert("first_visible_line", getFirstVisibleLine());
    metadata.insert("total_columns", getTotalColumns());
    metadata.insert("visible_columns", getVisibleColumns());
    metadata.insert("first_visible_column", getFirstVisibleColumn());

    QJsonObject counters;
    counters.insert("update_requests",
        static_cast<qint64>(m_profiling_state->update_requests.load(std::memory_order_acquire)));
    counters.insert("snapshot_build_count",
        static_cast<qint64>(m_profiling_state->snapshot_build_count.load(std::memory_order_acquire)));
    counters.insert("snapshot_line_total",
        static_cast<qint64>(m_profiling_state->snapshot_line_total.load(std::memory_order_acquire)));
    counters.insert("snapshot_line_max",
        static_cast<qint64>(m_profiling_state->snapshot_line_max.load(std::memory_order_acquire)));
    counters.insert("wheel_event_count",
        static_cast<qint64>(m_profiling_state->wheel_event_count.load(std::memory_order_acquire)));
    counters.insert("horizontal_scroll_command_count",
        static_cast<qint64>(m_profiling_state->horizontal_scroll_command_count.load(std::memory_order_acquire)));
    counters.insert("vertical_scroll_command_count",
        static_cast<qint64>(m_profiling_state->vertical_scroll_command_count.load(std::memory_order_acquire)));
    counters.insert("blink_only_update_count",
        static_cast<qint64>(m_profiling_state->blink_only_update_count.load(std::memory_order_acquire)));
    counters.insert("overlay_only_update_count",
        static_cast<qint64>(m_profiling_state->overlay_only_update_count.load(std::memory_order_acquire)));
    counters.insert("full_update_count",
        static_cast<qint64>(m_profiling_state->full_update_count.load(std::memory_order_acquire)));

    QJsonObject metrics;
    metrics.insert("update_polish", profiling_metric_to_json(m_profiling_state->update_polish));
    metrics.insert("build_render_snapshot", profiling_metric_to_json(m_profiling_state->build_render_snapshot));
    metrics.insert("update_paint_node", profiling_metric_to_json(m_profiling_state->update_paint_node));
    metrics.insert("update_quick_view", profiling_metric_to_json(m_profiling_state->update_quick_view));
    metrics.insert("wheel_event", profiling_metric_to_json(m_profiling_state->wheel_event));
    metrics.insert("scroll_horizontal", profiling_metric_to_json(m_profiling_state->scroll_horizontal));
    metrics.insert("scroll_vertical", profiling_metric_to_json(m_profiling_state->scroll_vertical));

    QJsonObject report;
    report.insert("tool", QStringLiteral("ScintillaQuick_item"));
    report.insert("metadata", metadata);
    report.insert("counters", counters);
    report.insert("metrics", metrics);
    report.insert("scope_tree", m_profiling_state->hierarchical_profiler.to_json());

    const QString report_path = profiling_report_path(m_profiling_state->output_directory);
    QSaveFile file(report_path);
    bool write_ok = false;
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
        write_ok = file.commit();
    }
    else {
        qWarning().noquote()
            << "ScintillaQuick profiling failed to open report file."
            << " path=" << report_path;
    }

    if (write_ok) {
        qWarning().noquote()
            << "ScintillaQuick profiling finished."
            << " report_path=" << report_path;
    }
    else {
        qWarning().noquote()
            << "ScintillaQuick profiling finished without report."
            << " report_path=" << report_path;
    }

    emit profilingActiveChanged();
    emit profilingFinished(report_path);
}

bool ScintillaQuick_item::profilingActive() const
{
    return m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire);
}

void ScintillaQuick_item::request_scene_graph_update(
    bool static_content_dirty,
    bool needs_style_sync,
    bool scrolling)
{
    if (!m_render_data || !m_updates_enabled) {
        return;
    }

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        m_profiling_state->update_requests.fetch_add(1, std::memory_order_relaxed);
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

#ifdef PLAT_QT_QML

void ScintillaQuick_item::scrollRow(int delta_lines)
{
    int current_line = m_core->TopLineOfMain();
    scrollVertical(current_line + delta_lines);
}

void ScintillaQuick_item::scrollColumn(int delta_columns)
{
    int current_column_in_pixel = send(SCI_GETXOFFSET);
    int new_value               = current_column_in_pixel + delta_columns * getCharWidth();
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

#endif

void ScintillaQuick_item::scrollHorizontal(int value)
{
    Hierarchical_profiler *profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(profiler, "item.scroll_horizontal");

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        m_profiling_state->horizontal_scroll_command_count.fetch_add(1, std::memory_order_relaxed);
    }
    m_core->HorizontalScrollTo(value);
    syncQuickViewProperties();
    request_scene_graph_update(true, false, false);
}

void ScintillaQuick_item::scrollVertical(int value)
{
    Hierarchical_profiler *profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(profiler, "item.scroll_vertical");

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        m_profiling_state->vertical_scroll_command_count.fetch_add(1, std::memory_order_relaxed);
    }
    m_core->ScrollTo(value);
    request_scene_graph_update(true, false, true);
}

bool ScintillaQuick_item::event(QEvent *event)
{
    bool result = false;

    if (event->type() == QEvent::KeyPress) {
        // Circumvent the tab focus convention.
        keyPressEvent(static_cast<QKeyEvent *>(event));
        result = event->isAccepted();
    }
    else
    if (event->type() == QEvent::Show) {
#ifndef PLAT_QT_QML
        setMouseTracking(true);
        result = QAbstractScrollArea::event(event);
#else
        result = QQuickItem::event(event);
#endif
    }
    else
    if (event->type() == QEvent::Hide) {
#ifndef PLAT_QT_QML
        setMouseTracking(false);
        result = QAbstractScrollArea::event(event);
#else
        result = QQuickItem::event(event);
#endif
    }
    else {
#ifndef PLAT_QT_QML
        result = QAbstractScrollArea::event(event);
#else
        result = QQuickItem::event(event);
#endif
    }

    return result;
}

#ifndef PLAT_QT_QML

void ScintillaQuick_item::paintEvent(QPaintEvent *event)
{
    m_core->PartialPaint(PRectFromQRect(event->rect()));
}

#endif

namespace {

bool isWheelEventHorizontal(QWheelEvent *event) {
    return event->angleDelta().y() == 0;
}

int wheelEventYDelta(QWheelEvent *event) {
    return event->angleDelta().y();
}

}

void ScintillaQuick_item::wheelEvent(QWheelEvent *event)
{
    Hierarchical_profiler *profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(profiler, "item.wheel_event");

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        m_profiling_state->wheel_event_count.fetch_add(1, std::memory_order_relaxed);
    }

    if (isWheelEventHorizontal(event)) {
#ifndef PLAT_QT_QML
        if (horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff)
            event->ignore();
        else
            QAbstractScrollArea::wheelEvent(event);
#else
            QQuickItem::wheelEvent(event);
#endif
    }
    else
    if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) {
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
    }
    else {
        // Ignore wheel events when the scroll bars are disabled.
#ifndef PLAT_QT_QML
        if (verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) {
            event->ignore();
        }
        else {
#endif
#ifdef PLAT_QT_QML
            // Scroll
            int linesToScroll = 3;
            if (event->angleDelta().y() > 0) {
                scrollVertical(m_core->topLine - linesToScroll);
            }
            else {
                scrollVertical(m_core->topLine + linesToScroll);
            }
            QQuickItem::wheelEvent(event);
#else
            QAbstractScrollArea::wheelEvent(event);
        }
#endif
    }
}

void ScintillaQuick_item::focusInEvent(QFocusEvent *event)
{
    m_core->SetFocusState(true);

#ifdef PLAT_QT_QML
    QQuickItem::focusInEvent(event);
    syncCaretBlinkTimer(true);
    request_scene_graph_update(false, false, false);
#else
    QAbstractScrollArea::focusInEvent(event);
#endif
}

void ScintillaQuick_item::focusOutEvent(QFocusEvent *event)
{
    m_core->SetFocusState(false);

#ifdef PLAT_QT_QML
    QQuickItem::focusOutEvent(event);
    syncCaretBlinkTimer(false);
    request_scene_graph_update(false, false, false);
#else
    QAbstractScrollArea::focusOutEvent(event);
#endif
}

#ifdef PLAT_QT_QML
void ScintillaQuick_item::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    // trigger resize handling only if the size of the control has changed
    // no update is needed for a position change
    if(newGeometry.width() != oldGeometry.width() || newGeometry.height() != oldGeometry.height() )
    {
        m_core->ChangeSize();
        emit resized();
        QQuickItem::geometryChange(newGeometry, oldGeometry);

        if (m_render_data) {
            m_render_data->content_modified_since_last_capture = true;
        }
        request_scene_graph_update(true, true, false);
    }
}

#else

void ScintillaQuick_item::resizeEvent(QResizeEvent *)
{
    m_core->ChangeSize();
    emit resized();
}

#endif

void ScintillaQuick_item::keyPressEvent(QKeyEvent *event)
{
    bool view_changed = false;

    // All keystrokes containing the meta modifier are
    // assumed to be shortcuts not handled by scintilla.
    if (QGuiApplication::keyboardModifiers() & Qt::MetaModifier) {
#ifdef PLAT_QT_QML
        QQuickItem::keyPressEvent(event);
#else
        QAbstractScrollArea::keyPressEvent(event);
#endif
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
    // do not use the current state, because a key event could also be triggered by the input context menu as meta keyboard event ! --> needed for edit context menu on android platform
    bool shift = event->modifiers() & Qt::ShiftModifier;
    bool ctrl  = event->modifiers() & Qt::ControlModifier;
    bool alt   = event->modifiers() & Qt::AltModifier;
#else
    bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
    bool ctrl  = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
    bool alt   = QGuiApplication::keyboardModifiers() & Qt::AltModifier;
#endif

    bool consumed = false;
    bool added    = m_core->KeyDownWithModifiers(static_cast<Keys>(key),
                           ModifierFlags(shift, ctrl, alt),
                           &consumed) != 0;
    if (!consumed)
        consumed = added;
    view_changed = consumed;

    if (!consumed) {
        // Don't insert text if the control key was pressed unless
        // it was pressed in conjunction with alt for AltGr emulation.
        bool input = (!ctrl || alt);

        // Additionally, on non-mac platforms, don't insert text
        // if the alt key was pressed unless control is also present.
        // On mac alt can be used to insert special characters.
#ifndef Q_WS_MAC
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

#ifdef PLAT_QT_QML
    if (view_changed) {
        cursorChangedUpdateMarker();
        request_scene_graph_update(false, false, false);
    }
#endif

    emit keyPressed(event);
}

#ifdef Q_WS_X11
static int modifierTranslated(int sciModifier)
{
    switch (sciModifier) {
        case SCMOD_SHIFT:
            return Qt::ShiftModifier;
        case SCMOD_CTRL:
            return Qt::ControlModifier;
        case SCMOD_ALT:
            return Qt::AltModifier;
        case SCMOD_SUPER:
            return Qt::MetaModifier;
        default:
            return 0;
    }
}
#endif

void ScintillaQuick_item::mousePressEvent(QMouseEvent *event)
{
    Point pos = PointFromQPoint(event->pos());

    emit buttonPressed(event);

    if (event->button() == Qt::MiddleButton &&
        QGuiApplication::clipboard()->supportsSelection()) {
        SelectionPosition selPos = m_core->SPositionFromLocation(
                    pos, false, false, m_core->UserVirtualSpace());
        m_core->sel.Clear();
        m_core->SetSelection(selPos, selPos);
        m_core->PasteFromMode(QClipboard::Selection);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
        bool ctrl  = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
#ifdef Q_WS_X11
        // On X allow choice of rectangular modifier since most window
        // managers grab alt + click for moving windows.
        bool alt   = QGuiApplication::keyboardModifiers() & modifierTranslated(m_core->m_rectangular_selection_modifier);
#else
        bool alt   = QGuiApplication::keyboardModifiers() & Qt::AltModifier;
#endif

        m_core->ButtonDownWithModifiers(pos, m_elapsed_timer.elapsed(), ModifierFlags(shift, ctrl, alt));

#ifdef PLAT_QT_QML
        cursorChangedUpdateMarker();
        request_scene_graph_update(false, false, false);
#endif
    }

    if (event->button() == Qt::RightButton) {
        m_core->RightButtonDownWithModifiers(pos, m_elapsed_timer.elapsed(), ModifiersOfKeyboard());

#ifdef PLAT_QT_QML
        Point pos = PointFromQPoint(event->globalPos());
        Point pt  = PointFromQPoint(event->pos());
        if (!m_core->PointInSelection(pt)) {
            m_core->SetEmptySelection(m_core->PositionFromLocation(pt));
        }
// TODO: call context menu callback if set otherwise use default context menu...
        if (m_core->ShouldDisplayPopup(pt)) {
            m_core->ContextMenu(pos);
        }
        request_scene_graph_update(false, false, false);
#endif
    }

#ifdef PLAT_QT_QML
    forceActiveFocus();

    emit enableScrollViewInteraction(false);

    event->setAccepted(true);
#endif
}


#ifdef PLAT_QT_QML
void ProcessScintillaContextMenu(Scintilla::Internal::Point pt, const Scintilla::Internal::Window & w, const QList<QPair<QString, QPair<int, bool>>> & menu)
{
    ScintillaQuick_item *qt_object = static_cast<ScintillaQuick_item *>(w.GetID());

    emit qt_object->clearContextMenu();
    for (const QPair<QString, QPair<int, bool>> &item : menu) {
        if (item.first.size() > 0) {
            emit qt_object->addToContextMenu(item.second.first, item.first, item.second.second);
        }
    }

    QPoint point(pt.x, pt.y);
    emit qt_object->showContextMenu(point);
}
#endif

void ScintillaQuick_item::mouseReleaseEvent(QMouseEvent *event)
{
    const QPoint point = event->pos();
    if (event->button() == Qt::LeftButton)
        m_core->ButtonUpWithModifiers(PointFromQPoint(point), m_elapsed_timer.elapsed(), ModifiersOfKeyboard());

    const sptr_t pos  = send(SCI_POSITIONFROMPOINT, point.x(), point.y());
    const sptr_t line = send(SCI_LINEFROMPOSITION, pos);
    int modifiers = QGuiApplication::keyboardModifiers();

    emit textAreaClicked(line, modifiers);
    emit buttonReleased(event);

#ifdef PLAT_QT_QML
    emit enableScrollViewInteraction(true);
    request_scene_graph_update(false, false, false);

    event->setAccepted(true);
#endif
}

void ScintillaQuick_item::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Scintilla does its own double-click detection.
#ifndef PLAT_QT_QML
    mousePressEvent(event);
#else
    Q_UNUSED(event);
#endif
}

void ScintillaQuick_item::mouseMoveEvent(QMouseEvent *event)
{
    Point pos = PointFromQPoint(event->pos());

    bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
    bool ctrl  = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
#ifdef Q_WS_X11
    // On X allow choice of rectangular modifier since most window
    // managers grab alt + click for moving windows.
    bool alt   = QGuiApplication::keyboardModifiers() & modifierTranslated(m_core->m_rectangular_selection_modifier);
#else
    bool alt   = QGuiApplication::keyboardModifiers() & Qt::AltModifier;
#endif

    const KeyMod modifiers                = ModifierFlags(shift, ctrl, alt);
    const int previous_first_visible_line = static_cast<int>(send(SCI_GETFIRSTVISIBLELINE));
    const int previous_x_offset           = static_cast<int>(send(SCI_GETXOFFSET));

    m_core->ButtonMoveWithModifiers(pos, m_elapsed_timer.elapsed(), modifiers);

#ifdef PLAT_QT_QML
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
#endif
}

#ifndef PLAT_QT_QML

void ScintillaQuick_item::contextMenuEvent(QContextMenuEvent *event)
{
    Point pos = PointFromQPoint(event->globalPos());
    Point pt  = PointFromQPoint(event->pos());
    if (!m_core->PointInSelection(pt)) {
        m_core->SetEmptySelection(m_core->PositionFromLocation(pt));
    }
    if (m_core->ShouldDisplayPopup(pt)) {
        m_core->ContextMenu(pos);
    }
}

#endif

void ScintillaQuick_item::dragEnterEvent(QDragEnterEvent *event)
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

void ScintillaQuick_item::dragLeaveEvent(QDragLeaveEvent * /* event */)
{
    m_core->DragLeave();
}

void ScintillaQuick_item::dragMoveEvent(QDragMoveEvent *event)
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

void ScintillaQuick_item::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        m_core->DropUrls(event->mimeData());
    }
    else
    if (event->mimeData()->hasText()) {
        event->acceptProposedAction();

        Point point = PointFromQPoint(event->pos());
        bool move = (event->source() == this &&
                 event->proposedAction() == Qt::MoveAction);
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
    return hangul_jamo || hangul_compatible_jamo  || hangul_syllable ||
                hangul_jamo_extended_a || hangul_jamo_extended_b;
}

void ScintillaQuick_item::MoveImeCarets(Scintilla::Position offset)
{
    // Move carets relatively by bytes
    for (size_t r=0; r < m_core->sel.Count(); r++) {
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
    for (size_t r=0; r< m_core-> sel.Count(); r++) {
        const Sci::Position positionInsert = m_core->sel.Range(r).Start().Position();
        m_core->pdoc->DecorationFillRange(positionInsert - len, 1, len);
    }
}

static int GetImeCaretPos(QInputMethodEvent *event)
{
    foreach (QInputMethodEvent::Attribute attr, event->attributes()) {
        if (attr.type == QInputMethodEvent::Cursor)
            return attr.start;
    }
    return 0;
}

static std::vector<int> MapImeIndicators(QInputMethodEvent *event)
{
    std::vector<int> ime_indicator(event->preeditString().size(), k_indicator_unknown);
    foreach (QInputMethodEvent::Attribute attr, event->attributes()) {
        if (attr.type == QInputMethodEvent::TextFormat) {
            QTextFormat format         = attr.value.value<QTextFormat>();
            QTextCharFormat char_format = format.toCharFormat();

            int indicator = k_indicator_unknown;
            switch (char_format.underlineStyle()) {
                case QTextCharFormat::NoUnderline: // win32, linux
                    indicator = k_indicator_target;
                    break;
                case QTextCharFormat::SingleUnderline: // osx
                case QTextCharFormat::DashUnderline: // win32, linux
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

            for (int i = attr.start; i < attr.start+attr.length; i++) {
                ime_indicator[i] = indicator;
            }
        }
    }
    return ime_indicator;
}

void ScintillaQuick_item::inputMethodEvent(QInputMethodEvent *event)
{
    // Copy & paste by johnsonj with a lot of helps of Neil
    // Great thanks for my forerunners, jiniya and BLUEnLIVE

    if (m_core->pdoc->IsReadOnly() || m_core->SelectionContainsProtected()) {
        // Here, a canceling and/or completing composition function is needed.
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
        const QInputMethodEvent::Attribute &a = event->attributes().at(i);
        if (a.type == QInputMethodEvent::Selection) {
            const Sci::Position cur_pos = m_core->CurrentPosition();
            const int para_start        = m_core->pdoc->ParaUp(cur_pos);

            SelectionPosition new_start(para_start + a.start);
            SelectionPosition new_end(para_start + a.start + a.length);
            if (new_start > new_end) {
                m_core->SetSelection(new_end, new_start);
            }
            else {
                m_core->SetSelection(new_start, new_end);
            }

            // update markers by triggering QtAndroidInputContext::updateSelectionHandles()
#ifdef PLAT_QT_QML
            cursorChangedUpdateMarker();
#endif
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
        const QString &commit_str = event->commitString();
        const int commit_str_len  = commit_str.length();

        for (int i = 0; i < commit_str_len;) {
            const int uc_width           = commit_str.at(i).isHighSurrogate() ? 2 : 1;
            const QString one_char_utf16 = commit_str.mid(i, uc_width);
            const QByteArray one_char    = m_core->BytesForDocument(one_char_utf16);

            m_core->InsertCharacter(std::string_view(one_char.data(), one_char.length()), CharacterSource::DirectInput);
            i += uc_width;
        }

    }
    else
    if (!event->preeditString().isEmpty()) {
        const QString preedit_str = event->preeditString();
        const int preedit_str_len = preedit_str.length();
        if (preedit_str_len == 0) {
            m_core->ShowCaretAtCurrentPosition();
            return;
        }

        if (initial_compose)
            m_core->ClearBeforeTentativeStart();
        m_core->pdoc->TentativeStart(); // TentativeActive() from now on.

        std::vector<int> ime_indicator = MapImeIndicators(event);

        for (int i = 0; i < preedit_str_len;) {
            const int uc_width           = preedit_str.at(i).isHighSurrogate() ? 2 : 1;
            const QString one_char_utf16 = preedit_str.mid(i, uc_width);
            const QByteArray one_char    = m_core->BytesForDocument(one_char_utf16);
            const int one_char_len       = one_char.length();

            m_core->InsertCharacter(std::string_view(one_char.data(), one_char_len), CharacterSource::TentativeInput);

            DrawImeIndicator(ime_indicator[i], one_char_len);
            i += uc_width;
        }

        // Move IME carets.
        int ime_caret_pos                     = GetImeCaretPos(event);
        int ime_end_to_ime_caret_u16         = ime_caret_pos - preedit_str_len;
        const Sci::Position ime_caret_pos_doc =
            m_core->pdoc->GetRelativePositionUTF16(m_core->CurrentPosition(), ime_end_to_ime_caret_u16);

        MoveImeCarets(- m_core->CurrentPosition() + ime_caret_pos_doc);

        if (IsHangul(preedit_str.at(0))) {
#ifndef Q_OS_WIN
            if (ime_caret_pos > 0) {
                int one_char_before = m_core->pdoc->GetRelativePosition(m_core->CurrentPosition(), -1);
                MoveImeCarets(- m_core->CurrentPosition() + one_char_before);
            }
#endif
            m_core->view.imeCaretBlockOverride = true;
        }

        // Set candidate box position for Qt::ImMicroFocus.
        m_preedit_pos = m_core->CurrentPosition();
        m_core->EnsureCaretVisible();
#ifndef PLAT_QT_QML
        updateMicroFocus();
#endif
    }
    m_core->ShowCaretAtCurrentPosition();
}

QVariant ScintillaQuick_item::inputMethodQuery(Qt::InputMethodQuery property, QVariant argument) const
{
    // see: QQuickTextEdit::inputMethodQuery(...)
    const PRectangle text_rect = m_core ? m_core->GetTextRectangle() : PRectangle();
    const QPointF text_offset(text_rect.left, text_rect.top);

    if (property == Qt::ImCursorPosition && !argument.isNull()) {
        argument         = QVariant(argument.toPointF() - text_offset);
        const QPointF pt = argument.toPointF();
        if (!pt.isNull()) {
            Point scintilla_point = PointFromQPointF(pt);
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
#ifdef PLAT_QT_QML
        case Qt::ImEnabled:
            {
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
                SelectionPosition sel_start = m_core->SelectionStart();
                SelectionPosition sel_end   = m_core->SelectionEnd();
                Point pt_start = m_core->LocationFromPosition(sel_start);
                Point pt_end   = m_core->LocationFromPosition(sel_end);

                int width  = send(SCI_GETCARETWIDTH);
                int height = send(SCI_TEXTHEIGHT, line);
                return QRect(pt_end.x, pt_end.y, width, height);
        }
        // selection == Position <--> AnchorPosition
        case Qt::ImAnchorPosition: {
                SelectionPosition sel_start = m_core->SelectionStart();
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
            QVarLengthArray<char,1024> buffer(para_end - para_start + 1);

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
            QVarLengthArray<char,1024> buffer(para_end - para_start + 1);

            Sci_CharacterRange charRange;
            charRange.cpMin = para_start;
            charRange.cpMax = pos;

            Sci_TextRange textRange;
            textRange.chrg      = charRange;
            textRange.lpstrText = buffer.data();

            send(SCI_GETTEXTRANGE, 0, (sptr_t)&textRange);

            return m_core->StringFromDocument(buffer.constData());
        }
#endif
        case Qt::ImCursorRectangle: {
            const Scintilla::Position start_pos = (m_preedit_pos >= 0) ? m_preedit_pos : pos;
            const Point pt                      = m_core->LocationFromPosition(start_pos);
            const int width                     = static_cast<int>(send(SCI_GETCARETWIDTH));
            const int height                    = static_cast<int>(send(SCI_TEXTHEIGHT, line));
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
            const std::string buffer            = m_core->RangeText(para_start, para_end);
            return m_core->StringFromDocument(buffer.c_str());
        }

        case Qt::ImCurrentSelection: {
            QVarLengthArray<char,1024> buffer(send(SCI_GETSELTEXT));
            sends(SCI_GETSELTEXT, 0, buffer.data());

            return m_core->StringFromDocument(buffer.constData());
        }

        default:
            return QVariant();
    }
}

#ifdef PLAT_QT_QML

void ScintillaQuick_item::touchEvent(QTouchEvent *event)
{
    if(m_core->pdoc->IsReadOnly()) {
        return;
    }

    forceActiveFocus();

    if( event->touchPointStates() == Qt::TouchPointPressed && event->touchPoints().count()>0 ) {
        m_last_touch_press_time = m_elapsed_timer.elapsed();
        cursorChangedUpdateMarker();
    }
    else
    if( event->touchPointStates() == Qt::TouchPointReleased && event->touchPoints().count()>0 ) {
        // is ths a short touch (m_elapsed_timer between press and release < 100ms) ?
        if(m_last_touch_press_time>=0 && (m_elapsed_timer.elapsed()-m_last_touch_press_time)<100) {
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
            if( hasActiveFocus() && !m_core->pdoc->IsReadOnly() )
            {
                QInputMethod *keyboard = qGuiApp->inputMethod();
                if(!keyboard->isVisible())
                {
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
    Hierarchical_profiler *profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(profiler, "item.update_polish");

    Profiling_scope scope(
        m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)
            ? &m_profiling_state->update_polish
            : nullptr);
    if (m_render_data) {
        m_render_data->update_pending = false;
    }
    if (m_render_data && m_render_data->snapshot_dirty) {
        build_render_snapshot();
    }
}

QSGNode *ScintillaQuick_item::updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *update_paint_node_data)
{
    Hierarchical_profiler *profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(profiler, "item.update_paint_node");

    Profiling_scope scope(
        m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)
            ? &m_profiling_state->update_paint_node
            : nullptr);
    Q_UNUSED(update_paint_node_data);

    if (!m_render_data) {
        delete old_node;
        return nullptr;
    }

    return m_render_data->renderer.update(window(), old_node, m_render_data->snapshot, m_render_data->frame);
}

void ScintillaQuick_item::build_render_snapshot()
{
    Hierarchical_profiler *profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(profiler, "item.build_render_snapshot");

    Profiling_scope scope(
        m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)
            ? &m_profiling_state->build_render_snapshot
            : nullptr);
    if (!m_render_data) {
        return;
    }

    if (!m_render_data->static_content_dirty && !m_render_data->overlay_content_dirty
        && !m_render_data->captured_caret_primitives.empty()) {
        const int caret_width = static_cast<int>(send(SCI_GETCARETWIDTH));
        if (hasActiveFocus() && m_core && m_core->caret.active && m_caret_blink_visible && caret_width > 0) {
            m_render_data->frame.caret_primitives = m_render_data->captured_caret_primitives;
        }
        else {
            m_render_data->frame.caret_primitives.clear();
        }
        m_render_data->snapshot_dirty = false;
        if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
            m_profiling_state->blink_only_update_count.fetch_add(1, std::memory_order_relaxed);
        }
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
    const int current_x_offset           = m_core ? static_cast<int>(send(SCI_GETXOFFSET)) : -1;
    const int previous_scroll_width      = m_core ? static_cast<int>(send(SCI_GETSCROLLWIDTH)) : 0;
    const int scroll_delta_lines =
        (m_render_data->previous_first_visible_line >= 0 && current_first_visible_line >= 0)
            ? (current_first_visible_line - m_render_data->previous_first_visible_line)
            : 0;
    const bool scroll_position_changed =
        m_render_data->previous_first_visible_line < 0 ||
        m_render_data->previous_x_offset < 0 ||
        current_first_visible_line != m_render_data->previous_first_visible_line ||
        current_x_offset != m_render_data->previous_x_offset;
    const bool static_content_dirty =
        scroll_position_changed ||
        (m_render_data->static_content_dirty && m_render_data->content_modified_since_last_capture);
    const bool overlay_only_capture =
        !static_content_dirty &&
        m_render_data->overlay_content_dirty;
    Render_frame frame;
    const int line_height = (m_core && m_core->vs.lineHeight > 0) ? m_core->vs.lineHeight : 1;
    const int capture_buffer_lines = std::max(
        k_vertical_scroll_reuse_buffer_min_lines,
        std::max(1, static_cast<int>(height() / line_height) / 3 + 2));
    const int capture_base_first_visible_line = m_render_data->capture_base_first_visible_line;
    const int capture_max_first_visible_line =
        capture_base_first_visible_line >= 0
            ? capture_base_first_visible_line + capture_buffer_lines
            : -1;
    const bool can_reuse_vertical_scroll =
        m_render_data->static_content_dirty &&
        m_render_data->scrolling_update &&
        !m_render_data->content_modified_since_last_capture &&
        current_x_offset >= 0 &&
        current_x_offset == m_render_data->previous_x_offset &&
        current_first_visible_line >= 0 &&
        scroll_delta_lines != 0 &&
        std::abs(scroll_delta_lines) <= capture_buffer_lines &&
        capture_base_first_visible_line >= 0 &&
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
        frame = m_core->current_render_frame(
            nullptr,
            false,
            false,
            false,
            0);
    }
    else
    if (m_core) {
        frame = m_core->current_render_frame(
            nullptr,
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

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        if (static_content_dirty && !can_reuse_vertical_scroll) {
            m_profiling_state->full_update_count.fetch_add(1, std::memory_order_relaxed);
        }
        else {
            m_profiling_state->overlay_only_update_count.fetch_add(1, std::memory_order_relaxed);
        }
        m_profiling_state->snapshot_build_count.fetch_add(1, std::memory_order_relaxed);
        const uint64_t line_count = static_cast<uint64_t>(frame.visual_lines.size());
        m_profiling_state->snapshot_line_total.fetch_add(line_count, std::memory_order_relaxed);

        uint64_t previous_max = m_profiling_state->snapshot_line_max.load(std::memory_order_relaxed);
        while (line_count > previous_max &&
               !m_profiling_state->snapshot_line_max.compare_exchange_weak(
                   previous_max,
                   line_count,
                   std::memory_order_relaxed))
        {
        }
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
    for (const Visual_line_frame &line : m_render_data->frame.visual_lines) {
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

const Render_frame &ScintillaQuick_item::rendered_frame_for_test() const
{
    static const Render_frame empty_frame;
    return m_render_data ? m_render_data->frame : empty_frame;
}

#endif

void ScintillaQuick_item::notifyParent(NotificationData scn)
{
    emit notify(&scn);
    switch (scn.nmhdr.code) {
        case Notification::StyleNeeded:
            emit styleNeeded(scn.position);
            if (m_render_data) {
                m_render_data->content_modified_since_last_capture = true;
            }
            request_scene_graph_update(true, true, false);
            break;

        case Notification::CharAdded:
            emit charAdded(scn.ch);
            break;

        case Notification::SavePointReached:
            emit savePointChanged(false);
            break;

        case Notification::SavePointLeft:
            emit savePointChanged(true);
            break;

        case Notification::ModifyAttemptRO:
            emit modifyAttemptReadOnly();
            break;

        case Notification::Key:
            emit key(scn.ch);
            break;

        case Notification::DoubleClick:
            emit doubleClick(scn.position, scn.line);
            break;

        case Notification::UpdateUI:
#ifdef PLAT_QT_QML
            updateQuickView(scn.updated);
#endif
            emit updateUi(scn.updated);
            break;

        case Notification::Modified:
        {
            const bool added   = FlagSet(scn.modificationType, ModificationFlags::InsertText);
            const bool deleted = FlagSet(scn.modificationType, ModificationFlags::DeleteText);

            const Scintilla::Position length = send(SCI_GETTEXTLENGTH);
            bool firstLineAdded = (added && length == 1) ||
                                  (deleted && length == 0);

            if (scn.linesAdded != 0) {
                emit linesAdded(scn.linesAdded);
            }
            else
            if (firstLineAdded) {
                emit linesAdded(added ? 1 : -1);
            }

            const QByteArray bytes = QByteArray::fromRawData(scn.text, scn.text ? scn.length : 0);
            emit modified(scn.modificationType, scn.position, scn.length,
                          scn.linesAdded, bytes, scn.line,
                          scn.foldLevelNow, scn.foldLevelPrev);
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


#ifdef PLAT_QT_QML

QString ScintillaQuick_item::getText() const
{
    const int textLength = static_cast<int>(send(SCI_GETTEXTLENGTH));
    QByteArray buffer(textLength + 1, Qt::Uninitialized);
    send(SCI_GETTEXT, textLength + 1, reinterpret_cast<sptr_t>(buffer.data()));
    return QString::fromUtf8(buffer.constData());
}

void ScintillaQuick_item::setText(const QString & txt)
{
    const QByteArray utf8 = txt.toUtf8();
    send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(utf8.constData()));
    send(SCI_EMPTYUNDOBUFFER);
    send(SCI_COLOURISE, 0, -1);
    setFocus(true);
    syncQuickViewProperties();
    emit textChanged();
}

void ScintillaQuick_item::setFont(const QFont & newFont)
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
    int char_height = send(SCI_TEXTHEIGHT);
    return char_height;
}

int ScintillaQuick_item::getCharWidth() const
{
    const char buf[] = "X";
    return static_cast<int>(send(SCI_TEXTWIDTH, 0, reinterpret_cast<sptr_t>(buf)));
}

int ScintillaQuick_item::getFirstVisibleLine() const
{
    int first_line = send(SCI_GETFIRSTVISIBLELINE);
    return first_line;
}

void ScintillaQuick_item::setFirstVisibleLine(int line_no)
{
    send(SCI_SETFIRSTVISIBLELINE, line_no);
    syncQuickViewProperties();
}

int ScintillaQuick_item::getTotalLines() const
{
    int line_count = send(SCI_GETLINECOUNT);
    return line_count;
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
    int count = send(SCI_LINESONSCREEN);
    return count;
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
    if (hints == m_input_method_hints)
        return;

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
    if(value != getReadonly())
    {
        send(SCI_SETREADONLY, value);

        syncQuickViewProperties();
        emit readonlyChanged();
    }
}

void ScintillaQuick_item::updateQuickView(Update updated)
{
    Hierarchical_profiler *profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(profiler, "item.update_quick_view");

    Profiling_scope scope(
        m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)
            ? &m_profiling_state->update_quick_view
            : nullptr);
    const bool needs_property_sync =
        FlagSet(updated, Update::Content) ||
        FlagSet(updated, Update::VScroll) ||
        FlagSet(updated, Update::HScroll);
    if (needs_property_sync) {
        syncQuickViewProperties();
    }

#ifdef PLAT_QT_QML
    cursorChangedUpdateMarker();
    request_scene_graph_update(
        FlagSet(updated, Update::Content) || FlagSet(updated, Update::VScroll) || FlagSet(updated, Update::HScroll),
        FlagSet(updated, Update::Content) || FlagSet(updated, Update::VScroll),
        FlagSet(updated, Update::VScroll));
#endif
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
    struct Scope_guard {
        bool &flag;
        ~Scope_guard() { flag = false; }
    } scope_guard{m_in_sync_quick_view_properties};

    const int char_height         = getCharHeight();
    const int char_width          = getCharWidth();
    const int line_count           = send(SCI_GETLINECOUNT);
    const int text_width           = send(SCI_GETSCROLLWIDTH);
    const int text_height          = line_count * char_height;
    const int total_columns        = (char_width > 0) ? (text_width / char_width) : 0;
    const int visible_lines        = send(SCI_LINESONSCREEN);
    const int visible_columns      = (char_width > 0) ? getVisibleColumns() : 0;
    const int first_visible_line   = send(SCI_GETFIRSTVISIBLELINE);
    const int first_visible_column = (char_width > 0) ? getFirstVisibleColumn() : 0;

    auto emit_if_changed = [this](int &cached_value, int current_value, void (ScintillaQuick_item::*signal)()) {
        if (cached_value != current_value) {
            cached_value = current_value;
            (this->*signal)();
        }
    };

    emit_if_changed(m_last_emitted_char_height, char_height, &ScintillaQuick_item::charHeightChanged);
    emit_if_changed(m_last_emitted_char_width, char_width, &ScintillaQuick_item::charWidthChanged);
    emit_if_changed(m_last_emitted_total_lines, line_count, &ScintillaQuick_item::totalLinesChanged);
    emit_if_changed(m_last_emitted_total_columns, total_columns, &ScintillaQuick_item::totalColumnsChanged);
    emit_if_changed(m_last_emitted_visible_lines, visible_lines, &ScintillaQuick_item::visibleLinesChanged);
    emit_if_changed(m_last_emitted_visible_columns, visible_columns, &ScintillaQuick_item::visibleColumnsChanged);
    emit_if_changed(m_last_emitted_first_visible_line, first_visible_line, &ScintillaQuick_item::firstVisibleLineChanged);
    emit_if_changed(m_last_emitted_first_visible_column, first_visible_column, &ScintillaQuick_item::firstVisibleColumnChanged);

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
void ScintillaQuick_item::setStylesFont(const QFont &f, int style)
{
    const QByteArray family = f.family().toLatin1();
    send(SCI_STYLESETFONT, style, reinterpret_cast<sptr_t>(family.constData()));
    send(SCI_STYLESETSIZEFRACTIONAL, style,
            long(f.pointSizeF() * SC_FONT_SIZE_MULTIPLIER));

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
    if(!m_core->pdoc->IsReadOnly())
    {
        syncCaretBlinkTimer(true);
        emit qGuiApp->inputMethod()->cursorRectangleChanged();   // IMPORTANT: this moves the handle !!! see: QQuickTextControl::updateCursorRectangle()
        emit qGuiApp->inputMethod()->anchorRectangleChanged();
        emit cursorPositionChanged();
    }
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

void RegisterScintillaType()
{
    qmlRegisterType<ScintillaQuick_item>("ScintillaQuick", 1, 0, "ScintillaQuick_item");
}

#endif
