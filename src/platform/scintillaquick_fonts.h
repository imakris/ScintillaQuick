// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QString>
#include <QStringList>

namespace Scintilla::Internal
{

QString configured_fixed_font_family_name();
void ensure_bundled_fonts_loaded();
QString bundled_fixed_font_family();
QStringList bundled_fixed_font_families();

} // namespace Scintilla::Internal
