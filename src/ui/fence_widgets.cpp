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
    return {clock};
}

bool isKnown(const QString& type)
{
    for (const Kind& kind : kinds())
        if (kind.type == type)
            return true;
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

void paint(QPainter& painter, const QRect& rect, const core::DesktopItem& item, const PaintContext& context)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    if (item.widgetType == QLatin1String("clock"))
        paintClock(painter, rect, item, context);
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

} // namespace starfish::ui::widgets
