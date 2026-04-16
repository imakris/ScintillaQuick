// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_item.h>

#include <QGuiApplication>
#include <QQuickWindow>

#include "scintillaquick_font.h"
#include "scintillaquick_window_binding.h"
#include "Scintilla.h"

namespace
{

QString sample_text()
{
    QStringList lines;
    lines.reserve(200);
    lines << "// ScintillaQuick minimal editor";
    lines << "";
    for (int i = 0; i < 197; ++i) {
        lines << QString("Line %1: the editor is running inside a QQuickWindow.").arg(i + 1, 3, 10, QChar('0'));
    }
    return lines.join('\n');
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QString font_error;
    if (!scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error)) {
        qFatal("%s", qPrintable(font_error));
    }

    QQuickWindow window;
    window.setTitle(QStringLiteral("ScintillaQuick Minimal Editor"));
    window.resize(1100, 720);
    window.setColor(Qt::white);

    ScintillaQuick_item editor;
    scintillaquick::examples::bind_item_to_window(editor, window);

    editor.setProperty("font", scintillaquick::shared::deterministic_test_font(11));
    editor.setProperty("text", sample_text());
    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    editor.send(SCI_STYLECLEARALL);

    window.show();
    editor.forceActiveFocus();

    return app.exec();
}
