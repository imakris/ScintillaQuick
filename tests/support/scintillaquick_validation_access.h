#pragma once

#define SCINTILLAQUICK_ENABLE_TEST_ACCESS

#include <scintillaquick/ScintillaQuickItem.h>
#include "ScintillaQuickCore.h"
#include "render_frame.h"

#include <algorithm>
#include <cmath>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>

#undef SCINTILLAQUICK_ENABLE_TEST_ACCESS

namespace Scintilla::Internal {

class scintillaquick_validation_access
{
public:
    static render_frame capture_frame(ScintillaQuickItem &item)
    {
        if (!item.m_core) {
            return {};
        }

        return item.m_core->current_render_frame();
    }

    static render_frame capture_cached_frame(ScintillaQuickItem &item)
    {
        if (!item.m_render_data) {
            return {};
        }

        item.updatePolish();
        return item.rendered_frame_for_test();
    }

    static QImage capture_raster_reference(ScintillaQuickItem &item)
    {
        if (!item.m_core) {
            return {};
        }

        const qreal dpr = item.window()
            ? std::max<qreal>(1.0, item.window()->effectiveDevicePixelRatio())
            : 1.0;
        const qreal logical_width = std::max<qreal>(1.0, std::ceil(item.width()));
        const qreal logical_height = std::max<qreal>(1.0, std::ceil(item.height()));
        const int width = std::max(1, static_cast<int>(std::ceil(logical_width * dpr)));
        const int height = std::max(1, static_cast<int>(std::ceil(logical_height * dpr)));

        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::white);

        QPainter painter(&image);
        item.m_core->PartialPaintQml(PRectangle(0.0, 0.0, logical_width, logical_height), &painter);
        painter.end();

        return image;
    }
};

} // namespace Scintilla::Internal
