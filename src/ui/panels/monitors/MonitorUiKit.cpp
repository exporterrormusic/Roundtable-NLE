/*
 * MonitorUiKit.cpp — shared widget builders for the monitor panels.
 * See MonitorUiKit.h.
 */

#include "panels/monitors/MonitorUiKit.h"

#include "Theme.h"
#include "UiScale.h"
#include "widgets/TransportButton.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QWidget>

namespace rt {
namespace monitorui {

QString comboStyle()
{
    return rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QComboBox { background: %1; border: 1px solid %2; "
        "border-radius: 3px; color: %3; font-size: %4px; "
        "padding: 4px 8px 4px 8px; }"
        "QComboBox::drop-down { border: none; width: 20px; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::typography().sizeXs));
}

QString checkedButtonStyle()
{
    return rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; "
        "border-radius: 3px; padding: 3px; } "
        "QPushButton:hover { background: %2; } "
        "QPushButton:checked { background: %3; border-color: %3; }")
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().accent)));
}

QString exportButtonStyle()
{
    return rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2; "
        "border-radius: 4px; color: %3; font-size: %5px; padding: 1px; } "
        "QPushButton:hover { background: %4; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textBright))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::typography().sizeSmall));
}

QString loopButtonStyle()
{
    return rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; "
        "border-radius: 3px; color: %2; font-size: 10px; font-weight: bold; "
        "padding: 2px 6px; } "
        "QPushButton:hover { background: %3; } "
        "QPushButton:checked { background: %4; color: %5; border-color: %4; }")
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().accent))
        .arg(Theme::hex(Theme::colors().textBright)));
}

QWidget* makeViewSpacer(QWidget* parent, int heightPx)
{
    auto* spacer = new QWidget(parent);
    rt::UiScale::setScaledFixedHeight(spacer, heightPx);
    spacer->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; }")
        .arg(Theme::hex(Theme::colors().surface0)));
    return spacer;
}

QWidget* makeControlBar(QWidget* parent, QHBoxLayout** outLayout)
{
    auto* controlBar = new QWidget(parent);
    controlBar->setObjectName(QStringLiteral("ControlBar"));
    rt::UiScale::setScaledFixedHeight(controlBar, 52);
    controlBar->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "#ControlBar { background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().border))));

    auto* layout = new QHBoxLayout(controlBar);
    layout->setContentsMargins(rt::UiScale::px(8), rt::UiScale::px(10),
                               rt::UiScale::px(8), rt::UiScale::px(10));
    layout->setSpacing(rt::UiScale::px(6));
    if (outLayout) *outLayout = layout;
    return controlBar;
}

QLabel* makeTimecodeLabel(QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("00:00:00:00"), parent);
    label->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: %1px; "
        "font-weight: bold; color: #00CC88; background: transparent; "
        "padding: 0px 6px 0px 0px; }")
        .arg(Theme::typography().sizeSmall)));
    rt::UiScale::setScaledFixedWidth(label, 120);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setCursor(Qt::IBeamCursor);
    label->setToolTip(QObject::tr("Click to enter timecode"));
    return label;
}

QLineEdit* makeTimecodeEdit(QWidget* parent)
{
    auto* edit = new QLineEdit(parent);
    edit->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLineEdit { font-family: 'Consolas', monospace; font-size: %2px; "
        "font-weight: bold; color: #00CC88; background: %1; "
        "border: 1px solid #00CC88; border-radius: 3px; "
        "padding: 0px 6px 0px 0px; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::typography().sizeSmall)));
    rt::UiScale::setScaledFixedWidth(edit, 120);
    edit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    edit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("\\d{0,2}:?\\d{0,2}:?\\d{0,2}:?\\d{0,2}")), edit));
    edit->setPlaceholderText(QStringLiteral("HH:MM:SS:FF"));
    edit->hide();
    return edit;
}

QComboBox* makeFitModeCombo(QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    combo->addItem(QObject::tr("Fit"));     // 0
    combo->addItem(QObject::tr("Fill"));    // 1
    combo->addItem(QObject::tr("25%"));     // 2
    combo->addItem(QObject::tr("50%"));     // 3
    combo->addItem(QObject::tr("75%"));     // 4
    combo->addItem(QObject::tr("100%"));    // 5
    combo->addItem(QObject::tr("150%"));    // 6
    combo->addItem(QObject::tr("200%"));    // 7
    combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    combo->setFocusPolicy(Qt::NoFocus);
    combo->setStyleSheet(comboStyle());
    rt::UiScale::setScaledMinimumWidth(combo, 80);
    rt::UiScale::setScaledFixedHeight(combo, 24);
    return combo;
}

QComboBox* makePlaybackResCombo(QWidget* parent, bool withAuto)
{
    auto* combo = new QComboBox(parent);
    combo->addItem(QObject::tr("Full"));
    combo->addItem(QStringLiteral("1/2"));
    combo->addItem(QStringLiteral("1/4"));
    combo->addItem(QStringLiteral("1/8"));
    if (withAuto)
        combo->addItem(QObject::tr("Auto"));
    combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    combo->setFocusPolicy(Qt::NoFocus);
    combo->setToolTip(QObject::tr("Playback Resolution"));
    combo->setStyleSheet(comboStyle());
    rt::UiScale::setScaledMinimumWidth(combo, 70);
    rt::UiScale::setScaledFixedHeight(combo, 24);
    return combo;
}

QIcon makeSafeAreaIcon(QColor fg)
{
    QPixmap px(24, 24);
    px.fill(Qt::transparent);
    QPainter ip(&px);
    ip.setRenderHint(QPainter::Antialiasing, false);
    ip.setPen(QPen(fg, 1.5));
    ip.setBrush(Qt::NoBrush);
    ip.drawRect(QRectF(1.5, 1.5, 21, 21));   // outer frame
    ip.drawRect(QRectF(5.5, 5.5, 13, 13));    // action-safe
    ip.setPen(QPen(fg, 1.0, Qt::DashLine));
    ip.drawRect(QRectF(8.5, 8.5, 7, 7));      // title-safe
    ip.end();
    return QIcon(px);
}

QPushButton* makeSafeAreaButton(QWidget* parent)
{
    auto* btn = new QPushButton(parent);
    btn->setIcon(makeSafeAreaIcon(Theme::colors().textSecondary));
    btn->setIconSize(QSize(rt::UiScale::px(16), rt::UiScale::px(16)));
    btn->setCheckable(true);
    rt::UiScale::setScaledFixedSize(btn, 24, 22);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setToolTip(QObject::tr("Toggle Safe Area Overlay"));
    btn->setStyleSheet(checkedButtonStyle());
    return btn;
}

QPushButton* makeExportFrameButton(QWidget* parent)
{
    auto* btn = new QPushButton(QStringLiteral("📷"), parent);
    btn->setToolTip(QObject::tr("Export Frame (Ctrl+Shift+E)"));
    rt::UiScale::setScaledFixedSize(btn, 28, 22);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(exportButtonStyle());
    return btn;
}

QLabel* makeZoomLabel(QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("100%"), parent);
    label->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: %2px; "
        "color: %1; padding: 0 4px; background: transparent; }")
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::typography().sizeXxs)));
    rt::UiScale::setScaledMinimumWidth(label, 60);
    label->setAlignment(Qt::AlignCenter);
    label->hide();
    return label;
}

QLabel* makeDurationLabel(QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("00:00:00:00"), parent);
    label->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: %2px; "
        "color: %1; background: transparent; padding: 0px 8px 0px 6px; }")
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::typography().sizeSmall)));
    rt::UiScale::setScaledFixedWidth(label, 120);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

QFrame* makeVDivider(QWidget* parent)
{
    auto* divider = new QFrame(parent);
    divider->setFrameShape(QFrame::VLine);
    rt::UiScale::setScaledFixedHeight(divider, 16);
    divider->setStyleSheet(QStringLiteral(
        "QFrame { color: %1; }").arg(Theme::hex(Theme::colors().border)));
    return divider;
}

TransportBarKit makeTransportBar(QWidget* parent)
{
    TransportBarKit kit;

    kit.bar = new QWidget(parent);
    kit.bar->setObjectName(QStringLiteral("TransportBar"));
    rt::UiScale::setScaledFixedHeight(kit.bar, 36);
    kit.bar->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "#TransportBar { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().border))));

    kit.layout = new QHBoxLayout(kit.bar);
    kit.layout->setContentsMargins(rt::UiScale::px(8), 0, rt::UiScale::px(8), 0);
    kit.layout->setSpacing(rt::UiScale::px(4));
    kit.layout->addStretch();

    kit.goStart     = new TransportButton(TransportButton::GoStart, kit.bar);
    kit.stepBack    = new TransportButton(TransportButton::StepBack, kit.bar);
    kit.playPause   = new TransportButton(TransportButton::Play, kit.bar);
    kit.stop        = new TransportButton(TransportButton::Stop, kit.bar);
    kit.stepForward = new TransportButton(TransportButton::StepForward, kit.bar);
    kit.goEnd       = new TransportButton(TransportButton::GoEnd, kit.bar);

    rt::UiScale::setScaledFixedSize(kit.goStart, 22, 22);
    kit.goStart->setToolTip(QObject::tr("Go to Start"));
    rt::UiScale::setScaledFixedSize(kit.stepBack, 22, 22);
    kit.stepBack->setToolTip(QObject::tr("Step Back"));
    rt::UiScale::setScaledFixedSize(kit.playPause, 26, 26);
    kit.playPause->setToolTip(QObject::tr("Play/Pause"));
    rt::UiScale::setScaledFixedSize(kit.stop, 22, 22);
    kit.stop->setToolTip(QObject::tr("Stop"));
    rt::UiScale::setScaledFixedSize(kit.stepForward, 22, 22);
    kit.stepForward->setToolTip(QObject::tr("Step Forward"));
    rt::UiScale::setScaledFixedSize(kit.goEnd, 22, 22);
    kit.goEnd->setToolTip(QObject::tr("Go to End"));

    kit.layout->addWidget(kit.goStart, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.stepBack, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.playPause, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.stop, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.stepForward, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.goEnd, 0, Qt::AlignVCenter);

    // Vertical divider + screenshot button
    kit.layout->addSpacing(rt::UiScale::px(4));
    kit.layout->addWidget(makeVDivider(kit.bar), 0, Qt::AlignVCenter);
    kit.layout->addSpacing(rt::UiScale::px(4));

    kit.screenshot = new TransportButton(TransportButton::Screenshot, kit.bar);
    kit.screenshot->setToolTip(QObject::tr("Take Screenshot"));
    rt::UiScale::setScaledFixedSize(kit.screenshot, 22, 22);
    kit.layout->addWidget(kit.screenshot, 0, Qt::AlignVCenter);

    // Divider + loop toggle
    kit.layout->addSpacing(rt::UiScale::px(4));
    kit.layout->addWidget(makeVDivider(kit.bar), 0, Qt::AlignVCenter);
    kit.layout->addSpacing(rt::UiScale::px(4));

    kit.loop = new QPushButton(QObject::tr("Loop"), kit.bar);
    kit.loop->setCheckable(true);
    rt::UiScale::setScaledFixedHeight(kit.loop, 22);
    kit.loop->setFocusPolicy(Qt::NoFocus);
    kit.loop->setToolTip(QObject::tr("Toggle Loop Playback"));
    kit.loop->setStyleSheet(loopButtonStyle());
    kit.layout->addWidget(kit.loop, 0, Qt::AlignVCenter);

    // Shuttle speed display label
    kit.shuttleSpeed = new QLabel(kit.bar);
    kit.shuttleSpeed->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: %1px; "
        "font-weight: bold; color: #FFD700; background: transparent; "
        "padding: 0px 8px; }")
        .arg(Theme::typography().sizeXs)));
    rt::UiScale::setScaledMinimumWidth(kit.shuttleSpeed, 50);
    kit.shuttleSpeed->setAlignment(Qt::AlignCenter);
    kit.shuttleSpeed->hide();
    kit.layout->addWidget(kit.shuttleSpeed, 0, Qt::AlignVCenter);

    return kit;
}

} // namespace monitorui
} // namespace rt
