// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

// Smoke coverage for ScintillaQuickItem's message pipeline.
//
// These tests do not run a full Qt Quick window — they drive the
// editor entirely through `send()` to exercise Scintilla's message
// dispatch behind the item. The goal is to guard against the most
// common kinds of upstream-Scintilla-update breakage (missed message
// classifications, notifications, document-state regressions) without
// requiring a desktop session.

#include <scintillaquick/ScintillaQuickItem.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QDebug>
#include <QGuiApplication>

#include "Scintilla.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define SQ_EXPECT(expr)                                                           \
    do {                                                                          \
        if (!(expr)) {                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);  \
            ++g_failures;                                                         \
        }                                                                         \
    }                                                                             \
    while (0)

void pump_events()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

int text_length(ScintillaQuickItem &editor)
{
    return static_cast<int>(editor.send(SCI_GETTEXTLENGTH));
}

int current_position(ScintillaQuickItem &editor)
{
    return static_cast<int>(editor.send(SCI_GETCURRENTPOS));
}

int selection_start(ScintillaQuickItem &editor)
{
    return static_cast<int>(editor.send(SCI_GETSELECTIONSTART));
}

int selection_end(ScintillaQuickItem &editor)
{
    return static_cast<int>(editor.send(SCI_GETSELECTIONEND));
}

void test_property_roundtrip(ScintillaQuickItem &editor)
{
    const QString text = QStringLiteral("smoke test\nsecond line");
    editor.setProperty("font", QFont(QStringLiteral("Consolas"), 11));
    editor.setProperty("text", text);
    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    pump_events();

    SQ_EXPECT(text_length(editor) == text.size());
    SQ_EXPECT(editor.width() == 640);
    SQ_EXPECT(editor.property("text").toString() == text);
}

void test_insert_and_delete(ScintillaQuickItem &editor)
{
    editor.setProperty("text", QString());
    SQ_EXPECT(text_length(editor) == 0);

    // Insert "hello world" at position 0.
    const char *hello = "hello world";
    editor.sends(SCI_INSERTTEXT, 0, hello);
    SQ_EXPECT(text_length(editor) == 11);

    // Delete the leading "hello " (6 chars).
    editor.send(SCI_DELETERANGE, 0, 6);
    SQ_EXPECT(text_length(editor) == 5);

    // Append another line.
    editor.send(SCI_SETCURRENTPOS, text_length(editor));
    editor.sends(SCI_APPENDTEXT, 6, "\nmore!");
    SQ_EXPECT(text_length(editor) == 11);
    SQ_EXPECT(editor.send(SCI_GETLINECOUNT) == 2);
}

void test_selection_api(ScintillaQuickItem &editor)
{
    editor.setProperty("text", QStringLiteral("abcdef"));
    editor.send(SCI_SETSEL, 1, 4);
    SQ_EXPECT(selection_start(editor) == 1);
    SQ_EXPECT(selection_end(editor) == 4);
    SQ_EXPECT(current_position(editor) == 4);

    editor.send(SCI_GOTOPOS, 0);
    SQ_EXPECT(current_position(editor) == 0);
    SQ_EXPECT(selection_start(editor) == 0);
    SQ_EXPECT(selection_end(editor) == 0);

    // Select all.
    editor.send(SCI_SELECTALL);
    SQ_EXPECT(selection_start(editor) == 0);
    SQ_EXPECT(selection_end(editor) == 6);
}

void test_undo_redo(ScintillaQuickItem &editor)
{
    editor.setProperty("text", QString());
    editor.send(SCI_EMPTYUNDOBUFFER);

    editor.sends(SCI_INSERTTEXT, 0, "first");
    SQ_EXPECT(text_length(editor) == 5);

    editor.send(SCI_UNDO);
    SQ_EXPECT(text_length(editor) == 0);

    editor.send(SCI_REDO);
    SQ_EXPECT(text_length(editor) == 5);
}

void test_readonly_property(ScintillaQuickItem &editor)
{
    editor.setProperty("text", QStringLiteral("seed"));
    SQ_EXPECT(text_length(editor) == 4);

    editor.setProperty("readonly", true);
    SQ_EXPECT(editor.property("readonly").toBool());

    // Writes through SCI_* should now be rejected. SCI_INSERTTEXT on a
    // read-only document is a no-op in upstream Scintilla.
    editor.sends(SCI_INSERTTEXT, 0, "nope");
    SQ_EXPECT(text_length(editor) == 4);

    editor.setProperty("readonly", false);
    editor.sends(SCI_INSERTTEXT, 0, "yes ");
    SQ_EXPECT(text_length(editor) == 8);
}

void test_wrap_mode_toggle(ScintillaQuickItem &editor)
{
    editor.setProperty("text", QStringLiteral("some text"));
    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    SQ_EXPECT(editor.send(SCI_GETWRAPMODE) == SC_WRAP_NONE);

    editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD);
    SQ_EXPECT(editor.send(SCI_GETWRAPMODE) == SC_WRAP_WORD);

    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    SQ_EXPECT(editor.send(SCI_GETWRAPMODE) == SC_WRAP_NONE);
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    ScintillaQuickItem editor;
    editor.setWidth(640);
    editor.setHeight(480);

    test_property_roundtrip(editor);
    test_insert_and_delete(editor);
    test_selection_api(editor);
    test_undo_redo(editor);
    test_readonly_property(editor);
    test_wrap_mode_toggle(editor);

    pump_events();

    if (g_failures != 0) {
        std::fprintf(stderr, "scintillaquick_smoke_test: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
