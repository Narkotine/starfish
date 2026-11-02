#pragma once

#include <QDateTime>
#include <QFont>
#include <QList>
#include <QPainter>
#include <QSize>
#include <QString>
#include <QVariantMap>

#include "core/desktop_item.hpp"

namespace starfish::ui::widgets {

// Session 79: StarFish's own widgets, living in a fence's grid like tiles.
//
// A widget is a DesktopItem whose `widgetType` is set and whose path is
// "widget:<uuid>" (never a file: nothing ever reaches the shell or the disk
// with it). It spans `span` cells (columns x rows) and keeps its settings as a
// small JSON-like map -- the same shape a third-party web widget's manifest
// will have later (id / type, span, settings).

struct Kind {
    QString type;      // "clock"
    QString label;     // translated, for menus
    QSize defaultSpan; // in cells
    QVariantMap defaults;
};

QList<Kind> kinds();
bool isKnown(const QString& type);
QString displayName(const QString& type); // translated; the type itself if unknown

// A new widget of `type`, with its default span and settings.
core::DesktopItem create(const QString& type);
// A widget read back from the document (an unknown type is kept as it is).
core::DesktopItem restore(const QString& path, const QString& type, const QSize& span, const QVariantMap& settings);

// The span sizes a widget offers in its menu.
QList<QSize> spanChoices();

struct PaintContext {
    QColor text;
    bool shadow = true;
    QFont font;
    QDateTime now;
};

// Draws `item` into `rect` (content-local, the whole span).
void paint(QPainter& painter, const QRect& rect, const core::DesktopItem& item, const PaintContext& context);

// Milliseconds until `item` looks different (aligned on the clock's tick), or
// -1 when it never changes by itself.
int msUntilNextChange(const core::DesktopItem& item, const QDateTime& now);

// The clock's text, exposed for the probe.
QString clockTime(const core::DesktopItem& item, const QDateTime& now);
QString clockDate(const core::DesktopItem& item, const QDateTime& now, bool longForm);

} // namespace starfish::ui::widgets
