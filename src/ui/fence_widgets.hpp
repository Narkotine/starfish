#pragma once

#include <QDateTime>
#include <functional>
#include <QFont>
#include <QList>
#include <QPainter>
#include <QSize>
#include <QString>
#include <QVariantMap>

#include "core/desktop_item.hpp"
#include "core/system_metrics.hpp"

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
    // Session 81: the widget's content is text the user writes, so a
    // double-click on it opens an editor instead of doing nothing.
    bool editable = false;
};

QList<Kind> kinds();
bool isKnown(const QString& type);
bool isEditable(const QString& type);
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

// --- notes (session 81) ------------------------------------------------------
//
// A note is a widget whose content is text the user writes in place: the fence
// paints it, the product's own window edits it (the fence is an image on the
// desktop, exactly like the title and caption edits). The text lives in the
// widget's settings ("text"), so it travels with the document.
QString noteText(const core::DesktopItem& item);
core::DesktopItem withNoteText(const core::DesktopItem& item, const QString& text);
// What an empty note shows instead of nothing.
QString notePlaceholder();
// `text` wrapped into at most `maxLines` lines for `width` px, the last one
// elided on the right when something is left over (`elided` says so). Pure, so
// the probe measures the same wrapping the paint does.
QStringList noteLines(const QString& text, const QFont& font, int width, int maxLines,
                      bool* elided = nullptr);

// --- image (session 95) ---------------------------------------------------
// settings: "path" (a local picture file), "fit" ("fit": whole picture; "fill":
// cropped to the widget). StarFish only reads the file.
QString imagePath(const core::DesktopItem& item);
bool imageFills(const core::DesktopItem& item);

// --- system widgets (session 88) -------------------------------------------
//
// "cpu", "ram", "gpu", "vram" and the compact "system" group draw readings
// from one shared sampler (ui::SystemMonitor). The painter only sees it through
// these two hooks, so tests and probes can feed it their own history:
//   source(metric) -> the readings so far (nullptr: none yet)
//   demand()       -> called whenever a system widget is scheduled to repaint;
//                     the sampler runs only while somebody keeps calling it.
using MetricsSource = std::function<const core::MetricHistory*(core::Metric)>;
void setMetricsSource(const MetricsSource& source, const std::function<void(bool gpu)>& demand);
bool isSystemWidget(const QString& type);
// The text a system widget shows for `metric` right now (probe / tests).
QString metricText(const core::DesktopItem& item, core::Metric metric);

// --- engines (session 80) ---------------------------------------------------
//
// The native widgets above need no engine: they are painted by the fence like
// its icons. A future web widget will need one (WebView2 / Qt WebEngine), and
// that costs memory -- so whatever hosts it registers a probe here, and
// Settings > General shows what runs and what it weighs.
struct EngineStatus {
    QString name;            // "WebView2"
    bool running = false;    // processes up right now
    qint64 memoryBytes = -1; // working set of its processes, -1 when unknown
    int widgets = 0;         // widgets it hosts
};
using EngineProbe = std::function<EngineStatus()>;
void registerEngine(const EngineProbe& probe);
void clearEngines(); // tests
QList<EngineStatus> engines();
// The line Settings shows, e.g. "None (native widgets only)", or
// "WebView2, running (~120 MB, 2 widgets)". `nativeWidgets` adds the count of
// native widgets in the fences when there are some.
QString engineSummary(int nativeWidgets = 0);

} // namespace starfish::ui::widgets
