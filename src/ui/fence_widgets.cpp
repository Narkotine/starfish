#include "ui/fence_widgets.hpp"

#include <QCache>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
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

MetricsSource& metricsSource()
{
    static MetricsSource source;
    return source;
}

std::function<void(bool)>& metricsDemand()
{
    static std::function<void(bool)> demand;
    return demand;
}

QColor metricColour(core::Metric metric)
{
    switch (metric) {
    case core::Metric::Cpu: return QColor(0x3B, 0x82, 0xF6);
    case core::Metric::Ram: return QColor(0x22, 0xC5, 0x5E);
    case core::Metric::Gpu: return QColor(0xF5, 0x9E, 0x0B);
    case core::Metric::Vram: return QColor(0xA8, 0x55, 0xF7);
    }
    return QColor(0x3B, 0x82, 0xF6);
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
    QFont smallFont = c.font;
    smallFont.setWeight(QFont::Normal);
    // The long form ("lundi 2 novembre") when it reads at a decent size.
    QString date = clockDate(item, c.now, true);
    QFont dateFont = fitted(smallFont, date, dateBox.size(), 0.62);
    if (dateFont.pixelSize() < 11) {
        date = clockDate(item, c.now, false);
        dateFont = fitted(smallFont, date, dateBox.size(), 0.62);
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

// Session 95: a picture the user chose, fitted (whole, letterboxed) or filling
// the widget (cropped). Decoded once per file version and kept small (longest
// side 2048 px), then scaled once per size: a repaint only blits.
QImage decodedImage(const QString& path)
{
    static QCache<QString, QImage> cache(96 * 1024); // KB
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile())
        return {};
    const QString key = path + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
    if (QImage* hit = cache.object(key))
        return *hit;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && qMax(size.width(), size.height()) > 2048)
        reader.setScaledSize(size.scaled(2048, 2048, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull())
        return {};
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    cache.insert(key, new QImage(image), qMax<qsizetype>(1, image.sizeInBytes() / 1024));
    return image;
}

void paintImage(QPainter& painter, const QRect& rect, const core::DesktopItem& item, const PaintContext& c)
{
    const QRect card = rect.adjusted(2, 2, -2, -2);
    if (card.width() < 8 || card.height() < 8)
        return;
    const QString path = imagePath(item);
    const QImage image = decodedImage(path);
    if (image.isNull()) {
        painter.setPen(QPen(QColor(c.text.red(), c.text.green(), c.text.blue(), 0x60), 1, Qt::DashLine));
        painter.setBrush(QColor(0, 0, 0, 0x30));
        painter.drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        QFont font = c.font;
        font.setPixelSize(qBound(9, card.height() / 6, 13));
        const QString hint = path.isEmpty()
                                 ? QCoreApplication::translate("FenceWidgets", "No image yet (Configure widget...)")
                                 : QCoreApplication::translate("FenceWidgets", "Image not found");
        const QRect box = card.adjusted(6, 6, -6, -6);
        painter.setFont(font);
        if (c.shadow) {
            painter.setPen(QColor(0, 0, 0, 0xA0));
            painter.drawText(box.translated(1, 1), Qt::AlignCenter | Qt::TextWordWrap, hint);
        }
        painter.setPen(c.text);
        painter.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, hint);
        return;
    }
    const bool fill = imageFills(item);
    const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const QSize target = (QSizeF(card.size()) * dpr).toSize();
    static QCache<QString, QImage> scaled(48 * 1024);
    const QString key = path + QLatin1Char('|') + QString::number(image.cacheKey()) + QLatin1Char('|')
                        + QString::number(target.width()) + QLatin1Char('x') + QString::number(target.height())
                        + (fill ? QStringLiteral("|fill") : QStringLiteral("|fit"));
    QImage picture;
    if (QImage* hit = scaled.object(key)) {
        picture = *hit;
    } else {
        picture = image.scaled(target, fill ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
        if (fill) {
            picture = picture.copy((picture.width() - target.width()) / 2, (picture.height() - target.height()) / 2,
                                   target.width(), target.height());
        }
        scaled.insert(key, new QImage(picture), qMax<qsizetype>(1, picture.sizeInBytes() / 1024));
    }
    picture.setDevicePixelRatio(dpr);
    const QSizeF shown = QSizeF(picture.size()) / dpr;
    const QRectF place(card.x() + (card.width() - shown.width()) / 2.0, card.y() + (card.height() - shown.height()) / 2.0,
                       shown.width(), shown.height());
    QPainterPath clip;
    clip.addRoundedRect(place, 6, 6);
    painter.save();
    painter.setClipPath(clip, Qt::IntersectClip);
    painter.drawImage(place.topLeft(), picture);
    painter.restore();
}

// Session 88: a bar from 0 to 100 %.
void paintBar(QPainter& painter, const QRectF& box, float percent, const QColor& colour, const PaintContext& c)
{
    const qreal radius = box.height() / 2.0;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(c.text.red(), c.text.green(), c.text.blue(), 0x30));
    painter.drawRoundedRect(box, radius, radius);
    if (percent <= 0)
        return;
    QRectF fill = box;
    fill.setWidth(qMax(box.height(), box.width() * qMin(100.0f, percent) / 100.0));
    painter.setBrush(colour);
    painter.drawRoundedRect(fill, radius, radius);
}

// Session 88: the history as a filled line, newest on the right.
void paintGraph(QPainter& painter, const QRectF& box, const QVector<float>& values, int slotCount, const QColor& colour,
                const PaintContext& c)
{
    painter.setPen(QPen(QColor(c.text.red(), c.text.green(), c.text.blue(), 0x28), 1));
    painter.setBrush(QColor(0, 0, 0, 0x22));
    painter.drawRoundedRect(box, 3, 3);
    if (values.isEmpty() || slotCount < 2)
        return;
    const qreal step = box.width() / qreal(slotCount - 1);
    QPainterPath line;
    QPainterPath area;
    bool open = false;
    qreal lastX = 0;
    const int offset = slotCount - int(values.size());
    for (int i = 0; i < values.size(); ++i) {
        const qreal x = box.left() + (offset + i) * step;
        if (values.at(i) < 0) {
            if (open) {
                area.lineTo(lastX, box.bottom());
                area.closeSubpath();
            }
            open = false;
            continue;
        }
        const qreal y = box.bottom() - box.height() * qBound(0.0f, values.at(i), 100.0f) / 100.0;
        if (!open) {
            line.moveTo(x, y);
            area.moveTo(x, box.bottom());
            area.lineTo(x, y);
            open = true;
        } else {
            line.lineTo(x, y);
            area.lineTo(x, y);
        }
        lastX = x;
    }
    if (open) {
        area.lineTo(lastX, box.bottom());
        area.closeSubpath();
    }
    QColor fill = colour;
    fill.setAlpha(0x55);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawPath(area);
    painter.setPen(QPen(colour, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(line);
}

void drawAligned(QPainter& painter, const QRect& box, const QString& text, const QFont& font, Qt::Alignment align,
                 const PaintContext& c, const QColor& colour)
{
    painter.setFont(font);
    if (c.shadow) {
        painter.setPen(QColor(0, 0, 0, 0xA0));
        painter.drawText(box.translated(1, 1), align | Qt::TextSingleLine, text);
    }
    painter.setPen(colour);
    painter.drawText(box, align | Qt::TextSingleLine, text);
}

core::MetricPoint latestOf(core::Metric metric)
{
    const core::MetricHistory* history = metricsSource() ? metricsSource()(metric) : nullptr;
    return history ? history->latest() : core::MetricPoint();
}

QVector<float> graphOf(core::Metric metric, const core::MetricSettings& s)
{
    const core::MetricHistory* history = metricsSource() ? metricsSource()(metric) : nullptr;
    if (!history)
        return {};
    return core::downsample(history->last(s.history * s.interval), s.interval, s.history, s.showFree);
}

QString metricLabel(core::Metric metric, bool free)
{
    const QString name = core::metricShortName(metric);
    if (!free)
        return name;
    return QCoreApplication::translate("FenceWidgets", "%1 free").arg(name);
}

// One metric, alone in its widget: the label and value, then the bar or graph.
void paintMetric(QPainter& painter, const QRect& rect, const core::DesktopItem& item, core::Metric metric,
                 const PaintContext& c)
{
    const core::MetricSettings s = core::MetricSettings::fromMap(item.widgetSettings, item.widgetType);
    const QRect inner = rect.adjusted(4, 3, -4, -3);
    if (inner.width() < 16 || inner.height() < 12)
        return;
    const core::MetricPoint point = latestOf(metric);
    const QString value = core::formatMetric(metric, point, s.showFree, s.bytes, uiLocale());
    const QString label = metricLabel(metric, s.showFree);
    const QColor colour = metricColour(metric);
    QFont base = c.font;
    base.setWeight(QFont::DemiBold);
    if (s.mode == core::MetricSettings::Mode::Number) {
        const int labelHeight = inner.height() * 30 / 100;
        const QRect labelBox(inner.x(), inner.y(), inner.width(), labelHeight);
        const QRect valueBox(inner.x(), inner.y() + labelHeight, inner.width(), inner.height() - labelHeight);
        QFont labelFont = c.font;
        drawText(painter, labelBox, label, fitted(labelFont, label, labelBox.size(), 0.8), c);
        drawText(painter, valueBox, value, fitted(base, value, valueBox.size(), 0.78), c);
        return;
    }
    // Header line: label left, value right.
    const int header = qBound(12, inner.height() / 3, 22);
    const QRect head(inner.x(), inner.y(), inner.width(), header);
    QFont headFont = c.font;
    headFont.setPixelSize(qMax(9, header - 3));
    drawAligned(painter, head, label, headFont, Qt::AlignLeft | Qt::AlignVCenter, c, c.text);
    headFont.setWeight(QFont::DemiBold);
    drawAligned(painter, head, value, headFont, Qt::AlignRight | Qt::AlignVCenter, c, c.text);
    const QRect body(inner.x(), inner.y() + header + 2, inner.width(), inner.height() - header - 2);
    if (body.height() < 4)
        return;
    if (s.mode == core::MetricSettings::Mode::Bar) {
        const qreal h = qBound(4.0, body.height() * 0.45, 12.0);
        paintBar(painter, QRectF(body.x(), body.center().y() - h / 2, body.width(), h),
                 core::shownPercent(point, s.showFree), colour, c);
        return;
    }
    paintGraph(painter, QRectF(body).adjusted(0.5, 0.5, -0.5, -0.5), graphOf(metric, s), s.history, colour, c);
}

// The compact group: one row per chosen metric -- name, value, bar or sparkline.
void paintGroup(QPainter& painter, const QRect& rect, const core::DesktopItem& item, const PaintContext& c)
{
    const core::MetricSettings s = core::MetricSettings::fromMap(item.widgetSettings, item.widgetType);
    const QRect inner = rect.adjusted(4, 3, -4, -3);
    if (s.metrics.isEmpty() || inner.width() < 24 || inner.height() < 10)
        return;
    const int rows = int(s.metrics.size());
    const int rowHeight = inner.height() / rows;
    QFont font = c.font;
    font.setPixelSize(qBound(9, rowHeight * 55 / 100, 14));
    QFont bold = font;
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics fm(font);
    const int nameWidth = fm.horizontalAdvance(QStringLiteral("VRAM")) + 6;
    const int valueWidth = QFontMetrics(bold).horizontalAdvance(QStringLiteral("00.0 GB")) + 4;
    for (int i = 0; i < rows; ++i) {
        const core::Metric metric = s.metrics.at(i);
        const QRect row(inner.x(), inner.y() + i * rowHeight, inner.width(), rowHeight);
        const core::MetricPoint point = latestOf(metric);
        const bool bytes = s.bytes && core::hasBytes(metric);
        const QString value = core::formatMetric(metric, point, s.showFree, bytes, uiLocale());
        drawAligned(painter, QRect(row.x(), row.y(), nameWidth, row.height()), core::metricShortName(metric), font,
                    Qt::AlignLeft | Qt::AlignVCenter, c, c.text);
        if (s.mode == core::MetricSettings::Mode::Number) {
            drawAligned(painter, row.adjusted(nameWidth, 0, 0, 0), value, bold, Qt::AlignRight | Qt::AlignVCenter, c,
                        c.text);
            continue;
        }
        drawAligned(painter, QRect(row.right() - valueWidth + 1, row.y(), valueWidth, row.height()), value, bold,
                    Qt::AlignRight | Qt::AlignVCenter, c, c.text);
        const QRect band(row.x() + nameWidth, row.y() + 2, row.width() - nameWidth - valueWidth - 4, row.height() - 4);
        if (band.width() < 8)
            continue;
        if (s.mode == core::MetricSettings::Mode::Bar) {
            const qreal h = qBound(3.0, band.height() * 0.5, 8.0);
            paintBar(painter, QRectF(band.x(), band.center().y() - h / 2, band.width(), h),
                     core::shownPercent(point, s.showFree), metricColour(metric), c);
        } else {
            paintGraph(painter, QRectF(band), graphOf(metric, s), s.history, metricColour(metric), c);
        }
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

    Kind image; // session 95
    image.type = QStringLiteral("image");
    image.label = QCoreApplication::translate("FenceWidgets", "Image");
    image.defaultSpan = QSize(2, 2);
    image.defaults = {{QStringLiteral("path"), QString()}, {QStringLiteral("fit"), QStringLiteral("fit")}};

    Kind notes; // session 81
    notes.type = QStringLiteral("notes");
    notes.label = QCoreApplication::translate("FenceWidgets", "Notes");
    notes.defaultSpan = QSize(2, 2);
    notes.defaults = {{QStringLiteral("text"), QString()}};
    notes.editable = true;

    // Session 88: system widgets.
    const auto system = [](const QString& type, const QString& label, const QSize& span) {
        Kind kind;
        kind.type = type;
        kind.label = label;
        kind.defaultSpan = span;
        kind.defaults = core::MetricSettings::defaults(type);
        return kind;
    };
    return {clock,
            notes,
            image,
            system(QStringLiteral("cpu"), QCoreApplication::translate("FenceWidgets", "CPU usage"), QSize(2, 1)),
            system(QStringLiteral("ram"), QCoreApplication::translate("FenceWidgets", "Memory (RAM)"), QSize(2, 1)),
            system(QStringLiteral("gpu"), QCoreApplication::translate("FenceWidgets", "GPU usage"), QSize(2, 1)),
            system(QStringLiteral("vram"), QCoreApplication::translate("FenceWidgets", "Video memory (VRAM)"), QSize(2, 1)),
            system(QStringLiteral("system"), QCoreApplication::translate("FenceWidgets", "System (compact)"),
                   QSize(2, 2))};
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

QString imagePath(const core::DesktopItem& item)
{
    return item.widgetSettings.value(QStringLiteral("path")).toString();
}

bool imageFills(const core::DesktopItem& item)
{
    return item.widgetSettings.value(QStringLiteral("fit")).toString() == QLatin1String("fill");
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
    // Session 83: only the lines the card can show are ever built. A pasted wall
    // of text used to be wrapped in full on *every* repaint of the fence -- and
    // an over-wide word was cut by measuring every one of its prefixes, which is
    // quadratic: 2 000 characters in one run measured 22 s, 20 000 never
    // finished. The paint runs on the UI thread while the user types, so the
    // product froze (and the note was never saved: the auto-save never got its
    // turn). Stopping at `maxLines` makes a huge note cost exactly what a short
    // one does, and the ellipsis says the rest is still there and still saved.
    bool more = false;

    // Does `word` certainly stick out of `width`? Measured on a *bounded window*
    // of characters, never on the whole run: shaping a 100 000-character string
    // to answer "does it fit" is what cost a second per repaint.
    const auto tooWide = [&metrics, width](const QString& word) {
        const int size = int(word.size());
        int window = qBound(64, width, 4096); // characters probed at once
        while (window < size) {
            if (metrics.horizontalAdvance(word.left(window)) > width)
                return true;
            window *= 4; // only narrow characters (a wall of 'i') get this far
        }
        return metrics.horizontalAdvance(word) > width;
    };

    // How many characters of `word` fit `width`, by bisection over the prefixes
    // that can possibly fit: measuring every prefix is the quadratic scan that
    // froze the product.
    const auto fittingPrefix = [&metrics, width](const QString& word) {
        const auto fits = [&](int count) { return metrics.horizontalAdvance(word.left(count)) <= width; };
        const int size = int(word.size());
        int lo = 0;  // known to fit
        int hi = 1;  // first prefix that does not
        while (hi < size && fits(hi)) {
            lo = hi;
            hi *= 2;
        }
        if (hi > size)
            hi = size;
        if (lo == 0 && !fits(1))
            return 1; // not even one character fits: cut one anyway, so it ends
        while (lo + 1 < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (fits(mid))
                lo = mid;
            else
                hi = mid;
        }
        return qBound(1, lo, qMax(1, size - 1)); // never 0, never all of it
    };

    // The user's own line breaks are kept: a note is written, not re-flowed
    // away. Each paragraph is then wrapped on spaces, and a word wider than the
    // card is cut by characters instead of sticking out of it.
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    for (const QString& paragraph : paragraphs) {
        if (out.size() >= maxLines) {
            // A blank paragraph has nothing to add to a card that is already
            // full: it is only the text that would have shown that is "more".
            if (paragraph.trimmed().isEmpty())
                continue;
            more = true;
            break;
        }
        if (paragraph.trimmed().isEmpty()) {
            out << QString();
            continue;
        }
        QString line;
        const QStringList words = paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString& word : words) {
            const QString candidate = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (!tooWide(candidate)) {
                line = candidate;
                continue;
            }
            if (!line.isEmpty()) {
                out << line;
                line.clear();
                if (out.size() >= maxLines) {
                    more = true; // this word and everything after it is left over
                    break;
                }
            }
            QString rest = word;
            while (rest.size() > 1 && tooWide(rest)) {
                const int cut = fittingPrefix(rest);
                const bool roomLeft = out.size() + 1 < maxLines;
                out << rest.left(cut);
                rest = rest.mid(cut);
                if (!roomLeft) {
                    more = !rest.isEmpty();
                    break;
                }
            }
            if (more)
                break;
            line = rest;
        }
        if (more)
            break;
        out << line;
    }
    while (!out.isEmpty() && out.last().isEmpty())
        out.removeLast();
    if (more || out.size() > maxLines) {
        out = out.mid(0, maxLines);
        if (elided)
            *elided = true;
        // The last line that fits says there was more, rather than the card
        // ending mid-sentence with no sign of it. (A note whose first lines are
        // blank keeps nothing at all: one ellipsis line beats an empty card.)
        if (out.isEmpty())
            out << metrics.elidedText(QString(QChar(0x2026)), Qt::ElideRight, width);
        else
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
    else if (item.widgetType == QLatin1String("image"))
        paintImage(painter, rect, item, context);
    else if (core::Metric metric; core::metricFromType(item.widgetType, &metric))
        paintMetric(painter, rect, item, metric, context);
    else if (item.widgetType == QLatin1String("system"))
        paintGroup(painter, rect, item, context);
    else
        paintUnknown(painter, rect, item, context);
    painter.restore();
}

void setMetricsSource(const MetricsSource& source, const std::function<void(bool)>& demand)
{
    metricsSource() = source;
    metricsDemand() = demand;
}

bool isSystemWidget(const QString& type)
{
    return core::isSystemWidgetType(type);
}

QString metricText(const core::DesktopItem& item, core::Metric metric)
{
    const core::MetricSettings s = core::MetricSettings::fromMap(item.widgetSettings, item.widgetType);
    return core::formatMetric(metric, latestOf(metric), s.showFree, s.bytes && core::hasBytes(metric), uiLocale());
}

int msUntilNextChange(const core::DesktopItem& item, const QDateTime& now)
{
    if (isSystemWidget(item.widgetType)) {
        // Session 88: keeps the shared sampler alive while this is painted, and
        // repaints once a second, like the sampler ticks.
        if (metricsDemand()) {
            const core::MetricSettings s = core::MetricSettings::fromMap(item.widgetSettings, item.widgetType);
            metricsDemand()(s.metrics.contains(core::Metric::Gpu) || s.metrics.contains(core::Metric::Vram));
        }
        return 1000 - now.time().msec() + 40;
    }
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
