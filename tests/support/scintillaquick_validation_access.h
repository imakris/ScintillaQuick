// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#define SCINTILLAQUICK_ENABLE_TEST_ACCESS

#include <scintillaquick/scintillaquick_item.h>
#include "scintillaquick_core.h"
#include "render_frame.h"

#undef SCINTILLAQUICK_ENABLE_TEST_ACCESS

namespace Scintilla::Internal
{

class ScintillaQuick_validation_access
{
public:
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
