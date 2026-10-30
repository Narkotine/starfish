#pragma once

// T-098 (session 63): a transparency as the user sees it -- a slider 0-100 with
// its value written next to it ("35 %"). Shared by Settings -> Fences (the
// default) and a fence's own dialog. No QObject of its own: a slider and a
// label in a row, the label kept in step by the slider.

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QWidget>

#include <cmath>

namespace starfish::ui::PercentSlider {

// 0 % = opaque (alpha 255), 100 % = fully clear (alpha 0).
inline int alphaFromPercent(int percent)
{
    return int(std::lround(255.0 * (100 - qBound(0, percent, 100)) / 100.0));
}

inline int percentFromAlpha(int alpha)
{
    return int(std::lround((255 - qBound(0, alpha, 255)) * 100.0 / 255.0));
}

inline QString percentText(int percent)
{
    return QStringLiteral("%1 %").arg(percent);
}

// The row; `slider` and `value` are handed back for the caller to wire.
inline QWidget* make(int percent, QWidget* parent, QSlider** slider, QLabel** value)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* s = new QSlider(Qt::Horizontal, row);
    s->setObjectName(QStringLiteral("transparencySlider"));
    s->setRange(0, 100);
    s->setSingleStep(1);
    s->setPageStep(10);
    s->setValue(percent);
    auto* v = new QLabel(percentText(percent), row);
    v->setObjectName(QStringLiteral("transparencyValue"));
    v->setMinimumWidth(v->fontMetrics().horizontalAdvance(QStringLiteral("100 %")) + 6);
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QObject::connect(s, &QSlider::valueChanged, v, [v](int p) { v->setText(percentText(p)); });
    layout->addWidget(s, 1);
    layout->addWidget(v);
    if (slider)
        *slider = s;
    if (value)
        *value = v;
    return row;
}

} // namespace starfish::ui::PercentSlider
