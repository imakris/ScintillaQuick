// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include "scintillaquick_fonts.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>

#include <mutex>

void init_scintillaquick_font_resources()
{
    Q_INIT_RESOURCE(scintillaquick_fonts);
}

namespace {

constexpr const char *k_cascadia_code_family = "Cascadia Code";
constexpr const char *k_cousine_family = "Cousine";
constexpr const char *k_fixed_font_family_env_var = "SCINTILLAQUICK_FIXED_FONT_FAMILY";

constexpr const char *k_bundled_font_resources[] = {
    ":/scintillaquick/fonts/CascadiaCode-Regular.ttf",
    ":/scintillaquick/fonts/CascadiaCode-Italic.ttf",
    ":/scintillaquick/fonts/CascadiaCode-Bold.ttf",
    ":/scintillaquick/fonts/CascadiaCode-BoldItalic.ttf",
    ":/scintillaquick/fonts/Cousine-Regular.ttf",
    ":/scintillaquick/fonts/Cousine-Italic.ttf",
    ":/scintillaquick/fonts/Cousine-Bold.ttf",
    ":/scintillaquick/fonts/Cousine-BoldItalic.ttf",
};

std::once_flag g_bundled_fonts_once;
QStringList g_bundled_fixed_font_families;

QString resolved_family_if_available(const QString &family_name)
{
    const QFontInfo info(QFont(family_name, 11));
    if (info.family().compare(family_name, Qt::CaseInsensitive) == 0) {
        return info.family();
    }

    return {};
}

void register_bundled_fonts_once()
{
    init_scintillaquick_font_resources();

    for (const char *resource_path : k_bundled_font_resources) {
        QFontDatabase::addApplicationFont(QString::fromUtf8(resource_path));
    }

    for (const QString &family_name : {
             QString::fromUtf8(k_cascadia_code_family),
             QString::fromUtf8(k_cousine_family),
         }) {
        const QString resolved_family = resolved_family_if_available(family_name);
        if (!resolved_family.isEmpty()) {
            g_bundled_fixed_font_families.push_back(resolved_family);
        }
    }
}

void register_bundled_fonts_at_startup()
{
    Scintilla::Internal::ensure_bundled_fonts_loaded();
}

Q_COREAPP_STARTUP_FUNCTION(register_bundled_fonts_at_startup)

}

namespace Scintilla::Internal {

QString configured_fixed_font_family_name()
{
    const QString configured_family =
        qEnvironmentVariable(k_fixed_font_family_env_var, QString::fromUtf8(k_cascadia_code_family));
    if (!configured_family.isEmpty()) {
        return configured_family;
    }

    return QString::fromUtf8(k_cascadia_code_family);
}

void ensure_bundled_fonts_loaded()
{
    std::call_once(g_bundled_fonts_once, register_bundled_fonts_once);
}

QString bundled_fixed_font_family()
{
    ensure_bundled_fonts_loaded();

    const QString configured_family = configured_fixed_font_family_name();
    const QString resolved_configured_family = resolved_family_if_available(configured_family);
    if (!resolved_configured_family.isEmpty()) {
        return resolved_configured_family;
    }

    if (!g_bundled_fixed_font_families.isEmpty()) {
        return g_bundled_fixed_font_families.front();
    }

    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

QStringList bundled_fixed_font_families()
{
    ensure_bundled_fonts_loaded();
    return g_bundled_fixed_font_families;
}

}
