// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_item.h>

#include "scintillaquick_test_macros.h"

#include <QClipboard>
#include <QDropEvent>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QObject>

#include "Scintilla.h"

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{

int g_failures = 0;

class Event_editor final : public ScintillaQuick_item
{
public:
    void deliver_drop(QDropEvent* event) { dropEvent(event); }
    void deliver_input_method(QInputMethodEvent* event) { inputMethodEvent(event); }
};

QString text_of(ScintillaQuick_item& editor)
{
    return editor.property("text").toString();
}

ScintillaQuick_edit_result apply_exactly(const ScintillaQuick_edit_transaction& transaction)
{
    const std::vector<ScintillaQuick_edit_replacement> replacements(
        transaction.replacements.begin(),
        transaction.replacements.end());
    return {
        ScintillaQuick_edit_disposition::HANDLED,
        [replacements](ScintillaQuick_item& editor) {
            editor.send(SCI_BEGINUNDOACTION);
            for (const ScintillaQuick_edit_replacement& replacement : replacements) {
                editor.send(
                    SCI_SETTARGETRANGE,
                    static_cast<Scintilla::uptr_t>(replacement.position),
                    replacement.position + replacement.deleted_length);
                editor.send(
                    SCI_REPLACETARGET,
                    static_cast<Scintilla::uptr_t>(replacement.inserted_text.size()),
                    reinterpret_cast<Scintilla::sptr_t>(replacement.inserted_text.constData()));
            }
            editor.send(SCI_ENDUNDOACTION);
            const ScintillaQuick_edit_replacement& replacement = replacements.back();
            const Scintilla::Position caret = replacement.position + replacement.inserted_text.size();
            editor.send(SCI_SETSEL, caret, caret);
        },
    };
}

void test_handler_absent_preserves_direct_edit()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("ac"));
    editor.sends(SCI_INSERTTEXT, 1, "b");
    SQ_EXPECT(text_of(editor) == QStringLiteral("abc"));
}

void test_handler_dispositions_and_reentrant_apply()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("ac"));

    int calls = 0;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            ++calls;
            SQ_EXPECT(transaction.transaction_id != 0);
            SQ_EXPECT(transaction.replacements.size() == 1);
            SQ_EXPECT(transaction.replacements.front().position == 1);
            SQ_EXPECT(transaction.replacements.front().inserted_text == QByteArray("b"));
            return apply_exactly(transaction);
        });
    editor.sends(SCI_INSERTTEXT, 1, "b");
    SQ_EXPECT(calls == 1);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abc"));

    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::REJECTED, {}};
        });
    SQ_EXPECT(editor.sends(SCI_INSERTTEXT, 0, "x") == 0);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abc"));
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);
    editor.send(SCI_SETSTATUS, SC_STATUS_OK);

    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction&) {
            editor.sends(SCI_INSERTTEXT, 0, "forbidden");
            return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::DECLINED, {}};
        });
    editor.sends(SCI_INSERTTEXT, 0, "z");
    SQ_EXPECT(text_of(editor) == QStringLiteral("zabc"));
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);
}

void test_direct_message_result_conventions()
{
    ScintillaQuick_item rejected_editor;
    rejected_editor.setProperty("text", QStringLiteral("abcd"));
    rejected_editor.send(SCI_SETTARGETRANGE, 1, 3);
    rejected_editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::REJECTED, {}};
        });
    const Scintilla::sptr_t rejected = rejected_editor.send(
        SCI_REPLACETARGET,
        1,
        reinterpret_cast<Scintilla::sptr_t>("X"));
    SQ_EXPECT(rejected == 0);
    SQ_EXPECT(text_of(rejected_editor) == QStringLiteral("abcd"));
    SQ_EXPECT(rejected_editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);

    ScintillaQuick_item declined_editor;
    declined_editor.setProperty("text", QStringLiteral("abcd"));
    declined_editor.send(SCI_SETTARGETRANGE, 1, 3);
    declined_editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::DECLINED, {}};
        });
    const Scintilla::sptr_t declined = declined_editor.send(
        SCI_REPLACETARGET,
        1,
        reinterpret_cast<Scintilla::sptr_t>("X"));
    SQ_EXPECT(declined == 1);
    SQ_EXPECT(text_of(declined_editor) == QStringLiteral("aXd"));

    ScintillaQuick_item handled_editor;
    handled_editor.setProperty("text", QStringLiteral("abcd"));
    handled_editor.send(SCI_SETTARGETRANGE, 1, 3);
    handled_editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction& transaction) {
            return apply_exactly(transaction);
        });
    const Scintilla::sptr_t handled = handled_editor.send(
        SCI_REPLACETARGET,
        1,
        reinterpret_cast<Scintilla::sptr_t>("X"));
    SQ_EXPECT(handled == 1);
    SQ_EXPECT(text_of(handled_editor) == QStringLiteral("aXd"));

    handled_editor.send(
        SCI_SETTEXT,
        0,
        reinterpret_cast<Scintilla::sptr_t>("new"));
    SQ_EXPECT(text_of(handled_editor) == QStringLiteral("new"));
}

void test_handler_evaluation_isolation()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abcd"));
    editor.send(SCI_SETSEL, 1, 3);
    editor.send(SCI_SETTARGETRANGE, 1, 2);

    int calls = 0;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction&) {
            ++calls;
            editor.send(SCI_SETTARGETRANGE, 0, 4);
            editor.send(SCI_BEGINUNDOACTION);
            SQ_EXPECT(editor.send(SCI_GETDIRECTPOINTER) == 0);

            QKeyEvent nested_key(
                QEvent::KeyPress,
                Qt::Key_X,
                Qt::NoModifier,
                QStringLiteral("X"));
            QGuiApplication::sendEvent(&editor, &nested_key);
            SQ_EXPECT(nested_key.isAccepted());

            QMouseEvent nested_mouse(
                QEvent::MouseButtonPress,
                QPointF(0.0, 0.0),
                QPointF(0.0, 0.0),
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier);
            QGuiApplication::sendEvent(&editor, &nested_mouse);
            SQ_EXPECT(nested_mouse.isAccepted());

            QInputMethodEvent nested_preedit(QStringLiteral("q"), {});
            QGuiApplication::sendEvent(&editor, &nested_preedit);
            SQ_EXPECT(nested_preedit.isAccepted());
            SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 1);
            SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 3);
            SQ_EXPECT(editor.send(SCI_GETTARGETSTART) == 1);
            SQ_EXPECT(editor.send(SCI_GETTARGETEND) == 2);
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::DECLINED,
                {}};
        });

    const Scintilla::sptr_t replaced = editor.send(
        SCI_REPLACETARGET,
        1,
        reinterpret_cast<Scintilla::sptr_t>("Z"));
    SQ_EXPECT(replaced == 1);
    SQ_EXPECT(calls == 1);
    SQ_EXPECT(text_of(editor) == QStringLiteral("aZcd"));
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);
}

void test_direct_status_reports_handler_rejection()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abcd"));
    editor.send(SCI_SETTARGETRANGE, 1, 3);

    auto direct_status = reinterpret_cast<SciFnDirectStatus>(
        editor.send(SCI_GETDIRECTSTATUSFUNCTION));
    const Scintilla::sptr_t direct_pointer = editor.send(SCI_GETDIRECTPOINTER);
    SQ_EXPECT(direct_status != nullptr);
    SQ_EXPECT(direct_pointer != 0);

    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction&) {
            SQ_EXPECT(editor.send(SCI_GETLENGTH) == 4);
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::REJECTED,
                {}};
        });

    editor.send(SCI_SETSTATUS, SC_STATUS_WARN_REGEX);
    int status = SC_STATUS_OK;
    const Scintilla::sptr_t replaced = direct_status(
        direct_pointer,
        SCI_REPLACETARGET,
        1,
        reinterpret_cast<Scintilla::sptr_t>("X"),
        &status);
    SQ_EXPECT(replaced == 0);
    SQ_EXPECT(status == SC_STATUS_FAILURE);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
}

void test_selection_replacement_and_target_return()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abcd"));
    editor.send(SCI_SETSEL, 1, 3);

    ScintillaQuick_edit_replacement observed;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            observed = transaction.replacements.front();
            return apply_exactly(transaction);
        });
    SQ_EXPECT(editor.send(SCI_REPLACESEL, 0, reinterpret_cast<Scintilla::sptr_t>("XY")) == 0);
    SQ_EXPECT(observed.position == 1);
    SQ_EXPECT(observed.deleted_length == 2);
    SQ_EXPECT(observed.inserted_text == QByteArray("XY"));
    SQ_EXPECT(text_of(editor) == QStringLiteral("aXYd"));
    SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 3);
    SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 3);

    editor.send(SCI_SETTARGETRANGE, 1, 3);
    const Scintilla::sptr_t replaced = editor.send(
        SCI_REPLACETARGET,
        3,
        reinterpret_cast<Scintilla::sptr_t>("uvw"));
    SQ_EXPECT(replaced == 3);
    SQ_EXPECT(text_of(editor) == QStringLiteral("auvwd"));
    SQ_EXPECT(editor.send(SCI_GETTARGETSTART) == 1);
    SQ_EXPECT(editor.send(SCI_GETTARGETEND) == 4);
}

void test_replace_all_transaction_grouping()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("one one one"));
    editor.setFindText(QStringLiteral("one"));
    editor.setReplacementText(QStringLiteral("two"));

    std::vector<std::uint64_t> ids;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            ids.push_back(transaction.transaction_id);
            return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::DECLINED, {}};
        });
    SQ_EXPECT(editor.replaceAll() == 3);
    SQ_EXPECT(text_of(editor) == QStringLiteral("two two two"));
    SQ_EXPECT(ids.size() == 3);
    SQ_EXPECT(ids[0] != 0 && ids[0] == ids[1] && ids[1] == ids[2]);
}

void test_keyboard_overtype_normalization()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abcd"));
    editor.send(SCI_SETSEL, 1, 1);
    editor.send(SCI_SETOVERTYPE, 1);

    ScintillaQuick_edit_replacement observed;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            observed = transaction.replacements.front();
            return apply_exactly(transaction);
        });
    QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("X"));
    QGuiApplication::sendEvent(&editor, &event);
    SQ_EXPECT(event.isAccepted());
    SQ_EXPECT(observed.position == 1);
    SQ_EXPECT(observed.deleted_length == 1);
    SQ_EXPECT(observed.inserted_text == QByteArray("X"));
    SQ_EXPECT(text_of(editor) == QStringLiteral("aXcd"));
}

void test_keyboard_delete_back_not_line_preserves_line_start()
{
    constexpr Scintilla::Position k_line_start = 5;

    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("left\nright"));
    editor.send(SCI_SETSEL, k_line_start, k_line_start);
    editor.send(SCI_ASSIGNCMDKEY, SCK_BACK, SCI_DELETEBACKNOTLINE);

    int handler_calls = 0;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            ++handler_calls;
            return apply_exactly(transaction);
        });

    int key_pressed_count = 0;
    QObject::connect(
        &editor,
        &ScintillaQuick_item::keyPressed,
        [&](QKeyEvent*) { ++key_pressed_count; });

    QKeyEvent event(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QGuiApplication::sendEvent(&editor, &event);

    SQ_EXPECT(event.isAccepted());
    SQ_EXPECT(key_pressed_count == 1);
    SQ_EXPECT(handler_calls == 0);
    SQ_EXPECT(text_of(editor) == QStringLiteral("left\nright"));
    SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == k_line_start);
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_OK);
}

void test_keyboard_delete_boundaries_are_no_ops()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abcd"));

    int handler_calls = 0;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction&) {
            ++handler_calls;
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::DECLINED,
                {}};
        });

    int key_pressed_count = 0;
    QObject::connect(
        &editor,
        &ScintillaQuick_item::keyPressed,
        [&](QKeyEvent*) { ++key_pressed_count; });

    editor.send(SCI_SETSEL, 0, 0);
    QKeyEvent backspace_event(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QGuiApplication::sendEvent(&editor, &backspace_event);
    SQ_EXPECT(backspace_event.isAccepted());
    SQ_EXPECT(key_pressed_count == 1);
    SQ_EXPECT(handler_calls == 0);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
    SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 0);
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_OK);

    editor.send(SCI_SETSEL, 4, 4);
    editor.send(SCI_SETSTATUS, SC_STATUS_OK);
    QKeyEvent delete_event(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QGuiApplication::sendEvent(&editor, &delete_event);
    SQ_EXPECT(delete_event.isAccepted());
    SQ_EXPECT(key_pressed_count == 2);
    SQ_EXPECT(handler_calls == 0);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
    SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 4);
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_OK);
}

void test_keyboard_backspace_unindents_only_blocks_unindent()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("    value"));
    editor.send(SCI_SETSEL, 7, 7);
    editor.send(SCI_SETBACKSPACEUNINDENTS, 1);

    int handler_calls = 0;
    ScintillaQuick_edit_replacement observed;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            ++handler_calls;
            observed = transaction.replacements.front();
            return apply_exactly(transaction);
        });

    int key_pressed_count = 0;
    QObject::connect(
        &editor,
        &ScintillaQuick_item::keyPressed,
        [&](QKeyEvent*) { ++key_pressed_count; });

    QKeyEvent event(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QGuiApplication::sendEvent(&editor, &event);

    SQ_EXPECT(event.isAccepted());
    SQ_EXPECT(key_pressed_count == 1);
    SQ_EXPECT(handler_calls == 1);
    SQ_EXPECT(observed.position == 6);
    SQ_EXPECT(observed.deleted_length == 1);
    SQ_EXPECT(observed.inserted_text.isEmpty());
    SQ_EXPECT(text_of(editor) == QStringLiteral("    vaue"));
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_OK);
}

void test_direct_delete_back_normalization()
{
    constexpr Scintilla::Position k_line_start = 6;

    ScintillaQuick_item handled_editor;
    handled_editor.setProperty("text", QStringLiteral("left\r\nright"));
    handled_editor.send(SCI_SETSEL, k_line_start, k_line_start);

    ScintillaQuick_edit_replacement handled_replacement;
    handled_editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            handled_replacement = transaction.replacements.front();
            return apply_exactly(transaction);
        });
    SQ_EXPECT(handled_editor.send(SCI_DELETEBACK) == 0);
    SQ_EXPECT(handled_replacement.position == 4);
    SQ_EXPECT(handled_replacement.deleted_length == 2);
    SQ_EXPECT(handled_replacement.inserted_text.isEmpty());
    SQ_EXPECT(text_of(handled_editor) == QStringLiteral("leftright"));

    ScintillaQuick_item declined_editor;
    declined_editor.setProperty("text", QStringLiteral("left\r\nright"));
    declined_editor.send(SCI_SETSEL, k_line_start, k_line_start);

    ScintillaQuick_edit_replacement declined_replacement;
    declined_editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            declined_replacement = transaction.replacements.front();
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::DECLINED,
                {}};
        });
    SQ_EXPECT(declined_editor.send(SCI_DELETEBACK) == 0);
    SQ_EXPECT(declined_replacement.position == 4);
    SQ_EXPECT(declined_replacement.deleted_length == 2);
    SQ_EXPECT(declined_replacement.inserted_text.isEmpty());
    SQ_EXPECT(text_of(declined_editor) == QStringLiteral("leftright"));

    ScintillaQuick_item rejected_editor;
    rejected_editor.setProperty("text", QStringLiteral("left\r\nright"));
    rejected_editor.send(SCI_SETSEL, k_line_start, k_line_start);

    ScintillaQuick_edit_replacement rejected_replacement;
    rejected_editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            rejected_replacement = transaction.replacements.front();
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::REJECTED,
                {}};
        });
    SQ_EXPECT(rejected_editor.send(SCI_DELETEBACK) == 0);
    SQ_EXPECT(rejected_replacement.position == 4);
    SQ_EXPECT(rejected_replacement.deleted_length == 2);
    SQ_EXPECT(rejected_replacement.inserted_text.isEmpty());
    SQ_EXPECT(text_of(rejected_editor) == QStringLiteral("left\r\nright"));
    SQ_EXPECT(rejected_editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);

    ScintillaQuick_item native_editor;
    native_editor.setProperty("text", QStringLiteral("left\r\nright"));
    native_editor.send(SCI_SETSEL, k_line_start, k_line_start);
    SQ_EXPECT(native_editor.send(SCI_DELETEBACK) == 0);
    SQ_EXPECT(text_of(native_editor) == QStringLiteral("leftright"));

    ScintillaQuick_item unindent_editor;
    unindent_editor.setProperty("text", QStringLiteral("    value"));
    unindent_editor.send(SCI_SETSEL, 4, 4);
    unindent_editor.send(SCI_SETBACKSPACEUNINDENTS, 1);

    int unindent_handler_calls = 0;
    unindent_editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction&) {
            ++unindent_handler_calls;
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::DECLINED,
                {}};
        });
    SQ_EXPECT(unindent_editor.send(SCI_DELETEBACK) == 0);
    SQ_EXPECT(unindent_handler_calls == 0);
    SQ_EXPECT(text_of(unindent_editor) == QStringLiteral("    value"));
    SQ_EXPECT(unindent_editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);
}

void test_committed_ime_restores_preedit_selection()
{
    Event_editor editor;
    editor.setProperty("text", QStringLiteral("abcd"));
    editor.send(SCI_SETSEL, 1, 3);

    ScintillaQuick_edit_replacement observed;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            observed = transaction.replacements.front();
            return apply_exactly(transaction);
        });

    QInputMethodEvent preedit(QStringLiteral("x"), {});
    editor.deliver_input_method(&preedit);
    SQ_EXPECT(preedit.isAccepted());

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("Y"));
    editor.deliver_input_method(&commit);
    SQ_EXPECT(commit.isAccepted());
    SQ_EXPECT(observed.position == 1);
    SQ_EXPECT(observed.deleted_length == 2);
    SQ_EXPECT(observed.inserted_text == QByteArray("Y"));
    SQ_EXPECT(text_of(editor) == QStringLiteral("aYd"));

    editor.setProperty("text", QStringLiteral("abcd"));
    editor.send(SCI_SETSEL, 1, 3);
    QInputMethodEvent cancelled_preedit(QStringLiteral("q"), {});
    editor.deliver_input_method(&cancelled_preedit);
    QInputMethodEvent cancel;
    editor.deliver_input_method(&cancel);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
    SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 1);
    SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 3);
}

void test_clipboard_and_drop_ingress()
{
    Event_editor editor;
    editor.setProperty("text", QStringLiteral("ac"));
    editor.send(SCI_SETSEL, 1, 1);
    QGuiApplication::clipboard()->setText(QStringLiteral("b"));

    std::vector<ScintillaQuick_edit_replacement> observed;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            observed.push_back(transaction.replacements.front());
            return apply_exactly(transaction);
        });
    editor.send(SCI_PASTE);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abc"));
    SQ_EXPECT(observed.size() == 1);
    SQ_EXPECT(observed.front().position == 1);
    SQ_EXPECT(observed.front().inserted_text == QByteArray("b"));

    editor.send(SCI_SETSEL, 0, 1);
    editor.send(SCI_CUT);
    SQ_EXPECT(text_of(editor) == QStringLiteral("bc"));
    SQ_EXPECT(QGuiApplication::clipboard()->text() == QStringLiteral("a"));

    QMimeData mime_data;
    mime_data.setText(QStringLiteral("z"));
    QDropEvent drop(
        QPointF(0.0, 0.0),
        Qt::CopyAction,
        &mime_data,
        Qt::LeftButton,
        Qt::NoModifier);
    editor.deliver_drop(&drop);
    SQ_EXPECT(drop.isAccepted());
    SQ_EXPECT(observed.size() == 3);
    SQ_EXPECT(observed.back().inserted_text == QByteArray("z"));

    Event_editor eol_editor;
    eol_editor.setProperty("text", QStringLiteral("x"));
    eol_editor.send(SCI_SETEOLMODE, SC_EOL_LF);
    ScintillaQuick_edit_replacement observed_drop;
    eol_editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            observed_drop = transaction.replacements.front();
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::REJECTED,
                {}};
        });
    QMimeData cross_eol_mime_data;
    cross_eol_mime_data.setText(QStringLiteral("a\r\nb"));
    QDropEvent cross_eol_drop(
        QPointF(0.0, 0.0),
        Qt::CopyAction,
        &cross_eol_mime_data,
        Qt::LeftButton,
        Qt::NoModifier);
    eol_editor.deliver_drop(&cross_eol_drop);
    SQ_EXPECT(cross_eol_drop.isAccepted());
    SQ_EXPECT(observed_drop.inserted_text == QByteArray("a\nb"));
    SQ_EXPECT(text_of(eol_editor) == QStringLiteral("x"));
}

void test_unsupported_multi_selection_fails_closed()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abcd"));
    editor.send(SCI_SETMULTIPLESELECTION, 1);
    editor.send(SCI_SETSEL, 0, 1);
    editor.send(SCI_ADDSELECTION, 3, 2);

    int calls = 0;
    editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction&) {
            ++calls;
            return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::DECLINED, {}};
        });
    editor.send(SCI_REPLACESEL, 0, reinterpret_cast<Scintilla::sptr_t>("x"));
    SQ_EXPECT(calls == 0);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    test_handler_absent_preserves_direct_edit();
    test_handler_dispositions_and_reentrant_apply();
    test_direct_message_result_conventions();
    test_handler_evaluation_isolation();
    test_direct_status_reports_handler_rejection();
    test_selection_replacement_and_target_return();
    test_replace_all_transaction_grouping();
    test_keyboard_overtype_normalization();
    test_keyboard_delete_back_not_line_preserves_line_start();
    test_keyboard_delete_boundaries_are_no_ops();
    test_keyboard_backspace_unindents_only_blocks_unindent();
    test_direct_delete_back_normalization();
    test_committed_ime_restores_preedit_selection();
    test_clipboard_and_drop_ingress();
    test_unsupported_multi_selection_fails_closed();

    if (g_failures != 0) {
        std::fprintf(stderr, "scintillaquick_edit_boundary_test: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
