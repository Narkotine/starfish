#include "ui/widget_settings_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageReader>
#include <QPushButton>
#include <QStandardPaths>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>

#include "ui/widget_resize.hpp"
#include <QVBoxLayout>

#include "core/system_metrics.hpp"
#include "ui/fence_widgets.hpp"

namespace starfish::ui {

bool WidgetSettingsDialog::hasOptions(const QString& type)
{
    return type == QLatin1String("clock") || type == QLatin1String("image") || core::isSystemWidgetType(type);
}

QString WidgetSettingsDialog::chooseImage(QWidget* parent, const QString& current)
{
    QStringList patterns;
    for (const QByteArray& format : QImageReader::supportedImageFormats())
        patterns << QStringLiteral("*.") + QString::fromLatin1(format);
    const QString start = current.isEmpty()
                              ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                              : QFileInfo(current).absolutePath();
    return QFileDialog::getOpenFileName(parent, tr("Choose an image"), start,
                                        tr("Images (%1)").arg(patterns.join(QLatin1Char(' '))));
}

WidgetSettingsDialog::WidgetSettingsDialog(const core::DesktopItem& widget, QWidget* parent)
    : QDialog(parent)
    , m_widget(widget)
{
    setObjectName(QStringLiteral("widgetSettings"));
    setWindowTitle(tr("Configure widget - %1").arg(widgets::displayName(widget.widgetType)));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    // Session 96: any size from 1 x 1 to 8 x 6 cells, without flooding the
    // widget's menu (which keeps a few common sizes).
    m_sizeW = new QSpinBox(this);
    m_sizeW->setObjectName(QStringLiteral("sizeWidth"));
    m_sizeW->setRange(1, kMaxSpan.width());
    m_sizeW->setValue(qBound(1, widget.span.width(), kMaxSpan.width()));
    m_sizeH = new QSpinBox(this);
    m_sizeH->setObjectName(QStringLiteral("sizeHeight"));
    m_sizeH->setRange(1, kMaxSpan.height());
    m_sizeH->setValue(qBound(1, widget.span.height(), kMaxSpan.height()));
    // Session 100: a locked widget keeps its size; the rest stays configurable.
    m_sizeW->setEnabled(!widget_resize::isLocked(widget));
    m_sizeH->setEnabled(!widget_resize::isLocked(widget));
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel(tr("Width"), this));
    sizeRow->addWidget(m_sizeW);
    sizeRow->addSpacing(12);
    sizeRow->addWidget(new QLabel(tr("Height"), this));
    sizeRow->addWidget(m_sizeH);
    sizeRow->addStretch(1);
    form->addRow(tr("Size (cells)"), sizeRow);

    const QVariantMap& settings = widget.widgetSettings;
    if (widget.widgetType == QLatin1String("image")) { // session 95
        m_imagePath = widgets::imagePath(widget);
        auto* row = new QHBoxLayout;
        m_imageLabel = new QLabel(this);
        m_imageLabel->setObjectName(QStringLiteral("imagePath"));
        m_imageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* browse = new QPushButton(tr("Browse..."), this);
        browse->setObjectName(QStringLiteral("imageBrowse"));
        row->addWidget(m_imageLabel, 1);
        row->addWidget(browse);
        form->addRow(tr("Image"), row);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString chosen = chooseImage(this, m_imagePath);
            if (!chosen.isEmpty()) {
                m_imagePath = chosen;
                updateEnabled();
            }
        });
        m_fit = new QComboBox(this);
        m_fit->setObjectName(QStringLiteral("fit"));
        m_fit->addItem(tr("Fit (whole picture)"), QStringLiteral("fit"));
        m_fit->addItem(tr("Fill (cropped)"), QStringLiteral("fill"));
        m_fit->setCurrentIndex(widgets::imageFills(widget) ? 1 : 0);
        form->addRow(tr("Display"), m_fit);
    } else if (widget.widgetType == QLatin1String("clock")) {
        m_seconds = new QCheckBox(tr("Show seconds"), this);
        m_seconds->setChecked(settings.value(QStringLiteral("seconds")).toBool());
        m_date = new QCheckBox(tr("Show the date"), this);
        m_date->setChecked(settings.value(QStringLiteral("date")).toBool());
        m_h12 = new QCheckBox(tr("12-hour clock"), this);
        m_h12->setChecked(settings.value(QStringLiteral("h12")).toBool());
        form->addRow(QString(), m_seconds);
        form->addRow(QString(), m_date);
        form->addRow(QString(), m_h12);
    } else if (core::isSystemWidgetType(widget.widgetType)) {
        const core::MetricSettings s = core::MetricSettings::fromMap(settings, widget.widgetType);
        core::Metric single;
        const bool isSingle = core::metricFromType(widget.widgetType, &single);
        if (!isSingle) {
            auto* box = new QGroupBox(tr("Metrics shown"), this);
            auto* row = new QHBoxLayout(box);
            for (int i = 0; i < core::kMetricCount; ++i) {
                auto* check = new QCheckBox(core::metricShortName(core::Metric(i)), box);
                check->setObjectName(QStringLiteral("metric_") + core::metricType(core::Metric(i)));
                check->setChecked(s.metrics.contains(core::Metric(i)));
                connect(check, &QCheckBox::toggled, this, &WidgetSettingsDialog::updateEnabled);
                row->addWidget(check);
                m_metrics << check;
            }
            layout->insertWidget(0, box);
        }
        m_show = new QComboBox(this);
        m_show->setObjectName(QStringLiteral("show"));
        m_show->addItem(tr("Used"), false);
        m_show->addItem(tr("Free / remaining"), true);
        m_show->setCurrentIndex(s.showFree ? 1 : 0);
        form->addRow(tr("Show"), m_show);

        m_mode = new QComboBox(this);
        m_mode->setObjectName(QStringLiteral("mode"));
        m_mode->addItem(tr("Number only"), int(core::MetricSettings::Mode::Number));
        m_mode->addItem(tr("Bar"), int(core::MetricSettings::Mode::Bar));
        m_mode->addItem(tr("History graph"), int(core::MetricSettings::Mode::Graph));
        m_mode->setCurrentIndex(qMax(0, m_mode->findData(int(s.mode))));
        connect(m_mode, &QComboBox::currentIndexChanged, this, &WidgetSettingsDialog::updateEnabled);
        form->addRow(tr("Display"), m_mode);

        if (!isSingle || core::hasBytes(single)) {
            m_unit = new QComboBox(this);
            m_unit->setObjectName(QStringLiteral("unit"));
            m_unit->addItem(tr("Percent"), false);
            m_unit->addItem(tr("Gigabytes (GB)"), true);
            m_unit->setCurrentIndex(s.bytes ? 1 : 0);
            form->addRow(isSingle ? tr("Unit") : tr("Unit (RAM, VRAM)"), m_unit);
        }

        m_history = new QSpinBox(this);
        m_history->setObjectName(QStringLiteral("history"));
        m_history->setRange(10, 600);
        m_history->setValue(s.history);
        m_history->setSuffix(tr(" points"));
        form->addRow(tr("Graph length"), m_history);

        m_interval = new QSpinBox(this);
        m_interval->setObjectName(QStringLiteral("interval"));
        m_interval->setRange(1, 5);
        m_interval->setValue(s.interval);
        m_interval->setSuffix(tr(" s per point"));
        form->addRow(tr("Graph step"), m_interval);

        auto* hint = new QLabel(tr("Read once a second by StarFish itself (no extra program). "
                                   "\"n/a\": this PC does not report the value."),
                                this);
        hint->setWordWrap(true);
        hint->setObjectName(QStringLiteral("hint"));
        layout->addWidget(hint);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    updateEnabled();
}

void WidgetSettingsDialog::setImagePath(const QString& path)
{
    m_imagePath = path;
    updateEnabled();
}

void WidgetSettingsDialog::updateEnabled()
{
    if (m_imageLabel) {
        m_imageLabel->setText(m_imagePath.isEmpty() ? tr("(none)") : QFileInfo(m_imagePath).fileName());
        m_imageLabel->setToolTip(m_imagePath);
    }
    if (m_history && m_mode) {
        const bool graph = m_mode->currentData().toInt() == int(core::MetricSettings::Mode::Graph);
        m_history->setEnabled(graph);
        m_interval->setEnabled(graph);
    }
    // The group keeps at least one metric.
    int checked = 0;
    for (QCheckBox* check : std::as_const(m_metrics))
        checked += check->isChecked() ? 1 : 0;
    for (QCheckBox* check : std::as_const(m_metrics))
        check->setEnabled(!(checked == 1 && check->isChecked()));
}

core::DesktopItem WidgetSettingsDialog::result() const
{
    core::DesktopItem widget = m_widget;
    widget.span = QSize(m_sizeW->value(), m_sizeH->value());
    if (m_fit) {
        widget.widgetSettings.insert(QStringLiteral("path"), m_imagePath);
        widget.widgetSettings.insert(QStringLiteral("fit"), m_fit->currentData().toString());
    }
    if (m_seconds) {
        widget.widgetSettings.insert(QStringLiteral("seconds"), m_seconds->isChecked());
        widget.widgetSettings.insert(QStringLiteral("date"), m_date->isChecked());
        widget.widgetSettings.insert(QStringLiteral("h12"), m_h12->isChecked());
    }
    if (m_mode) {
        core::MetricSettings s = core::MetricSettings::fromMap(widget.widgetSettings, widget.widgetType);
        s.showFree = m_show->currentData().toBool();
        s.mode = core::MetricSettings::Mode(m_mode->currentData().toInt());
        if (m_unit)
            s.bytes = m_unit->currentData().toBool();
        s.history = m_history->value();
        s.interval = m_interval->value();
        if (!m_metrics.isEmpty()) {
            s.metrics.clear();
            for (int i = 0; i < m_metrics.size(); ++i)
                if (m_metrics.at(i)->isChecked())
                    s.metrics << core::Metric(i);
        }
        s.writeTo(widget.widgetSettings, widget.widgetType);
    }
    return widget;
}

} // namespace starfish::ui
