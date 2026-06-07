// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

// Smoke coverage for ScintillaQuick_item's message pipeline.
//
// These tests do not run a full Qt Quick window - they drive the
// editor entirely through `send()` to exercise Scintilla's message
// dispatch behind the item. The goal is to guard against the most
// common kinds of upstream-Scintilla-update breakage (missed message
// classifications, notifications, document-state regressions) without
// requiring a desktop session.

#include <scintillaquick/scintillaquick_item.h>

#include "scintillaquick_core.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QDebug>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QMetaType>
#include <QPointer>
#include <QQuickItem>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTextCharFormat>
#include <QTextFormat>
#include <QUrl>
#include <qqml.h>

#include "Scintilla.h"
#include "scintillaquick_platqt.h"
#include "scintillaquick_font.h"
#include "scintillaquick_test_macros.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

namespace
{

int g_failures = 0;

class Send_override_probe_editor final : public ScintillaQuick_item
{
public:
    mutable int total_send_override_calls = 0;
    mutable int direct_target_send_override_calls = 0;

    sptr_t send(unsigned int i_message, uptr_t w_param = 0, sptr_t l_param = 0) const override
    {
        ++total_send_override_calls;
        switch (i_message) {
            case SCI_APPENDTEXT:
            case SCI_STYLEGETBOLD:
            case SCI_MARKERGET:
            case SCI_AUTOCGETCURRENT:
                ++direct_target_send_override_calls;
                break;
            default:
                break;
        }
        return ScintillaQuick_item::send(i_message, w_param, l_param);
    }
};

void pump_events()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

int text_length(ScintillaQuick_item& editor)
{
    return static_cast<int>(editor.send(SCI_GETTEXTLENGTH));
}

int current_position(ScintillaQuick_item& editor)
{
    return static_cast<int>(editor.send(SCI_GETCURRENTPOS));
}

Scintilla::NotificationData make_notification(Scintilla::Notification code)
{
    Scintilla::NotificationData scn{};
    scn.nmhdr.hwndFrom = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234));
    scn.nmhdr.idFrom = 17;
    scn.nmhdr.code = code;
    scn.position = 23;
    scn.ch = 65;
    scn.modifiers = Scintilla::KeyMod::Ctrl;
    scn.modificationType = Scintilla::ModificationFlags::InsertText;
    scn.length = 5;
    scn.linesAdded = 1;
    scn.message = Scintilla::Message::Null;
    scn.wParam = 0;
    scn.lParam = 0;
    scn.line = 3;
    scn.foldLevelNow = Scintilla::FoldLevel::Base;
    scn.foldLevelPrev = Scintilla::FoldLevel::None;
    scn.margin = 2;
    scn.listType = 4;
    scn.x = 11;
    scn.y = 13;
    scn.token = 19;
    scn.annotationLinesAdded = 7;
    scn.updated = Scintilla::Update::Content;
    scn.listCompletionMethod = Scintilla::CompletionMethods::Command;
    scn.characterSource = Scintilla::CharacterSource::DirectInput;
    return scn;
}

int selection_start(ScintillaQuick_item& editor)
{
    return static_cast<int>(editor.send(SCI_GETSELECTIONSTART));
}

int selection_end(ScintillaQuick_item& editor)
{
    return static_cast<int>(editor.send(SCI_GETSELECTIONEND));
}

void test_property_roundtrip(ScintillaQuick_item& editor)
{
    const QString text = QStringLiteral("smoke test\nsecond line");
    editor.setProperty("font", scintillaquick::shared::deterministic_test_font(11));
    editor.setProperty("text", text);
    editor.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
    pump_events();

    SQ_EXPECT(text_length(editor) == text.size());
    SQ_EXPECT(editor.width() == 640);
    SQ_EXPECT(editor.property("text").toString() == text);
}

void test_insert_and_delete(ScintillaQuick_item& editor)
{
    editor.setProperty("text", QString());
    SQ_EXPECT(text_length(editor) == 0);

    // Insert "hello world" at position 0.
    const char* hello = "hello world";
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

void test_sends_syncs_properties(ScintillaQuick_item& editor)
{
    editor.setProperty("text", QStringLiteral("one"));
    pump_events();

    int total_lines_changed = 0;
    const QMetaObject::Connection total_lines_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::totalLinesChanged,
        &editor,
        [&]() {
            ++total_lines_changed;
        });

    editor.sends(SCI_APPENDTEXT, 4, "\ntwo");
    SQ_EXPECT(text_length(editor) == 7);
    SQ_EXPECT(editor.property("totalLines").toInt() == 2);
    SQ_EXPECT(total_lines_changed > 0);
    QObject::disconnect(total_lines_connection);
}

void test_direct_function_syncs_properties(ScintillaQuick_item& editor)
{
    editor.setProperty("text", QStringLiteral("one"));
    pump_events();

    auto direct = reinterpret_cast<SciFnDirect>(editor.send(SCI_GETDIRECTFUNCTION));
    auto direct_status =
        reinterpret_cast<SciFnDirectStatus>(editor.send(SCI_GETDIRECTSTATUSFUNCTION));
    const sptr_t direct_pointer = editor.send(SCI_GETDIRECTPOINTER);
    SQ_EXPECT(direct != nullptr);
    SQ_EXPECT(direct_status != nullptr);
    SQ_EXPECT(direct_pointer != 0);

    int total_lines_changed = 0;
    const QMetaObject::Connection total_lines_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::totalLinesChanged,
        &editor,
        [&]() {
            ++total_lines_changed;
        });

    direct(direct_pointer, SCI_APPENDTEXT, 4, reinterpret_cast<sptr_t>("\ntwo"));
    SQ_EXPECT(text_length(editor) == 7);
    SQ_EXPECT(editor.property("totalLines").toInt() == 2);
    SQ_EXPECT(total_lines_changed > 0);

    int status = -1;
    direct_status(direct_pointer, SCI_APPENDTEXT, 6, reinterpret_cast<sptr_t>("\nthree"), &status);
    SQ_EXPECT(status == 0);
    SQ_EXPECT(text_length(editor) == 13);
    SQ_EXPECT(editor.property("totalLines").toInt() == 3);

    direct_status(direct_pointer, SCI_APPENDTEXT, 1, reinterpret_cast<sptr_t>("!"), nullptr);
    SQ_EXPECT(text_length(editor) == 14);

    int captured_status = -1;
    direct_status(
        direct_pointer,
        SCI_SETSTATUS,
        static_cast<uptr_t>(Scintilla::Status::RegEx),
        0,
        &captured_status);
    SQ_EXPECT(captured_status == static_cast<int>(Scintilla::Status::RegEx));
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == static_cast<sptr_t>(Scintilla::Status::RegEx));

    direct_status(
        direct_pointer,
        SCI_SETSTATUS,
        static_cast<uptr_t>(Scintilla::Status::Ok),
        0,
        &captured_status);
    SQ_EXPECT(captured_status == static_cast<int>(Scintilla::Status::Ok));

    bool modified_slot_changed_status = false;
    const QMetaObject::Connection status_from_modified_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::modified,
        &editor,
        [&](auto, auto, auto, auto, const QByteArray& text, auto, auto, auto) {
            if (!modified_slot_changed_status && text == QByteArray("\nstatus")) {
                modified_slot_changed_status = true;
                editor.send(
                    SCI_SETSTATUS,
                    static_cast<uptr_t>(Scintilla::Status::RegEx),
                    0);
            }
        });

    int status_after_modified_slot = -1;
    direct_status(
        direct_pointer,
        SCI_APPENDTEXT,
        7,
        reinterpret_cast<sptr_t>("\nstatus"),
        &status_after_modified_slot);
    SQ_EXPECT(modified_slot_changed_status);
    SQ_EXPECT(status_after_modified_slot == static_cast<int>(Scintilla::Status::RegEx));
    SQ_EXPECT(editor.send(SCI_GETSTATUS) == static_cast<sptr_t>(Scintilla::Status::RegEx));

    direct_status(
        direct_pointer,
        SCI_SETSTATUS,
        static_cast<uptr_t>(Scintilla::Status::Ok),
        0,
        &captured_status);
    SQ_EXPECT(captured_status == static_cast<int>(Scintilla::Status::Ok));
    QObject::disconnect(status_from_modified_connection);
    QObject::disconnect(total_lines_connection);
}

void test_direct_callbacks_bypass_send_override()
{
    Send_override_probe_editor editor;
    editor.setWidth(640);
    editor.setHeight(480);
    editor.setProperty("text", QStringLiteral("one"));
    pump_events();

    auto direct = reinterpret_cast<SciFnDirect>(
        editor.ScintillaQuick_item::send(SCI_GETDIRECTFUNCTION));
    auto direct_status = reinterpret_cast<SciFnDirectStatus>(
        editor.ScintillaQuick_item::send(SCI_GETDIRECTSTATUSFUNCTION));
    const sptr_t direct_pointer =
        editor.ScintillaQuick_item::send(SCI_GETDIRECTPOINTER);
    SQ_EXPECT(direct != nullptr);
    SQ_EXPECT(direct_status != nullptr);
    SQ_EXPECT(direct_pointer != 0);

    editor.direct_target_send_override_calls = 0;
    editor.total_send_override_calls = 0;
    direct(direct_pointer, SCI_STYLEGETBOLD, STYLE_DEFAULT, 0);
    direct(direct_pointer, SCI_MARKERGET, 0, 0);
    int status = -1;
    direct_status(direct_pointer, SCI_AUTOCGETCURRENT, 0, 0, &status);
    SQ_EXPECT(editor.total_send_override_calls == 0);
    SQ_EXPECT(editor.direct_target_send_override_calls == 0);

    direct(direct_pointer, SCI_APPENDTEXT, 4, reinterpret_cast<sptr_t>("\ntwo"));

    direct_status(direct_pointer, SCI_APPENDTEXT, 6, reinterpret_cast<sptr_t>("\nthree"), &status);

    SQ_EXPECT(editor.direct_target_send_override_calls == 0);
    SQ_EXPECT(editor.ScintillaQuick_item::send(SCI_GETTEXTLENGTH) == 13);
    SQ_EXPECT(editor.property("totalLines").toInt() == 3);
    SQ_EXPECT(status == static_cast<int>(Scintilla::Status::Ok));
}

void test_modified_signal_owns_text_bytes(ScintillaQuick_item& editor)
{
    editor.setProperty("text", QString());
    pump_events();

    QByteArray emitted_text;
    const QMetaObject::Connection modified_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::modified,
        &editor,
        [&](auto, auto, auto, auto, const QByteArray& text, auto, auto, auto) {
            if (!text.isEmpty()) {
                emitted_text = text;
            }
        });

    QByteArray inserted("owned");
    editor.sends(SCI_INSERTTEXT, 0, inserted.constData());
    SQ_EXPECT(!emitted_text.isEmpty());

    inserted.fill('x');
    SQ_EXPECT(emitted_text == QByteArray("owned"));
    QObject::disconnect(modified_connection);
}

void test_notification_metatypes_registered()
{
    SQ_EXPECT(QMetaType::fromName("ScintillaQuick_notification").isValid());
    SQ_EXPECT(QMetaType::fromName("Scintilla::Position").isValid());
    SQ_EXPECT(QMetaType::fromName("Scintilla::ModificationFlags").isValid());
    SQ_EXPECT(QMetaType::fromName("Scintilla::FoldLevel").isValid());
}

void test_notification_received_order_and_legacy_mutation(ScintillaQuick_item& editor)
{
    std::vector<int> order;
    int snapshot_ch = -1;
    int typed_ch = -1;

    const QMetaObject::Connection legacy_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notify,
        &editor,
        [&](Scintilla::NotificationData* scn) {
            order.push_back(1);
            scn->ch = 99;
        });
    const QMetaObject::Connection safe_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notificationReceived,
        &editor,
        [&](const ScintillaQuick_notification& notification) {
            order.push_back(2);
            snapshot_ch = notification.ch;
        });
    const QMetaObject::Connection typed_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::charAdded,
        &editor,
        [&](int ch) {
            order.push_back(3);
            typed_ch = ch;
        });

    Scintilla::NotificationData scn = make_notification(Scintilla::Notification::CharAdded);
    scn.ch = 65;
    editor.notifyParent(scn);

    SQ_EXPECT(order.size() == 3);
    if (order.size() == 3) {
        SQ_EXPECT(order[0] == 1);
        SQ_EXPECT(order[1] == 2);
        SQ_EXPECT(order[2] == 3);
    }
    SQ_EXPECT(snapshot_ch == 65);
    SQ_EXPECT(typed_ch == 99);

    QObject::disconnect(legacy_connection);
    QObject::disconnect(safe_connection);
    QObject::disconnect(typed_connection);
}

void test_modified_signal_preserves_legacy_mutated_text_with_safe_receiver(ScintillaQuick_item& editor)
{
    QByteArray original_text("or\0ig", 5);
    QByteArray legacy_text("post\0notify", 11);
    QByteArray snapshot_text;
    QByteArray typed_text;
    Scintilla::Position snapshot_length = -1;
    Scintilla::Position typed_length = -1;
    int snapshot_count = 0;
    int typed_count = 0;

    const QMetaObject::Connection legacy_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notify,
        &editor,
        [&](Scintilla::NotificationData* scn) {
            if (scn->nmhdr.code != Scintilla::Notification::Modified) {
                return;
            }
            scn->text = legacy_text.constData();
            scn->length = legacy_text.size();
        });
    const QMetaObject::Connection safe_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notificationReceived,
        &editor,
        [&](const ScintillaQuick_notification& notification) {
            ++snapshot_count;
            snapshot_text = notification.text;
            snapshot_length = notification.length;
        });
    const QMetaObject::Connection typed_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::modified,
        &editor,
        [&](
            Scintilla::ModificationFlags,
            Scintilla::Position,
            Scintilla::Position length,
            Scintilla::Position,
            const QByteArray& text,
            Scintilla::Position,
            Scintilla::FoldLevel,
            Scintilla::FoldLevel) {
            ++typed_count;
            typed_text = text;
            typed_length = length;
        });

    Scintilla::NotificationData scn = make_notification(Scintilla::Notification::Modified);
    scn.text = original_text.constData();
    scn.length = original_text.size();
    editor.notifyParent(scn);

    SQ_EXPECT(snapshot_count == 1);
    SQ_EXPECT(snapshot_text == original_text);
    SQ_EXPECT(snapshot_length == original_text.size());
    SQ_EXPECT(typed_count == 1);
    SQ_EXPECT(typed_text == legacy_text);
    SQ_EXPECT(typed_length == legacy_text.size());

    QObject::disconnect(legacy_connection);
    QObject::disconnect(safe_connection);
    QObject::disconnect(typed_connection);
}

void test_modified_signal_reuses_safe_snapshot_text_when_unchanged(ScintillaQuick_item& editor)
{
    QByteArray source("same\0bytes", 10);
    const QByteArray expected = source;
    QByteArray snapshot_text;
    QByteArray typed_text;
    int snapshot_count = 0;
    int typed_count = 0;

    const QMetaObject::Connection safe_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notificationReceived,
        &editor,
        [&](const ScintillaQuick_notification& notification) {
            ++snapshot_count;
            snapshot_text = notification.text;
        });
    const QMetaObject::Connection typed_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::modified,
        &editor,
        [&](
            Scintilla::ModificationFlags,
            Scintilla::Position,
            Scintilla::Position,
            Scintilla::Position,
            const QByteArray& text,
            Scintilla::Position,
            Scintilla::FoldLevel,
            Scintilla::FoldLevel) {
            ++typed_count;
            typed_text = text;
        });

    Scintilla::NotificationData scn = make_notification(Scintilla::Notification::Modified);
    scn.text = source.constData();
    scn.length = source.size();
    editor.notifyParent(scn);

    source.fill('x');

    SQ_EXPECT(snapshot_count == 1);
    SQ_EXPECT(snapshot_text == expected);
    SQ_EXPECT(typed_count == 1);
    SQ_EXPECT(typed_text == expected);

    QObject::disconnect(safe_connection);
    QObject::disconnect(typed_connection);
}

void test_modified_signal_queued_owns_text_bytes(ScintillaQuick_item& editor)
{
    QObject receiver;
    int received_count = 0;
    QByteArray received_text;
    Scintilla::ModificationFlags received_type = Scintilla::ModificationFlags::None;
    Scintilla::Position received_length = 0;
    Scintilla::FoldLevel received_fold_now = Scintilla::FoldLevel::None;

    const QMetaObject::Connection modified_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::modified,
        &receiver,
        [&](
            Scintilla::ModificationFlags type,
            Scintilla::Position,
            Scintilla::Position length,
            Scintilla::Position,
            const QByteArray& text,
            Scintilla::Position,
            Scintilla::FoldLevel fold_now,
            Scintilla::FoldLevel) {
            ++received_count;
            received_type = type;
            received_length = length;
            received_text = text;
            received_fold_now = fold_now;
        },
        Qt::QueuedConnection);
    SQ_EXPECT(modified_connection);

    QByteArray source("m\0d", 3);
    const QByteArray expected = source;
    Scintilla::NotificationData scn = make_notification(Scintilla::Notification::Modified);
    scn.text = source.constData();
    scn.length = source.size();
    scn.foldLevelNow = Scintilla::FoldLevel::Base;

    editor.notifyParent(scn);
    source.fill('x');
    pump_events();

    SQ_EXPECT(received_count == 1);
    SQ_EXPECT(received_type == Scintilla::ModificationFlags::InsertText);
    SQ_EXPECT(received_length == expected.size());
    SQ_EXPECT(received_text == expected);
    SQ_EXPECT(received_fold_now == Scintilla::FoldLevel::Base);
}

void test_notification_received_queued_modified_embedded_nul(ScintillaQuick_item& editor)
{
    QObject receiver;
    int received_count = 0;
    ScintillaQuick_notification received;

    const QMetaObject::Connection notification_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notificationReceived,
        &receiver,
        [&](const ScintillaQuick_notification& notification) {
            ++received_count;
            received = notification;
        },
        Qt::QueuedConnection);
    SQ_EXPECT(notification_connection);

    QByteArray source("ab\0cd", 5);
    const QByteArray expected = source;
    Scintilla::NotificationData scn = make_notification(Scintilla::Notification::Modified);
    scn.text = source.constData();
    scn.length = source.size();

    editor.notifyParent(scn);
    source.fill('x');
    pump_events();

    SQ_EXPECT(received_count == 1);
    SQ_EXPECT(received.code == Scintilla::Notification::Modified);
    SQ_EXPECT(received.hwndFrom == static_cast<Scintilla::uptr_t>(0x1234));
    SQ_EXPECT(received.idFrom == 17);
    SQ_EXPECT(received.textAvailable);
    SQ_EXPECT(received.text == expected);
    SQ_EXPECT(received.length == expected.size());
    SQ_EXPECT(!received.lParamTextAvailable);
}

void test_notification_received_queued_string_payload(ScintillaQuick_item& editor)
{
    QObject receiver;
    std::vector<ScintillaQuick_notification> received;

    const QMetaObject::Connection notification_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notificationReceived,
        &receiver,
        [&](const ScintillaQuick_notification& notification) {
            received.push_back(notification);
        },
        Qt::QueuedConnection);
    SQ_EXPECT(notification_connection);

    QByteArray uri_source("file:///tmp/scintillaquick.txt");
    const QByteArray expected_uri = uri_source;
    Scintilla::NotificationData uri_scn = make_notification(Scintilla::Notification::URIDropped);
    uri_scn.text = uri_source.constData();
    editor.notifyParent(uri_scn);
    uri_source.fill('x');

    QByteArray selection_source("selected");
    const QByteArray expected_selection = selection_source;
    Scintilla::NotificationData selection_scn =
        make_notification(Scintilla::Notification::AutoCSelection);
    selection_scn.text = selection_source.constData();
    selection_scn.lParam = 42;
    editor.notifyParent(selection_scn);
    selection_source.fill('x');

    pump_events();

    SQ_EXPECT(received.size() == 2);
    if (received.size() == 2) {
        SQ_EXPECT(received[0].code == Scintilla::Notification::URIDropped);
        SQ_EXPECT(received[0].textAvailable);
        SQ_EXPECT(received[0].text == expected_uri);
        SQ_EXPECT(received[0].lParamKind == ScintillaQuick_lparam_kind::None);

        SQ_EXPECT(received[1].code == Scintilla::Notification::AutoCSelection);
        SQ_EXPECT(received[1].textAvailable);
        SQ_EXPECT(received[1].text == expected_selection);
        SQ_EXPECT(received[1].lParamKind == ScintillaQuick_lparam_kind::Numeric);
        SQ_EXPECT(received[1].lParamValue == 42);
    }
}

void test_notification_received_queued_macro_payloads(ScintillaQuick_item& editor)
{
    QObject receiver;
    std::vector<ScintillaQuick_notification> received;

    const QMetaObject::Connection notification_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notificationReceived,
        &receiver,
        [&](const ScintillaQuick_notification& notification) {
            received.push_back(notification);
        },
        Qt::QueuedConnection);
    SQ_EXPECT(notification_connection);

    QByteArray replace_source("replacement");
    const QByteArray expected_replace = replace_source;
    Scintilla::NotificationData replace_scn = make_notification(Scintilla::Notification::MacroRecord);
    replace_scn.message = Scintilla::Message::ReplaceSel;
    replace_scn.lParam = reinterpret_cast<Scintilla::sptr_t>(replace_source.constData());
    editor.notifyParent(replace_scn);
    replace_source.fill('x');

    QByteArray add_source("A\0B", 3);
    const QByteArray expected_add = add_source;
    Scintilla::NotificationData add_scn = make_notification(Scintilla::Notification::MacroRecord);
    add_scn.message = Scintilla::Message::AddText;
    add_scn.wParam = static_cast<Scintilla::uptr_t>(add_source.size());
    add_scn.lParam = reinterpret_cast<Scintilla::sptr_t>(add_source.constData());
    editor.notifyParent(add_scn);
    add_source.fill('x');

    QByteArray search_source("needle");
    const QByteArray expected_search = search_source;
    Scintilla::NotificationData search_scn = make_notification(Scintilla::Notification::MacroRecord);
    search_scn.message = Scintilla::Message::SearchNext;
    search_scn.lParam = reinterpret_cast<Scintilla::sptr_t>(search_source.constData());
    editor.notifyParent(search_scn);
    search_source.fill('x');

    Scintilla::NotificationData line_scroll_scn = make_notification(Scintilla::Notification::MacroRecord);
    line_scroll_scn.message = Scintilla::Message::LineScroll;
    line_scroll_scn.wParam = 3;
    line_scroll_scn.lParam = 4;
    editor.notifyParent(line_scroll_scn);

    pump_events();

    SQ_EXPECT(received.size() == 4);
    if (received.size() == 4) {
        SQ_EXPECT(received[0].code == Scintilla::Notification::MacroRecord);
        SQ_EXPECT(received[0].message == Scintilla::Message::ReplaceSel);
        SQ_EXPECT(received[0].lParamKind == ScintillaQuick_lparam_kind::Text);
        SQ_EXPECT(received[0].lParamValue == 0);
        SQ_EXPECT(received[0].lParamTextAvailable);
        SQ_EXPECT(received[0].lParamText == expected_replace);

        SQ_EXPECT(received[1].message == Scintilla::Message::AddText);
        SQ_EXPECT(received[1].wParam == static_cast<Scintilla::uptr_t>(expected_add.size()));
        SQ_EXPECT(received[1].lParamKind == ScintillaQuick_lparam_kind::Text);
        SQ_EXPECT(received[1].lParamValue == 0);
        SQ_EXPECT(received[1].lParamTextAvailable);
        SQ_EXPECT(received[1].lParamText == expected_add);

        SQ_EXPECT(received[2].message == Scintilla::Message::SearchNext);
        SQ_EXPECT(received[2].lParamKind == ScintillaQuick_lparam_kind::Text);
        SQ_EXPECT(received[2].lParamValue == 0);
        SQ_EXPECT(received[2].lParamTextAvailable);
        SQ_EXPECT(received[2].lParamText == expected_search);

        SQ_EXPECT(received[3].message == Scintilla::Message::LineScroll);
        SQ_EXPECT(received[3].lParamKind == ScintillaQuick_lparam_kind::Numeric);
        SQ_EXPECT(received[3].lParamValue == 4);
        SQ_EXPECT(!received[3].lParamTextAvailable);
    }
}

void test_notification_received_queued_scalar_notification(ScintillaQuick_item& editor)
{
    QObject receiver;
    int received_count = 0;
    ScintillaQuick_notification received;

    const QMetaObject::Connection notification_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::notificationReceived,
        &receiver,
        [&](const ScintillaQuick_notification& notification) {
            ++received_count;
            received = notification;
        },
        Qt::QueuedConnection);
    SQ_EXPECT(notification_connection);

    Scintilla::NotificationData scn = make_notification(Scintilla::Notification::MarginClick);
    editor.notifyParent(scn);
    pump_events();

    SQ_EXPECT(received_count == 1);
    SQ_EXPECT(received.code == Scintilla::Notification::MarginClick);
    SQ_EXPECT(received.position == 23);
    SQ_EXPECT(received.margin == 2);
    SQ_EXPECT(received.modifiers == Scintilla::KeyMod::Ctrl);
    SQ_EXPECT(received.x == 11);
    SQ_EXPECT(received.y == 13);
    SQ_EXPECT(!received.textAvailable);
    SQ_EXPECT(received.lParamKind == ScintillaQuick_lparam_kind::None);
    SQ_EXPECT(received.lParamValue == 0);
    SQ_EXPECT(!received.lParamTextAvailable);
}

void test_qml_item_loads_with_existing_typed_signal()
{
    qmlRegisterType<ScintillaQuick_item>("ScintillaQuick", 1, 0, "ScintillaQuick_item");

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "import ScintillaQuick 1.0\n"
            "Item {\n"
            "    ScintillaQuick_item {\n"
            "        id: editor\n"
            "        objectName: \"editor\"\n"
            "        onCharAdded: objectName = \"typedSignalStillUsable\"\n"
            "    }\n"
            "}\n"),
        QUrl());

    QObject* root = component.create();
    if (!component.errors().isEmpty()) {
        qWarning() << component.errors();
    }
    SQ_EXPECT(root != nullptr);
    if (!root) {
        return;
    }

    auto* qml_editor = root->findChild<ScintillaQuick_item*>(QStringLiteral("editor"));
    SQ_EXPECT(qml_editor != nullptr);
    if (qml_editor) {
        Scintilla::NotificationData scn = make_notification(Scintilla::Notification::CharAdded);
        scn.ch = 'q';
        qml_editor->notifyParent(scn);
        pump_events();
        SQ_EXPECT(qml_editor->objectName() == QStringLiteral("typedSignalStillUsable"));
    }

    delete root;
}

void test_selection_api(ScintillaQuick_item& editor)
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

void test_select_current_word_uses_scintilla_word_chars(ScintillaQuick_item& editor)
{
    auto* core = editor.findChild<Scintilla::Internal::ScintillaQuick_core*>();
    SQ_EXPECT(core != nullptr);
    if (!core) {
        return;
    }

    editor.send(SCI_SETCHARSDEFAULT);
    editor.setProperty("text", QStringLiteral("foo-bar baz"));
    editor.sends(
        SCI_SETWORDCHARS,
        0,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");

    editor.send(SCI_GOTOPOS, 4);
    core->selectCurrentWord();
    SQ_EXPECT(selection_start(editor) == 0);
    SQ_EXPECT(selection_end(editor) == 7);

    editor.send(SCI_SETCHARSDEFAULT);
}

void test_undo_redo(ScintillaQuick_item& editor)
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

void test_readonly_property(ScintillaQuick_item& editor)
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

void test_ime_attribute_bounds_and_readonly_acceptance(ScintillaQuick_item& editor)
{
    editor.setProperty("text", QStringLiteral("abc"));
    editor.setProperty("readonly", false);
    editor.send(SCI_GOTOPOS, 1);

    QList<QInputMethodEvent::Attribute> attributes;
    QTextCharFormat underline_format;
    underline_format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
    const QTextFormat text_format = underline_format;
    attributes.append(QInputMethodEvent::Attribute(
        QInputMethodEvent::TextFormat,
        -10,
        std::numeric_limits<int>::max(),
        QVariant::fromValue(text_format)));
    attributes.append(QInputMethodEvent::Attribute(
        QInputMethodEvent::Cursor,
        std::numeric_limits<int>::max(),
        0,
        QVariant()));
    attributes.append(QInputMethodEvent::Attribute(
        QInputMethodEvent::Selection,
        -20,
        std::numeric_limits<int>::max(),
        QVariant()));

    QInputMethodEvent malformed_preedit(QStringLiteral("xy"), attributes);
    QCoreApplication::sendEvent(&editor, &malformed_preedit);
    SQ_EXPECT(malformed_preedit.isAccepted());
    SQ_EXPECT(text_length(editor) > 0);

    editor.setProperty("text", QStringLiteral("abc"));
    editor.setProperty("readonly", true);
    int cursor_updates = 0;
    const QMetaObject::Connection cursor_connection = QObject::connect(
        &editor,
        &ScintillaQuick_item::cursorPositionChanged,
        &editor,
        [&]() {
            ++cursor_updates;
        });

    QInputMethodEvent readonly_commit;
    readonly_commit.setCommitString(QStringLiteral("z"));
    QCoreApplication::sendEvent(&editor, &readonly_commit);
    SQ_EXPECT(readonly_commit.isAccepted());
    SQ_EXPECT(editor.property("text").toString() == QStringLiteral("abc"));
    SQ_EXPECT(cursor_updates > 0);
    QObject::disconnect(cursor_connection);

    editor.setProperty("readonly", false);
}

void expect_utf8_measurement_positions_written(std::string_view text)
{
    const Scintilla::Internal::XYPOSITION guard = 987654.25;
    std::vector<Scintilla::Internal::XYPOSITION> positions(
        text.size() + 1,
        std::numeric_limits<Scintilla::Internal::XYPOSITION>::quiet_NaN());
    positions.back() = guard;

    const int utf16_length = QString::fromUtf8(text.data(), static_cast<int>(text.size())).size();
    Scintilla::Internal::fill_utf8_cursor_positions_from_cursor(
        text,
        utf16_length,
        positions.data(),
        [](int cursor_position) {
            return static_cast<Scintilla::Internal::XYPOSITION>(cursor_position);
        });

    for (size_t i = 0; i < text.size(); ++i) {
        SQ_EXPECT(!std::isnan(positions[i]));
    }
    SQ_EXPECT(positions.back() == guard);
}

void test_malformed_utf8_measurement_positions()
{
    const char invalid_leads[] = {
        static_cast<char>(0x80),
        static_cast<char>(0xc0),
        static_cast<char>(0xff),
    };
    expect_utf8_measurement_positions_written(std::string_view(invalid_leads, sizeof(invalid_leads)));

    const char truncated_three_byte[] = {
        'a',
        static_cast<char>(0xe2),
        static_cast<char>(0x82),
    };
    expect_utf8_measurement_positions_written(std::string_view(
        truncated_three_byte,
        sizeof(truncated_three_byte)));

    const char truncated_four_byte[] = {
        static_cast<char>(0xf0),
        static_cast<char>(0x9f),
        static_cast<char>(0x92),
    };
    expect_utf8_measurement_positions_written(std::string_view(
        truncated_four_byte,
        sizeof(truncated_four_byte)));

    const char mixed_valid_and_malformed[] = {
        'a',
        static_cast<char>(0xe2),
        'b',
        static_cast<char>(0xf0),
        static_cast<char>(0x9f),
    };
    expect_utf8_measurement_positions_written(std::string_view(
        mixed_valid_and_malformed,
        sizeof(mixed_valid_and_malformed)));
}

QQuickItem* newest_child_not_in(const QList<QQuickItem*>& before, const QList<QQuickItem*>& after)
{
    for (QQuickItem* child : after) {
        if (!before.contains(child)) {
            return child;
        }
    }
    return nullptr;
}

void test_call_tip_owned_destroy_and_external_delete(ScintillaQuick_item& editor)
{
    editor.send(SCI_CALLTIPCANCEL);
    const QList<QQuickItem*> before_first_tip = editor.childItems();

    editor.sends(SCI_CALLTIPSHOW, 0, "first tip");
    SQ_EXPECT(editor.send(SCI_CALLTIPACTIVE) != 0);
    QQuickItem* first_tip = newest_child_not_in(before_first_tip, editor.childItems());
    QPointer<QQuickItem> watched_first_tip(first_tip);
    SQ_EXPECT(first_tip != nullptr);

    editor.send(SCI_CALLTIPCANCEL);
    SQ_EXPECT(editor.send(SCI_CALLTIPACTIVE) == 0);
    SQ_EXPECT(watched_first_tip.isNull());

    const QList<QQuickItem*> before_second_tip = editor.childItems();
    editor.sends(SCI_CALLTIPSHOW, 0, "second tip");
    QQuickItem* second_tip = newest_child_not_in(before_second_tip, editor.childItems());
    QPointer<QQuickItem> watched_second_tip(second_tip);
    SQ_EXPECT(second_tip != nullptr);

    delete second_tip;
    SQ_EXPECT(watched_second_tip.isNull());

    const QList<QQuickItem*> before_third_tip = editor.childItems();
    editor.sends(SCI_CALLTIPSHOW, 0, "third tip");
    SQ_EXPECT(editor.send(SCI_CALLTIPACTIVE) != 0);
    QQuickItem* third_tip = newest_child_not_in(before_third_tip, editor.childItems());
    QPointer<QQuickItem> watched_third_tip(third_tip);
    SQ_EXPECT(third_tip != nullptr);

    editor.send(SCI_CALLTIPCANCEL);
    SQ_EXPECT(watched_third_tip.isNull());
}

void test_autocomplete_list_box_owned_destroy_and_external_delete(ScintillaQuick_item& editor)
{
    editor.send(SCI_AUTOCCANCEL);
    editor.setProperty("text", QStringLiteral("a"));
    editor.send(SCI_GOTOPOS, 1);

    const QList<QQuickItem*> before_first_list = editor.childItems();
    editor.sends(SCI_AUTOCSHOW, 0, "alpha beta");
    SQ_EXPECT(editor.send(SCI_AUTOCACTIVE) != 0);
    QQuickItem* first_list = newest_child_not_in(before_first_list, editor.childItems());
    QPointer<QQuickItem> watched_first_list(first_list);
    SQ_EXPECT(first_list != nullptr);

    editor.send(SCI_AUTOCCANCEL);
    SQ_EXPECT(editor.send(SCI_AUTOCACTIVE) == 0);
    SQ_EXPECT(watched_first_list.isNull());

    const QList<QQuickItem*> before_second_list = editor.childItems();
    editor.sends(SCI_AUTOCSHOW, 0, "gamma delta");
    QQuickItem* second_list = newest_child_not_in(before_second_list, editor.childItems());
    QPointer<QQuickItem> watched_second_list(second_list);
    SQ_EXPECT(second_list != nullptr);

    delete second_list;
    SQ_EXPECT(watched_second_list.isNull());

    editor.send(SCI_AUTOCCANCEL);
    SQ_EXPECT(editor.send(SCI_AUTOCACTIVE) == 0);

    editor.setProperty("text", QStringLiteral("a\nb"));
    editor.send(SCI_GOTOPOS, 0);
    editor.send(SCI_LINEDOWN);
    SQ_EXPECT(editor.send(SCI_LINEFROMPOSITION, current_position(editor)) == 1);

    const QList<QQuickItem*> before_third_list = editor.childItems();
    editor.sends(SCI_AUTOCSHOW, 0, "epsilon zeta");
    QQuickItem* third_list = newest_child_not_in(before_third_list, editor.childItems());
    QPointer<QQuickItem> watched_third_list(third_list);
    SQ_EXPECT(third_list != nullptr);

    editor.send(SCI_AUTOCCANCEL);
    SQ_EXPECT(editor.send(SCI_AUTOCACTIVE) == 0);
    SQ_EXPECT(watched_third_list.isNull());
}

void test_wrap_mode_toggle(ScintillaQuick_item& editor)
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

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QString font_error;
    if (!scintillaquick::shared::ensure_bundled_test_fonts_loaded(&font_error)) {
        qFatal("%s", qPrintable(font_error));
    }

    ScintillaQuick_item editor;
    editor.setWidth(640);
    editor.setHeight(480);

    test_property_roundtrip(editor);
    test_insert_and_delete(editor);
    test_sends_syncs_properties(editor);
    test_direct_function_syncs_properties(editor);
    test_direct_callbacks_bypass_send_override();
    test_modified_signal_owns_text_bytes(editor);
    test_notification_metatypes_registered();
    test_notification_received_order_and_legacy_mutation(editor);
    test_modified_signal_preserves_legacy_mutated_text_with_safe_receiver(editor);
    test_modified_signal_reuses_safe_snapshot_text_when_unchanged(editor);
    test_modified_signal_queued_owns_text_bytes(editor);
    test_notification_received_queued_modified_embedded_nul(editor);
    test_notification_received_queued_string_payload(editor);
    test_notification_received_queued_macro_payloads(editor);
    test_notification_received_queued_scalar_notification(editor);
    test_qml_item_loads_with_existing_typed_signal();
    test_selection_api(editor);
    test_select_current_word_uses_scintilla_word_chars(editor);
    test_undo_redo(editor);
    test_readonly_property(editor);
    test_ime_attribute_bounds_and_readonly_acceptance(editor);
    test_malformed_utf8_measurement_positions();
    test_call_tip_owned_destroy_and_external_delete(editor);
    test_autocomplete_list_box_owned_destroy_and_external_delete(editor);
    test_wrap_mode_toggle(editor);

    pump_events();

    if (g_failures != 0) {
        std::fprintf(stderr, "scintillaquick_smoke_test: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
