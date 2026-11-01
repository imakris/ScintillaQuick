// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include "scintillaquick_item.h"

#include <QByteArray>
#include <QColor>
#include <QQuickItem>
#include <QVariantMap>

#include <memory>

/** A bounded, asynchronously rasterized source overview of an existing editor.
 * Native document access stays on the editor thread. Scroll and bookmark
 * overlays do not invalidate the source image. Lines in the public API are
 * one-based logical lines; the host retains its display-line scrollbar.
 */
class SCINTILLAQUICK_EXPORT ScintillaQuick_source_preview : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(ScintillaQuick_item* editor READ editor WRITE set_editor NOTIFY editor_changed)
    Q_PROPERTY(QVariantMap bookmark_projection
               READ bookmark_projection WRITE set_bookmark_projection NOTIFY bookmark_projection_changed)
    Q_PROPERTY(QColor bookmark_color
               READ bookmark_color WRITE set_bookmark_color NOTIFY bookmark_color_changed)
    Q_PROPERTY(QColor viewport_color
               READ viewport_color WRITE set_viewport_color NOTIFY viewport_color_changed)
    Q_PROPERTY(QByteArray occurrence_rows
               READ occurrence_rows WRITE set_occurrence_rows NOTIFY occurrence_rows_changed)
    Q_PROPERTY(QColor occurrence_color
               READ occurrence_color WRITE set_occurrence_color NOTIFY occurrence_color_changed)

public:
    explicit ScintillaQuick_source_preview(QQuickItem* parent = nullptr);
    ~ScintillaQuick_source_preview() override;

    ScintillaQuick_item* editor() const;
    void set_editor(ScintillaQuick_item* editor);
    // Atomic presentation snapshot: lines/valid, a guarded native source, and
    // that same source's binding/line/marker revision strings.
    QVariantMap bookmark_projection() const;
    void set_bookmark_projection(const QVariantMap& projection);
    QColor bookmark_color() const;
    void set_bookmark_color(const QColor& color);
    QColor viewport_color() const;
    void set_viewport_color(const QColor& color);
    // At most 8192 equally sized logical-line buckets; nonzero means a hit.
    QByteArray occurrence_rows() const;
    void set_occurrence_rows(const QByteArray& rows);
    QColor occurrence_color() const;
    void set_occurrence_color(const QColor& color);

    // Native style setters have no common notification. Hosts call this after
    // applying a palette or rebinding the same editor to another document.
    Q_INVOKABLE void refresh_style();

signals:
    void editor_changed();
    void bookmark_projection_changed();
    void bookmark_color_changed();
    void viewport_color_changed();
    void occurrence_rows_changed();
    void occurrence_color_changed();
    void line_requested(int line_number);

protected:
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;

private:
#ifdef SCINTILLAQUICK_ENABLE_TEST_ACCESS
    friend class Scintilla::Internal::ScintillaQuick_validation_access;
#endif
    quint64 published_image_serial_for_test() const;

    class State;
    std::unique_ptr<State> m_state;

    void invalidate_source();
    void visibility_changed();
    void cancel_capture();
    void capture_next_chunk();
    void update_viewport();
    void request_line(qreal y);
};
