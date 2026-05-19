// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <scintillaquick/scintillaquick_item.h>

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQuickWindow>
#include <QWheelEvent>
#include <Qt>

// Shared Qt-event-injection helpers used by tests and benchmarks that
// drive ScintillaQuick_item through synthetic input. Each helper computes
// the local/scene/global coordinate triple Qt expects and dispatches the
// event synchronously to the target item.

inline void send_wheel_event(
    QQuickWindow&         window,
    ScintillaQuick_item&  editor,
    QPointF               local_pos,
    QPoint                pixel_delta,
    QPoint                angle_delta,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF scene_pos  = editor.mapToScene(local_pos);
    const QPointF global_pos = window.mapToGlobal(scene_pos.toPoint());
    QWheelEvent event(
        local_pos, global_pos, pixel_delta, angle_delta,
        Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&editor, &event);
}

inline void send_mouse_event(
    QQuickWindow&         window,
    ScintillaQuick_item&  editor,
    QEvent::Type          type,
    QPointF               local_pos,
    Qt::MouseButton       button,
    Qt::MouseButtons      buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF scene_pos  = editor.mapToScene(local_pos);
    const QPoint  global_pos = window.mapToGlobal(scene_pos.toPoint());
    QMouseEvent event(type, local_pos, scene_pos, global_pos, button, buttons, modifiers);
    QCoreApplication::sendEvent(&editor, &event);
}

inline void send_key_press_event(
    QObject&              target,
    int                   key,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(&target, &event);
}
