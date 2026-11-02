// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick/scintillaquick_source_preview.h"

#include "scintillaquick_source_preview_raster_p.h"

#include <QBitArray>
#include <QCache>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointer>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGSimpleRectNode>
#include <QSGTexture>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace preview = ScintillaQuick_preview;

namespace {

constexpr qreal k_bookmark_lane        = 4;
constexpr qreal k_detailed_width       = 96;
constexpr qreal k_compact_width        = 16;
constexpr qreal k_min_viewport_height  = 3;
constexpr qreal k_viewport_fade_height = 12;
constexpr qint64 k_capture_slice_ns = 1000000;
constexpr int k_max_image_width = 1024;
constexpr int k_max_image_height = 8192;
constexpr int k_max_image_pixels = 1024 * 1024;
constexpr int k_hidden_image_bytes = 8 * 1024 * 1024;
constexpr int k_hidden_entry_minimum_cost = k_hidden_image_bytes / 16;

struct Projection
{
    QSize pixels;
    QColor background;
    int line_count = 1;
    qreal line_advance = 1;
};

struct Published_preview
{
    QImage image;
    Projection projection;
    QSizeF item_size;
    qreal dpr = 1;
    quint64 serial = 0;
    quint64 binding = 0;
    quint64 content = 0;
    quint64 line_revision = 0;
    quint64 style_revision = 0;
    quint64 geometry_revision = 0;
};

QCache<quintptr, Published_preview>& hidden_previews()
{
    // GUI-thread ownership only. The minimum charge also bounds the number of
    // tiny entries; no hidden item retains another reference to these images.
    static QCache<quintptr, Published_preview> cache(k_hidden_image_bytes);
    return cache;
}

class Preview_thread : public QThread
{
public:
    explicit Preview_thread(QObject* parent)
    :
        QThread(parent)
    {
        setObjectName(QStringLiteral("source-preview-raster"));
        start(QThread::LowPriority);
    }

    ~Preview_thread() override
    {
        quit();
        wait();
    }
};

QThread* raster_thread()
{
    // Qt text layout and raster work stay on this one thread. Each view submits at
    // most one bounded chunk, so a large document cannot monopolize its queue.
    static QPointer<Preview_thread> thread;
    if (!thread) {
        thread = new Preview_thread(QCoreApplication::instance());
    }
    return thread;
}

QColor native_color(sptr_t value)
{
    return QColor(value & 255, (value >> 8) & 255, (value >> 16) & 255);
}

QSize image_size(qreal width, qreal height, qreal dpr)
{
    qreal pixel_width = std::clamp(width * dpr, qreal(1), qreal(k_max_image_width));
    qreal pixel_height = std::clamp(height * dpr, qreal(1), qreal(k_max_image_height));
    const qreal reduction = std::min<qreal>(
        1, std::sqrt(k_max_image_pixels / (pixel_width * pixel_height)));
    return QSize(std::max(1, (int)(pixel_width * reduction)),
        std::max(1, (int)(pixel_height * reduction)));
}

class Tick_strip : public QSGNode
{
public:
    ~Tick_strip() override { delete m_image; }

    void update(QQuickWindow* window, const QBitArray& rows, const QColor& color, const QRectF& rect,
        QSGTexture::Filtering filtering = QSGTexture::Nearest)
    {
        if (rows != m_rows || color != m_color) {
            m_rows = rows;
            m_color = color;
            if (rows.isEmpty()) {
                clear();
            }
            else {
                // One texel per already-reduced rail row, at most 8192 rows.
                // Factory image nodes work in both software and RHI scenes.
                QImage image(1, rows.size(), QImage::Format_ARGB32_Premultiplied);
                if (image.isNull()) {
                    clear();
                    return;
                }
                image.fill(Qt::transparent);
                const QRgb pixel = qPremultiply(color.rgba());
                for (qsizetype row = 0; row < rows.size(); ++row) {
                    if (rows.testBit(row)) {
                        image.setPixel(0, row, pixel);
                    }
                }
                auto texture = std::unique_ptr<QSGTexture>(window->createTextureFromImage(image));
                if (!texture) {
                    clear();
                    return;
                }
                if (!m_image) {
                    m_image = window->createImageNode();
                    m_image->setFiltering(filtering);
                    m_image->setTexture(texture.get());
                    m_image->setRect(rect);
                    // An attached software scene inspects a child immediately.
                    // Its texture must be valid before the node is published.
                    appendChildNode(m_image);
                }
                else {
                    m_image->setTexture(texture.get());
                }
                m_texture = std::move(texture);
            }
        }
        if (m_image) {
            m_image->setFiltering(filtering);
            m_image->setRect(rect);
        }
    }

private:
    void clear()
    {
        delete m_image;
        m_image = nullptr;
        m_texture.reset();
    }

    QSGImageNode* m_image = nullptr;
    std::unique_ptr<QSGTexture> m_texture;
    QBitArray m_rows;
    QColor m_color;
};

class Preview_node : public QSGNode
{
public:
    Preview_node()
    {
        appendChildNode(m_background = new QSGSimpleRectNode);
        appendChildNode(m_viewport = new QSGSimpleRectNode);
        appendChildNode(m_viewport_top = new Tick_strip);
        appendChildNode(m_viewport_bottom = new Tick_strip);
        appendChildNode(m_bookmarks = new Tick_strip);
        appendChildNode(m_occurrences = new Tick_strip);
    }

    ~Preview_node() override { delete m_image; }

    QSGSimpleRectNode* m_background = nullptr;
    QSGSimpleRectNode* m_viewport = nullptr;
    Tick_strip* m_viewport_top = nullptr;
    Tick_strip* m_viewport_bottom = nullptr;
    Tick_strip* m_bookmarks = nullptr;
    Tick_strip* m_occurrences = nullptr;
    QSGImageNode* m_image = nullptr;
    std::unique_ptr<QSGTexture> m_texture;
    quint64 m_image_serial = 0;
    quint64 m_bookmark_revision = std::numeric_limits<quint64>::max();
    quint64 m_occurrence_revision = std::numeric_limits<quint64>::max();
    QSizeF m_geometry_size;
};

}

class ScintillaQuick_source_preview::State
{
public:
    explicit State(ScintillaQuick_source_preview* item)
    :
        m_owner(item),
        m_timer(item),
        m_bookmark_timer(item),
        m_occurrence_timer(item),
        m_generation(std::make_shared<std::atomic<quint64>>(0)),
        m_worker(new preview::Raster_worker(m_generation))
    {
        m_timer.setSingleShot(true);
        m_bookmark_timer.setSingleShot(true);
        m_occurrence_timer.setSingleShot(true);
        m_worker->moveToThread(raster_thread());
        QObject::connect(m_worker->thread(), &QThread::finished, m_worker, &QObject::deleteLater);
        QObject::connect(&m_bookmark_timer, &QTimer::timeout, m_owner, [this] { reduce_bookmarks(); });
        QObject::connect(&m_occurrence_timer, &QTimer::timeout, m_owner, [this] { reduce_occurrences(); });
    }

    bool active() const
    {
        return m_editor && m_owner->window() && m_owner->isVisible() &&
            m_owner->width() > k_bookmark_lane && m_owner->height() > 0;
    }

    const Projection& projection() const
    {
        return m_published ? m_published->projection : m_unpublished_projection;
    }

    bool published_source_is_current() const
    {
        return m_published && m_editor &&
            m_published->binding == m_editor->native_binding_revision() &&
            m_published->content == m_editor->native_content_revision();
    }

    bool published_mapping_is_current() const
    {
        // Primitive edits can replace/reorder rows while keeping the final
        // line count. Only the native topology stamp proves row continuity.
        return m_published && m_editor && m_owner->window() &&
            m_published->binding == m_editor->native_binding_revision() &&
            m_published->line_revision == m_editor->native_line_revision() &&
            m_published->geometry_revision == m_geometry_revision;
    }

    bool published_geometry_is_current() const
    {
        // A resize/DPR change can scale a compatible old image and its original
        // map together, while still requiring a new raster for the exact size.
        return published_mapping_is_current() &&
            m_published->item_size == m_owner->size() &&
            m_published->dpr == m_owner->window()->effectiveDevicePixelRatio();
    }

    bool published_image_is_current() const
    {
        return published_source_is_current() && published_geometry_is_current() &&
            m_published->style_revision == m_style_revision;
    }

    bool overlay_source_is_current() const
    {
        if (m_published) {
            return published_mapping_is_current();
        }
        return m_editor && !m_unpublished_projection.pixels.isEmpty() &&
            m_unpublished_binding == m_editor->native_binding_revision() &&
            m_unpublished_line_revision == m_editor->native_line_revision() &&
            m_unpublished_geometry_revision == m_geometry_revision &&
            m_unpublished_item_size == m_owner->size() && m_owner->window() &&
            m_unpublished_dpr == m_owner->window()->effectiveDevicePixelRatio();
    }

    void refresh_bookmark_binding()
    {
        // GUI thread only. Revision counters belong to individual items, so
        // compare the payload with its guarded owner and prove shared native
        // document identity separately from the displayed view's revisions.
        m_bookmark_view_binding = m_editor ? m_editor->native_binding_revision() : 0;
        m_bookmark_same_document = m_editor && m_bookmark_source &&
            m_editor->send(SCI_GETDOCPOINTER) == m_bookmark_source->send(SCI_GETDOCPOINTER);
    }

    bool bookmark_projection_is_current() const
    {
        // Only copied counter getters are read during scene-graph sync; all
        // native document queries remain in refresh_bookmark_binding().
        return m_bookmark_projection_valid && m_bookmark_same_document &&
            m_editor && m_bookmark_source &&
            m_bookmark_view_binding == m_editor->native_binding_revision() &&
            m_bookmark_binding == m_bookmark_source->native_binding_revision() &&
            m_bookmark_line_revision == m_bookmark_source->native_line_revision() &&
            m_bookmark_marker_revision == m_bookmark_source->native_marker_revision();
    }

    void reset_overlays()
    {
        m_bookmark_rows.clear();
        m_occurrence_pixels.clear();
        ++m_bookmark_revision;
        ++m_occurrence_revision;
        rebuild_bookmarks();
        rebuild_occurrences();
    }

    void rebuild_bookmarks()
    {
        m_bookmark_cursor = 0;
        m_pending_bookmark_rows = QBitArray(std::max(0, projection().pixels.height()));
        if (active() && overlay_source_is_current() && bookmark_projection_is_current()) {
            m_bookmark_timer.start(0);
        }
        else {
            m_bookmark_timer.stop();
        }
    }

    void reduce_bookmarks()
    {
        if (!active() || !overlay_source_is_current() || !bookmark_projection_is_current() ||
            m_pending_bookmark_rows.isEmpty())
        {
            return;
        }
        QElapsedTimer slice;
        slice.start();
        while (m_bookmark_cursor < m_bookmark_lines.size()) {
            bool valid = false;
            const qlonglong line = m_bookmark_lines[m_bookmark_cursor++].toLongLong(&valid);
            if (valid && line >= 1 && line <= projection().line_count) {
                const int row = std::min(
                    m_pending_bookmark_rows.size() - 1,
                    (qsizetype)((line - 1) * projection().line_advance));
                m_pending_bookmark_rows.setBit(row);
            }
            if (slice.nsecsElapsed() >= k_capture_slice_ns) {
                m_bookmark_timer.start(0);
                return;
            }
        }
        m_bookmark_rows = std::move(m_pending_bookmark_rows);
        ++m_bookmark_revision;
        m_owner->update();
    }

    void rebuild_occurrences()
    {
        m_occurrence_cursor = 0;
        m_pending_occurrence_pixels = QBitArray(std::max(0, projection().pixels.height()));
        if (active() && overlay_source_is_current()) {
            m_occurrence_timer.start(0);
        }
        else {
            m_occurrence_timer.stop();
        }
    }

    void reduce_occurrences()
    {
        if (!active() || !overlay_source_is_current() || m_pending_occurrence_pixels.isEmpty()) {
            return;
        }
        QElapsedTimer slice;
        slice.start();
        while (m_occurrence_cursor < m_occurrence_rows.size()) {
            const qsizetype bucket = m_occurrence_cursor++;
            if (m_occurrence_rows[bucket]) {
                const qreal line = bucket * (qreal)projection().line_count / m_occurrence_rows.size();
                const int row = std::min(
                    m_pending_occurrence_pixels.size() - 1,
                    (qsizetype)(line * projection().line_advance));
                m_pending_occurrence_pixels.setBit(row);
            }
            if (slice.nsecsElapsed() >= k_capture_slice_ns) {
                m_occurrence_timer.start(0);
                return;
            }
        }
        m_occurrence_pixels = std::move(m_pending_occurrence_pixels);
        ++m_occurrence_revision;
        m_owner->update();
    }

    bool source_is_current() const
    {
        return m_editor && m_editor->native_binding_revision() == m_binding &&
            m_editor->native_content_revision() == m_content &&
            m_editor->native_line_revision() == m_line_revision;
    }

    ScintillaQuick_source_preview* m_owner;
    QPointer<ScintillaQuick_item> m_editor;
    bool m_detailed_view = true;
    QVector<QMetaObject::Connection> m_editor_connections;
    QTimer m_timer;
    QTimer m_bookmark_timer;
    QTimer m_occurrence_timer;
    std::shared_ptr<std::atomic<quint64>> m_generation;
    preview::Raster_worker* m_worker;
    preview::Configuration m_configuration;
    QSizeF m_capture_item_size;
    qreal m_capture_dpr = 1;
    quint64 m_capture_style_revision = 0;
    quint64 m_capture_geometry_revision = 0;
    bool m_initialized = false;
    bool m_worker_started = false;
    bool m_worker_has_storage = false;
    bool m_chunk_pending = false;
    int m_style_index = 0;
    int m_next_line = 0;
    sptr_t m_document_length = 0;
    quint64 m_binding = 0;
    quint64 m_content = 0;
    quint64 m_line_revision = 0;
    QFont m_base_font;
    int m_zoom = 0;
    quint64 m_style_revision = 0;
    quint64 m_geometry_revision = 0;
    std::unique_ptr<Published_preview> m_published;
    Projection m_unpublished_projection;
    quint64 m_unpublished_binding = 0;
    quint64 m_unpublished_line_revision = 0;
    quint64 m_unpublished_geometry_revision = 0;
    QSizeF m_unpublished_item_size;
    qreal m_unpublished_dpr = 1;
    std::atomic<bool> m_node_may_exist = false;
    bool m_release_update_requested = false;
    qreal m_first_visible_line = 0;
    qreal m_last_visible_line = 1;
    QVariantMap m_bookmark_projection;
    QVariantList m_bookmark_lines;
    QPointer<ScintillaQuick_item> m_bookmark_source;
    quint64 m_bookmark_binding = 0;
    quint64 m_bookmark_line_revision = 0;
    quint64 m_bookmark_marker_revision = 0;
    quint64 m_bookmark_view_binding = 0;
    bool m_bookmark_projection_valid = false;
    bool m_bookmark_same_document = false;
    QColor m_bookmark_color = Qt::transparent;
    QColor m_viewport_color = Qt::transparent;
    QByteArray m_occurrence_rows;
    QColor m_occurrence_color = Qt::transparent;
    QBitArray m_occurrence_pixels;
    QBitArray m_pending_occurrence_pixels;
    qsizetype m_occurrence_cursor = 0;
    quint64 m_occurrence_revision = 0;
    QBitArray m_bookmark_rows;
    QBitArray m_pending_bookmark_rows;
    qsizetype m_bookmark_cursor = 0;
    quint64 m_bookmark_revision = 0;
};

ScintillaQuick_source_preview::ScintillaQuick_source_preview(QQuickItem* parent)
:
    QQuickItem(parent),
    m_state(std::make_unique<State>(this))
{
    setFlag(ItemHasContents);
    setClip(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setImplicitWidth(k_detailed_width);
    connect(&m_state->m_timer, &QTimer::timeout, this, &ScintillaQuick_source_preview::capture_next_chunk);
    connect(this, &QQuickItem::visibleChanged, this, &ScintillaQuick_source_preview::visibility_changed);
    connect(this, &QQuickItem::windowChanged, this, &ScintillaQuick_source_preview::visibility_changed);
    connect(m_state->m_worker, &preview::Raster_worker::chunk_finished, this,
        [this](quint64 serial, bool final, const QImage& image, bool allocation_failed) {
            auto& state = *m_state;
            state.m_chunk_pending = false;
            if (serial != state.m_generation->load() || !state.source_is_current()) {
                // Cancellation can arrive after consume() emitted a partial
                // result. In that ordering it has not cleared its canvas;
                // enqueue the release before any replacement chunk.
                QMetaObject::invokeMethod(state.m_worker,
                    [worker = state.m_worker] { worker->clear(); }, Qt::QueuedConnection);
                state.m_worker_has_storage = false;
                if (state.m_detailed_view && state.active() &&
                    !state.m_timer.isActive() && !state.published_image_is_current())
                {
                    state.m_timer.start(0);
                }
                return;
            }
            if (!final) {
                // This acknowledgment is already a queued GUI-thread turn.
                // Capture at most 1 ms / 128 lines here instead of adding a
                // second event hop. One worker chunk remains in flight.
                capture_next_chunk();
                return;
            }
            state.m_worker_has_storage = false;
            if (allocation_failed) {
                return;
            }
            auto published = std::make_unique<Published_preview>();
            published->image = image;
            published->projection = {
                state.m_configuration.pixels, state.m_configuration.background,
                state.m_configuration.line_count, state.m_configuration.line_advance
            };
            published->item_size = state.m_capture_item_size;
            published->dpr = state.m_capture_dpr;
            published->serial = serial;
            published->binding = state.m_binding;
            published->content = state.m_content;
            published->line_revision = state.m_line_revision;
            published->style_revision = state.m_capture_style_revision;
            published->geometry_revision = state.m_capture_geometry_revision;
            const bool keep_overlays = state.overlay_source_is_current() &&
                state.projection().pixels == published->projection.pixels &&
                state.projection().line_count == published->projection.line_count &&
                state.projection().line_advance == published->projection.line_advance;
            state.m_published = std::move(published);
            if (!keep_overlays) {
                state.reset_overlays();
            }
            update_viewport();
            update();
        }, Qt::QueuedConnection);
}

ScintillaQuick_source_preview::~ScintillaQuick_source_preview()
{
    // QQuickItem detaches from its window during base destruction, after our
    // State has gone. Its self-signals must no longer call these lambdas.
    QObject::disconnect(this, nullptr, this, nullptr);
    for (const auto& connection : m_state->m_editor_connections) {
        QObject::disconnect(connection);
    }
    QObject::disconnect(m_state->m_worker, nullptr, this, nullptr);
    ++*m_state->m_generation;
    hidden_previews().remove(reinterpret_cast<quintptr>(this));
    m_state->m_worker->deleteLater();
}

ScintillaQuick_item* ScintillaQuick_source_preview::editor() const { return m_state->m_editor; }
bool ScintillaQuick_source_preview::detailed_view() const { return m_state->m_detailed_view; }
QVariantMap ScintillaQuick_source_preview::bookmark_projection() const { return m_state->m_bookmark_projection; }
QColor ScintillaQuick_source_preview::bookmark_color() const { return m_state->m_bookmark_color; }
QColor ScintillaQuick_source_preview::viewport_color() const { return m_state->m_viewport_color; }
QByteArray ScintillaQuick_source_preview::occurrence_rows() const { return m_state->m_occurrence_rows; }
QColor ScintillaQuick_source_preview::occurrence_color() const { return m_state->m_occurrence_color; }
quint64 ScintillaQuick_source_preview::published_image_serial_for_test() const
{
    return m_state->m_published ? m_state->m_published->serial : 0;
}

void ScintillaQuick_source_preview::set_editor(ScintillaQuick_item* editor)
{
    auto& state = *m_state;
    if (state.m_editor == editor) {
        return;
    }
    for (const auto& connection : state.m_editor_connections) {
        QObject::disconnect(connection);
    }
    state.m_editor_connections.clear();
    state.m_editor = editor;
    if (editor) {
        state.m_editor_connections = {
            connect(editor, &ScintillaQuick_item::textChanged,
                this, &ScintillaQuick_source_preview::invalidate_source),
            connect(editor, &ScintillaQuick_item::fontChanged,
                this, &ScintillaQuick_source_preview::refresh_style),
            connect(editor, &ScintillaQuick_item::zoom,
                this, &ScintillaQuick_source_preview::refresh_style),
            connect(editor, &ScintillaQuick_item::vertical_scroll_value_changed,
                this, &ScintillaQuick_source_preview::update_viewport),
            connect(editor, &ScintillaQuick_item::vertical_scroll_page_changed,
                this, &ScintillaQuick_source_preview::update_viewport),
            connect(editor, &ScintillaQuick_item::updateUi,
                this, &ScintillaQuick_source_preview::update_viewport),
            connect(editor, &ScintillaQuick_item::modified, this,
                [this](Scintilla::ModificationFlags flags) {
                    if ((int)flags & (int)Scintilla::ModificationFlags::ChangeStyle) {
                        // Lexer style assignments do not change the default
                        // font geometry used to place logical source rows.
                        ++m_state->m_style_revision;
                        invalidate_source();
                    }
                }),
            connect(editor, &QObject::destroyed, this, [this] {
                m_state->m_editor = nullptr;
                invalidate_source();
                emit editor_changed();
            })
        };
    }
    state.m_published.reset();
    state.m_unpublished_projection = {};
    hidden_previews().remove(reinterpret_cast<quintptr>(this));
    invalidate_source();
    emit editor_changed();
}

void ScintillaQuick_source_preview::set_detailed_view(bool detailed)
{
    auto& state = *m_state;
    if (state.m_detailed_view == detailed) {
        return;
    }
    state.m_detailed_view = detailed;
    cancel_capture();
    state.m_published.reset();
    state.m_unpublished_projection = {};
    hidden_previews().remove(reinterpret_cast<quintptr>(this));
    setImplicitWidth(detailed ? k_detailed_width : k_compact_width);
    invalidate_source();
    emit detailed_view_changed();
}

void ScintillaQuick_source_preview::set_bookmark_projection(const QVariantMap& projection)
{
    auto& state = *m_state;
    state.m_bookmark_projection = projection;
    const QVariant source = projection.value(QStringLiteral("source"));
    // Accept only a guarded pointer value. A raw QObject* in a retained
    // QVariant could already be dangling before this setter is called.
    state.m_bookmark_source = source.metaType() == QMetaType::fromType<QPointer<ScintillaQuick_item>>()
        ? source.value<QPointer<ScintillaQuick_item>>() : QPointer<ScintillaQuick_item>();
    bool binding_valid = false;
    bool line_valid = false;
    bool marker_valid = false;
    state.m_bookmark_binding = projection.value(QStringLiteral("binding_revision")).toString().toULongLong(&binding_valid);
    state.m_bookmark_line_revision = projection.value(QStringLiteral("line_revision")).toString().toULongLong(&line_valid);
    state.m_bookmark_marker_revision = projection.value(QStringLiteral("marker_revision")).toString().toULongLong(&marker_valid);
    state.m_bookmark_projection_valid = projection.value(QStringLiteral("valid")).toBool() &&
        binding_valid && line_valid && marker_valid;
    state.m_bookmark_lines = projection.value(QStringLiteral("lines")).toList();
    state.refresh_bookmark_binding();
    state.m_bookmark_rows.clear();
    ++state.m_bookmark_revision;
    state.rebuild_bookmarks();
    update();
    emit bookmark_projection_changed();
}

void ScintillaQuick_source_preview::set_bookmark_color(const QColor& color)
{
    if (m_state->m_bookmark_color != color) {
        m_state->m_bookmark_color = color;
        ++m_state->m_bookmark_revision;
        update();
        emit bookmark_color_changed();
    }
}

void ScintillaQuick_source_preview::set_viewport_color(const QColor& color)
{
    if (m_state->m_viewport_color != color) {
        m_state->m_viewport_color = color;
        update();
        emit viewport_color_changed();
    }
}

void ScintillaQuick_source_preview::set_occurrence_rows(const QByteArray& rows)
{
    m_state->m_occurrence_rows = rows.left(8192);
    m_state->rebuild_occurrences();
    emit occurrence_rows_changed();
}

void ScintillaQuick_source_preview::set_occurrence_color(const QColor& color)
{
    if (m_state->m_occurrence_color != color) {
        m_state->m_occurrence_color = color;
        ++m_state->m_occurrence_revision;
        update();
        emit occurrence_color_changed();
    }
}

void ScintillaQuick_source_preview::refresh_style()
{
    ++m_state->m_style_revision;
    ++m_state->m_geometry_revision;
    invalidate_source();
}

void ScintillaQuick_source_preview::cancel_capture()
{
    auto& state = *m_state;
    const bool clear_idle_worker = state.m_worker_has_storage && !state.m_chunk_pending;
    ++*state.m_generation;
    state.m_initialized = false;
    state.m_worker_started = false;
    state.m_timer.stop();
    if (clear_idle_worker) {
        state.m_worker_has_storage = false;
        // The QObject context owns this queued call; no native state crosses
        // to the worker, and another invalidation cannot enqueue another clear.
        QMetaObject::invokeMethod(state.m_worker, [worker = state.m_worker] { worker->clear(); },
            Qt::QueuedConnection);
    }
}

void ScintillaQuick_source_preview::visibility_changed()
{
    auto& state = *m_state;
    const auto cache_key = reinterpret_cast<quintptr>(this);
    if (state.active()) {
        state.m_release_update_requested = false;
        const bool restoring = !state.m_published;
        if (restoring) {
            state.m_published.reset(hidden_previews().take(cache_key));
        }
        if (restoring || !state.published_mapping_is_current()) {
            state.reset_overlays();
        }
        update_viewport();
        if (state.published_image_is_current()) {
            // A resize can return to the published size while another size is
            // still rendering. That superseded result must not replace it.
            cancel_capture();
            update();
            return;
        }
        invalidate_source();
        return;
    }
    cancel_capture();
    state.m_bookmark_timer.stop();
    state.m_occurrence_timer.stop();
    if (!state.m_editor) {
        state.m_published.reset();
        hidden_previews().remove(cache_key);
    }
    else
    if (state.m_published) {
        const int cost = (int)std::max<qsizetype>(k_hidden_entry_minimum_cost,
            state.m_published->image.sizeInBytes() + sizeof(Published_preview));
        hidden_previews().insert(cache_key, state.m_published.release(), cost);
    }
    state.m_unpublished_projection = {};
    // Content dirtiness plus a window update reaches updatePaintNode even for
    // a hidden item. Its old node/texture are deleted on the render thread at
    // the next sync, rather than retained outside the shared CPU-image budget.
    if (state.m_node_may_exist.load() && !state.m_release_update_requested) {
        state.m_release_update_requested = true;
        update();
        if (window()) {
            window()->update();
        }
    }
}

void ScintillaQuick_source_preview::invalidate_source()
{
    auto& state = *m_state;
    if (!state.active()) {
        visibility_changed();
        return;
    }
    cancel_capture();
    const bool navigation_current = state.overlay_source_is_current();
    if (state.m_published && !state.published_mapping_is_current()) {
        // An image with different logical rows must not masquerade as the
        // current click/annotation map. Same-line stale images remain useful.
        state.m_published.reset();
    }
    if (!state.m_published) {
        const int count = std::max(1, (int)state.m_editor->send(SCI_GETLINECOUNT));
        const qreal dpr = window()->effectiveDevicePixelRatio();
        const QSize pixels = image_size(width() - k_bookmark_lane, height(), dpr);
        state.m_unpublished_projection = {
            pixels, native_color(state.m_editor->send(SCI_STYLEGETBACK, STYLE_DEFAULT)),
            count, (qreal)pixels.height() / count
        };
        state.m_unpublished_binding = state.m_editor->native_binding_revision();
        state.m_unpublished_line_revision = state.m_editor->native_line_revision();
        state.m_unpublished_geometry_revision = state.m_geometry_revision;
        state.m_unpublished_item_size = size();
        state.m_unpublished_dpr = dpr;
    }
    state.refresh_bookmark_binding();
    if (!navigation_current) {
        state.reset_overlays();
    }
    // Compatible geometry keeps completed and in-progress annotations intact
    // during ordinary same-line typing; only the source image is debounced.
    if (state.m_detailed_view) {
        state.m_timer.start(20);
    }
    update_viewport();
    update();
}

void ScintillaQuick_source_preview::capture_next_chunk()
{
    auto& state = *m_state;
    if (!state.m_detailed_view || !state.active() || state.m_chunk_pending) {
        return;
    }
    QElapsedTimer slice;
    slice.start();
    if (!state.m_initialized) {
        state.m_binding = state.m_editor->native_binding_revision();
        state.m_content = state.m_editor->native_content_revision();
        state.m_line_revision = state.m_editor->native_line_revision();
        state.m_capture_item_size = size();
        state.m_capture_dpr = window()->effectiveDevicePixelRatio();
        state.m_capture_style_revision = state.m_style_revision;
        state.m_capture_geometry_revision = state.m_geometry_revision;
        state.m_document_length = state.m_editor->send(SCI_GETLENGTH);
        state.m_configuration = {};
        state.m_configuration.line_count = std::max(1, (int)state.m_editor->send(SCI_GETLINECOUNT));
        state.m_configuration.tab_width = std::max(1, (int)state.m_editor->send(SCI_GETTABWIDTH));
        state.m_configuration.pixels = image_size(
            width() - k_bookmark_lane, height(), state.m_capture_dpr);
        state.m_configuration.background = native_color(state.m_editor->send(SCI_STYLEGETBACK, STYLE_DEFAULT));
        state.m_base_font = state.m_editor->property("font").value<QFont>();
        state.m_zoom = state.m_editor->send(SCI_GETZOOM);
        state.m_style_index = 0;
        state.m_next_line = 0;
        state.m_initialized = true;
    }
    if (!state.source_is_current()) {
        invalidate_source();
        return;
    }

    while (state.m_style_index < preview::k_style_count) {
        const int index = state.m_style_index++;
        auto& style = state.m_configuration.styles[index];
        style.font = state.m_base_font;
        const int family_length = state.m_editor->send(SCI_STYLEGETFONT, index);
        if (family_length > 0 && family_length <= 1024) {
            QByteArray family(family_length + 1, '\0');
            state.m_editor->send(SCI_STYLEGETFONT, index, reinterpret_cast<sptr_t>(family.data()));
            style.font.setFamily(QString::fromUtf8(family.constData(), family_length));
        }
        const int hundredths = state.m_editor->send(SCI_STYLEGETSIZEFRACTIONAL, index);
        style.font.setPointSizeF(std::max<qreal>(1, hundredths / 100.0 + state.m_zoom));
        style.font.setBold(state.m_editor->send(SCI_STYLEGETBOLD, index) != 0);
        style.font.setItalic(state.m_editor->send(SCI_STYLEGETITALIC, index) != 0);
        style.foreground = native_color(state.m_editor->send(SCI_STYLEGETFORE, index));
        if (slice.nsecsElapsed() >= k_capture_slice_ns) {
            state.m_timer.start(0);
            return;
        }
    }

    if (!state.m_worker_started) {
        const QFontMetricsF metrics(state.m_configuration.styles[STYLE_DEFAULT].font);
        const qreal column_width = std::max<qreal>(1, metrics.horizontalAdvance(QLatin1Char('M')));
        const qreal natural_advance = metrics.height() * state.m_configuration.pixels.width() /
            (column_width * state.m_configuration.columns);
        state.m_configuration.line_advance = std::min(
            natural_advance, (qreal)state.m_configuration.pixels.height() / state.m_configuration.line_count);
    }

    preview::Chunk chunk;
    chunk.serial = state.m_generation->load();
    chunk.lines.reserve(preview::k_chunk_lines);
    do {
        const sptr_t start = state.m_editor->send(SCI_POSITIONFROMLINE, state.m_next_line);
        const sptr_t end = state.m_next_line + 1 < state.m_configuration.line_count
            ? state.m_editor->send(SCI_POSITIONFROMLINE, state.m_next_line + 1)
            : state.m_document_length;
        const auto count = std::min<sptr_t>(preview::k_prefix_bytes, end - start);
        preview::Line line;
        line.number = state.m_next_line++;
        line.styled_bytes.resize(count * 2 + 2);
        Scintilla::TextRangeFull range{{start, start + count}, line.styled_bytes.data()};
        state.m_editor->send(SCI_GETSTYLEDTEXTFULL, 0, reinterpret_cast<sptr_t>(&range));
        line.styled_bytes.resize(count * 2);
        chunk.lines.append(std::move(line));
    } while (state.m_next_line < state.m_configuration.line_count &&
        chunk.lines.size() < preview::k_chunk_lines && slice.nsecsElapsed() < k_capture_slice_ns);

    chunk.final = state.m_next_line == state.m_configuration.line_count;
    auto configuration = state.m_worker_started
        ? std::optional<preview::Configuration>() : std::optional(state.m_configuration);
    state.m_worker_started = true;
    state.m_worker_has_storage = true;
    state.m_chunk_pending = true;
    QMetaObject::invokeMethod(state.m_worker,
        [
            worker = state.m_worker,
            configuration = std::move(configuration),
            chunk = std::move(chunk)
        ]() mutable
        {
            if (configuration) {
                worker->begin(std::move(*configuration), chunk.serial);
            }
            worker->consume(std::move(chunk));
        }, Qt::QueuedConnection);
}

void ScintillaQuick_source_preview::update_viewport()
{
    auto& state = *m_state;
    if (!state.m_editor || !state.active()) {
        return;
    }
    const int first = state.m_editor->vertical_scroll_value();
    const int page  = std::max(1, state.m_editor->vertical_scroll_page());
    const auto last = std::min<qint64>(
        (qint64)state.m_editor->vertical_scroll_max() + page - 1, (qint64)first + page - 1);
    state.m_first_visible_line = std::max<sptr_t>(0, state.m_editor->send(SCI_DOCLINEFROMVISIBLE, first));
    state.m_last_visible_line = std::max(
        state.m_first_visible_line + 1,
        (qreal)state.m_editor->send(SCI_DOCLINEFROMVISIBLE, last) + 1);
    update();
}

void ScintillaQuick_source_preview::geometryChange(const QRectF& geometry, const QRectF& old_geometry)
{
    QQuickItem::geometryChange(geometry, old_geometry);
    if (geometry.size() != old_geometry.size()) {
        visibility_changed();
    }
}

void ScintillaQuick_source_preview::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged) {
        visibility_changed();
    }
}

void ScintillaQuick_source_preview::request_line(qreal y)
{
    const auto& state = *m_state;
    if (!state.active() || !state.overlay_source_is_current()) {
        return;
    }
    const auto& projection = state.projection();
    const qreal row = std::clamp(y / height(), qreal(0), qreal(1)) * projection.pixels.height();
    const int line = std::clamp(
        (int)(row / projection.line_advance), 0, projection.line_count - 1);
    emit line_requested(line + 1);
}

void ScintillaQuick_source_preview::mousePressEvent(QMouseEvent* event)
{
    request_line(event->position().y());
    event->accept();
}

void ScintillaQuick_source_preview::mouseMoveEvent(QMouseEvent* event)
{
    request_line(event->position().y());
    event->accept();
}

void ScintillaQuick_source_preview::wheelEvent(QWheelEvent* event)
{
    auto* editor = m_state->m_editor.data();
    if (!editor) {
        QQuickItem::wheelEvent(event);
        return;
    }
    QWheelEvent forwarded(
        editor->mapFromItem(this, event->position()),
        event->globalPosition(),
        event->pixelDelta(),
        event->angleDelta(),
        event->buttons(),
        event->modifiers(),
        event->phase(),
        event->inverted(),
        event->source(),
        event->pointingDevice());
    forwarded.setTimestamp(event->timestamp());
    QCoreApplication::sendEvent(editor, &forwarded);
    event->setAccepted(forwarded.isAccepted());
}

QSGNode* ScintillaQuick_source_preview::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*)
{
    auto& state = *m_state;
    const auto& projection = state.projection();
    if (!isVisible() || !window() || width() <= k_bookmark_lane || height() <= 0 ||
        projection.pixels.isEmpty())
    {
        state.m_node_may_exist.store(false);
        delete old_node;
        return nullptr;
    }
    auto* node = static_cast<Preview_node*>(old_node);
    if (!node) {
        node = new Preview_node;
    }
    state.m_node_may_exist.store(true);
    node->m_background->setRect(boundingRect());
    node->m_background->setColor(projection.background);
    if (!state.m_published && node->m_image) {
        node->removeChildNode(node->m_image);
        delete node->m_image;
        node->m_image = nullptr;
        node->m_texture.reset();
        node->m_image_serial = 0;
    }
    if (state.m_published && node->m_image_serial != state.m_published->serial) {
        auto texture = std::unique_ptr<QSGTexture>(window()->createTextureFromImage(state.m_published->image));
        if (!node->m_image) {
            node->m_image = window()->createImageNode();
            node->m_image->setFiltering(QSGTexture::Linear);
            node->m_image->setTexture(texture.get());
            node->insertChildNodeAfter(node->m_image, node->m_background);
        }
        else {
            node->m_image->setTexture(texture.get());
        }
        node->m_texture = std::move(texture);
        node->m_image_serial = state.m_published->serial;
    }
    if (node->m_image) {
        node->m_image->setRect(QRectF(k_bookmark_lane, 0, width() - k_bookmark_lane, height()));
    }
    const qreal logical_pixel = height() / projection.pixels.height();
    const qreal advance = projection.line_advance * logical_pixel;
    const qreal top = std::clamp(state.m_first_visible_line * advance, qreal(0), height());
    const qreal bottom = std::clamp(state.m_last_visible_line * advance, top, height());
    const qreal natural_height = std::max(logical_pixel, bottom - top);
    const qreal viewport_height = std::min(height(), std::max(k_min_viewport_height, natural_height));
    const qreal viewport_top = std::clamp(
        top - (viewport_height - natural_height) / 2, qreal(0), height() - viewport_height);
    const qreal viewport_opacity = 0.4 + 0.3 * std::log2(
        k_viewport_fade_height /
        std::clamp(natural_height, k_min_viewport_height, k_viewport_fade_height));
    QColor viewport_color = state.m_viewport_color;
    viewport_color.setAlphaF(viewport_color.alphaF() * viewport_opacity);
    const bool current = state.overlay_source_is_current();
    const QRectF viewport_rect = current
        ? QRectF(0, viewport_top, width(), viewport_height) : QRectF();
    // Two texels give a one-device-pixel transition at each logical edge.
    // Keep their positions fractional; only the frame and marker rails snap.
    const qreal edge_span = std::min(viewport_rect.height(), 2.0 / window()->effectiveDevicePixelRatio());
    const qreal half_edge = edge_span / 2;
    node->m_viewport->setRect(QRectF(viewport_rect.x(), viewport_rect.y() + half_edge,
        viewport_rect.width(), std::max(qreal(0), viewport_rect.height() - edge_span)));
    node->m_viewport->setColor(viewport_color);
    QBitArray top_rows(current ? 2 : 0);
    QBitArray bottom_rows(current ? 2 : 0);
    if (current) {
        top_rows.setBit(1);
        bottom_rows.setBit(0);
    }
    node->m_viewport_top->update(window(), top_rows, viewport_color,
        QRectF(viewport_rect.x(), viewport_rect.top() - half_edge, viewport_rect.width(), edge_span),
        QSGTexture::Linear);
    node->m_viewport_bottom->update(window(), bottom_rows, viewport_color,
        QRectF(viewport_rect.x(), viewport_rect.bottom() - half_edge, viewport_rect.width(), edge_span),
        QSGTexture::Linear);
    const bool geometry_changed = node->m_geometry_size != size();
    node->m_geometry_size = size();
    const bool bookmarks_current = current && state.bookmark_projection_is_current();
    const auto bookmark_revision = bookmarks_current
        ? state.m_bookmark_revision : std::numeric_limits<quint64>::max();
    if (geometry_changed || node->m_bookmark_revision != bookmark_revision) {
        node->m_bookmarks->update(window(), bookmarks_current ? state.m_bookmark_rows : QBitArray(),
            state.m_bookmark_color, QRectF(0, 0, k_bookmark_lane, height()));
        node->m_bookmark_revision = bookmark_revision;
    }
    const auto occurrence_revision = current
        ? state.m_occurrence_revision : std::numeric_limits<quint64>::max();
    if (geometry_changed || node->m_occurrence_revision != occurrence_revision) {
        node->m_occurrences->update(window(), current ? state.m_occurrence_pixels : QBitArray(),
            state.m_occurrence_color, QRectF(width() - k_bookmark_lane, 0, k_bookmark_lane, height()));
        node->m_occurrence_revision = occurrence_revision;
    }
    return node;
}
