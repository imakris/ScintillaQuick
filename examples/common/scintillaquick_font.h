// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QStringList>

namespace scintillaquick::shared
{

enum class Bundled_test_font_family
{
    CascadiaCode,
    Cousine,
};

inline Bundled_test_font_family configured_test_font_family_kind()
{
    QString family_name = QString::fromUtf8(qgetenv("SCINTILLAQUICK_FIXED_FONT_FAMILY")).trimmed();
    if (family_name.isEmpty()) {
        family_name = QString::fromUtf8(qgetenv("SCINTILLAQUICK_TEST_FONT_FAMILY")).trimmed();
    }
    if (family_name.compare(QStringLiteral("Cascadia Code"), Qt::CaseInsensitive) == 0 ||
        family_name.compare(QStringLiteral("CascadiaCode"), Qt::CaseInsensitive) == 0)
    {
        return Bundled_test_font_family::CascadiaCode;
    }

    if (family_name.compare(QStringLiteral("Cousine"), Qt::CaseInsensitive) == 0) {
        return Bundled_test_font_family::Cousine;
    }

    return Bundled_test_font_family::CascadiaCode;
}

inline QString bundled_test_font_family_name(Bundled_test_font_family family)
{
    switch (family) {
        case Bundled_test_font_family::CascadiaCode: return QStringLiteral("Cascadia Code");
        case Bundled_test_font_family::Cousine:      return QStringLiteral("Cousine");
        default:                                     return QStringLiteral("Cascadia Code");
    }
}

inline QString bundled_test_font_directory_name(Bundled_test_font_family family)
{
    switch (family) {
        case Bundled_test_font_family::CascadiaCode: return QStringLiteral("CascadiaCode");
        case Bundled_test_font_family::Cousine:      return QStringLiteral("Cousine");
        default:                                     return QStringLiteral("CascadiaCode");
    }
}

inline QStringList bundled_test_font_file_names(Bundled_test_font_family family)
{
    const QString base_name = bundled_test_font_directory_name(family);
    return {
        base_name + QStringLiteral("-Regular.ttf"),
        base_name + QStringLiteral("-Italic.ttf"),
        base_name + QStringLiteral("-Bold.ttf"),
        base_name + QStringLiteral("-BoldItalic.ttf"),
    };
}

inline QStringList bundled_test_font_search_roots()
{
    QStringList roots;

    const auto add_root = [&](const QString& path) {
        const QString clean_path = QDir::cleanPath(path);
        if (!clean_path.isEmpty() && !roots.contains(clean_path, Qt::CaseInsensitive)) {
            roots.push_back(clean_path);
        }
    };

    const auto add_ancestor_chain = [&](const QString& seed_path) {
        QDir directory(seed_path);
        for (int i = 0; i < 5; ++i) {
            add_root(directory.absolutePath());
            if (!directory.cdUp()) {
                break;
            }
        }
    };

    add_ancestor_chain(QCoreApplication::applicationDirPath());
    add_ancestor_chain(QDir::currentPath());
    return roots;
}

inline QString locate_bundled_test_font_file(Bundled_test_font_family family, const QString& file_name)
{
    const QString relative_path =
        QStringLiteral("fonts/%1/%2").arg(bundled_test_font_directory_name(family)).arg(file_name);

    for (const QString& root : bundled_test_font_search_roots()) {
        const QString candidate = QDir(root).filePath(relative_path);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

inline bool load_bundled_test_font_family(Bundled_test_font_family family, QString* error_message = nullptr)
{
    QStringList failures;
    const QStringList files = bundled_test_font_file_names(family);
    for (const QString& file_name : files) {
        const QString path = locate_bundled_test_font_file(family, file_name);
        if (path.isEmpty()) {
            failures.push_back(
                QStringLiteral("%1/%2 not found").arg(bundled_test_font_directory_name(family)).arg(file_name));
            continue;
        }

        const int font_id = QFontDatabase::addApplicationFont(path);
        if (font_id < 0) {
            failures.push_back(QStringLiteral("%1/%2 could not be loaded from %3")
                .arg(bundled_test_font_directory_name(family))
                .arg(file_name)
                .arg(path));
        }
    }

    if (!failures.isEmpty()) {
        if (error_message) {
            *error_message = failures.join(QStringLiteral("; "));
        }
        return false;
    }

    return true;
}

inline bool ensure_bundled_test_fonts_loaded(QString* error_message = nullptr)
{
    static bool initialized = false;
    static bool success = false;
    static QString cached_error;

    if (!initialized) {
        initialized = true;

        QStringList failures;
        QString error;
        if (!load_bundled_test_font_family(Bundled_test_font_family::CascadiaCode, &error)) {
            failures.push_back(error);
        }
        if (!load_bundled_test_font_family(Bundled_test_font_family::Cousine, &error)) {
            failures.push_back(error);
        }

        success = failures.isEmpty();
        if (!success) {
            cached_error = failures.join(QStringLiteral("; "));
        }
    }

    if (!success && error_message) {
        *error_message = cached_error;
    }

    return success;
}

inline QString deterministic_test_font_family()
{
    return bundled_test_font_family_name(configured_test_font_family_kind());
}

inline QByteArray deterministic_test_font_family_utf8()
{
    return deterministic_test_font_family().toUtf8();
}

inline QFont deterministic_test_font(int point_size = 11)
{
    return QFont(deterministic_test_font_family(), point_size);
}

} // namespace scintillaquick::shared
