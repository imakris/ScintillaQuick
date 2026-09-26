// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#ifndef SCINTILLAQUICK_SCINTILLAQUICK_SYNTAX_H
#define SCINTILLAQUICK_SCINTILLAQUICK_SYNTAX_H

#include <QString>

class ScintillaQuick_item;

namespace scintillaquick
{

// Reset lexical foreground, bold, italic and keyword sets without replacing the
// lexer or changing the host's font, backgrounds, margins or predefined styles.
void reset_syntax_styles(ScintillaQuick_item& pane);

// Select the default lexer, keywords and colors for path. Unknown extensions
// select plain text. Call once with the final path/theme before host overrides.
void configure_default_syntax(ScintillaQuick_item& pane, const QString& path, bool dark_mode);

}

#endif
