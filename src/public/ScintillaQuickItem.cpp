//
//          Copyright (c) 1990-2011, Scientific Toolworks, Inc.
//
// The License.txt file describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//
// Additions Copyright (c) 2011 Archaeopteryx Software, Inc. d/b/a Wingware
// @file ScintillaQuickItem.cpp - Qt Quick item (QQuickItem) that wraps
// ScintillaQuickCore and drives events, input method handling and
// scene-graph rendering. This is NOT a QWidget.
//
// Additions Copyright (c) 2020 Michael Neuroth
// Scintilla platform layer for Qt QML/Quick

#include <scintillaquick/ScintillaQuickItem.h>
#include "../core/scintillaquick_hierarchical_profiler.h"
#include "ScintillaQuickCore.h"
#include "scintillaquick_dispatch_table.h"
#include "scintillaquick_platqt.h"
#include "scintillaquick_scene_graph_renderer.h"

// Internal Scintilla headers that used to be pulled in transitively via the
// public ScintillaQuickItem.h. They are required here for Platform::,
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

constexpr int IndicatorInput = static_cast<int>(Scintilla::IndicatorNumbers::Ime);
constexpr int IndicatorTarget = IndicatorInput + 1;
constexpr int IndicatorConverted = IndicatorInput + 2;
constexpr int IndicatorUnknown = IndicatorInput + 3;

// Q_WS_MAC and Q_WS_X11 aren't defined in Qt6
#ifdef Q_OS_MAC
#define Q_WS_MAC 1
#endif

#if !defined(Q_OS_MAC) && !defined(Q_OS_WIN)
#define Q_WS_X11 1
#endif

using namespace Scintilla;
using namespace Scintilla::Internal;

class ScintillaQuickItem::Render_data
{
public:
    render_snapshot_t snapshot;
    Render_frame frame;
    Scene_graph_renderer renderer;
    bool snapshot_dirty = true;
    bool static_content_dirty = true;
    bool style_sync_needed = true;
    bool scrolling_update = false;
    bool overlay_content_dirty = true;
    bool content_modified_since_last_capture = true;
    bool update_pending = false;
    int capture_base_first_visible_line = -1;
    int previous_first_visible_line = -1;
    int previous_x_offset = -1;
    std::vector<caret_primitive_t> captured_caret_primitives;
};

struct profiling_metric_t
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

class ScintillaQuickItem::Profiling_state
{
public:
    explicit Profiling_state(ScintillaQuickItem *owner)
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

    Hierarchical_profiler *hierarchical_profiler_if_active()
    {
        return active.load(std::memory_order_acquire) ? &hierarchical_profiler : nullptr;
    }

    ScintillaQuickItem *m_owner = nullptr;
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
    profiling_metric_t update_polish;
    profiling_metric_t build_render_snapshot;
    profiling_metric_t update_paint_node;
    profiling_metric_t update_quick_view;
    profiling_metric_t wheel_event;
    profiling_metric_t scroll_horizontal;
    profiling_metric_t scroll_vertical;
    Hierarchical_profiler hierarchical_profiler;
};

namespace {

constexpr int k_margin_count = SC_MAX_MARGIN + 1;
constexpr int k_vertical_scroll_reuse_buffer_min_lines = 16;

// `scene_graph_update_request`, `scene_graph_update_request_t` and
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

    for (visual_line_frame_t &visual_line : frame.visual_lines) {
        translate_point(visual_line.origin, dy);
        visual_line.baseline_y += dy;
        translate_rect(visual_line.clip_rect, dy);
        for (text_run_t &run : visual_line.text_runs) {
            translate_point(run.position, dy);
        }
    }

    for (Selection_primitive &selection : frame.selection_primitives) {
        translate_rect(selection.rect, dy);
    }
    for (caret_primitive_t &caret : frame.caret_primitives) {
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

QString text_for_visual_line(const visual_line_frame_t &line)
{
    QString text;
    for (const text_run_t &run : line.text_runs) {
        text += run.text;
    }
    return text;
}

QColor color_from_scintilla(sptr_t value)
{
    return QColorFromColourRGBA(ColourRGBA::FromIpRGB(value));
}

struct style_attributes_t
{
    QColor foreground;
    QColor background;
    QFont font;
};

using style_cache_t = std::array<std::optional<style_attributes_t>, STYLE_MAX + 1>;

QFont font_for_style(const ScintillaQuickItem *item, int style)
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

const style_attributes_t &style_attributes_for(
    const ScintillaQuickItem *item,
    style_cache_t &style_cache,
    int style)
{
    const int bounded_style = std::clamp(style, 0, STYLE_MAX);
    std::optional<style_attributes_t> &cached_attributes = style_cache[bounded_style];
    if (!cached_attributes.has_value()) {
        style_attributes_t attributes;
        attributes.foreground = color_from_scintilla(item->send(SCI_STYLEGETFORE, bounded_style));
        attributes.background = color_from_scintilla(item->send(SCI_STYLEGETBACK, bounded_style));
        attributes.font = font_for_style(item, bounded_style);
        cached_attributes = std::move(attributes);
    }
    return *cached_attributes;
}

int total_margin_width(const ScintillaQuickItem *item)
{
    int total = 0;
    for (int margin = 0; margin < k_margin_count; ++margin) {
        total += static_cast<int>(item->send(SCI_GETMARGINWIDTHN, margin));
    }
    return total;
}

QColor margin_background_color_for(
    const ScintillaQuickItem *item,
    style_cache_t &style_cache,
    int margin)
{
    const int margin_type = static_cast<int>(item->send(SCI_GETMARGINTYPEN, margin));
    const int margin_mask = static_cast<int>(item->send(SCI_GETMARGINMASKN, margin));

    if ((margin_mask & SC_MASK_FOLDERS) != 0) {
        return QColorFromColourRGBA(Platform::ChromeHighlight());
    }

    switch (margin_type) {
    case SC_MARGIN_BACK:
        return style_attributes_for(item, style_cache, STYLE_DEFAULT).background;
    case SC_MARGIN_FORE:
        return style_attributes_for(item, style_cache, STYLE_DEFAULT).foreground;
    case SC_MARGIN_COLOUR:
        return color_from_scintilla(item->send(SCI_GETMARGINBACKN, margin));
    default:
        return style_attributes_for(item, style_cache, STYLE_LINENUMBER).background;
    }
}

void record_profiling_metric(profiling_metric_t &metric, uint64_t elapsed_ns)
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
    explicit Profiling_scope(profiling_metric_t *metric)
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
    profiling_metric_t *m_metric = nullptr;
    QElapsedTimer m_timer;
};

QJsonObject profiling_metric_to_json(const profiling_metric_t &metric)
{
    const uint64_t count = metric.count.load(std::memory_order_acquire);
    const uint64_t total_ns = metric.total_ns.load(std::memory_order_acquire);
    const uint64_t max_ns = metric.max_ns.load(std::memory_order_acquire);
    const double total_ms = static_cast<double>(total_ns) / 1000000.0;
    const double max_ms = static_cast<double>(max_ns) / 1000000.0;
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

ScintillaQuickItem::ScintillaQuickItem(QQuickItem *parent)
#ifdef PLAT_QT_QML
: QQuickItem(parent)
#else
: QAbstractScrollArea(parent)
#endif
, enableUpdateFlag(true), logicalWidth(0), logicalHeight(0)
#ifdef PLAT_QT_QML
, dataInputMethodHints(Qt::ImhNone)
, aLastTouchPressTime(-1)
#endif
, sqt(new ScintillaQuickCore(this)), preeditPos(-1), m_render_data(std::make_unique<Render_data>())
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

	time.start();

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
	sqt->vs.indicators[IndicatorUnknown] = Indicator(IndicatorStyle::Hidden, colourIME);
	sqt->vs.indicators[IndicatorInput] = Indicator(IndicatorStyle::Dots, colourIME);
	sqt->vs.indicators[IndicatorConverted] = Indicator(IndicatorStyle::CompositionThick, colourIME);
	sqt->vs.indicators[IndicatorTarget] = Indicator(IndicatorStyle::StraightBox, colourIME);

	connect(sqt, SIGNAL(notifyParent(Scintilla::NotificationData)),
		this, SLOT(notifyParent(Scintilla::NotificationData)));

	// Connect scroll bars.
#ifndef PLAT_QT_QML
	// scrollbars are handled by the QML ScrollView outside this class
	connect(verticalScrollBar(), SIGNAL(valueChanged(int)),
	        this, SLOT(scrollVertical(int)));
	connect(horizontalScrollBar(), SIGNAL(valueChanged(int)),
	        this, SLOT(scrollHorizontal(int)));
#endif

	// Connect pass-through signals.
	connect(sqt, SIGNAL(horizontalRangeChanged(int,int)),
	        this, SIGNAL(horizontalRangeChanged(int,int)));
	connect(sqt, SIGNAL(verticalRangeChanged(int,int)),
	        this, SIGNAL(verticalRangeChanged(int,int)));
	connect(sqt, SIGNAL(horizontalScrolled(int)),
	        this, SIGNAL(horizontalScrolled(int)));
	connect(sqt, SIGNAL(verticalScrolled(int)),
	        this, SIGNAL(verticalScrolled(int)));

	connect(sqt, SIGNAL(notifyChange()),
	        this, SIGNAL(notifyChange()));

    connect(sqt, SIGNAL(command(Scintilla::uptr_t,Scintilla::sptr_t)),
		this, SLOT(event_command(Scintilla::uptr_t,Scintilla::sptr_t)));

	connect(sqt, SIGNAL(aboutToCopy(QMimeData*)),
		this, SIGNAL(aboutToCopy(QMimeData*)));

#ifdef PLAT_QT_QML
    connect(sqt, SIGNAL(cursorPositionChanged()), this, SIGNAL(cursorPositionChanged()));   // needed to update markers on android platform
#endif

    m_caret_blink_timer.setSingleShot(false);
    QObject::connect(&m_caret_blink_timer, &QTimer::timeout, this, [this]() {
        m_caret_blink_visible = !m_caret_blink_visible;
        if (m_render_data && enableUpdateFlag) {
            m_render_data->snapshot_dirty = true;
            polish();
            update();
        }
    });

    m_profiling_state = std::make_unique<Profiling_state>(this);

    send(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE);
    send(SCI_SETSCROLLWIDTHTRACKING, 1);
}

ScintillaQuickItem::~ScintillaQuickItem()
{
    stopProfilingSession();
}

sptr_t ScintillaQuickItem::send(
	unsigned int iMessage,
	uptr_t wParam,
	sptr_t lParam) const
{
	// NOTE: `send()` is declared `const` because Qt's Q_PROPERTY READ getters
	// in this class funnel through it for SCI_GET* queries and READ functions
	// must be const. A subset of messages (SCI_SET*, mutators) do observably
	// change editor state and will also schedule scene-graph updates here, so
	// this method is not logically const for those messages. We cast `this`
	// once, in a single, well-marked place.
	ScintillaQuickItem *self = const_cast<ScintillaQuickItem *>(this);

	const sptr_t result = sqt->WndProc(static_cast<Message>(iMessage), wParam, lParam);
	if (tracked_scroll_width_should_reset(iMessage)) {
		self->reset_tracked_scroll_width();
	}
	const scene_graph_update_request_t update_request = scene_graph_update_request(iMessage);
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

sptr_t ScintillaQuickItem::sends(
    unsigned int iMessage,
    uptr_t wParam,
    const char *s) const
{
	return sqt->WndProc(static_cast<Message>(iMessage), wParam, reinterpret_cast<sptr_t>(s));
}

bool ScintillaQuickItem::startProfilingSession(const QString &outputDirectory, double durationSeconds)
{
    if (!m_profiling_state) {
        return false;
    }

    if (durationSeconds <= 0.0 || !std::isfinite(durationSeconds)) {
        durationSeconds = 10.0;
    }

    if (m_profiling_state->active.exchange(true, std::memory_order_acq_rel)) {
        stopProfilingSession();
        m_profiling_state->active.store(true, std::memory_order_release);
    }

    m_profiling_state->reset();
    m_profiling_state->requested_duration_seconds = durationSeconds;
    m_profiling_state->output_directory = profiling_output_directory(outputDirectory);
    m_profiling_state->started_at_utc = QDateTime::currentDateTimeUtc();
    m_profiling_state->elapsed.start();
    m_profiling_state->timer.start(static_cast<int>(durationSeconds * 1000.0));
    qWarning().noquote()
        << "ScintillaQuick profiling started."
        << " output_dir=" << m_profiling_state->output_directory
        << " duration_seconds=" << durationSeconds;
    emit profilingActiveChanged();
    return true;
}

void ScintillaQuickItem::stopProfilingSession()
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
    metadata.insert("logical_width", logicalWidth);
    metadata.insert("logical_height", logicalHeight);
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
    report.insert("tool", QStringLiteral("ScintillaQuickItem"));
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
    } else {
        qWarning().noquote()
            << "ScintillaQuick profiling failed to open report file."
            << " path=" << report_path;
    }

    if (write_ok) {
        qWarning().noquote()
            << "ScintillaQuick profiling finished."
            << " report_path=" << report_path;
    } else {
        qWarning().noquote()
            << "ScintillaQuick profiling finished without report."
            << " report_path=" << report_path;
    }

    emit profilingActiveChanged();
    emit profilingFinished(report_path);
}

bool ScintillaQuickItem::profilingActive() const
{
    return m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire);
}

void ScintillaQuickItem::request_scene_graph_update(
    bool static_content_dirty,
    bool needs_style_sync,
    bool scrolling)
{
    if (!m_render_data || !enableUpdateFlag) {
        return;
    }

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        m_profiling_state->update_requests.fetch_add(1, std::memory_order_relaxed);
    }

    m_render_data->snapshot_dirty = true;
    m_render_data->static_content_dirty = m_render_data->static_content_dirty || static_content_dirty;
    m_render_data->style_sync_needed = m_render_data->style_sync_needed || needs_style_sync;
    m_render_data->scrolling_update = m_render_data->scrolling_update || scrolling;
    m_render_data->overlay_content_dirty = true;
    if (m_render_data->update_pending) {
        return;
    }
    m_render_data->update_pending = true;
    polish();
    update();
}

#ifdef PLAT_QT_QML

void ScintillaQuickItem::scrollRow(int deltaLines)
{
    int currentLine = sqt->TopLineOfMain();
    scrollVertical(currentLine + deltaLines);
}

void ScintillaQuickItem::scrollColumn(int deltaColumns)
{
    int currentColumnInPixel = send(SCI_GETXOFFSET);
    int newValue = currentColumnInPixel + deltaColumns * getCharWidth();
    if(newValue < 0)
    {
        newValue = 0;
    }
//    send(SCI_SETXOFFSET,newValue);
    scrollHorizontal(newValue);
}

void ScintillaQuickItem::cmdContextMenu(int menuID)
{
    sqt->Command(menuID);
}

void ScintillaQuickItem::enableUpdate(bool enable)
{
    enableUpdateFlag = enable;
    if (enableUpdateFlag) {
        request_scene_graph_update();
    }
}

#endif

void ScintillaQuickItem::scrollHorizontal(int value)
{
    Hierarchical_profiler *hierarchical_profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(hierarchical_profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(hierarchical_profiler, "item.scroll_horizontal");

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        m_profiling_state->horizontal_scroll_command_count.fetch_add(1, std::memory_order_relaxed);
    }
	sqt->HorizontalScrollTo(value);
    syncQuickViewProperties();
    request_scene_graph_update(true, false, false);
}

void ScintillaQuickItem::scrollVertical(int value)
{
    Hierarchical_profiler *hierarchical_profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(hierarchical_profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(hierarchical_profiler, "item.scroll_vertical");

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        m_profiling_state->vertical_scroll_command_count.fetch_add(1, std::memory_order_relaxed);
    }
	sqt->ScrollTo(value);
    request_scene_graph_update(true, false, true);
}

bool ScintillaQuickItem::event(QEvent *event)
{
	bool result = false;

	if (event->type() == QEvent::KeyPress) {
		// Circumvent the tab focus convention.
		keyPressEvent(static_cast<QKeyEvent *>(event));
		result = event->isAccepted();
	} else if (event->type() == QEvent::Show) {
#ifndef PLAT_QT_QML
		setMouseTracking(true);
		result = QAbstractScrollArea::event(event);
#else
		result = QQuickItem::event(event);
#endif
	} else if (event->type() == QEvent::Hide) {
#ifndef PLAT_QT_QML
		setMouseTracking(false);
		result = QAbstractScrollArea::event(event);
#else
		result = QQuickItem::event(event);
#endif
	} else {
#ifndef PLAT_QT_QML
		result = QAbstractScrollArea::event(event);
#else
		result = QQuickItem::event(event);
#endif
	}

	return result;
}

#ifndef PLAT_QT_QML

void ScintillaQuickItem::paintEvent(QPaintEvent *event)
{
	sqt->PartialPaint(PRectFromQRect(event->rect()));
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

void ScintillaQuickItem::wheelEvent(QWheelEvent *event)
{
    Hierarchical_profiler *hierarchical_profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(hierarchical_profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(hierarchical_profiler, "item.wheel_event");

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
	} else {
		if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) {
			// Zoom! We play with the font sizes in the styles.
			// Number of steps/line is ignored, we just care if sizing up or down
			if (wheelEventYDelta(event) > 0) {
				sqt->KeyCommand(Message::ZoomIn);
			} else {
				sqt->KeyCommand(Message::ZoomOut);
			}
            if (m_render_data) {
                m_render_data->content_modified_since_last_capture = true;
            }
            syncQuickViewProperties();
            request_scene_graph_update(true, true, false);
		} else {
			// Ignore wheel events when the scroll bars are disabled.
#ifndef PLAT_QT_QML
			if (verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) {
				event->ignore();
			} else {
#endif
#ifdef PLAT_QT_QML
				// Scroll
				int linesToScroll = 3;
                if (event->angleDelta().y() > 0) {
                    scrollVertical(sqt->topLine - linesToScroll);
				} else {
					scrollVertical(sqt->topLine + linesToScroll);
				}
				QQuickItem::wheelEvent(event);
#else
				QAbstractScrollArea::wheelEvent(event);
			}
#endif
		}
	}
}

void ScintillaQuickItem::focusInEvent(QFocusEvent *event)
{
	sqt->SetFocusState(true);

#ifdef PLAT_QT_QML
	QQuickItem::focusInEvent(event);
    syncCaretBlinkTimer(true);
    request_scene_graph_update(false, false, false);
#else
	QAbstractScrollArea::focusInEvent(event);
#endif
}

void ScintillaQuickItem::focusOutEvent(QFocusEvent *event)
{
	sqt->SetFocusState(false);

#ifdef PLAT_QT_QML
	QQuickItem::focusOutEvent(event);
    syncCaretBlinkTimer(false);
    request_scene_graph_update(false, false, false);
#else
	QAbstractScrollArea::focusOutEvent(event);
#endif
}

#ifdef PLAT_QT_QML
void ScintillaQuickItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
	// trigger resize handling only if the size of the control has changed
	// no update is needed for a position change
	if(newGeometry.width() != oldGeometry.width() || newGeometry.height() != oldGeometry.height() )
	{
		sqt->ChangeSize();
		emit resized();
        QQuickItem::geometryChange(newGeometry, oldGeometry);

        if (m_render_data) {
            m_render_data->content_modified_since_last_capture = true;
        }
        request_scene_graph_update(true, true, false);
	}
}

#else

void ScintillaQuickItem::resizeEvent(QResizeEvent *)
{
	sqt->ChangeSize();
	emit resized();
}

#endif

void ScintillaQuickItem::keyPressEvent(QKeyEvent *event)
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
	bool added = sqt->KeyDownWithModifiers(static_cast<Keys>(key),
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
			QByteArray utext = sqt->BytesForDocument(text);
			sqt->InsertCharacter(std::string_view(utext.data(), utext.size()), CharacterSource::DirectInput);
            view_changed = true;
		} else {
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

void ScintillaQuickItem::mousePressEvent(QMouseEvent *event)
{
	Point pos = PointFromQPoint(event->pos());

	emit buttonPressed(event);

	if (event->button() == Qt::MiddleButton &&
	    QGuiApplication::clipboard()->supportsSelection()) {
		SelectionPosition selPos = sqt->SPositionFromLocation(
					pos, false, false, sqt->UserVirtualSpace());
		sqt->sel.Clear();
		sqt->SetSelection(selPos, selPos);
		sqt->PasteFromMode(QClipboard::Selection);
		return;
	}

	if (event->button() == Qt::LeftButton) {
		bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
		bool ctrl  = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
#ifdef Q_WS_X11
		// On X allow choice of rectangular modifier since most window
		// managers grab alt + click for moving windows.
		bool alt   = QGuiApplication::keyboardModifiers() & modifierTranslated(sqt->rectangularSelectionModifier);
#else
		bool alt   = QGuiApplication::keyboardModifiers() & Qt::AltModifier;
#endif

        sqt->ButtonDownWithModifiers(pos, time.elapsed(), ModifierFlags(shift, ctrl, alt));

#ifdef PLAT_QT_QML
        cursorChangedUpdateMarker();
        request_scene_graph_update(false, false, false);
#endif
	}

	if (event->button() == Qt::RightButton) {
		sqt->RightButtonDownWithModifiers(pos, time.elapsed(), ModifiersOfKeyboard());

#ifdef PLAT_QT_QML
		Point pos = PointFromQPoint(event->globalPos());
		Point pt = PointFromQPoint(event->pos());
		if (!sqt->PointInSelection(pt)) {
			sqt->SetEmptySelection(sqt->PositionFromLocation(pt));
		}
// TODO: call context menu callback if set otherwise use default context menu...
		if (sqt->ShouldDisplayPopup(pt)) {
			sqt->ContextMenu(pos); 
		}
        request_scene_graph_update(false, false, false);
#endif
	}

#ifdef PLAT_QT_QML
//    setFocus(true);
    forceActiveFocus();

    emit enableScrollViewInteraction(false);

	event->setAccepted(true);
#endif
}


#ifdef PLAT_QT_QML
void ProcessScintillaContextMenu(Scintilla::Internal::Point pt, const Scintilla::Internal::Window & w, const QList<QPair<QString, QPair<int, bool>>> & menu)
{
	ScintillaQuickItem * pQtObj = static_cast<ScintillaQuickItem *>(w.GetID());

	emit pQtObj->clearContextMenu();
	for (QPair<QString, QPair<int, bool>> item: menu)
	{
		if (item.first.size() > 0)
		{
			emit pQtObj->addToContextMenu(item.second.first, item.first, item.second.second);
		}	
	}

	QPoint aPoint(pt.x, pt.y);
	emit pQtObj->showContextMenu(aPoint);
}
#endif

void ScintillaQuickItem::mouseReleaseEvent(QMouseEvent *event)
{
	const QPoint point = event->pos();
	if (event->button() == Qt::LeftButton)
		sqt->ButtonUpWithModifiers(PointFromQPoint(point), time.elapsed(), ModifiersOfKeyboard());

	const sptr_t pos = send(SCI_POSITIONFROMPOINT, point.x(), point.y());
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

void ScintillaQuickItem::mouseDoubleClickEvent(QMouseEvent *event)
{
	// Scintilla does its own double-click detection.
#ifndef PLAT_QT_QML
	mousePressEvent(event);
#else	
	Q_UNUSED(event);
#endif
}

void ScintillaQuickItem::mouseMoveEvent(QMouseEvent *event)
{
	Point pos = PointFromQPoint(event->pos());

	bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
	bool ctrl  = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
#ifdef Q_WS_X11
	// On X allow choice of rectangular modifier since most window
	// managers grab alt + click for moving windows.
	bool alt   = QGuiApplication::keyboardModifiers() & modifierTranslated(sqt->rectangularSelectionModifier);
#else
	bool alt   = QGuiApplication::keyboardModifiers() & Qt::AltModifier;
#endif

    const KeyMod modifiers = ModifierFlags(shift, ctrl, alt);
    const int previous_first_visible_line = static_cast<int>(send(SCI_GETFIRSTVISIBLELINE));
    const int previous_x_offset = static_cast<int>(send(SCI_GETXOFFSET));

	sqt->ButtonMoveWithModifiers(pos, time.elapsed(), modifiers);

#ifdef PLAT_QT_QML
    cursorChangedUpdateMarker();
    const int current_first_visible_line = static_cast<int>(send(SCI_GETFIRSTVISIBLELINE));
    const int current_x_offset = static_cast<int>(send(SCI_GETXOFFSET));
    const bool vertical_scroll_changed = current_first_visible_line != previous_first_visible_line;
    const bool horizontal_scroll_changed = current_x_offset != previous_x_offset;
    if (vertical_scroll_changed || horizontal_scroll_changed) {
        syncQuickViewProperties();
        request_scene_graph_update(true, false, vertical_scroll_changed);
    } else {
        request_scene_graph_update();
    }

    event->setAccepted(true);
#endif
}

#ifndef PLAT_QT_QML

void ScintillaQuickItem::contextMenuEvent(QContextMenuEvent *event)
{
	Point pos = PointFromQPoint(event->globalPos());
	Point pt = PointFromQPoint(event->pos());
	if (!sqt->PointInSelection(pt)) {
		sqt->SetEmptySelection(sqt->PositionFromLocation(pt));
	}
	if (sqt->ShouldDisplayPopup(pt)) {
		sqt->ContextMenu(pos);
	}
}

#endif

void ScintillaQuickItem::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasUrls()) {
		event->acceptProposedAction();
	} else if (event->mimeData()->hasText()) {
		event->acceptProposedAction();

		Point point = PointFromQPoint(event->pos());
		sqt->DragEnter(point);
	} else {
		event->ignore();
	}
}

void ScintillaQuickItem::dragLeaveEvent(QDragLeaveEvent * /* event */)
{
	sqt->DragLeave();
}

void ScintillaQuickItem::dragMoveEvent(QDragMoveEvent *event)
{
	if (event->mimeData()->hasUrls()) {
		event->acceptProposedAction();
	} else if (event->mimeData()->hasText()) {
		event->acceptProposedAction();

		Point point = PointFromQPoint(event->pos());
		sqt->DragMove(point);
	} else {
		event->ignore();
	}
}

void ScintillaQuickItem::dropEvent(QDropEvent *event)
{
	if (event->mimeData()->hasUrls()) {
		event->acceptProposedAction();
		sqt->DropUrls(event->mimeData());
	} else if (event->mimeData()->hasText()) {
		event->acceptProposedAction();

		Point point = PointFromQPoint(event->pos());
		bool move = (event->source() == this &&
                 event->proposedAction() == Qt::MoveAction);
		sqt->Drop(point, event->mimeData(), move);
	} else {
		event->ignore();
	}
}

bool ScintillaQuickItem::IsHangul(const QChar qchar)
{
	unsigned int unicode = qchar.unicode();
	// Korean character ranges used for preedit chars.
	// http://www.programminginkorean.com/programming/hangul-in-unicode/
	const bool HangulJamo = (0x1100 <= unicode && unicode <= 0x11FF);
	const bool HangulCompatibleJamo = (0x3130 <= unicode && unicode <= 0x318F);
	const bool HangulJamoExtendedA = (0xA960 <= unicode && unicode <= 0xA97F);
	const bool HangulJamoExtendedB = (0xD7B0 <= unicode && unicode <= 0xD7FF);
	const bool HangulSyllable = (0xAC00 <= unicode && unicode <= 0xD7A3);
	return HangulJamo || HangulCompatibleJamo  || HangulSyllable ||
				HangulJamoExtendedA || HangulJamoExtendedB;
}

void ScintillaQuickItem::MoveImeCarets(Scintilla::Position offset)
{
	// Move carets relatively by bytes
	for (size_t r=0; r < sqt->sel.Count(); r++) {
		const Sci::Position positionInsert = sqt->sel.Range(r).Start().Position();
		sqt->sel.Range(r).caret.SetPosition(positionInsert + offset);
		sqt->sel.Range(r).anchor.SetPosition(positionInsert + offset);
 	}
}

void ScintillaQuickItem::DrawImeIndicator(int indicator, int len)
{
	// Emulate the visual style of IME characters with indicators.
	// Draw an indicator on the character before caret by the character bytes of len
	// so it should be called after InsertCharacter().
	// It does not affect caret positions.
	if (indicator < INDICATOR_CONTAINER || indicator > INDICATOR_MAX) {
		return;
	}
	sqt->pdoc->DecorationSetCurrentIndicator(indicator);
	for (size_t r=0; r< sqt-> sel.Count(); r++) {
		const Sci::Position positionInsert = sqt->sel.Range(r).Start().Position();
		sqt->pdoc->DecorationFillRange(positionInsert - len, 1, len);
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
	std::vector<int> imeIndicator(event->preeditString().size(), IndicatorUnknown);
	foreach (QInputMethodEvent::Attribute attr, event->attributes()) {
		if (attr.type == QInputMethodEvent::TextFormat) {
			QTextFormat format = attr.value.value<QTextFormat>();
			QTextCharFormat charFormat = format.toCharFormat();

			int indicator = IndicatorUnknown;
			switch (charFormat.underlineStyle()) {
				case QTextCharFormat::NoUnderline: // win32, linux
					indicator = IndicatorTarget;
					break;
				case QTextCharFormat::SingleUnderline: // osx
				case QTextCharFormat::DashUnderline: // win32, linux
					indicator = IndicatorInput;
					break;
				case QTextCharFormat::DotLine:
				case QTextCharFormat::DashDotLine:
				case QTextCharFormat::WaveUnderline:
				case QTextCharFormat::SpellCheckUnderline:
					indicator = IndicatorConverted;
					break;

				default:
					indicator = IndicatorUnknown;
			}

			if (format.hasProperty(QTextFormat::BackgroundBrush)) // win32, linux
				indicator = IndicatorTarget;

#ifdef Q_OS_OSX
			if (charFormat.underlineStyle() == QTextCharFormat::SingleUnderline) {
				QColor uc = charFormat.underlineColor();
				if (uc.lightness() < 2) { // osx
					indicator = IndicatorTarget;
				}
			}
#endif

			for (int i = attr.start; i < attr.start+attr.length; i++) {
				imeIndicator[i] = indicator;
			}
		}
	}
	return imeIndicator;
}

void ScintillaQuickItem::inputMethodEvent(QInputMethodEvent *event)
{
	// Copy & paste by johnsonj with a lot of helps of Neil
	// Great thanks for my forerunners, jiniya and BLUEnLIVE

	if (sqt->pdoc->IsReadOnly() || sqt->SelectionContainsProtected()) {
		// Here, a canceling and/or completing composition function is needed.
		return;
	}

    // used from QQuickTextControlPrivate::inputMethodEvent()
    for (int i = 0; i < event->attributes().size(); ++i) {
        const QInputMethodEvent::Attribute &a = event->attributes().at(i);
        if (a.type == QInputMethodEvent::Selection) {
            Sci::Position curPos = sqt->CurrentPosition();
            SelectionPosition selStart = sqt->SelectionStart();
            SelectionPosition selEnd = sqt->SelectionEnd();

            int paraStart = sqt->pdoc->ParaUp(curPos);

            //selStart.Add(a.length);
            SelectionPosition newStart(paraStart + a.start);
            SelectionPosition newEnd(paraStart + a.start + a.length);
            if(newStart>newEnd)
            {
                sqt->SetSelection(newEnd, newStart);
            }
            else
            {
                sqt->SetSelection(newStart, newEnd);
            }
            curPos = sqt->CurrentPosition();

            // update markers by triggering QtAndroidInputContext::updateSelectionHandles()
#ifdef PLAT_QT_QML
            cursorChangedUpdateMarker();
#endif
        }
        if (a.type == QInputMethodEvent::Cursor) {
            //hasImState = true;
            //preeditCursor = a.start;
            //cursorVisible = a.length != 0;
        }
    }

	bool initialCompose = false;
	if (sqt->pdoc->TentativeActive()) {
		sqt->pdoc->TentativeUndo();
	} else {
		// No tentative undo means start of this composition so
		// Fill in any virtual spaces.
		initialCompose = true;
	}

	sqt->view.imeCaretBlockOverride = false;

	if (!event->commitString().isEmpty()) {
		const QString &commitStr = event->commitString();
		const int commitStrLen = commitStr.length();

		for (int i = 0; i < commitStrLen;) {
			const int ucWidth = commitStr.at(i).isHighSurrogate() ? 2 : 1;
			const QString oneCharUTF16 = commitStr.mid(i, ucWidth);
			const QByteArray oneChar = sqt->BytesForDocument(oneCharUTF16);

			sqt->InsertCharacter(std::string_view(oneChar.data(), oneChar.length()), CharacterSource::DirectInput);
			i += ucWidth;
		}

	} else if (!event->preeditString().isEmpty()) {
		const QString preeditStr = event->preeditString();
		const int preeditStrLen = preeditStr.length();
		if (preeditStrLen == 0) {
			sqt->ShowCaretAtCurrentPosition();
			return;
		}

		if (initialCompose)
			sqt->ClearBeforeTentativeStart();
		sqt->pdoc->TentativeStart(); // TentativeActive() from now on.

		std::vector<int> imeIndicator = MapImeIndicators(event);

		for (int i = 0; i < preeditStrLen;) {
			const int ucWidth = preeditStr.at(i).isHighSurrogate() ? 2 : 1;
			const QString oneCharUTF16 = preeditStr.mid(i, ucWidth);
			const QByteArray oneChar = sqt->BytesForDocument(oneCharUTF16);
			const int oneCharLen = oneChar.length();

			sqt->InsertCharacter(std::string_view(oneChar.data(), oneCharLen), CharacterSource::TentativeInput);

			DrawImeIndicator(imeIndicator[i], oneCharLen);
			i += ucWidth;
		}

		// Move IME carets.
		int imeCaretPos = GetImeCaretPos(event);
		int imeEndToImeCaretU16 = imeCaretPos - preeditStrLen;
		const Sci::Position imeCaretPosDoc = sqt->pdoc->GetRelativePositionUTF16(sqt->CurrentPosition(), imeEndToImeCaretU16);

		MoveImeCarets(- sqt->CurrentPosition() + imeCaretPosDoc);

		if (IsHangul(preeditStr.at(0))) {
#ifndef Q_OS_WIN
			if (imeCaretPos > 0) {
				int oneCharBefore = sqt->pdoc->GetRelativePosition(sqt->CurrentPosition(), -1);
				MoveImeCarets(- sqt->CurrentPosition() + oneCharBefore);
			}
#endif
			sqt->view.imeCaretBlockOverride = true;
		}

		// Set candidate box position for Qt::ImMicroFocus.
		preeditPos = sqt->CurrentPosition();
		sqt->EnsureCaretVisible();
#ifndef PLAT_QT_QML		
		updateMicroFocus();
#endif
	}
	sqt->ShowCaretAtCurrentPosition();
}

QVariant ScintillaQuickItem::inputMethodQuery(Qt::InputMethodQuery property, QVariant argument) const
{
    // see: QQuickTextEdit::inputMethodQuery(...)
    const PRectangle textRect = sqt ? sqt->GetTextRectangle() : PRectangle();
    const QPointF textOffset(textRect.left, textRect.top);

    if (property == Qt::ImCursorPosition && !argument.isNull()) {
        argument = QVariant(argument.toPointF() - textOffset);
        const QPointF pt = argument.toPointF();
        if (!pt.isNull()) {
            Point scintillaPoint = PointFromQPointF(pt);
            Sci::Position ptPos = sqt->PositionFromLocation(scintillaPoint);
            int pos = send(SCI_GETCURRENTPOS);
            int paraStart = sqt->pdoc->ParaUp(pos);
            return QVariant((int)ptPos - paraStart);
        }
        return inputMethodQuery(property);
    }

    auto v = inputMethodQuery(property);
    if (property == Qt::ImCursorRectangle || property == Qt::ImAnchorRectangle)
    {
        v = QVariant(v.toRectF().translated(textOffset));
    }

    return v;
}

QVariant ScintillaQuickItem::inputMethodQuery(Qt::InputMethodQuery query) const
{
	const Scintilla::Position pos = send(SCI_GETCURRENTPOS);
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
        case Qt::ImAnchorRectangle:
            {
                SelectionPosition selStart = sqt->SelectionStart();
                SelectionPosition selEnd = sqt->SelectionEnd();
                //Sci::Position ptStart = selStart.Position();
                //Sci::Position ptEnd = selEnd.Position();

                Point ptStart = sqt->LocationFromPosition(selStart);
                Point ptEnd = sqt->LocationFromPosition(selEnd);

                int width = send(SCI_GETCARETWIDTH);
                int height = send(SCI_TEXTHEIGHT, line);
                return QRect(ptEnd.x, ptEnd.y, width, height);
            }
        // selection == Position <--> AnchorPosition
        case Qt::ImAnchorPosition:
            {
                //Sci::Position curPos = sqt->CurrentPosition();
                SelectionPosition selStart = sqt->SelectionStart();
                SelectionPosition selEnd = sqt->SelectionEnd();
                ////Point ptEnd = selEnd.Position();
                //return QVariant(/*ptEnd*/10);

                int paraStart = sqt->pdoc->ParaUp(pos);
                return (int)selEnd.Position() - paraStart;
            }
        case Qt::ImAbsolutePosition:
            {
                //Sci::Position curPos = sqt->CurrentPosition();
                //return QVariant((int)curPos);
                return QVariant((int)pos);
            }
        case Qt::ImTextAfterCursor:
        {
            // from Qt::ImSurroundingText:
            int paraStart = sqt->pdoc->ParaUp(pos);
            int paraEnd = sqt->pdoc->ParaDown(pos);
            QVarLengthArray<char,1024> buffer(paraEnd - paraStart + 1);

            Sci_CharacterRange charRange;
            charRange.cpMin = pos;
            charRange.cpMax = paraEnd;

            Sci_TextRange textRange;
            textRange.chrg = charRange;
            textRange.lpstrText = buffer.data();

            send(SCI_GETTEXTRANGE, 0, (sptr_t)&textRange);

            return sqt->StringFromDocument(buffer.constData());
        }
        case Qt::ImTextBeforeCursor:
        {
            // from Qt::ImSurroundingText:
            int paraStart = sqt->pdoc->ParaUp(pos);
            int paraEnd = sqt->pdoc->ParaDown(pos);
            QVarLengthArray<char,1024> buffer(paraEnd - paraStart + 1);

            Sci_CharacterRange charRange;
            charRange.cpMin = paraStart;
            charRange.cpMax = pos;

            Sci_TextRange textRange;
            textRange.chrg = charRange;
            textRange.lpstrText = buffer.data();

            send(SCI_GETTEXTRANGE, 0, (sptr_t)&textRange);

            return sqt->StringFromDocument(buffer.constData());
        }
#endif
		case Qt::ImCursorRectangle:
		{
			const Scintilla::Position startPos = (preeditPos >= 0) ? preeditPos : pos;
			const Point pt = sqt->LocationFromPosition(startPos);
			const int width = static_cast<int>(send(SCI_GETCARETWIDTH));
			const int height = static_cast<int>(send(SCI_TEXTHEIGHT, line));
			return QRectF(pt.x, pt.y, width, height).toRect();
		}

		case Qt::ImFont:
		{
			const sptr_t style = send(SCI_GETSTYLEAT, pos);
			return font_for_style(this, static_cast<int>(style));
		}

		case Qt::ImCursorPosition:
		{
			const Scintilla::Position paraStart = sqt->pdoc->ParaUp(pos);
			return QVariant(static_cast<int>(pos - paraStart));
		}

		case Qt::ImSurroundingText:
		{
			const Scintilla::Position paraStart = sqt->pdoc->ParaUp(pos);
			const Scintilla::Position paraEnd = sqt->pdoc->ParaDown(pos);
			const std::string buffer = sqt->RangeText(paraStart, paraEnd);
			return sqt->StringFromDocument(buffer.c_str());
		}

		case Qt::ImCurrentSelection:
		{
			QVarLengthArray<char,1024> buffer(send(SCI_GETSELTEXT));
			sends(SCI_GETSELTEXT, 0, buffer.data());

			return sqt->StringFromDocument(buffer.constData());
		}

		default:
			return QVariant();
	}
}

#ifdef PLAT_QT_QML

void ScintillaQuickItem::touchEvent(QTouchEvent *event)
{
    if(sqt->pdoc->IsReadOnly())
    {
        //event->ignore();        // --> transformiert touchEvents in mouseEvents !!!
        return;
    }

    forceActiveFocus();

    if( event->touchPointStates() == Qt::TouchPointPressed && event->touchPoints().count()>0 )
    {
        aLastTouchPressTime = time.elapsed();
        cursorChangedUpdateMarker();
    }
    else if( event->touchPointStates() == Qt::TouchPointReleased && event->touchPoints().count()>0 )
    {
        // is ths a short touch (time between press and release < 100ms) ?
        if(aLastTouchPressTime>=0 && (time.elapsed()-aLastTouchPressTime)<100)
        {
            QTouchEvent::TouchPoint point = event->touchPoints().first();
            QPoint mousePressedPoint = point.pos().toPoint();
            Point scintillaPoint = PointFromQPoint(mousePressedPoint);

            Sci::Position pos = sqt->PositionFromLocation(scintillaPoint);
            sqt->MovePositionTo(pos);

#ifdef Q_OS_ANDROID
            // Android: trigger software keyboard for inputs:
            // https://stackoverflow.com/questions/39436518/how-to-get-the-android-keyboard-to-appear-when-using-qt-for-android
            // https://stackoverflow.com/questions/5724811/how-to-show-the-keyboard-on-qt

            // Check if not in readonly modus --> pdoc->IsReadOnly()
            if( hasActiveFocus() && !sqt->pdoc->IsReadOnly() )
            {
                // TODO working: QGuiApplication::inputMethod()->commit();

                // QML: Qt.inputMethod.show();
                QInputMethod *keyboard = qGuiApp->inputMethod();
                //QInputMethod *keyboard = QGuiApplication::inputMethod();
                if(!keyboard->isVisible())
                {
                    keyboard->show();
                }
            }
#endif

            cursorChangedUpdateMarker();
        }
    }
    else
    {
        QQuickItem::touchEvent(event);
        return;
    }

    event->accept();
}

void ScintillaQuickItem::updatePolish()
{
    Hierarchical_profiler *hierarchical_profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(hierarchical_profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(hierarchical_profiler, "item.update_polish");

    Profiling_scope profiling_scope(
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

QSGNode *ScintillaQuickItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *updatePaintNodeData)
{
    Hierarchical_profiler *hierarchical_profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(hierarchical_profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(hierarchical_profiler, "item.update_paint_node");

    Profiling_scope profiling_scope(
        m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)
            ? &m_profiling_state->update_paint_node
            : nullptr);
    Q_UNUSED(updatePaintNodeData);

    if (!m_render_data) {
        delete oldNode;
        return nullptr;
    }

    return m_render_data->renderer.update(window(), oldNode, m_render_data->snapshot, m_render_data->frame);
}

void ScintillaQuickItem::build_render_snapshot()
{
    Hierarchical_profiler *hierarchical_profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(hierarchical_profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(hierarchical_profiler, "item.build_render_snapshot");

    Profiling_scope profiling_scope(
        m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)
            ? &m_profiling_state->build_render_snapshot
            : nullptr);
    if (!m_render_data) {
        return;
    }

    if (!m_render_data->static_content_dirty && !m_render_data->overlay_content_dirty
        && !m_render_data->captured_caret_primitives.empty()) {
        const int caret_width = static_cast<int>(send(SCI_GETCARETWIDTH));
        if (hasActiveFocus() && sqt && sqt->caret.active && m_caret_blink_visible && caret_width > 0) {
            m_render_data->frame.caret_primitives = m_render_data->captured_caret_primitives;
        } else {
            m_render_data->frame.caret_primitives.clear();
        }
        m_render_data->snapshot_dirty = false;
        if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
            m_profiling_state->blink_only_update_count.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    render_snapshot_t snapshot;
    style_cache_t style_cache;
    snapshot.item_size = QSizeF(width(), height());
    snapshot.background = style_attributes_for(this, style_cache, STYLE_DEFAULT).background;
    snapshot.gutter_bands.reserve(k_margin_count);
    qreal margin_left = 0.0;
    for (int margin = 0; margin < k_margin_count; ++margin) {
        const int margin_width = static_cast<int>(send(SCI_GETMARGINWIDTHN, margin));
        if (margin_width <= 0) {
            continue;
        }

        snapshot.gutter_bands.push_back({
            QRectF(margin_left, 0.0, static_cast<qreal>(margin_width), height()),
            margin_background_color_for(this, style_cache, margin),
        });
        margin_left += margin_width;
    }

    const int current_first_visible_line = sqt ? static_cast<int>(send(SCI_GETFIRSTVISIBLELINE)) : -1;
    const int current_x_offset = sqt ? static_cast<int>(send(SCI_GETXOFFSET)) : -1;
    const int previous_scroll_width = sqt ? static_cast<int>(send(SCI_GETSCROLLWIDTH)) : 0;
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
    const int line_height = (sqt && sqt->vs.lineHeight > 0) ? sqt->vs.lineHeight : 1;
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
    } else if (overlay_only_capture && sqt) {
        frame = sqt->current_render_frame(
            nullptr,
            false,
            false,
            false,
            0);
    } else if (sqt) {
        frame = sqt->current_render_frame(
            nullptr,
            static_content_dirty,
            m_render_data->style_sync_needed && static_content_dirty,
            m_render_data->scrolling_update,
            capture_buffer_lines);
    }

    if (can_reuse_vertical_scroll || !static_content_dirty) {
        frame.text_rect = m_render_data->frame.text_rect;
        frame.margin_rect = m_render_data->frame.margin_rect;
        if (!can_reuse_vertical_scroll) {
            frame.visual_lines = std::move(m_render_data->frame.visual_lines);
            frame.indicator_primitives = std::move(m_render_data->frame.indicator_primitives);
            frame.marker_primitives = std::move(m_render_data->frame.marker_primitives);
            frame.margin_text_primitives = std::move(m_render_data->frame.margin_text_primitives);
            frame.fold_display_texts = std::move(m_render_data->frame.fold_display_texts);
            frame.eol_annotations = std::move(m_render_data->frame.eol_annotations);
            frame.annotations = std::move(m_render_data->frame.annotations);
            frame.whitespace_marks = std::move(m_render_data->frame.whitespace_marks);
            frame.decoration_underlines = std::move(m_render_data->frame.decoration_underlines);
            frame.indent_guides = std::move(m_render_data->frame.indent_guides);
        }
    }

    m_render_data->captured_caret_primitives = frame.caret_primitives;

    const int caret_width = static_cast<int>(send(SCI_GETCARETWIDTH));
    if (!(hasActiveFocus() && sqt && sqt->caret.active && m_caret_blink_visible && caret_width > 0)) {
        frame.caret_primitives.clear();
    }

    if (m_profiling_state && m_profiling_state->active.load(std::memory_order_acquire)) {
        if (static_content_dirty && !can_reuse_vertical_scroll) {
            m_profiling_state->full_update_count.fetch_add(1, std::memory_order_relaxed);
        } else {
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

    m_render_data->snapshot = std::move(snapshot);
    m_render_data->frame = std::move(frame);
    m_render_data->snapshot_dirty = false;
    m_render_data->static_content_dirty = false;
    m_render_data->style_sync_needed = false;
    m_render_data->scrolling_update = false;
    m_render_data->overlay_content_dirty = false;
    m_render_data->content_modified_since_last_capture = false;
    if (!can_reuse_vertical_scroll) {
        m_render_data->capture_base_first_visible_line = current_first_visible_line;
    }
    m_render_data->previous_first_visible_line = current_first_visible_line;
    m_render_data->previous_x_offset = current_x_offset;

    if (sqt) {
        const int current_scroll_width = static_cast<int>(send(SCI_GETSCROLLWIDTH));
        if (current_scroll_width != previous_scroll_width) {
            syncQuickViewProperties();
        }
    }
}

std::vector<displayed_row_for_test_t> ScintillaQuickItem::displayed_rows_for_test() const
{
    if (!m_render_data) {
        return {};
    }

    std::vector<displayed_row_for_test_t> rows;
    rows.reserve(m_render_data->frame.visual_lines.size());
    for (const visual_line_frame_t &line : m_render_data->frame.visual_lines) {
        displayed_row_for_test_t row;
        row.document_line = line.key.document_line;
        row.subline_index = line.key.subline_index;
        row.top = line.clip_rect.top();
        row.bottom = line.clip_rect.bottom();
        row.text = text_for_visual_line(line);
        rows.push_back(std::move(row));
    }
    return rows;
}

const Render_frame &ScintillaQuickItem::rendered_frame_for_test() const
{
    static const Render_frame empty_frame;
    return m_render_data ? m_render_data->frame : empty_frame;
}

#endif

void ScintillaQuickItem::notifyParent(NotificationData scn)
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
			const bool added = FlagSet(scn.modificationType, ModificationFlags::InsertText);
			const bool deleted = FlagSet(scn.modificationType, ModificationFlags::DeleteText);

			const Scintilla::Position length = send(SCI_GETTEXTLENGTH);
			bool firstLineAdded = (added && length == 1) ||
			                      (deleted && length == 0);

			if (scn.linesAdded != 0) {
				emit linesAdded(scn.linesAdded);
			} else if (firstLineAdded) {
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

void ScintillaQuickItem::event_command(uptr_t wParam, sptr_t lParam)
{
	emit command(wParam, lParam);
}

KeyMod ScintillaQuickItem::ModifiersOfKeyboard()
{
	const bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
	const bool ctrl  = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
	const bool alt   = QGuiApplication::keyboardModifiers() & Qt::AltModifier;

    return ModifierFlags(shift, ctrl, alt);
}


#ifdef PLAT_QT_QML

QString ScintillaQuickItem::getText() const
{
	const int textLength = static_cast<int>(send(SCI_GETTEXTLENGTH));
	QByteArray buffer(textLength + 1, Qt::Uninitialized);
	send(SCI_GETTEXT, textLength + 1, reinterpret_cast<sptr_t>(buffer.data()));
	return QString::fromUtf8(buffer.constData());
}

void ScintillaQuickItem::setText(const QString & txt)
{
	const QByteArray utf8 = txt.toUtf8();
	send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(utf8.constData()));
	send(SCI_EMPTYUNDOBUFFER);
	send(SCI_COLOURISE, 0, -1);
	setFocus(true);
    syncQuickViewProperties();
    emit textChanged();
}

void ScintillaQuickItem::setFont(const QFont & newFont)
{
//TODO: maybe    QApplication::setFont(newFont);
    if (newFont == aFont) {
        return;
    }

	aFont = newFont;

	// Set the font for a style.
	setStylesFont(newFont, 0);
    syncQuickViewProperties();
    emit fontChanged();
    if (m_render_data) {
        m_render_data->content_modified_since_last_capture = true;
    }
    request_scene_graph_update(true, true, false);
}

int ScintillaQuickItem::getLogicalWidth() const
{
	return logicalWidth;
}

int ScintillaQuickItem::getLogicalHeight() const
{
	return logicalHeight;
}

int ScintillaQuickItem::getCharHeight() const
{
	int charHeight = send(SCI_TEXTHEIGHT);
	return charHeight;
}

int ScintillaQuickItem::getCharWidth() const
{
	const char buf[] = "X";
	return static_cast<int>(send(SCI_TEXTWIDTH, 0, reinterpret_cast<sptr_t>(buf)));
}

int ScintillaQuickItem::getFirstVisibleLine() const
{
	int firstLine = send(SCI_GETFIRSTVISIBLELINE);
	return firstLine;
}

void ScintillaQuickItem::setFirstVisibleLine(int lineNo)
{
	send(SCI_SETFIRSTVISIBLELINE, lineNo);
    syncQuickViewProperties();
}

int ScintillaQuickItem::getTotalLines() const
{
	int lineCount = send(SCI_GETLINECOUNT);
	return lineCount;
}

int ScintillaQuickItem::getFirstVisibleColumn() const
{
	const int charWidth = getCharWidth();
	if (charWidth <= 0) {
		return 0;
	}
	return static_cast<int>(send(SCI_GETXOFFSET)) / charWidth;
}

int ScintillaQuickItem::getTotalColumns() const
{
	const int charWidth = getCharWidth();
	if (charWidth <= 0) {
		return 0;
	}
	return static_cast<int>(send(SCI_GETSCROLLWIDTH)) / charWidth;
}

int ScintillaQuickItem::getVisibleLines() const
{
	int count = send(SCI_LINESONSCREEN);
	return count;
}

int ScintillaQuickItem::getVisibleColumns() const
{
	const int charWidth = getCharWidth();
	if (charWidth <= 0) {
		return 0;
	}

	const PRectangle textRect = sqt->GetTextRectangle();
	const int visibleWidth = std::max(0, static_cast<int>(textRect.Width()));
	return visibleWidth / charWidth;
}

Qt::InputMethodHints ScintillaQuickItem::inputMethodHints() const
{
	return dataInputMethodHints | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText | Qt::ImhMultiLine;
}

void ScintillaQuickItem::setInputMethodHints(Qt::InputMethodHints hints)
{
	if (hints == dataInputMethodHints)
		return;

	dataInputMethodHints = hints;
	updateInputMethod(Qt::ImHints);
	emit inputMethodHintsChanged();
}

bool ScintillaQuickItem::getReadonly() const
{
	bool flag = (bool)send(SCI_GETREADONLY);
	return flag;
}

void ScintillaQuickItem::setReadonly(bool value)
{
    if(value != getReadonly())
    {
        send(SCI_SETREADONLY, value);

        syncQuickViewProperties();
        emit readonlyChanged();
    }
}

void ScintillaQuickItem::updateQuickView(Update updated)
{
    Hierarchical_profiler *hierarchical_profiler =
        m_profiling_state ? m_profiling_state->hierarchical_profiler_if_active() : nullptr;
    Active_hierarchical_profiler_binding hierarchical_binding(hierarchical_profiler);
    (void)hierarchical_binding;
    SCINTILLAQUICK_PROFILE_SCOPE(hierarchical_profiler, "item.update_quick_view");

    Profiling_scope profiling_scope(
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

void ScintillaQuickItem::syncQuickViewProperties()
{
    if (!sqt) {
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

    const int charHeight = getCharHeight();
    const int charWidth = getCharWidth();
    const int lineCount = send(SCI_GETLINECOUNT);
    const int textWidth = send(SCI_GETSCROLLWIDTH);
    const int textHeight = lineCount * charHeight;
    const int totalColumns = (charWidth > 0) ? (textWidth / charWidth) : 0;
    const int visibleLines = send(SCI_LINESONSCREEN);
    const int visibleColumns = (charWidth > 0) ? getVisibleColumns() : 0;
    const int firstVisibleLine = send(SCI_GETFIRSTVISIBLELINE);
    const int firstVisibleColumn = (charWidth > 0) ? getFirstVisibleColumn() : 0;

    auto emit_if_changed = [this](int &cached_value, int current_value, void (ScintillaQuickItem::*signal)()) {
        if (cached_value != current_value) {
            cached_value = current_value;
            (this->*signal)();
        }
    };

    emit_if_changed(m_cached_char_height, charHeight, &ScintillaQuickItem::charHeightChanged);
    emit_if_changed(m_cached_char_width, charWidth, &ScintillaQuickItem::charWidthChanged);
    emit_if_changed(m_cached_total_lines, lineCount, &ScintillaQuickItem::totalLinesChanged);
    emit_if_changed(m_cached_total_columns, totalColumns, &ScintillaQuickItem::totalColumnsChanged);
    emit_if_changed(m_cached_visible_lines, visibleLines, &ScintillaQuickItem::visibleLinesChanged);
    emit_if_changed(m_cached_visible_columns, visibleColumns, &ScintillaQuickItem::visibleColumnsChanged);
    emit_if_changed(m_cached_first_visible_line, firstVisibleLine, &ScintillaQuickItem::firstVisibleLineChanged);
    emit_if_changed(m_cached_first_visible_column, firstVisibleColumn, &ScintillaQuickItem::firstVisibleColumnChanged);

    if (logicalWidth != textWidth) {
        logicalWidth = textWidth;
        emit logicalWidthChanged();
    }
    if (logicalHeight != textHeight) {
        logicalHeight = textHeight;
        emit logicalHeightChanged();
    }
}

void ScintillaQuickItem::reset_tracked_scroll_width()
{
    if (!sqt) {
        return;
    }

    if (!sqt->WndProc(Message::GetScrollWidthTracking, 0, 0)) {
        return;
    }

    sqt->reset_tracked_scroll_width_to_viewport();
}

// taken from QScintilla
void ScintillaQuickItem::setStylesFont(const QFont &f, int style)
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
	if (style == 0)
		setStylesFont(f, STYLE_DEFAULT);
}

void ScintillaQuickItem::cursorChangedUpdateMarker()
{
    if(!sqt->pdoc->IsReadOnly())
    {
        syncCaretBlinkTimer(true);
        emit qGuiApp->inputMethod()->cursorRectangleChanged();   // IMPORTANT: this moves the handle !!! see: QQuickTextControl::updateCursorRectangle()
        emit qGuiApp->inputMethod()->anchorRectangleChanged();
        emit cursorPositionChanged();
    }
}

void ScintillaQuickItem::syncCaretBlinkTimer(bool resetPhase)
{
    const bool caret_should_blink = sqt && hasActiveFocus() && sqt->caret.active;
    const int caret_period = sqt ? static_cast<int>(send(SCI_GETCARETPERIOD)) : 0;

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
    qmlRegisterType<ScintillaQuickItem>("ScintillaQuick", 1, 0, "ScintillaQuickItem");
}

#endif
