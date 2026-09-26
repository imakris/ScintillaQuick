// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QColor>
#include <QRectF>
#include <QSizeF>
#include <cstdint>
#include <vector>

class QQuickWindow;
class QSGNode;

namespace Scintilla::Internal
{

struct Render_frame;

struct Gutter_band
{
    QRectF rect;
    QColor color;
    QColor pattern_color;
    int pattern_phase = 0;

    friend bool operator==(const Gutter_band&, const Gutter_band&) = default;
};

struct Render_snapshot
{
    QSizeF item_size;
    QColor background;
    std::vector<Gutter_band> gutter_bands;
    // Zero denotes an unversioned caller; positive revisions identify static
    // snapshots and remain unchanged for selection/caret-only updates.
    std::uint64_t static_revision = 0;
};

class Scene_graph_renderer
{
public:
    QSGNode* update(
        QQuickWindow*           window,
        QSGNode*                old_node,
        const Render_snapshot&  snapshot,
        const Render_frame&     frame);
};

} // namespace Scintilla::Internal
