#pragma once

#include <QWidget>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;

#include "core/desktop_item.hpp"
#include "ui/icon_grid.hpp"

namespace starfish::ui {

// A fence: a rounded panel with a title bar, hosting an icon grid of desktop
// items (T-005). It renders itself onto any QPaintDevice (so the very same
// paint path feeds both a live window and the offscreen image DesktopSurface
// blits to the desktop), and handles the local interactions -- selection and
// double-click-to-launch. The actual launch and any shell context menu happen
// outside the widget (ADR-3); FenceView only reports intent through signals.
class FenceView : public QWidget
{
    Q_OBJECT

public:
    explicit FenceView(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const;

    // The items shown in the grid and how they are ordered.
    void setItems(QList<core::DesktopItem> items);
    const QList<core::DesktopItem>& items() const { return m_grid.items(); }
    void setSort(const QString& by, bool ascending);

    // Where icons come from. Bound by the application to a ShellIconCache.
    void setIconProvider(IconGrid::IconProvider provider) { m_iconProvider = std::move(provider); }

    // Content area in widget-local coordinates (excludes the title bar and the
    // fence's outer padding). The grid is drawn here.
    QRect contentRect() const;

    // The item under a widget-local point, or -1.
    int itemAt(const QPoint& localPoint) const;

    // Accepts items dropped from the Explorer (T-006-3) and appends them to the
    // grid. `contentChanged` is emitted so the owner re-renders. The source
    // (Windows Shell resolution) is done by the caller; this method is pure.
    void addDroppedItems(const QList<core::DesktopItem>& items);

signals:
    // Fired whenever the visual content changes (selection, items, sort) so the
    // owner can re-render the offscreen frame and push it to the surface.
    void contentChanged();

    // A double-click on `index` asked to launch it.
    void itemActivated(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

    // Explorer drag-and-drop into the fence (T-006-3). Only files/folders
    // (CF_HDROP, exposed by Qt as mimeData()->hasUrls()) are accepted; the
    // dropped paths are resolved to DesktopItems and appended.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    // Pops the Windows shell context menu for the item at `index` (-1 = empty
    // area, nothing is shown). Implemented in the .cpp so the shell headers
    // stay out of the widget header. `globalPos` is screen coordinates.
    void showContextMenuForIndex(int index, const QPoint& globalPos);

    // Draws the "insert here" highlight around the cell at `index` while a
    // drag is in progress. No-op when index < 0.
    void paintDropIndicator(int index, QPainter& painter);

    static constexpr int kTitleBarHeight = 30;
    static constexpr int kCornerRadius = 10;
    static constexpr int kPadding = 12;

    QString m_title;
    IconGrid m_grid;
    IconGrid::IconProvider m_iconProvider;

    // Drag state (T-006-2). The press sets `m_dragOriginIndex` and
    // `m_dragStartPos`; the move enters drag mode past the system threshold,
    // updates `m_dropTargetIndex` as the cursor moves; the release performs
    // the reorder and clears the state.
    int m_dragOriginIndex = -1;
    int m_dropTargetIndex = -1;
    QPoint m_dragStartPos;
    bool m_dragging = false;
};

} // namespace starfish::ui
