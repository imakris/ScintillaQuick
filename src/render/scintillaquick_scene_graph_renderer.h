// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QColor>
#include <QRectF>
#include <QSizeF>
#include <vector>

class QQuickWindow;
class QSGNode;

namespace Scintilla::Internal {

struct render_frame;

struct gutter_band
{
    QRectF rect;
    QColor color;
};

struct render_snapshot
{
    QSizeF item_size;
    QColor background;
    std::vector<gutter_band> gutter_bands;
};

class scene_graph_renderer
{
public:
    QSGNode *update(
        QQuickWindow *window,
        QSGNode *old_node,
        const render_snapshot &snapshot,
        const render_frame &frame);
};

}
