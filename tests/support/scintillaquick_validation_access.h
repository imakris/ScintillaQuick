// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#define SCINTILLAQUICK_ENABLE_TEST_ACCESS

#include <scintillaquick/scintillaquick_item.h>
#include "scintillaquick_core.h"
#include "render_frame.h"
#include <QTimerEvent>

#undef SCINTILLAQUICK_ENABLE_TEST_ACCESS

namespace Scintilla::Internal
{

class ScintillaQuick_validation_access
{
public:
    static void run_idle(ScintillaQuick_item& item) { item.m_core->onIdle(); }

    static void complete_drag(ScintillaQuick_item& item, Qt::DropAction action, bool outside)
    {
        item.m_core->dropWentOutside = outside;
        item.m_core->complete_drag(action);
    }

    static void tick_caret(ScintillaQuick_item& item)
    {
        QTimerEvent event(item.m_core->timers[static_cast<size_t>(Editor::TickReason::caret)]);
        item.m_core->timerEvent(&event);
    }

    static void drag_selection(ScintillaQuick_item& item, Sci::Position caret, Sci::Position anchor)
    {
        item.m_core->SetSelection(SelectionPosition(caret), SelectionPosition(anchor));
        item.cursorChangedUpdateMarker();
    }

    static Render_frame capture_frame(ScintillaQuick_item& item)
    {
        if (!item.m_core) {
            return {};
        }

        return item.m_core->current_render_frame();
    }

    static Render_frame capture_cached_frame(ScintillaQuick_item& item)
    {
        if (!item.m_render_data) {
            return {};
        }

        item.updatePolish();
        return item.rendered_frame_for_test();
    }
};

} // namespace Scintilla::Internal
