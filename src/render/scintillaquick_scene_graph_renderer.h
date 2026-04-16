// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QColor>
#include <QRectF>
#include <QSizeF>
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
};

struct Render_snapshot
{
    QSizeF item_size;
    QColor background;
    std::vector<Gutter_band> gutter_bands;
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
