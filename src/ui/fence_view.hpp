#pragma once

#include <QString>
#include <QWidget>

namespace starfish::ui {

// Minimal fence: a rounded panel with a title bar and an (empty) icon area.
// Phase 1 only needs "an empty zone drawn at the right z-order"; the icon grid,
// selection and interactions land in T-005/T-006.
//
// The panel is painted on an opaque background: per-pixel transparency, which
// a translucent fence background over the wallpaper would need, requires
// UpdateLayeredWindow compositing and is not implemented yet
// (see docs/technique.md ADR-4).
class FenceView : public QWidget
{
    Q_OBJECT

public:
    explicit FenceView(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_title;
};

} // namespace starfish::ui
