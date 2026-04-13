#pragma once

#include <QColor>
#include <QRectF>
#include <QSizeF>
#include <vector>

class QQuickWindow;
class QSGNode;

namespace Scintilla::Internal {

struct Render_frame;

struct gutter_band_t {
    QRectF rect;
    QColor color;
};

struct render_snapshot_t {
    QSizeF item_size;
    QColor background;
    std::vector<gutter_band_t> gutter_bands;
};

class Scene_graph_renderer
{
public:
    QSGNode *update(
        QQuickWindow *window,
        QSGNode *old_node,
        const render_snapshot_t &snapshot,
        const Render_frame &frame);
};

}
