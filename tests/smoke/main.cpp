#include <scintillaquick/ScintillaQuickItem.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QDebug>
#include <QGuiApplication>

#include "Scintilla.h"

namespace {

bool require(bool condition, const char *message) {
    if (!condition) {
        qCritical("%s", message);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);

    ScintillaQuickItem editor;
    editor.setWidth(640);
    editor.setHeight(480);

    const QString text = QStringLiteral("smoke test\nsecond line");
    editor.setProperty("font", QFont(QStringLiteral("Consolas"), 11));
    editor.setProperty("text", text);
    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);

    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    if (!require(editor.send(SCI_GETTEXTLENGTH) == text.size(), "unexpected text length after property update")) {
        return 1;
    }
    if (!require(editor.width() == 640, "editor width was not applied")) {
        return 1;
    }

    return 0;
}
