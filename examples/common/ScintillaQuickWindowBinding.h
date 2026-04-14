// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <QObject>
#include <QQuickItem>
#include <QQuickWindow>

#include <scintillaquick/ScintillaQuickItem.h>

namespace scintillaquick::examples {

inline void bindItemToWindow(ScintillaQuickItem &editor, QQuickWindow &window) {
    auto *root = window.contentItem();
    editor.setParentItem(root);
    editor.setPosition({0.0, 0.0});
    editor.setSize(root->size());

    QObject::connect(root, &QQuickItem::widthChanged, &editor, [&editor, root]() {
        editor.setWidth(root->width());
    });
    QObject::connect(root, &QQuickItem::heightChanged, &editor, [&editor, root]() {
        editor.setHeight(root->height());
    });
}

} // namespace scintillaquick::examples
