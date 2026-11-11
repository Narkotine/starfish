#include "ui/button_skin.hpp"
#include "ui/zone_shape.hpp"

#include <QFile>
#include <QGlobalStatic>
#include <QImage>
#include <QImageReader>
#include <QMovie>
#include <QMutex>
#include <QMutexLocker>
#include <QPainterPath>
#include <QVector>
#include <utility>

namespace starfish::ui::zone_shape {
namespace {

constexpr int kMaxNativeSide = 512;
constexpr int kMaxScaledSide = 384;
constexpr int kMaxRawEntries = 6;
constexpr int kMaxScaledEntries = 6;

QImage normalizeNative(QImage im)
{
    if (im.isNull())
        return {};
    if (im.format() != QImage::Format_ARGB32 && im.format() != QImage::Format_ARGB32_Premultiplied)
        im = im.convertToFormat(QImage::Format_ARGB32);
    const int side = qMax(im.width(), im.height());
    if (side > kMaxNativeSide) {
        const qreal s = qreal(kMaxNativeSide) / qreal(side);
        im = im.scaled(qMax(1, int(qRound(im.width() * s))),
                       qMax(1, int(qRound(im.height() * s))),
                       Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return im;
}

QVector<QImage> decodeGif(const QString& path)
{
    QVector<QImage> frames;
    if (path.isEmpty())
        return frames;

    auto take = [&](QImage im) {
        im = normalizeNative(std::move(im));
        if (!im.isNull())
            frames.append(std::move(im));
    };

    // QMovie::jumpToFrame is the reliable way to walk a GIF; QImageReader's
    // imageCount / jumpToImage often report a single frame for the same file.
    QMovie movie(path);
    if (movie.isValid()) {
        if (movie.jumpToFrame(0)) {
            const int declared = qMax(1, movie.frameCount());
            for (int i = 0; i < declared; ++i) {
                if (!movie.jumpToFrame(i))
                    break;
                take(movie.currentImage());
            }
        }
    }
    if (frames.isEmpty()) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        while (reader.canRead()) {
            take(reader.read());
            if (!reader.jumpToNextImage())
                break;
        }
    }
    if (frames.isEmpty())
        take(QImage(path));
    return frames;
}

struct RawEntry {
    QString path;
    QVector<QImage> frames;
};

struct ScaledEntry {
    QString path;
    int pixelSize = 0;
    QVector<QImage> frames;
};

struct GifCache {
    QMutex mutex;
    QVector<RawEntry> raw;       // most-recent last
    QVector<ScaledEntry> scaled;

    const RawEntry* rawFor(const QString& path)
    {
        for (int i = raw.size() - 1; i >= 0; --i) {
            if (raw[i].path == path)
                return &raw[i];
        }
        RawEntry entry;
        entry.path = path;
        entry.frames = decodeGif(path);
        if (entry.frames.isEmpty())
            return nullptr;
        raw.append(std::move(entry));
        while (raw.size() > kMaxRawEntries)
            raw.removeFirst();
        return &raw.last();
    }

    const ScaledEntry* scaledFor(const QString& path, int pixelSize)
    {
        const int side = qBound(1, pixelSize, kMaxScaledSide);
        for (int i = scaled.size() - 1; i >= 0; --i) {
            if (scaled[i].path == path && scaled[i].pixelSize == side)
                return &scaled[i];
        }
        const RawEntry* src = rawFor(path);
        if (!src || src->frames.isEmpty())
            return nullptr;
        ScaledEntry entry;
        entry.path = path;
        entry.pixelSize = side;
        entry.frames.reserve(src->frames.size());
        for (const QImage& im : src->frames) {
            QImage s = im;
            if (s.width() != side || s.height() != side)
                s = im.scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            entry.frames.append(std::move(s));
        }
        scaled.append(std::move(entry));
        while (scaled.size() > kMaxScaledEntries)
            scaled.removeFirst();
        return &scaled.last();
    }
};

Q_GLOBAL_STATIC(GifCache, gifCache)

void paintGifLayer(QPainter& painter, const ButtonSkinPaint& ctx, const QString& path)
{
    if (path.isEmpty())
        return;
    const qreal logical = ctx.expandedSide > 1 ? ctx.expandedSide
                                               : qMax(qreal(1), qMin(ctx.disc.width(), ctx.disc.height()));
    const qreal dpr = ctx.dpr > 0 ? ctx.dpr : 1;
    const int physical = buttonPhysicalDiameter(qMax(1, int(qRound(logical))), dpr);
    const int cacheSide = qMin(kMaxScaledSide, physical);

    QImage copy;
    {
        QMutexLocker lock(&gifCache->mutex);
        const ScaledEntry* entry = gifCache->scaledFor(path, cacheSide);
        if (!entry || entry->frames.isEmpty())
            return;
        const int index = buttonSkinFrameIndex(ctx.openProgress, entry->frames.size());
        copy = entry->frames.at(qBound(0, index, entry->frames.size() - 1));
    }
    if (copy.isNull())
        return;
    // Draw in logical space: the image's devicePixelRatio maps `cacheSide`
    // physical pixels onto `logical` device-independent pixels so the
    // centre stays put at fractional DPR (session 106).
    if (logical > 0)
        copy.setDevicePixelRatio(qreal(cacheSide) / logical);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QPainterPath clip;
    clip.addEllipse(ctx.disc.adjusted(1, 1, -1, -1));
    painter.setClipPath(clip, Qt::IntersectClip);
    QRectF dest(0, 0, logical, logical);
    dest.moveCenter(ctx.disc.center());
    painter.drawImage(dest, copy);
    painter.restore();
}

class GifButtonSkin final : public ButtonSkin {
public:
    explicit GifButtonSkin(QString id)
        : m_id(std::move(id))
    {
    }

    QString id() const override { return m_id; }

    void setAssets(const ButtonSkinAssets& assets) override { m_assets = assets; }

    int frameCount() const override
    {
        const QString path = !m_assets.behind.isEmpty() ? m_assets.behind : m_assets.front;
        if (path.isEmpty())
            return 0;
        QMutexLocker lock(&gifCache->mutex);
        const RawEntry* raw = gifCache->rawFor(path);
        return raw ? raw->frames.size() : 0;
    }

    void paintBehind(QPainter& painter, const ButtonSkinPaint& ctx) const override
    {
        PlainButtonSkin{}.paintBehind(painter, ctx);
        paintGifLayer(painter, ctx, m_assets.behind);
    }

    void paintFront(QPainter& painter, const ButtonSkinPaint& ctx) const override
    {
        paintGifLayer(painter, ctx, m_assets.front);
    }

private:
    QString m_id;
    ButtonSkinAssets m_assets;
};

} // namespace

QString resolvedButtonSkinPath(const QString& id, const QString& path)
{
    if (id == QLatin1String(kButtonSkinShutter))
        return QString::fromUtf8(kButtonSkinShutterResource);
    if (id == QLatin1String(kButtonSkinGif))
        return path;
    return {};
}

bool buttonSkinPathReadable(const QString& path)
{
    if (path.isEmpty())
        return false;
    if (path.startsWith(QLatin1Char(':'))) {
        QImageReader reader(path);
        return reader.canRead();
    }
    if (!QFile::exists(path))
        return false;
    QImageReader reader(path);
    return reader.canRead();
}

std::unique_ptr<ButtonSkin> makeButtonSkin(const QString& id, const QString& path)
{
    const QString resolved = resolvedButtonSkinPath(id, path);
    if (id == QLatin1String(kButtonSkinShutter) && buttonSkinPathReadable(resolved)) {
        auto skin = std::make_unique<GifButtonSkin>(QString::fromUtf8(kButtonSkinShutter));
        return skin;
    }
    if (id == QLatin1String(kButtonSkinGif) && buttonSkinPathReadable(resolved)) {
        auto skin = std::make_unique<GifButtonSkin>(QString::fromUtf8(kButtonSkinGif));
        return skin;
    }
    return std::make_unique<PlainButtonSkin>();
}

} // namespace starfish::ui::zone_shape
