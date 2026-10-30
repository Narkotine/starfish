#include "ui/drag_image.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace starfish::ui {

namespace {
const QColor kPlate(0x99, 0xD1, 0xFF, 0x66);
const QColor kPlateBorder(0xCC, 0xE8, 0xFF, 0xB0);
const QColor kText(0xFF, 0xFF, 0xFF);
const QColor kShadow(0x00, 0x00, 0x00, 0xB0);

// Two lines at most, broken at the last space that fits, the second elided
// ("...") -- like the desktop's own captions.
QStringList captionLines(const QString& text, const QFontMetrics& fm, int width)
{
    if (fm.horizontalAdvance(text) <= width)
        return {text};
    int cut = text.size();
    while (cut > 1 && fm.horizontalAdvance(text.left(cut)) > width)
        --cut;
    const int space = text.lastIndexOf(QLatin1Char(' '), cut);
    const int at = space > 0 ? space : cut;
    const QString rest = text.mid(space > 0 ? at + 1 : at);
    return {text.left(at), fm.elidedText(rest, Qt::ElideRight, width)};
}
} // namespace

QImage DragImage::compose(const QList<DragTile>& tiles, const QFont& font, qreal opacity, qreal dpr,
                          QPoint* topLeft)
{
    QRect bounds;
    for (const DragTile& t : tiles)
        bounds = bounds.united(t.icon.united(t.label));
    if (bounds.isEmpty())
        return {};
    bounds.adjust(-4, -4, 4, 4);
    if (topLeft)
        *topLeft = bounds.topLeft();

    const qreal scale = qMax<qreal>(1.0, dpr);
    QImage layer(QSize(qCeil(bounds.width() * scale), qCeil(bounds.height() * scale)),
                 QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.scale(scale, scale);
        p.translate(-bounds.topLeft());
        p.setFont(font);
        const QFontMetrics fm(font);
        for (const DragTile& t : tiles) {
            const QRectF plate = QRectF(t.icon.united(t.label)).adjusted(-2.5, -2.5, 2.5, 2.5);
            p.setPen(QPen(kPlateBorder, 1));
            p.setBrush(kPlate);
            p.drawRoundedRect(plate, 4, 4);
            if (!t.pixmap.isNull()) {
                const QSize s = t.pixmap.size().scaled(t.icon.size(), Qt::KeepAspectRatio);
                p.drawPixmap(QRect(t.icon.x() + (t.icon.width() - s.width()) / 2,
                                   t.icon.y() + (t.icon.height() - s.height()) / 2, s.width(), s.height()),
                             t.pixmap);
            }
            // Two lines at most, like the desktop's own captions.
            const int flags = Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine;
            const QStringList lines = captionLines(t.text, fm, t.label.width());
            for (int i = 0; i < lines.size(); ++i) {
                const QRectF line(t.label.x(), t.label.y() + i * fm.height(), t.label.width(), fm.height());
                p.setPen(kShadow);
                p.drawText(line.translated(1, 1), flags, lines.at(i));
                p.setPen(kText);
                p.drawText(line, flags, lines.at(i));
            }
        }
    }
    QImage out(layer.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setOpacity(qBound<qreal>(0.0, opacity, 1.0));
    p.drawImage(0, 0, layer);
    p.end();
    out.setDevicePixelRatio(scale);
    return out;
}

DragGhost::DragGhost()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                           | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus
                           | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setObjectName(QStringLiteral("dragGhost"));
}

void DragGhost::setImage(const QImage& image, const QPoint& hotspotLogical)
{
    m_image = image;
    m_hotspot = hotspotLogical;
    resize(image.deviceIndependentSize().toSize());
    update();
}

void DragGhost::follow(const QPoint& globalLogical)
{
    move(globalLogical - m_hotspot);
    if (!isVisible())
        show();
}

void DragGhost::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(0, 0, m_image);
}

} // namespace starfish::ui
