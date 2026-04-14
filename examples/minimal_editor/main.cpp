// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/ScintillaQuickItem.h>

#include <QFont>
#include <QGuiApplication>
#include <QQuickWindow>

#include "ScintillaQuickWindowBinding.h"
#include "Scintilla.h"

namespace {

QString sampleText() {
    QStringList lines;
    lines.reserve(200);
    lines << "// ScintillaQuick minimal editor";
    lines << "";
    for (int i = 0; i < 197; ++i) {
        lines << QString("Line %1: the editor is running inside a QQuickWindow.")
                     .arg(i + 1, 3, 10, QChar('0'));
    }
    return lines.join('\n');
}

}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);

    QQuickWindow window;
    window.setTitle(QStringLiteral("ScintillaQuick Minimal Editor"));
    window.resize(1100, 720);
    window.setColor(Qt::white);

    ScintillaQuickItem editor;
    scintillaquick::examples::bindItemToWindow(editor, window);

    editor.setProperty("font", QFont(QStringLiteral("Consolas"), 11));
    editor.setProperty("text", sampleText());
    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    editor.send(SCI_STYLECLEARALL);

    window.show();
    editor.forceActiveFocus();

    return app.exec();
}
