// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <scintillaquick/scintillaquick_item.h>

#include <QEventLoop>
#include <QSGNode>
#include <QTimer>

#include <functional>

// Shared paint-instrumented ScintillaQuick_item used by tests and
// benchmarks that need to observe scene-graph paint boundaries. The
// only purpose of the subclass is to expose a callback fired from
// updatePaintNode so callers can drive an event loop until a paint
// actually happened.
class Paint_counted_editor : public ScintillaQuick_item
{
public:
    using ScintillaQuick_item::ScintillaQuick_item;

    std::function<void()> on_paint_node_updated;

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override
    {
        QSGNode* node = ScintillaQuick_item::updatePaintNode(old_node, data);
        if (on_paint_node_updated) {
            on_paint_node_updated();
        }
        return node;
    }
};

// Pump the Qt event loop until the editor reports a new paint or the
// timeout expires. The caller owns paint_counter and is expected to
// increment it from on_paint_node_updated (typically wired up in the
// fixture's constructor). Returns whether a paint actually happened.
inline bool wait_for_next_paint(
    Paint_counted_editor& editor,
    quint64&              paint_counter,
    quint64               previous_paint_counter,
    int                   timeout_ms = 50)
{
    if (paint_counter > previous_paint_counter) {
        return true;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto previous_callback = editor.on_paint_node_updated;
    editor.on_paint_node_updated = [&]() {
        if (previous_callback) {
            previous_callback();
        }
        if (paint_counter > previous_paint_counter) {
            loop.quit();
        }
    };

    timeout.start(timeout_ms);
    loop.exec();
    editor.on_paint_node_updated = previous_callback;
    return paint_counter > previous_paint_counter;
}
