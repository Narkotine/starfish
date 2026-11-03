#include "ui/fence_widgets.hpp"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QLocale>
#include <QPainterPath>
#include <QUuid>

#include "core/i18n.hpp"

namespace starfish::ui::widgets {

namespace {

QLocale uiLocale()
{
    const QString language = core::i18n::installedLanguage();
    return language.isEmpty() ? QLocale::system() : QLocale(language);
}

bool flag(const core::DesktopItem& item, const char* key)
{
    return item.widgetSettings.value(QString::fromLatin1(key)).toBool();
}

// The largest pixel size at which `text` fits `box` (bounded by its height).
QFont fitted(QFont font, const QString& text, const QSize& box, qreal heightShare)
{
    font.setPixelSize(100);
    const int width = qMax(1, QFontMetrics(font).horizontalAdvance(text));
    const int byWidth = int(100.0 * box.width() * 0.94 / width);
    const int byHeight = int(box.height() * heightShare);
    font.setPixelSize(qMax(7, qMin(byWidth, byHeight)));
    return font;
}

void drawText(QPainter& painter, const QRect& box, const QString& text, const QFont& font, const PaintContext& c)
{
    painter.setFont(font);
    if (c.shadow) {
        painter.setPen(QColor(0, 0, 0, 0xA0));
        painter.drawText(box.translated(1, 1), Qt::AlignCenter | Qt::TextSingleLine, text);
    }
    painter.setPen(c.text);
    painter.drawText(box, Qt::AlignCenter | Qt::TextSingleLine, text);
}

void paintClock(QPainter& painter, const QRect& rect, const core::DesktopItem& item, const PaintContext& c)
{
    const QRect inner = rect.adjusted(3, 2, -3, -2);
    const bool withDate = flag(item, "date");
    const QString time = clockTime(item, c.now);
    QFont base = c.font;
    base.setWeight(QFont::DemiBold);
    if (!withDate) {
        drawText(painter, inner, time, fitted(base, time, inner.size(), 0.78), c);
        return;
    }
    const int timeHeight = inner.height() * 62 / 100;
    const QRect timeBox(inner.x(), inner.y(), inner.width(), timeHeight);
    const QRect dateBox(inner.x(), inner.y() + timeHeight, inner.width(), inner.height() - timeHeight);
    drawText(painter, timeBox, time, fitted(base, time, timeBox.size(), 0.82), c);
    QFont small = c.font;
    small.setWeight(QFont::Normal);
    // The long form ("lundi 2 novembre") when it reads at a decent size.
    QString date = clockDate(item, c.now, true);
    QFont dateFont = fitted(small, date, dateBox.size(), 0.62);
    if (dateFont.pixelSize() < 11) {
        date = clockDate(item, c.now, false);
        dateFont = fitted(small, date, dateBox.size(), 0.62);
    }
    drawText(painter, dateBox, date, dateFont, c);
}

// Session 81: a note -- the widget's text, wrapped into its cell. The card is
// what makes a note readable on any panel colour: a recessed surface and a
// hairline, then the text (the user's, or the hint that says how to write one).
void paintNotes(QPainter& painter, const QRect& rect, const core::DesktopItem& item, const PaintContext& c)
{
    const QRect card = rect.adjusted(2, 2, -2, -2);
    if (card.width() < 12 || card.height() < 12)
        return;
    painter.setPen(QPen(QColor(c.text.red(), c.text.green(), c.text.blue(), 0x38), 1));
    painter.setBrush(QColor(0, 0, 0, 0x30));
    painter.drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

    const QRect inner = card.adjusted(8, 6, -8, -6);
    if (inner.width() <= 0 || inner.height() <= 0)
        return;
    QFont font = c.font;
    font.setWeight(QFont::Normal);
    font.setPixelSize(qBound(10, qMin(14, inner.height() / 6), 14));
    const QFontMetrics metrics(font);
    const int lineHeight = qMax(1, metrics.lineSpacing());
    const QString text = noteText(item).trimmed();
    const bool empty = text.isEmpty();
    const QStringList lines =
        noteLines(empty ? notePlaceholder() : text, font, inner.width(), qMax(1, inner.height() / lineHeight));
    painter.setFont(font);
    int top = inner.top();
    for (const QString& line : lines) {
        const QRect box(inner.x(), top, inner.width(), lineHeight);
        if (c.shadow) {
            painter.setPen(QColor(0, 0, 0, 0x90));
            painter.drawText(box.translated(1, 1), Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, line);
        }
        QColor colour = c.text;
        if (empty)
            colour.setAlpha(0x90); // the hint is not the note
        painter.setPen(colour);
        painter.drawText(box, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, line);
        top += lineHeight;
    }
}

void paintUnknown(QPainter& painter, const QRect& rect, const core::DesktopItem& item, const PaintContext& c)
{
    // A widget this version does not know (a newer StarFish wrote it): kept in
    // the document untouched, shown as a placeholder.
    painter.setPen(QPen(QColor(c.text.red(), c.text.green(), c.text.blue(), 120), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect).adjusted(2.5, 2.5, -2.5, -2.5), 6, 6);
    QFont font = c.font;
    font.setPixelSize(qMax(9, qMin(13, rect.height() / 3)));
    drawText(painter, rect, QStringLiteral("? ") + item.widgetType, font, c);
}

} // namespace

QList<Kind> kinds()
{
    Kind clock;
    clock.type = QStringLiteral("clock");
    clock.label = QCoreApplication::translate("FenceWidgets", "Clock");
    clock.defaultSpan = QSize(2, 1);
    clock.defaults = {{QStringLiteral("seconds"), false},
                      {QStringLiteral("date"), true},
                      {QStringLiteral("h12"), false}};

    Kind notes; // session 81
    notes.type = QStringLiteral("notes");
    notes.label = QCoreApplication::translate("FenceWidgets", "Notes");
    notes.defaultSpan = QSize(2, 2);
    notes.defaults = {{QStringLiteral("text"), QString()}};
    notes.editable = true;
    return {clock, notes};
}

bool isKnown(const QString& type)
{
    for (const Kind& kind : kinds())
        if (kind.type == type)
            return true;
    return false;
}

bool isEditable(const QString& type)
{
    for (const Kind& kind : kinds())
        if (kind.type == type)
            return kind.editable;
    return false;
}

QString displayName(const QString& type)
{
    for (const Kind& kind : kinds())
        if (kind.type == type)
            return kind.label;
    return type;
}

core::DesktopItem create(const QString& type)
{
    for (const Kind& kind : kinds()) {
        if (kind.type == type)
            return restore(QStringLiteral("widget:") + QUuid::createUuid().toString(QUuid::WithoutBraces), type,
                           kind.defaultSpan, kind.defaults);
    }
    return {};
}

core::DesktopItem restore(const QString& path, const QString& type, const QSize& span, const QVariantMap& settings)
{
    core::DesktopItem item;
    item.path = path;
    item.widgetType = type;
    item.span = QSize(qBound(1, span.width(), 8), qBound(1, span.height(), 8));
    item.widgetSettings = settings;
    item.name = displayName(type);
    item.fileName = item.name;
    return item;
}

QList<QSize> spanChoices()
{
    return {QSize(1, 1), QSize(2, 1), QSize(3, 1), QSize(2, 2), QSize(3, 2)};
}

QString clockTime(const core::DesktopItem& item, const QDateTime& now)
{
    const bool seconds = flag(item, "seconds");
    const QString format = flag(item, "h12") ? (seconds ? QStringLiteral("h:mm:ss AP") : QStringLiteral("h:mm AP"))
                                             : (seconds ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm"));
    return uiLocale().toString(now.time(), format);
}

QString clockDate(const core::DesktopItem&, const QDateTime& now, bool longForm)
{
    QString date = uiLocale().toString(now.date(), longForm ? QStringLiteral("dddd d MMMM") : QStringLiteral("ddd d MMM"));
    if (!date.isEmpty())
        date[0] = date.at(0).toUpper();
    return date;
}

QString noteText(const core::DesktopItem& item)
{
    return item.widgetSettings.value(QStringLiteral("text")).toString();
}

core::DesktopItem withNoteText(const core::DesktopItem& item, const QString& text)
{
    core::DesktopItem note = item;
    note.widgetSettings.insert(QStringLiteral("text"), text);
    return note;
}

QString notePlaceholder()
{
    return QCoreApplication::translate("FenceWidgets", "Double-click to write");
}

QStringList noteLines(const QString& text, const QFont& font, int width, int maxLines, bool* elided)
{
    if (elided)
        *elided = false;
    QStringList out;
    if (text.isEmpty() || width <= 0 || maxLines <= 0)
        return out;
    const QFontMetrics metrics(font);
    // The user's own line breaks are kept: a note is written, not re-flowed
    // away. Each paragraph is then wrapped on spaces, and a word wider than the
    // card is cut by characters instead of sticking out of it.
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    for (const QString& paragraph : paragraphs) {
        if (paragraph.trimmed().isEmpty()) {
            out << QString();
            continue;
        }
        QString line;
        const QStringList words = paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString& word : words) {
            const QString candidate = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (metrics.horizontalAdvance(candidate) <= width) {
                line = candidate;
                continue;
            }
            if (!line.isEmpty()) {
                out << line;
                line.clear();
            }
            QString rest = word;
            while (metrics.horizontalAdvance(rest) > width && rest.size() > 1) {
                int cut = rest.size() - 1;
                while (cut > 1 && metrics.horizontalAdvance(rest.left(cut)) > width)
                    --cut;
                out << rest.left(cut);
                rest = rest.mid(cut);
            }
            line = rest;
        }
        out << line;
    }
    while (!out.isEmpty() && out.last().isEmpty())
        out.removeLast();
    if (out.size() > maxLines) {
        out = out.mid(0, maxLines);
        if (elided)
            *elided = true;
        // The last line that fits says there was more, rather than the card
        // ending mid-sentence with no sign of it.
        out.last() = metrics.elidedText(out.last() + QChar(0x2026), Qt::ElideRight, width);
    }
    return out;
}

void paint(QPainter& painter, const QRect& rect, const core::DesktopItem& item, const PaintContext& context)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    if (item.widgetType == QLatin1String("clock"))
        paintClock(painter, rect, item, context);
    else if (item.widgetType == QLatin1String("notes"))
        paintNotes(painter, rect, item, context);
    else
        paintUnknown(painter, rect, item, context);
    painter.restore();
}

int msUntilNextChange(const core::DesktopItem& item, const QDateTime& now)
{
    if (item.widgetType != QLatin1String("clock"))
        return -1;
    const QTime t = now.time();
    // A few ms past the boundary, so the new second / minute is the one drawn.
    if (flag(item, "seconds"))
        return 1000 - t.msec() + 15;
    return 60000 - (t.second() * 1000 + t.msec()) + 15;
}

namespace {
QList<EngineProbe>& probes()
{
    static QList<EngineProbe> list;
    return list;
}
} // namespace

void registerEngine(const EngineProbe& probe)
{
    if (probe)
        probes().append(probe);
}

void clearEngines()
{
    probes().clear();
}

QList<EngineStatus> engines()
{
    QList<EngineStatus> list;
    for (const EngineProbe& probe : probes())
        list.append(probe());
    return list;
}

QString engineSummary(int nativeWidgets)
{
    QStringList parts;
    for (const EngineStatus& engine : engines()) {
        QString line = engine.running
                           ? QCoreApplication::translate("FenceWidgets", "%1, running").arg(engine.name)
                           : QCoreApplication::translate("FenceWidgets", "%1, idle").arg(engine.name);
        QStringList details;
        if (engine.running && engine.memoryBytes >= 0)
            details << QCoreApplication::translate("FenceWidgets", "~%1 MB")
                           .arg(qMax<qint64>(1, (engine.memoryBytes + 512 * 1024) / (1024 * 1024)));
        if (engine.widgets > 0)
            details << (engine.widgets == 1 ? QCoreApplication::translate("FenceWidgets", "1 widget")
                                            : QCoreApplication::translate("FenceWidgets", "%1 widgets").arg(engine.widgets));
        if (!details.isEmpty())
            line += QStringLiteral(" (%1)").arg(details.join(QStringLiteral(", ")));
        parts << line;
    }
    if (parts.isEmpty()) {
        QString none = QCoreApplication::translate("FenceWidgets", "None (native widgets only)");
        if (nativeWidgets > 0)
            none += QStringLiteral(" \u00B7 ")
                    + (nativeWidgets == 1
                           ? QCoreApplication::translate("FenceWidgets", "1 native widget")
                           : QCoreApplication::translate("FenceWidgets", "%1 native widgets").arg(nativeWidgets));
        return none;
    }
    return parts.join(QStringLiteral(" ; "));
}

} // namespace starfish::ui::widgets
