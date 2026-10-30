#pragma once

#include <QFont>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

namespace starfish::ui {

// Session 62 -- what the user carries in a drag, drawn as Windows draws it:
// each icon with its caption on a light selection plate, laid out as they sit,
// the whole slightly translucent. One picture for both drags: the desktop's
// (handed to the shell's drag helper, so Explorer and the drop targets show it
// too) and a fence's own (shown by DragGhost).
struct DragTile {
    QRect icon;   // where the icon sits, in any space common to all tiles
    QRect label;  // where its caption sits
    QPixmap pixmap;
    QString text;
};

class DragImage
{
public:
    // `opacity` applies to everything; `dpr` > 1 renders logical rects sharp.
    // `topLeft` receives the tiles' space position of the image's (0,0).
    static QImage compose(const QList<DragTile>& tiles, const QFont& font, qreal opacity, qreal dpr,
                          QPoint* topLeft);
    static constexpr qreal kOpacity = 0.78;
};

// A fence's drag is its own gesture (not OLE): the picture follows the cursor
// in a window that takes no input and never activates.
class DragGhost : public QWidget
{
public:
    DragGhost();
    void setImage(const QImage& image, const QPoint& hotspotLogical);
    void follow(const QPoint& globalLogical);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage m_image;
    QPoint m_hotspot;
};

} // namespace starfish::ui
