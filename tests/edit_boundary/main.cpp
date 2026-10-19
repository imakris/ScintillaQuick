// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#include <scintillaquick/scintillaquick_item.h>

#include "scintillaquick_test_macros.h"

#include <QClipboard>
#include <QDropEvent>
#include <QGuiApplication>
#include <QEventLoop>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QObject>

#include "Scintilla.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace
{

int g_failures = 0;

class Event_editor final : public ScintillaQuick_item
{
public:
    void capture() { updatePolish(); }
    void deliver_drop(QDropEvent* event) { dropEvent(event); }
    void deliver_input_method(QInputMethodEvent* event) { inputMethodEvent(event); }
    QVariant query_input_method(Qt::InputMethodQuery query) const { return inputMethodQuery(query); }
    QVariant query_input_method(Qt::InputMethodQuery query, QVariant argument) const
    {
        return inputMethodQuery(query, argument);
    }
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
            // Deletions retain the selection translated by Scintilla. An
            // insertion places the model-owned caret after the inserted text.
            if (replacement.inserted_text.isEmpty()) {
                return;
            }
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

void test_handler_snapshot_and_transaction_cleanup()
{
    ScintillaQuick_item self_clearing_editor;
    self_clearing_editor.setProperty("text", QStringLiteral("a"));
    auto state = std::make_shared<int>(1001);
    self_clearing_editor.set_edit_handler(
        [&self_clearing_editor, state](const ScintillaQuick_edit_transaction&) {
            self_clearing_editor.set_edit_handler({});
            SQ_EXPECT(*state == 1001);
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::DECLINED,
                {}};
        });
    state.reset();
    self_clearing_editor.sends(SCI_INSERTTEXT, 1, "b");
    SQ_EXPECT(text_of(self_clearing_editor) == QStringLiteral("ab"));

    ScintillaQuick_item transaction_editor;
    transaction_editor.setProperty("text", QStringLiteral("a"));
    transaction_editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::DECLINED,
                {}};
        });
    transaction_editor.send(SCI_BEGINUNDOACTION);
    transaction_editor.set_edit_handler({});
    transaction_editor.send(SCI_ENDUNDOACTION);

    std::vector<std::uint64_t> transaction_ids;
    transaction_editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            transaction_ids.push_back(transaction.transaction_id);
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::DECLINED,
                {}};
        });
    transaction_editor.sends(SCI_INSERTTEXT, 1, "b");
    transaction_editor.sends(SCI_INSERTTEXT, 2, "c");
    SQ_EXPECT(transaction_ids.size() == 2);
    if (transaction_ids.size() == 2) {
        SQ_EXPECT(transaction_ids[0] != transaction_ids[1]);
    }
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

struct Ime_edits
{
    int calls = 0;
    std::vector<ScintillaQuick_edit_replacement> replacements;
};

void observe_ime_edits(Event_editor& editor, Ime_edits& observed)
{
    editor.set_edit_handler(
        [&observed](const ScintillaQuick_edit_transaction& transaction) {
            ++observed.calls;
            observed.replacements.assign(transaction.replacements.begin(), transaction.replacements.end());
            return apply_exactly(transaction);
        });
}

void expect_ime_context(
    const Event_editor& editor,
    const QString& surrounding,
    int cursor,
    int anchor,
    int absolute,
    const QString& selection = {})
{
    SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition).toInt() == cursor);
    SQ_EXPECT(editor.query_input_method(Qt::ImAnchorPosition).toInt() == anchor);
    SQ_EXPECT(editor.query_input_method(Qt::ImAbsolutePosition).toInt() == absolute);
    SQ_EXPECT(editor.query_input_method(Qt::ImSurroundingText).toString() == surrounding);
    SQ_EXPECT(editor.query_input_method(Qt::ImTextBeforeCursor).toString() == surrounding.left(cursor));
    SQ_EXPECT(editor.query_input_method(Qt::ImTextAfterCursor).toString() == surrounding.mid(cursor));
    SQ_EXPECT(editor.query_input_method(Qt::ImCurrentSelection).toString() == selection);
}

void test_ime_queries_exclude_preedit()
{
    // Qt's logical queries exclude preedit, including its Cursor offset.
    // Newlines and overtype must not change the committed paragraph either.
    const QString surrounding = QString::fromUcs4(U"\u00E9\U0001F600xyz");
    const QString committed   = QStringLiteral("prefix\n\n") + surrounding;
    const QString preedits[] = {
        QString::fromUcs4(U"\U0001F600Y"),
        QStringLiteral("x\n\nY"),
        QString::fromUcs4(U"\uAC00Y"),
    };
    for (const bool handled : {false, true}) {
        for (const bool overtype : {false, true}) {
            for (const QString& preedit : preedits) {
                Event_editor editor;
                editor.setWidth(640);
                editor.setHeight(240);
                editor.setProperty("text", committed);
                editor.send(SCI_SETSEL,      14, 14);
                editor.send(SCI_SETOVERTYPE, overtype);
                if (handled) {
                    editor.set_edit_handler(apply_exactly);
                }
                for (const int cursor : {0, (int)preedit.size()}) {
                    QInputMethodEvent composing(preedit, {
                        {QInputMethodEvent::Cursor, cursor, 1, {}},
                    });
                    editor.deliver_input_method(&composing);
                    expect_ime_context(editor, surrounding, 3, 3, 11);

                    const auto visual_caret = editor.send(SCI_GETCURRENTPOS);
                    const QRectF rectangle  = editor.query_input_method(Qt::ImCursorRectangle).toRectF();
                    SQ_EXPECT(qAbs(rectangle.x() - editor.send(SCI_POINTXFROMPOSITION, 0, visual_caret)) <= 1);
                    SQ_EXPECT(rectangle.y() == editor.send(SCI_POINTYFROMPOSITION, 0, visual_caret));
                    SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition, rectangle.center()).toInt() == 3);
                }
                QInputMethodEvent cancel;
                editor.deliver_input_method(&cancel);
                SQ_EXPECT(text_of(editor) == committed);
                expect_ime_context(editor, surrounding, 3, 3, 11);
            }
        }
    }
}

void test_ime_point_queries_exclude_preedit()
{
    Event_editor editor;
    editor.setWidth(640);
    editor.setHeight(240);
    editor.setProperty("text", QString::fromUcs4(U"\u00E9\U0001F600xy"));
    editor.send(SCI_SETSEL, 6, 6);
    QInputMethodEvent composing(QString::fromUcs4(U"\U0001F600Y"), {
        {QInputMethodEvent::Cursor, 2, 1, {}},
    });
    editor.deliver_input_method(&composing);

    const std::pair<Scintilla::Position, int> positions[] = {
        {2, 1}, {6, 3}, {10, 3}, {11, 3}, {12, 4}, {13, 5},
    };
    for (const auto& [byte_position, logical_position] : positions) {
        const QPointF point(
            editor.send(SCI_POINTXFROMPOSITION, 0, byte_position),
            editor.send(SCI_POINTYFROMPOSITION, 0, byte_position) + 1);
        SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition, point).toInt() == logical_position);
    }
}

void test_ime_queries_with_multiple_carets()
{
    Event_editor editor;
    editor.setProperty("text", QStringLiteral("ab cd"));
    editor.send(SCI_SETMULTIPLESELECTION,         1);
    editor.send(SCI_SETADDITIONALSELECTIONTYPING, 1);
    editor.send(SCI_SETSEL,                      1, 1);
    editor.send(SCI_ADDSELECTION,                4, 4);
    editor.send(SCI_SETMAINSELECTION,            1);
    QInputMethodEvent composing(QStringLiteral("XY"), {
        {QInputMethodEvent::Cursor, 1, 1, {}},
    });
    editor.deliver_input_method(&composing);
    SQ_EXPECT(text_of(editor) == QStringLiteral("aXYb cXYd"));
    expect_ime_context(editor, QStringLiteral("ab cd"), 4, 4, 4);

    QInputMethodEvent cancel;
    editor.deliver_input_method(&cancel);
    SQ_EXPECT(text_of(editor) == QStringLiteral("ab cd"));
    expect_ime_context(editor, QStringLiteral("ab cd"), 4, 4, 4);
}

void test_ime_queries_with_adjacent_overtype_carets()
{
    for (const int secondary : {-1, 0, 1}) {
        for (const int cursor : {0, 1}) {
            Event_editor editor;
            editor.setWidth(640);
            editor.setHeight(240);
            editor.setProperty("text", QStringLiteral("abcd"));
            editor.send(SCI_SETOVERTYPE, 1);
            if (secondary < 0) {
                editor.send(SCI_SETSEL, 2, 2);
            }
            else {
                editor.send(SCI_SETMULTIPLESELECTION,         1);
                editor.send(SCI_SETADDITIONALSELECTIONTYPING, 1);
                editor.send(SCI_SETSEL,                      secondary, secondary);
                editor.send(SCI_ADDSELECTION,                2, 2);
                editor.send(SCI_SETMAINSELECTION,            1);
            }
            QInputMethodEvent composing(QStringLiteral("X"), {
                {QInputMethodEvent::Cursor, cursor, 1, {}},
            });
            editor.deliver_input_method(&composing);
            expect_ime_context(editor, QStringLiteral("abcd"), 2, 2, 2);

            // Both ends of the main preedit map to its logical origin,
            // even when the start also ends a secondary overtype preedit.
            for (const Scintilla::Position position : {2, 3}) {
                const QPointF point(
                    editor.send(SCI_POINTXFROMPOSITION, 0, position),
                    editor.send(SCI_POINTYFROMPOSITION, 0, position) + 1);
                SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition, point).toInt() == 2);
            }
            const QRectF caret = editor.query_input_method(Qt::ImCursorRectangle).toRectF();
            SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition, caret.center()).toInt() == 2);
            const QPointF end_point(
                editor.send(SCI_POINTXFROMPOSITION, 0, 4),
                editor.send(SCI_POINTYFROMPOSITION, 0, 4) + 1);
            SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition, end_point).toInt() == 4);

            QInputMethodEvent cancel;
            editor.deliver_input_method(&cancel);
            SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
        }
    }
}

void test_ime_replacement_ranges()
{
    struct Replacement_case
    {
        QString text;
        Scintilla::Position caret;
        QString commit;
        int start;
        int length;
        QString expected;
        Scintilla::Position byte_position;
        Scintilla::Position deleted_bytes;
    };
    const Replacement_case cases[] = {
        {QStringLiteral("teh"), 3, QStringLiteral("the"), -3, 3, QStringLiteral("the"), 0, 3},
        {QStringLiteral("abc"), 3, {}, -1, 1, QStringLiteral("ab"), 2, 1},
        {QStringLiteral("ac"), 0, QStringLiteral("b"), 1, 0, QStringLiteral("abc"), 1, 0},
        {QStringLiteral("abcd"), 2, QStringLiteral("Z"), -1, 2, QStringLiteral("aZd"), 1, 2},
        {QString::fromUcs4(U"\u00E9\U0001F600x"), 6, QStringLiteral("Z"), -2, 2,
            QString::fromUcs4(U"\u00E9Zx"), 2, 4},
        {QString::fromUcs4(U"\u00E9\U0001F600x"), 2, QStringLiteral("q"), 2, 1,
            QString::fromUcs4(U"\u00E9\U0001F600q"), 6, 1},
        {QString::fromUcs4(U"t\u00E9"), 3, QString::fromUcs4(U"\U0001F600"), -1, 1,
            QString::fromUcs4(U"t\U0001F600"), 1, 2},
    };
    // Qt measures replacement offsets in UTF-16 and excludes the old preedit,
    // even when the IME caret is in the middle of that tentative text.
    for (const bool handled : {false, true}) {
        for (const bool composing : {false, true}) {
            for (const Replacement_case& test : cases) {
                Event_editor editor;
                editor.setProperty("text", test.text);
                editor.send(SCI_SETSEL, test.caret, test.caret);
                Ime_edits observed;
                if (handled) {
                    observe_ime_edits(editor, observed);
                }
                if (composing) {
                    QInputMethodEvent preedit(QStringLiteral("candidate"), {
                        {QInputMethodEvent::Cursor, 2, 1, {}},
                    });
                    editor.deliver_input_method(&preedit);
                }

                QInputMethodEvent commit;
                commit.setCommitString(test.commit, test.start, test.length);
                editor.deliver_input_method(&commit);
                SQ_EXPECT(commit.isAccepted());
                SQ_EXPECT(text_of(editor) == test.expected);
                if (handled) {
                    SQ_EXPECT(observed.calls == 1);
                    SQ_EXPECT(observed.replacements.size() == 1);
                    if (observed.replacements.size() == 1) {
                        const auto& replacement = observed.replacements.front();
                        SQ_EXPECT(replacement.position == test.byte_position);
                        SQ_EXPECT(replacement.deleted_length == test.deleted_bytes);
                        SQ_EXPECT(replacement.inserted_text == test.commit.toUtf8());
                    }
                }

                editor.set_edit_handler({});
                editor.send(SCI_UNDO);
                SQ_EXPECT(text_of(editor) == test.text);
                editor.send(SCI_REDO);
                SQ_EXPECT(text_of(editor) == test.expected);
            }
        }
    }
}

void test_ime_selection_and_replacement_are_one_transaction()
{
    for (const bool handled : {false, true}) {
        for (const bool backward_selection : {false, true}) {
            Event_editor editor;
            editor.setProperty("text", QStringLiteral("abcdef"));
            editor.send(SCI_SETSEL, backward_selection ? 4 : 2, backward_selection ? 2 : 4);
            Ime_edits observed;
            if (handled) {
                editor.set_edit_handler(
                    [&](const ScintillaQuick_edit_transaction& transaction) {
                        // Selection removal and replacement must both wait for
                        // the model's decision, with ordered post-edit offsets.
                        SQ_EXPECT(text_of(editor) == QStringLiteral("abcdef"));
                        SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 2);
                        SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 4);
                        ++observed.calls;
                        observed.replacements.assign(
                            transaction.replacements.begin(),
                            transaction.replacements.end());
                        return apply_exactly(transaction);
                    });
            }
            QInputMethodEvent commit;
            commit.setCommitString(QStringLiteral("X"), -1, 2);
            editor.deliver_input_method(&commit);
            SQ_EXPECT(text_of(editor) == QStringLiteral("aXf"));
            if (handled) {
                SQ_EXPECT(observed.calls == 1);
                SQ_EXPECT(observed.replacements.size() == 2);
                if (observed.replacements.size() == 2) {
                    SQ_EXPECT(observed.replacements[0].position == 2);
                    SQ_EXPECT(observed.replacements[0].deleted_length == 2);
                    SQ_EXPECT(observed.replacements[0].inserted_text.isEmpty());
                    SQ_EXPECT(observed.replacements[1].position == 1);
                    SQ_EXPECT(observed.replacements[1].deleted_length == 2);
                    SQ_EXPECT(observed.replacements[1].inserted_text == QByteArray("X"));
                }
            }
            editor.set_edit_handler({});
            editor.send(SCI_UNDO);
            SQ_EXPECT(text_of(editor) == QStringLiteral("abcdef"));
        }
    }
}

void test_ime_commit_continues_preedit()
{
    const QString next_preedit = QString::fromUcs4(U"\U0001F600Y");
    for (const bool handled : {false, true}) {
        for (const bool composing : {false, true}) {
            Event_editor editor;
            editor.setProperty("text", QStringLiteral("A"));
            editor.send(SCI_SETSEL, 1, 1);
            Ime_edits observed;
            if (handled) {
                observe_ime_edits(editor, observed);
            }
            if (composing) {
                QInputMethodEvent preedit(QStringLiteral("old"), {});
                editor.deliver_input_method(&preedit);
            }
            QInputMethodEvent continued(next_preedit, {
                {QInputMethodEvent::Cursor, 2, 1, {}},
            });
            continued.setCommitString(QStringLiteral("B"));
            editor.deliver_input_method(&continued);
            SQ_EXPECT(continued.isAccepted());
            SQ_EXPECT(text_of(editor) == QStringLiteral("AB") + next_preedit);
            SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 6);
            expect_ime_context(editor, QStringLiteral("AB"), 2, 2, 2);
            if (handled) {
                SQ_EXPECT(observed.calls == 1);
                SQ_EXPECT(observed.replacements.size() == 1);
                if (observed.replacements.size() == 1) {
                    SQ_EXPECT(observed.replacements.front().inserted_text == QByteArray("B"));
                }
            }

            QInputMethodEvent cancel;
            editor.deliver_input_method(&cancel);
            SQ_EXPECT(text_of(editor) == QStringLiteral("AB"));
            SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 2);
            editor.set_edit_handler({});
            editor.send(SCI_UNDO);
            SQ_EXPECT(text_of(editor) == QStringLiteral("A"));
            editor.send(SCI_REDO);
            SQ_EXPECT(text_of(editor) == QStringLiteral("AB"));
        }
    }
}

void test_ime_replacement_continues_preedit()
{
    for (const bool handled : {false, true}) {
        for (const bool delete_only : {false, true}) {
            Event_editor editor;
            editor.setProperty("text", QStringLiteral("teh!"));
            editor.send(SCI_SETSEL, 3, 3);
            Ime_edits observed;
            if (handled) {
                observe_ime_edits(editor, observed);
            }
            QInputMethodEvent continued(QStringLiteral("next"), {});
            continued.setCommitString(
                delete_only ? QString() : QStringLiteral("the"),
                delete_only ? -1 : -3,
                delete_only ? 1 : 3);
            editor.deliver_input_method(&continued);
            SQ_EXPECT(text_of(editor) == (delete_only ? QStringLiteral("tenext!") : QStringLiteral("thenext!")));

            QInputMethodEvent cancel;
            editor.deliver_input_method(&cancel);
            SQ_EXPECT(text_of(editor) == (delete_only ? QStringLiteral("te!") : QStringLiteral("the!")));
            editor.set_edit_handler({});
            editor.send(SCI_UNDO);
            SQ_EXPECT(text_of(editor) == QStringLiteral("teh!"));
        }
    }
}

void test_ime_selection_attribute_follows_commit()
{
    struct Selection_case
    {
        int start;
        int length;
        QString composing_text;
    };
    for (const bool handled : {false, true}) {
        for (const bool unicode : {false, true}) {
            const QString commit_text = unicode
                ? QString::fromUcs4(U"\U0001F600")
                : QStringLiteral("B");
            const int commit_length      = commit_text.size();
            const QString committed_text = QStringLiteral("a") + commit_text + QStringLiteral("c");
            const Selection_case cases[] = {
                {2 + commit_length, 0, committed_text + QStringLiteral("Y")},
                {1, commit_length, QStringLiteral("a") + commit_text + QStringLiteral("Yc")},
                {1 + commit_length, -commit_length, QStringLiteral("aY") + commit_text + QStringLiteral("c")},
            };
            for (const Selection_case& test : cases) {
                for (const bool change_preedit : {false, true}) {
                    Event_editor editor;
                    editor.setProperty("text", QStringLiteral("ac"));
                    editor.send(SCI_SETSEL, 1, 1);
                    editor.send(SCI_EMPTYUNDOBUFFER);
                    Ime_edits observed;
                    if (handled) {
                        observe_ime_edits(editor, observed);
                    }
                    QInputMethodEvent continued(QStringLiteral("Y"), {
                        {QInputMethodEvent::Selection, test.start, test.length, {}},
                        {QInputMethodEvent::Cursor, 1, 1, {}},
                    });
                    continued.setCommitString(commit_text);
                    editor.deliver_input_method(&continued);
                    SQ_EXPECT(text_of(editor) == test.composing_text);
                    const int logical_caret = test.start + test.length;
                    const QString selected  = test.length != 0 ? commit_text : QString();
                    expect_ime_context(
                        editor, committed_text, logical_caret, test.start, logical_caret, selected);

                    // Updating only the preedit cursor must not consume the
                    // surrounding selection established by the previous event.
                    QInputMethodEvent unchanged(QStringLiteral("Y"), {
                        {QInputMethodEvent::Cursor, 0, 1, {}},
                    });
                    editor.deliver_input_method(&unchanged);
                    SQ_EXPECT(text_of(editor) == test.composing_text);
                    expect_ime_context(
                        editor, committed_text, logical_caret, test.start, logical_caret, selected);
                    if (handled) {
                        SQ_EXPECT(observed.calls == 1);
                    }

                    const bool has_selection = test.length != 0;
                    if (change_preedit) {
                        QInputMethodEvent changed(QStringLiteral("Z"), {
                            {QInputMethodEvent::Cursor, 1, 1, {}},
                        });
                        editor.deliver_input_method(&changed);
                        SQ_EXPECT(text_of(editor) == (has_selection
                            ? QStringLiteral("aZc")
                            : committed_text + QStringLiteral("Z")));
                    }

                    // A changed/cancelled preedit removes the still-active
                    // surrounding selection as its own committed edit.
                    QInputMethodEvent cancel;
                    editor.deliver_input_method(&cancel);
                    SQ_EXPECT(text_of(editor) == (has_selection ? QStringLiteral("ac") : committed_text));
                    const int caret = has_selection ? 1 : test.start;
                    SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition).toInt() == caret);
                    SQ_EXPECT(editor.query_input_method(Qt::ImAnchorPosition).toInt() == caret);
                    if (handled) {
                        SQ_EXPECT(observed.calls == (has_selection ? 2 : 1));
                        if (has_selection) {
                            SQ_EXPECT(observed.replacements.size() == 1);
                            SQ_EXPECT(observed.replacements.front().position == 1);
                            SQ_EXPECT(observed.replacements.front().deleted_length == commit_text.toUtf8().size());
                            SQ_EXPECT(observed.replacements.front().inserted_text.isEmpty());
                        }
                    }
                    editor.set_edit_handler({});
                    if (has_selection) {
                        editor.send(SCI_UNDO);
                        SQ_EXPECT(text_of(editor) == committed_text);
                        SQ_EXPECT(editor.send(SCI_CANUNDO) != 0);
                    }
                    editor.send(SCI_UNDO);
                    SQ_EXPECT(text_of(editor) == QStringLiteral("ac"));
                    SQ_EXPECT(editor.send(SCI_CANUNDO) == 0);
                    editor.send(SCI_REDO);
                    SQ_EXPECT(text_of(editor) == committed_text);
                    if (has_selection) {
                        editor.send(SCI_REDO);
                        SQ_EXPECT(text_of(editor) == QStringLiteral("ac"));
                    }
                }
            }
        }
    }
}

void test_ime_surrounding_selection_handler_dispositions()
{
    for (const bool backward : {false, true}) {
        for (const auto disposition : {
                ScintillaQuick_edit_disposition::DECLINED,
                ScintillaQuick_edit_disposition::REJECTED,
                ScintillaQuick_edit_disposition::HANDLED})
        {
            Event_editor editor;
            editor.setProperty("text", QStringLiteral("ac"));
            editor.send(SCI_SETSEL, 1, 1);
            editor.send(SCI_EMPTYUNDOBUFFER);
            editor.set_edit_handler(apply_exactly);
            QInputMethodEvent continued(QStringLiteral("Y"), {
                {QInputMethodEvent::Selection, backward ? 2 : 1, backward ? -1 : 1, {}},
                {QInputMethodEvent::Cursor, 1, 1, {}},
            });
            continued.setCommitString(QStringLiteral("B"));
            editor.deliver_input_method(&continued);
            SQ_EXPECT(text_of(editor) == (backward ? QStringLiteral("aYBc") : QStringLiteral("aBYc")));

            int calls = 0;
            editor.set_edit_handler(
                [&](const ScintillaQuick_edit_transaction& transaction) {
                    ++calls;
                    SQ_EXPECT(text_of(editor) == QStringLiteral("aBc"));
                    SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == (backward ? 1 : 2));
                    SQ_EXPECT(editor.send(SCI_GETANCHOR) == (backward ? 2 : 1));
                    SQ_EXPECT(transaction.replacements.size() == 1);
                    SQ_EXPECT(transaction.replacements.front().position == 1);
                    SQ_EXPECT(transaction.replacements.front().deleted_length == 1);
                    SQ_EXPECT(transaction.replacements.front().inserted_text.isEmpty());
                    return ScintillaQuick_edit_result{disposition, {}};
                });
            QInputMethodEvent cancel;
            editor.deliver_input_method(&cancel);
            SQ_EXPECT(cancel.isAccepted());
            SQ_EXPECT(calls == 1);
            if (disposition == ScintillaQuick_edit_disposition::DECLINED) {
                SQ_EXPECT(text_of(editor) == QStringLiteral("ac"));
                SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 1);
                SQ_EXPECT(editor.send(SCI_GETANCHOR) == 1);
            }
            else {
                // A rejection or external HANDLED result does not authorize
                // native deletion of the committed surrounding text.
                SQ_EXPECT(text_of(editor) == QStringLiteral("aBc"));
                SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == (backward ? 1 : 2));
                SQ_EXPECT(editor.send(SCI_GETANCHOR) == (backward ? 2 : 1));
            }
            SQ_EXPECT(editor.send(SCI_GETSTATUS) ==
                (disposition == ScintillaQuick_edit_disposition::REJECTED ? SC_STATUS_FAILURE : SC_STATUS_OK));
            editor.set_edit_handler({});
            editor.send(SCI_UNDO);
            SQ_EXPECT(text_of(editor) == (disposition == ScintillaQuick_edit_disposition::DECLINED
                ? QStringLiteral("aBc")
                : QStringLiteral("ac")));
        }
    }
}

void test_ime_changed_preedit_consumes_selection_before_new_selection()
{
    // Qt consumes the previous surrounding selection before applying a new
    // Selection attribute. Both ranges therefore address different documents.
    for (const int handler_mode : {0, 1, 2}) {
        for (const bool unicode : {false, true}) {
            Event_editor editor;
            editor.setProperty("text", QStringLiteral("AB"));
            editor.send(SCI_SETSEL, 2, 2);
            editor.send(SCI_EMPTYUNDOBUFFER);
            int calls = 0;
            if (handler_mode != 0) {
                editor.set_edit_handler(
                    [&](const ScintillaQuick_edit_transaction& transaction) {
                        ++calls;
                        if (handler_mode == 1) {
                            return apply_exactly(transaction);
                        }
                        return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::DECLINED, {}};
                    });
            }
            const QString first_preedit = unicode ? QString::fromUcs4(U"\u00E9") : QStringLiteral("n");
            const QString next_preedit  = unicode ? QString::fromUcs4(U"\u03BB") : QStringLiteral("nn");
            QInputMethodEvent first(first_preedit, {{QInputMethodEvent::Selection, 0, 1, {}}});
            first.setCommitString(QStringLiteral("C"));
            QCoreApplication::sendEvent(&editor, &first);
            SQ_EXPECT(text_of(editor) == QStringLiteral("A") + first_preedit + QStringLiteral("BC"));

            QInputMethodEvent next(next_preedit, {{QInputMethodEvent::Selection, 1, 0, {}}});
            QCoreApplication::sendEvent(&editor, &next);
            SQ_EXPECT(text_of(editor) == QStringLiteral("B") + next_preedit + QStringLiteral("C"));
            SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 1);
            SQ_EXPECT(calls == (handler_mode == 0 ? 0 : 2));

            QInputMethodEvent cancel;
            QCoreApplication::sendEvent(&editor, &cancel);
            SQ_EXPECT(text_of(editor) == QStringLiteral("BC"));
            SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 1);
            editor.set_edit_handler({});
            editor.send(SCI_UNDO);
            SQ_EXPECT(text_of(editor) == QStringLiteral("ABC"));
            editor.send(SCI_UNDO);
            SQ_EXPECT(text_of(editor) == QStringLiteral("AB"));
            SQ_EXPECT(editor.send(SCI_CANUNDO) == 0);
        }
    }
}

void test_ime_deletion_preserves_composition_origin()
{
    struct Deletion_case
    {
        QString text;
        Scintilla::Position caret;
        int start;
        int length;
        QString committed_text;
        QString composing_text;
        Scintilla::Position origin;
    };
    const Deletion_case cases[] = {
        {QStringLiteral("abcdef"), 6, -4, 1, QStringLiteral("abdef"), QStringLiteral("abdefY"), 5},
        {QStringLiteral("abcdef"), 1, 2, 1, QStringLiteral("abcef"), QStringLiteral("aYbcef"), 1},
        {QString::fromUcs4(U"\u00E9\U0001F600abc"), 9, -5, 2,
            QString::fromUcs4(U"\u00E9abc"), QString::fromUcs4(U"\u00E9abcY"), 5},
    };
    // QWidgetLineControl (Qt 6.12) restores the translated original cursor for
    // an empty commit. It does not retain the deletion cursor as the IME origin.
    for (const bool handled : {false, true}) {
        for (const bool composing : {false, true}) {
            for (const Deletion_case& test : cases) {
                Event_editor editor;
                editor.setProperty("text", test.text);
                editor.send(SCI_SETSEL, test.caret, test.caret);
                Ime_edits observed;
                if (handled) {
                    // The model callback owns the surviving composition origin.
                    observe_ime_edits(editor, observed);
                }
                if (composing) {
                    QInputMethodEvent previous(QStringLiteral("old"), {});
                    editor.deliver_input_method(&previous);
                }
                QInputMethodEvent continued(QStringLiteral("Y"), {
                    {QInputMethodEvent::Cursor, 1, 1, {}},
                });
                continued.setCommitString({}, test.start, test.length);
                editor.deliver_input_method(&continued);
                SQ_EXPECT(text_of(editor) == test.composing_text);
                SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == test.origin + 1);

                QInputMethodEvent cancel;
                editor.deliver_input_method(&cancel);
                SQ_EXPECT(text_of(editor) == test.committed_text);
                SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == test.origin);
                if (handled) {
                    SQ_EXPECT(observed.calls == 1);
                }
                editor.set_edit_handler({});
                editor.send(SCI_UNDO);
                SQ_EXPECT(text_of(editor) == test.text);
            }
        }
    }

    Event_editor externally_handled;
    externally_handled.setProperty("text", QStringLiteral("abcdef"));
    externally_handled.send(SCI_SETSEL, 6, 6);
    externally_handled.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::HANDLED, {}};
        });
    QInputMethodEvent external_commit;
    external_commit.setCommitString({}, -4, 1);
    externally_handled.deliver_input_method(&external_commit);
    SQ_EXPECT(text_of(externally_handled) == QStringLiteral("abcdef"));
    SQ_EXPECT(externally_handled.send(SCI_GETCURRENTPOS) == 6);
}

void test_ime_handled_deletion_preserves_projected_selection()
{
    // A merge pane retains an alignment row after deleting a source line, so
    // its callback maps the surviving caret into a different byte layout.
    Event_editor editor;
    editor.setProperty("text", QStringLiteral("A\nB\nC"));
    editor.send(SCI_SETSEL, 5, 5);
    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction& transaction) {
            SQ_EXPECT(transaction.replacements.size() == 1);
            const auto& replacement = transaction.replacements.front();
            SQ_EXPECT(replacement.position == 2);
            SQ_EXPECT(replacement.deleted_length == 2);
            SQ_EXPECT(replacement.inserted_text.isEmpty());
            return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::HANDLED,
                [](ScintillaQuick_item& target) {
                    target.setProperty("text", QStringLiteral("A\n\nC"));
                    target.send(SCI_SETSEL, 4, 4);
                }};
        });
    QInputMethodEvent deletion(QStringLiteral("Y"), {});
    deletion.setCommitString({}, -3, 2);
    QCoreApplication::sendEvent(&editor, &deletion);
    SQ_EXPECT(text_of(editor) == QStringLiteral("A\n\nCY"));
    SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 4);

    QInputMethodEvent cancel;
    QCoreApplication::sendEvent(&editor, &cancel);
    SQ_EXPECT(text_of(editor) == QStringLiteral("A\n\nC"));
    SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 4);
}

void test_ime_selection_attribute_preserves_direction()
{
    for (const bool handled : {false, true}) {
        for (const bool unicode : {false, true}) {
            for (const bool backward : {false, true}) {
                Event_editor editor;
                editor.setProperty("text", unicode
                    ? QString::fromUcs4(U"\u00E9\U0001F600x")
                    : QStringLiteral("abcd"));
                Ime_edits observed;
                if (handled) {
                    observe_ime_edits(editor, observed);
                }
                QInputMethodEvent selection({}, {
                    {QInputMethodEvent::Selection, backward ? 3 : 1, backward ? -2 : 2, {}},
                });
                editor.deliver_input_method(&selection);
                SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition).toInt() == (backward ? 1 : 3));
                SQ_EXPECT(editor.query_input_method(Qt::ImAnchorPosition).toInt() == (backward ? 3 : 1));
                SQ_EXPECT(observed.calls == 0);
            }
        }
    }
}

void test_ime_explicit_replacement_handler_dispositions()
{
    for (const auto disposition : {
            ScintillaQuick_edit_disposition::DECLINED,
            ScintillaQuick_edit_disposition::REJECTED})
    {
        Event_editor editor;
        editor.setProperty("text", QStringLiteral("abcdef"));
        editor.send(SCI_SETSEL, 2, 4);
        editor.send(SCI_SETOVERTYPE, 1);
        int calls = 0;
        editor.set_edit_handler(
            [&](const ScintillaQuick_edit_transaction& transaction) {
                ++calls;
                SQ_EXPECT(text_of(editor) == QStringLiteral("abcdef"));
                SQ_EXPECT(transaction.replacements.size() == 2);
                return ScintillaQuick_edit_result{disposition, {}};
            });
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("X"), -1, 2);
        editor.deliver_input_method(&commit);
        SQ_EXPECT(commit.isAccepted());
        SQ_EXPECT(calls == 1);
        SQ_EXPECT(editor.send(SCI_GETOVERTYPE) == 1);
        if (disposition == ScintillaQuick_edit_disposition::REJECTED) {
            SQ_EXPECT(text_of(editor) == QStringLiteral("abcdef"));
            SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 2);
            SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 4);
            SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);
        }
        else {
            SQ_EXPECT(text_of(editor) == QStringLiteral("aXf"));
            editor.set_edit_handler({});
            editor.send(SCI_UNDO);
            SQ_EXPECT(text_of(editor) == QStringLiteral("abcdef"));
        }
    }
}

void test_ime_replacement_respects_readonly_and_protected_text()
{
    for (const bool handled : {false, true}) {
        for (const bool readonly : {false, true}) {
            Event_editor editor;
            editor.setProperty("text", QStringLiteral("abcd"));
            editor.send(SCI_SETSEL, 3, 3);
            if (readonly) {
                editor.setProperty("readonly", true);
            }
            else {
                // The replacement reaches protected text outside the caret's
                // empty selection, so checking only the selection is insufficient.
                editor.send(SCI_STYLESETCHANGEABLE, 1, 0);
                editor.send(SCI_STARTSTYLING, 1);
                editor.send(SCI_SETSTYLING, 1, 1);
                editor.query_input_method(Qt::ImFont);
            }
            Ime_edits observed;
            if (handled) {
                observe_ime_edits(editor, observed);
            }
            QInputMethodEvent commit;
            commit.setCommitString(QStringLiteral("X"), -2, 1);
            editor.deliver_input_method(&commit);
            SQ_EXPECT(commit.isAccepted());
            SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
            SQ_EXPECT(observed.calls == 0);
            if (!readonly) {
                SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);
            }
        }
    }
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
    SQ_EXPECT(!cross_eol_drop.isAccepted());
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

void test_native_edit_commands_obey_edit_handler_boundary()
{
    // The edit handler owns all document changes, including native commands
    // that cannot be represented by a single replacement transaction.
    for (const unsigned int command : {SCI_LINEINDENT, SCI_LINEDEDENT, SCI_CUTALLOWLINE}) {
        ScintillaQuick_item editor;
        const QString text = QStringLiteral("    first line\n    second line");
        editor.setProperty("text", text);
        editor.send(SCI_SETSEL, 6, 6);
        const auto revision = editor.native_content_revision();
        editor.set_edit_handler(
            [](const ScintillaQuick_edit_transaction&) {
                return ScintillaQuick_edit_result{ScintillaQuick_edit_disposition::DECLINED, {}};
            });
        editor.send(command);
        SQ_EXPECT(text_of(editor) == text);
        SQ_EXPECT(editor.native_content_revision() == revision);
        SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);
    }
}

void test_anchor_queries_track_selection_anchor()
{
    Event_editor forward_editor;
    forward_editor.setProperty("text", QStringLiteral("abcdefgh"));
    forward_editor.send(SCI_SETSEL, 2, 6); // anchor 2, caret 6
    SQ_EXPECT(forward_editor.query_input_method(Qt::ImCursorPosition).toInt() == 6);
    SQ_EXPECT(forward_editor.query_input_method(Qt::ImAnchorPosition).toInt() == 2);

    Event_editor backward_editor;
    backward_editor.setProperty("text", QStringLiteral("abcdefgh"));
    backward_editor.send(SCI_SETSEL, 6, 2); // anchor 6, caret 2
    SQ_EXPECT(backward_editor.query_input_method(Qt::ImCursorPosition).toInt() == 2);
    SQ_EXPECT(backward_editor.query_input_method(Qt::ImAnchorPosition).toInt() == 6);
}

void test_supplementary_plane_key_text_inserts()
{
    const QString emoji = QString::fromUcs4(U"\U0001F600");

    ScintillaQuick_item native_editor;
    native_editor.setProperty("text", QStringLiteral("a"));
    native_editor.send(SCI_SETSEL, 1, 1);
    QKeyEvent native_event(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, emoji);
    QGuiApplication::sendEvent(&native_editor, &native_event);
    SQ_EXPECT(native_event.isAccepted());
    SQ_EXPECT(text_of(native_editor) == QStringLiteral("a") + emoji);

    ScintillaQuick_item handled_editor;
    handled_editor.setProperty("text", QStringLiteral("a"));
    handled_editor.send(SCI_SETSEL, 1, 1);
    ScintillaQuick_edit_replacement observed;
    handled_editor.set_edit_handler(
        [&](const ScintillaQuick_edit_transaction& transaction) {
            observed = transaction.replacements.front();
            return apply_exactly(transaction);
        });
    QKeyEvent handled_event(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, emoji);
    QGuiApplication::sendEvent(&handled_editor, &handled_event);
    SQ_EXPECT(handled_event.isAccepted());
    SQ_EXPECT(observed.inserted_text == emoji.toUtf8());
    SQ_EXPECT(text_of(handled_editor) == QStringLiteral("a") + emoji);
}

void test_text_changed_notification_coalescing()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abc"));

    int text_changed_count = 0;
    QObject::connect(
        &editor,
        &ScintillaQuick_item::textChanged,
        [&] { ++text_changed_count; });

    // setText produces delete+insert (two SCN_MODIFIED) but must emit once.
    editor.setProperty("text", QStringLiteral("xyz"));
    SQ_EXPECT(text_changed_count == 1);

    // A direct mutation through send() emits once.
    editor.sends(SCI_INSERTTEXT, 3, "!");
    SQ_EXPECT(text_of(editor) == QStringLiteral("xyz!"));
    SQ_EXPECT(text_changed_count == 2);

    // Read-only queries must not emit.
    editor.send(SCI_GETTEXTLENGTH);
    editor.send(SCI_GETCURRENTPOS);
    SQ_EXPECT(text_changed_count == 2);

    // A rejected edit must not emit.
    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::REJECTED,
                {}};
        });
    editor.sends(SCI_INSERTTEXT, 4, "q");
    SQ_EXPECT(text_of(editor) == QStringLiteral("xyz!"));
    SQ_EXPECT(text_changed_count == 2);
}

void test_rejected_settext_skips_post_actions()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abc"));
    editor.sends(SCI_INSERTTEXT, 3, "d");
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));

    int text_changed_count = 0;
    QObject::connect(
        &editor,
        &ScintillaQuick_item::textChanged,
        [&] { ++text_changed_count; });

    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::REJECTED,
                {}};
        });
    editor.setProperty("text", QStringLiteral("xyz"));
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
    SQ_EXPECT(text_changed_count == 0);
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == SC_STATUS_FAILURE);

    // The rejected setText must not have emptied the undo history.
    editor.set_edit_handler({});
    editor.send(SCI_UNDO);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abc"));
}

void test_find_replace_edit_boundary_outcomes()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("foo bar foo"));
    editor.setProperty("findText", QStringLiteral("foo"));
    editor.setProperty("replacementText", QStringLiteral("baz"));
    SQ_EXPECT(editor.findNext());

    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::REJECTED,
                {}};
        });
    SQ_EXPECT(!editor.replaceSelection());
    SQ_EXPECT(text_of(editor) == QStringLiteral("foo bar foo"));
    // A rejected replacement must not advance past the current match.
    SQ_EXPECT(!editor.replaceAndFind());
    SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 0);
    SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 3);

    // HANDLED without an apply callback: claimed externally, local document
    // and target state unchanged.
    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::HANDLED,
                {}};
        });
    SQ_EXPECT(editor.replaceSelection());
    SQ_EXPECT(text_of(editor) == QStringLiteral("foo bar foo"));
    SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 0);
    SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 3);

    // HANDLED with an apply callback: local document changes.
    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction& transaction) {
            return apply_exactly(transaction);
        });
    SQ_EXPECT(editor.replaceSelection());
    SQ_EXPECT(text_of(editor) == QStringLiteral("baz bar foo"));

    // replaceAll stops after an externally handled replacement instead of
    // advancing on stale target state.
    ScintillaQuick_item external_editor;
    external_editor.setProperty("text", QStringLiteral("foo foo foo"));
    external_editor.setProperty("findText", QStringLiteral("foo"));
    external_editor.setProperty("replacementText", QStringLiteral("baz"));
    external_editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::HANDLED,
                {}};
        });
    SQ_EXPECT(external_editor.replaceAll() == 1);
    SQ_EXPECT(text_of(external_editor) == QStringLiteral("foo foo foo"));

    // replaceAll stops on rejection.
    ScintillaQuick_item rejected_editor;
    rejected_editor.setProperty("text", QStringLiteral("foo foo foo"));
    rejected_editor.setProperty("findText", QStringLiteral("foo"));
    rejected_editor.setProperty("replacementText", QStringLiteral("baz"));
    rejected_editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::REJECTED,
                {}};
        });
    SQ_EXPECT(rejected_editor.replaceAll() == 0);
    SQ_EXPECT(text_of(rejected_editor) == QStringLiteral("foo foo foo"));

    // Native replaceAll still replaces every match.
    ScintillaQuick_item native_editor;
    native_editor.setProperty("text", QStringLiteral("foo foo foo"));
    native_editor.setProperty("findText", QStringLiteral("foo"));
    native_editor.setProperty("replacementText", QStringLiteral("baz"));
    SQ_EXPECT(native_editor.replaceAll() == 3);
    SQ_EXPECT(text_of(native_editor) == QStringLiteral("baz baz baz"));
}

void test_native_paste_requires_text_clipboard()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("selected text"));
    editor.send(SCI_EMPTYUNDOBUFFER);
    editor.send(SCI_SETSAVEPOINT);
    editor.send(SCI_SETSEL, 8, 0);

    auto* image_mime = new QMimeData;
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::red);
    image_mime->setImageData(image);
    SQ_EXPECT(!image_mime->hasText());
    QGuiApplication::clipboard()->setMimeData(image_mime);

    SQ_EXPECT(editor.send(SCI_CANPASTE) == 0);
    editor.send(SCI_PASTE);
    QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    QGuiApplication::sendEvent(&editor, &paste);
    SQ_EXPECT(text_of(editor) == QStringLiteral("selected text"));
    SQ_EXPECT(editor.send(SCI_GETANCHOR) == 8);
    SQ_EXPECT(editor.send(SCI_GETCURRENTPOS) == 0);
    SQ_EXPECT(editor.send(SCI_CANUNDO) == 0);
    SQ_EXPECT(editor.send(SCI_GETMODIFY) == 0);

    QGuiApplication::clipboard()->setText(QStringLiteral("pasted"));
    SQ_EXPECT(editor.send(SCI_CANPASTE) != 0);
    editor.send(SCI_PASTE);
    SQ_EXPECT(text_of(editor) == QStringLiteral("pasted text"));
    editor.send(SCI_UNDO);
    SQ_EXPECT(text_of(editor) == QStringLiteral("selected text"));
    editor.setProperty("readonly", true);
    SQ_EXPECT(editor.send(SCI_CANPASTE) == 0);
}

void test_regex_selection_keeps_document_context()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("CD"));
    editor.setFindOptions(SCFIND_MATCHCASE | SCFIND_REGEXP | SCFIND_CXX11REGEX);
    editor.setFindText(QStringLiteral("(C)(?=D)"));
    editor.setReplacementText(QStringLiteral("\\1X"));
    SQ_EXPECT(editor.findNext());
    SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 0);
    SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 1);
    SQ_EXPECT(editor.replaceSelection());
    SQ_EXPECT(text_of(editor) == QStringLiteral("CXD"));

    editor.setProperty("text", QStringLiteral("CD"));
    editor.setFindText(QStringLiteral("C(?!D)"));
    editor.setReplacementText(QStringLiteral("X"));
    editor.send(SCI_SETSEL, 0, 1);
    SQ_EXPECT(!editor.replaceSelection());
    SQ_EXPECT(text_of(editor) == QStringLiteral("CD"));

    editor.setFindText(QStringLiteral("C$"));
    editor.send(SCI_SETSEL, 0, 1);
    SQ_EXPECT(!editor.replaceSelection());
    SQ_EXPECT(text_of(editor) == QStringLiteral("CD"));

    editor.setFindText(QStringLiteral("C.*"));
    editor.send(SCI_SETSEL, 0, 1);
    SQ_EXPECT(!editor.replaceSelection());
    SQ_EXPECT(text_of(editor) == QStringLiteral("CD"));
}

void test_readonly_notification_covers_direct_messages()
{
    ScintillaQuick_item editor;

    int readonly_changed_count = 0;
    QObject::connect(
        &editor,
        &ScintillaQuick_item::readonlyChanged,
        [&] { ++readonly_changed_count; });

    // A direct message bypassing the property setter must still notify.
    editor.send(SCI_SETREADONLY, 1);
    SQ_EXPECT(readonly_changed_count == 1);
    SQ_EXPECT(editor.property("readonly").toBool());

    // Re-applying the same value must not notify.
    editor.send(SCI_SETREADONLY, 1);
    SQ_EXPECT(readonly_changed_count == 1);

    // The property setter notifies exactly once.
    editor.setProperty("readonly", false);
    SQ_EXPECT(readonly_changed_count == 2);
    SQ_EXPECT(!editor.property("readonly").toBool());
}

void test_transaction_tracking_survives_unhandled_nested_undo_groups()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("a"));

    std::vector<std::uint64_t> transaction_ids;
    ScintillaQuick_edit_handler handler =
        [&](const ScintillaQuick_edit_transaction& transaction) {
            transaction_ids.push_back(transaction.transaction_id);
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::DECLINED,
                {}};
        };

    editor.set_edit_handler(handler);
    editor.send(SCI_BEGINUNDOACTION);
    editor.sends(SCI_INSERTTEXT, 1, "b");

    // An untracked nested group while the handler is temporarily absent must
    // not make the still-open outer group lose its transaction identity.
    editor.set_edit_handler({});
    editor.send(SCI_BEGINUNDOACTION);
    editor.send(SCI_ENDUNDOACTION);

    editor.set_edit_handler(handler);
    editor.sends(SCI_INSERTTEXT, 2, "c");
    editor.send(SCI_ENDUNDOACTION);

    SQ_EXPECT(text_of(editor) == QStringLiteral("abc"));
    SQ_EXPECT(transaction_ids.size() == 2);
    if (transaction_ids.size() == 2) {
        SQ_EXPECT(transaction_ids[0] == transaction_ids[1]);
    }
}

void test_native_readonly_settext_preserves_undo()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abc"));
    editor.sends(SCI_INSERTTEXT, 3, "d");
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));
    SQ_EXPECT(editor.send(SCI_CANUNDO) != 0);

    editor.setProperty("readonly", true);
    editor.setProperty("text", QStringLiteral("xyz"));
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));

    editor.setProperty("readonly", false);
    SQ_EXPECT(editor.send(SCI_CANUNDO) != 0);
    editor.send(SCI_UNDO);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abc"));
}

void test_noop_apply_is_not_reported_as_local_change()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abc"));
    editor.sends(SCI_INSERTTEXT, 3, "d");
    SQ_EXPECT(editor.send(SCI_CANUNDO) != 0);

    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::HANDLED,
                [](ScintillaQuick_item&) {
                    // Deliberately does not mutate the local document.
                }};
        });
    editor.setProperty("text", QStringLiteral("xyz"));
    SQ_EXPECT(text_of(editor) == QStringLiteral("abcd"));

    editor.set_edit_handler({});
    SQ_EXPECT(editor.send(SCI_CANUNDO) != 0);
    editor.send(SCI_UNDO);
    SQ_EXPECT(text_of(editor) == QStringLiteral("abc"));
}

void test_text_changed_is_emitted_after_settext_post_actions()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("abc"));

    bool signal_seen = false;
    bool can_undo_when_signalled = true;
    QObject::connect(
        &editor,
        &ScintillaQuick_item::textChanged,
        [&] {
            signal_seen = true;
            can_undo_when_signalled = editor.send(SCI_CANUNDO) != 0;
        });

    editor.setProperty("text", QStringLiteral("xyz"));
    SQ_EXPECT(signal_seen);
    SQ_EXPECT(!can_undo_when_signalled);
}

void test_text_changed_uses_actual_mutations_not_public_notifications()
{
    ScintillaQuick_item masked_editor;
    masked_editor.setProperty("text", QStringLiteral("a"));
    int masked_count = 0;
    QObject::connect(
        &masked_editor,
        &ScintillaQuick_item::textChanged,
        [&] { ++masked_count; });

    masked_editor.send(SCI_SETMODEVENTMASK, 0);
    masked_editor.sends(SCI_INSERTTEXT, 1, "b");
    SQ_EXPECT(text_of(masked_editor) == QStringLiteral("ab"));
    SQ_EXPECT(masked_count == 1);

    ScintillaQuick_item forged_editor;
    forged_editor.setProperty("text", QStringLiteral("a"));
    int forged_count = 0;
    QObject::connect(
        &forged_editor,
        &ScintillaQuick_item::textChanged,
        [&] { ++forged_count; });

    Scintilla::NotificationData forged = {};
    forged.nmhdr.code = Scintilla::Notification::Modified;
    forged.modificationType = Scintilla::ModificationFlags::InsertText;
    forged_editor.notifyParent(forged);
    SQ_EXPECT(text_of(forged_editor) == QStringLiteral("a"));
    SQ_EXPECT(forged_count == 0);
}

void test_text_changed_is_scoped_to_logical_operations()
{
    ScintillaQuick_item key_editor;
    key_editor.setProperty("text", QStringLiteral("abc"));
    key_editor.send(SCI_SETSEL, 1, 2);
    int key_change_count = 0;
    QObject::connect(
        &key_editor,
        &ScintillaQuick_item::textChanged,
        [&] { ++key_change_count; });

    QKeyEvent key_event(
        QEvent::KeyPress,
        Qt::Key_X,
        Qt::NoModifier,
        QStringLiteral("x"));
    QGuiApplication::sendEvent(&key_editor, &key_event);
    SQ_EXPECT(key_event.isAccepted());
    SQ_EXPECT(text_of(key_editor) == QStringLiteral("axc"));
    SQ_EXPECT(key_change_count == 1);

    ScintillaQuick_item replace_all_editor;
    replace_all_editor.setProperty("text", QStringLiteral("foo foo foo"));
    replace_all_editor.setProperty("findText", QStringLiteral("foo"));
    replace_all_editor.setProperty("replacementText", QStringLiteral("bar"));
    int replace_all_change_count = 0;
    QObject::connect(
        &replace_all_editor,
        &ScintillaQuick_item::textChanged,
        [&] { ++replace_all_change_count; });

    SQ_EXPECT(replace_all_editor.replaceAll() == 3);
    SQ_EXPECT(text_of(replace_all_editor) == QStringLiteral("bar bar bar"));
    SQ_EXPECT(replace_all_change_count == 1);
}

void test_external_replace_and_find_does_not_use_stale_target()
{
    ScintillaQuick_item editor;
    editor.setProperty("text", QStringLiteral("foo foo"));
    editor.setProperty("findText", QStringLiteral("foo"));
    editor.setProperty("replacementText", QStringLiteral("bar"));
    SQ_EXPECT(editor.findNext());
    SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 0);
    SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 3);

    editor.set_edit_handler(
        [](const ScintillaQuick_edit_transaction&) {
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::HANDLED,
                {}};
        });
    SQ_EXPECT(editor.replaceAndFind());
    SQ_EXPECT(text_of(editor) == QStringLiteral("foo foo"));
    SQ_EXPECT(editor.send(SCI_GETSELECTIONSTART) == 0);
    SQ_EXPECT(editor.send(SCI_GETSELECTIONEND) == 3);
}

void test_handled_target_replacement_restores_target()
{
    // SCI_REPLACETARGET defines the target as the inserted text even when
    // the owning model applies its edit through other Scintilla messages.
    for (const QByteArray replacement : {QByteArray("f"), QByteArray("bar\nfoo")}) {
        ScintillaQuick_item editor;
        editor.setProperty("text", QStringLiteral("foo foo"));
        editor.setProperty("findText", QStringLiteral("foo"));
        editor.setProperty("replacementText", QString::fromUtf8(replacement));
        int calls = 0;
        const auto handler = [&](const ScintillaQuick_edit_transaction& transaction) {
            ++calls;
            // Bound a broken Replace All loop so the regression fails
            // instead of exhausting memory on repeated inserted matches.
            if (calls > 2) {
                return ScintillaQuick_edit_result{
                    ScintillaQuick_edit_disposition::REJECTED, {}};
            }
            const auto edit = transaction.replacements.front();
            return ScintillaQuick_edit_result{
                ScintillaQuick_edit_disposition::HANDLED,
                [edit](ScintillaQuick_item& target) {
                    target.send(SCI_DELETERANGE, edit.position, edit.deleted_length);
                    target.sends(SCI_INSERTTEXT, edit.position, edit.inserted_text.constData());
                }};
        };
        editor.set_edit_handler(handler);
        editor.send(SCI_SETTARGETRANGE, 0, 3);
        editor.sends(SCI_REPLACETARGET, replacement.size(), replacement.constData());
        SQ_EXPECT(editor.send(SCI_GETTARGETSTART) == 0);
        SQ_EXPECT(editor.send(SCI_GETTARGETEND) == replacement.size());
        editor.set_edit_handler({});
        editor.setProperty("text", QStringLiteral("foo foo"));
        calls = 0;
        editor.set_edit_handler(handler);
        SQ_EXPECT(editor.replaceAll() == 2);
        SQ_EXPECT(calls == 2);
        SQ_EXPECT(text_of(editor).toUtf8() == replacement + " " + replacement);
    }
}

void test_input_method_offsets_use_utf16_units()
{
    Event_editor editor;
    const QString text = QString::fromUcs4(U"\u00E9\U0001F600x");
    editor.setProperty("text", text);

    // UTF-8 byte position 6 is UTF-16 position 3 (é = 1 unit, emoji = 2).
    editor.send(SCI_SETSEL, 6, 6);
    SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition).toInt() == 3);
    SQ_EXPECT(editor.query_input_method(Qt::ImAnchorPosition).toInt() == 3);

    editor.send(SCI_SETSEL, 6, 0); // anchor after the emoji, caret at start
    SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition).toInt() == 0);
    SQ_EXPECT(editor.query_input_method(Qt::ImAnchorPosition).toInt() == 3);
}

void test_input_method_geometry_after_composition()
{
    for (const bool commit : {false, true}) {
        Event_editor editor;
        editor.setWidth(640);
        editor.setHeight(240);
        editor.send(SCI_SETMARGINWIDTHN, 0, 60);
        editor.setProperty("text", QStringLiteral("initial text"));
        editor.send(SCI_SETSEL, 2, 2);
        QInputMethodEvent preedit(QStringLiteral("candidate"), {});
        editor.deliver_input_method(&preedit);
        QInputMethodEvent finish;
        if (commit) {
            finish.setCommitString(QStringLiteral("done"));
        }
        editor.deliver_input_method(&finish);
        editor.send(SCI_SETSEL, 1, 1);
        const QRectF cursor = editor.query_input_method(Qt::ImCursorRectangle).toRectF();
        const QRectF anchor = editor.query_input_method(Qt::ImAnchorRectangle).toRectF();
        SQ_EXPECT(cursor.topLeft() == anchor.topLeft());
        SQ_EXPECT(editor.query_input_method(Qt::ImCursorRectangle, {}).toRectF() == cursor);
        SQ_EXPECT(editor.query_input_method(Qt::ImAnchorRectangle, {}).toRectF() == anchor);
        SQ_EXPECT(editor.query_input_method(Qt::ImCursorPosition, anchor.center()).toInt() == 1);
    }
}

void test_horizontal_scroll_survives_edit_and_margin_update()
{
    Event_editor editor;
    editor.setWidth(640);
    editor.setHeight(240);
    editor.setProperty("text", QString(400, QLatin1Char('x')));
    editor.capture();
    editor.send(SCI_SETSEL, 100, 100);
    const int caret_x = static_cast<int>(editor.send(SCI_POINTXFROMPOSITION, 0, 100));
    const int offset = caret_x + static_cast<int>(editor.send(SCI_GETXOFFSET)) - 250;
    SQ_EXPECT(offset > 0);
    editor.send(SCI_SETXOFFSET, offset);
    SQ_EXPECT(editor.send(SCI_POINTXFROMPOSITION, 0, 100) == 250);
    editor.send(SCI_DELETEBACK);
    SQ_EXPECT(editor.send(SCI_GETXOFFSET) == offset);
    editor.capture();
    SQ_EXPECT(editor.send(SCI_GETXOFFSET) == offset);
    const auto margin_width = editor.send(SCI_GETMARGINWIDTHN, 0);
    editor.send(SCI_SETMARGINWIDTHN, 0, margin_width);
    SQ_EXPECT(editor.send(SCI_GETXOFFSET) == offset);
    editor.capture();
    SQ_EXPECT(editor.send(SCI_GETXOFFSET) == offset);
}

void test_ui_and_capture_notifications()
{
    Event_editor editor;
    editor.setWidth(640);
    editor.setHeight(240);
    editor.setProperty("text", QStringLiteral("sample"));
    QGuiApplication::processEvents(QEventLoop::AllEvents);
    int selection_updates = 0;
    int completed_captures = 0;
    QObject::connect(&editor, &ScintillaQuick_item::updateUi, [&](Scintilla::Update updated) {
        if ((static_cast<int>(updated) & SC_UPDATE_SELECTION) != 0) {
            ++selection_updates;
        }
    });
    QObject::connect(&editor, &ScintillaQuick_item::painted, [&] { ++completed_captures; });
    editor.send(SCI_SETSEL, 1, 1);
    QGuiApplication::processEvents(QEventLoop::AllEvents);
    SQ_EXPECT(selection_updates > 0);
    selection_updates = 0;
    QKeyEvent select_right(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
    QGuiApplication::sendEvent(&editor, &select_right);
    QGuiApplication::processEvents(QEventLoop::AllEvents);
    SQ_EXPECT(selection_updates > 0);
    editor.capture();
    SQ_EXPECT(completed_captures > 0);

    bool reentered = false;
    QObject::connect(&editor, &ScintillaQuick_item::updateUi, [&](Scintilla::Update updated) {
        if (!reentered && (static_cast<int>(updated) & SC_UPDATE_SELECTION) != 0) {
            reentered = true;
            editor.send(SCI_SETSEL, 4, 4);
        }
    });
    selection_updates = 0;
    editor.send(SCI_SETSEL, 3, 3);
    QGuiApplication::processEvents(QEventLoop::AllEvents);
    QGuiApplication::processEvents(QEventLoop::AllEvents);
    SQ_EXPECT(reentered);
    SQ_EXPECT(selection_updates == 2);
}

void test_hidden_property_sync_and_observer_edits()
{
    Event_editor editor;
    editor.setWidth(640);
    editor.setHeight(240);
    editor.setProperty("text", QStringLiteral("one"));
    QGuiApplication::processEvents(QEventLoop::AllEvents);
    int line_changes = 0;
    QObject::connect(&editor, &ScintillaQuick_item::totalLinesChanged, [&] {
        ++line_changes;
        if (line_changes == 1) {
            editor.sends(SCI_APPENDTEXT, 6, "\nthree");
            QGuiApplication::processEvents(QEventLoop::AllEvents);
        }
    });
    editor.sends(SCI_APPENDTEXT, 4, "\ntwo");
    SQ_EXPECT(editor.property("totalLines").toInt() == 2);
    SQ_EXPECT(editor.property("logicalHeight").toInt() ==
        2 * editor.property("charHeight").toInt());
    QGuiApplication::processEvents(QEventLoop::AllEvents);
    QGuiApplication::processEvents(QEventLoop::AllEvents);
    SQ_EXPECT(line_changes == 2);
    SQ_EXPECT(editor.property("totalLines").toInt() == 3);
    SQ_EXPECT(text_of(editor) == QStringLiteral("one\ntwo\nthree"));
    editor.send(SCI_SETSCROLLWIDTH, 1200);
    SQ_EXPECT(editor.property("logicalWidth").toInt() == 1200);
}

void test_current_selection_preserves_embedded_nul()
{
    Event_editor editor;
    const char bytes[] = {'a', '\0', 'b'};
    editor.send(
        SCI_ADDTEXT,
        3,
        reinterpret_cast<Scintilla::sptr_t>(bytes));
    editor.send(SCI_SETSEL, 0, 3);

    const QString selected = editor.query_input_method(Qt::ImCurrentSelection).toString();
    const QString expected = QString::fromLatin1(bytes, 3);
    SQ_EXPECT(selected.size() == 3);
    SQ_EXPECT(selected == expected);
}

void test_document_pointer_updates_text_and_readonly_properties()
{
    ScintillaQuick_item source;
    source.setProperty("text", QStringLiteral("shared"));
    source.setProperty("readonly", true);

    ScintillaQuick_item target;
    target.setProperty("text", QStringLiteral("private"));
    int text_changed_count = 0;
    int readonly_changed_count = 0;
    QObject::connect(
        &target,
        &ScintillaQuick_item::textChanged,
        [&] { ++text_changed_count; });
    QObject::connect(
        &target,
        &ScintillaQuick_item::readonlyChanged,
        [&] { ++readonly_changed_count; });

    const Scintilla::sptr_t document = source.send(SCI_GETDOCPOINTER);
    target.send(SCI_SETDOCPOINTER, 0, document);
    SQ_EXPECT(text_of(target) == QStringLiteral("shared"));
    SQ_EXPECT(target.property("readonly").toBool());
    SQ_EXPECT(text_changed_count == 1);
    SQ_EXPECT(readonly_changed_count == 1);
}

void test_readonly_drop_preserves_modify_attempt_notification()
{
    Event_editor editor;
    editor.setProperty("text", QStringLiteral("a"));
    editor.setProperty("readonly", true);

    int modify_attempt_count = 0;
    QObject::connect(
        &editor,
        &ScintillaQuick_item::modifyAttemptReadOnly,
        [&] { ++modify_attempt_count; });

    QMimeData mime_data;
    mime_data.setText(QStringLiteral("b"));
    QDropEvent drop(
        QPointF(0.0, 0.0),
        Qt::CopyAction,
        &mime_data,
        Qt::LeftButton,
        Qt::NoModifier);
    editor.deliver_drop(&drop);

    SQ_EXPECT(!drop.isAccepted());
    SQ_EXPECT(text_of(editor) == QStringLiteral("a"));
    SQ_EXPECT(modify_attempt_count == 1);
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--horizontal-scroll-contract"))) {
        test_horizontal_scroll_survives_edit_and_margin_update();
        return g_failures ? 1 : 0;
    }


    test_handler_absent_preserves_direct_edit();
    test_handler_dispositions_and_reentrant_apply();
    test_handler_snapshot_and_transaction_cleanup();
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
    test_ime_queries_exclude_preedit();
    test_ime_point_queries_exclude_preedit();
    test_ime_queries_with_multiple_carets();
    test_ime_queries_with_adjacent_overtype_carets();
    test_ime_replacement_ranges();
    test_ime_selection_and_replacement_are_one_transaction();
    test_ime_commit_continues_preedit();
    test_ime_replacement_continues_preedit();
    test_ime_selection_attribute_follows_commit();
    test_ime_surrounding_selection_handler_dispositions();
    test_ime_changed_preedit_consumes_selection_before_new_selection();
    test_ime_deletion_preserves_composition_origin();
    test_ime_handled_deletion_preserves_projected_selection();
    test_ime_selection_attribute_preserves_direction();
    test_ime_explicit_replacement_handler_dispositions();
    test_ime_replacement_respects_readonly_and_protected_text();
    test_clipboard_and_drop_ingress();
    test_unsupported_multi_selection_fails_closed();
    test_native_edit_commands_obey_edit_handler_boundary();
    test_anchor_queries_track_selection_anchor();
    test_supplementary_plane_key_text_inserts();
    test_text_changed_notification_coalescing();
    test_rejected_settext_skips_post_actions();
    test_find_replace_edit_boundary_outcomes();
    test_native_paste_requires_text_clipboard();
    test_regex_selection_keeps_document_context();
    test_readonly_notification_covers_direct_messages();
    test_transaction_tracking_survives_unhandled_nested_undo_groups();
    test_native_readonly_settext_preserves_undo();
    test_noop_apply_is_not_reported_as_local_change();
    test_text_changed_is_emitted_after_settext_post_actions();
    test_text_changed_uses_actual_mutations_not_public_notifications();
    test_text_changed_is_scoped_to_logical_operations();
    test_external_replace_and_find_does_not_use_stale_target();
    test_handled_target_replacement_restores_target();
    test_input_method_offsets_use_utf16_units();
    test_ui_and_capture_notifications();
    test_horizontal_scroll_survives_edit_and_margin_update();
    test_input_method_geometry_after_composition();
    test_hidden_property_sync_and_observer_edits();
    test_current_selection_preserves_embedded_nul();
    test_document_pointer_updates_text_and_readonly_properties();
    test_readonly_drop_preserves_modify_attempt_notification();

    if (g_failures != 0) {
        std::fprintf(stderr, "scintillaquick_edit_boundary_test: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
