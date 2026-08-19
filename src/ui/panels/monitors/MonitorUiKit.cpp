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
        "border-radius: 2px; color: %3; font-size: %4px; "
        "padding: 2px 6px; }"
        "QComboBox::drop-down { border: none; width: 16px; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::typography().sizeXs));
}

QString checkedButtonStyle()
{
    return rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; "
        "border-radius: 2px; padding: 2px; } "
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
        "border-radius: 2px; color: %3; font-size: %5px; padding: 1px; } "
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
        "border-radius: 2px; color: %2; font-size: 10px; font-weight: 600; "
        "padding: 1px 5px; } "
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
    rt::UiScale::setScaledFixedHeight(controlBar, 34);
    controlBar->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "#ControlBar { background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().border))));

    auto* layout = new QHBoxLayout(controlBar);
    layout->setContentsMargins(rt::UiScale::px(6), rt::UiScale::px(4),
                               rt::UiScale::px(6), rt::UiScale::px(4));
    layout->setSpacing(rt::UiScale::px(4));
    if (outLayout) *outLayout = layout;
    return controlBar;
}

QLabel* makeTimecodeLabel(QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("00:00:00:00"), parent);
    label->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: %1px; "
        "font-weight: 600; color: %2; background: transparent; "
        "padding: 0px 6px 0px 0px; }")
        .arg(Theme::typography().sizeSmall)
        .arg(Theme::hex(Theme::colors().accent))));
    rt::UiScale::setScaledFixedWidth(label, 96);
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
        "font-weight: 600; color: %3; background: %1; "
        "border: 1px solid %3; border-radius: 2px; "
        "padding: 0px 6px 0px 0px; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::typography().sizeSmall)
        .arg(Theme::hex(Theme::colors().accent))));
    rt::UiScale::setScaledFixedWidth(edit, 96);
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
    rt::UiScale::setScaledMinimumWidth(combo, 64);
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
    rt::UiScale::setScaledMinimumWidth(combo, 58);
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
    auto* btn = new QPushButton(parent);
    btn->setToolTip(QObject::tr("Export Frame (Ctrl+Shift+E)"));
    QPixmap px(20, 20);
    px.fill(Qt::transparent);
    QPainter painter(&px);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(Theme::colors().textSecondary, 1.4,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(3.0, 6.0, 14.0, 10.0), 1.5, 1.5);
    painter.drawEllipse(QRectF(7.5, 8.0, 5.0, 5.0));
    painter.drawLine(QPointF(6.0, 6.0), QPointF(8.0, 4.0));
    painter.drawLine(QPointF(8.0, 4.0), QPointF(12.0, 4.0));
    painter.drawLine(QPointF(12.0, 4.0), QPointF(14.0, 6.0));
    painter.end();
    btn->setIcon(QIcon(px));
    btn->setIconSize(QSize(rt::UiScale::px(16), rt::UiScale::px(16)));
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
    rt::UiScale::setScaledFixedWidth(label, 96);
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

TransportBarKit makeTransportBar(QWidget* parent, QHBoxLayout* layout)
{
    TransportBarKit kit;
    kit.bar = parent;
    kit.layout = layout;

    kit.goStart     = new TransportButton(TransportButton::GoStart, kit.bar);
    kit.stepBack    = new TransportButton(TransportButton::StepBack, kit.bar);
    kit.playPause   = new TransportButton(TransportButton::Play, kit.bar);
    kit.stop        = new TransportButton(TransportButton::Stop, kit.bar);
    kit.stepForward = new TransportButton(TransportButton::StepForward, kit.bar);
    kit.goEnd       = new TransportButton(TransportButton::GoEnd, kit.bar);

    rt::UiScale::setScaledFixedSize(kit.goStart, 20, 20);
    kit.goStart->setToolTip(QObject::tr("Go to Start"));
    rt::UiScale::setScaledFixedSize(kit.stepBack, 20, 20);
    kit.stepBack->setToolTip(QObject::tr("Step Back"));
    rt::UiScale::setScaledFixedSize(kit.playPause, 24, 24);
    kit.playPause->setToolTip(QObject::tr("Play/Pause"));
    rt::UiScale::setScaledFixedSize(kit.stop, 20, 20);
    kit.stop->setToolTip(QObject::tr("Stop"));
    rt::UiScale::setScaledFixedSize(kit.stepForward, 20, 20);
    kit.stepForward->setToolTip(QObject::tr("Step Forward"));
    rt::UiScale::setScaledFixedSize(kit.goEnd, 20, 20);
    kit.goEnd->setToolTip(QObject::tr("Go to End"));

    kit.layout->addWidget(kit.goStart, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.stepBack, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.playPause, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.stop, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.stepForward, 0, Qt::AlignVCenter);
    kit.layout->addWidget(kit.goEnd, 0, Qt::AlignVCenter);

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
        "font-weight: 600; color: %2; background: transparent; "
        "padding: 0px 8px; }")
        .arg(Theme::typography().sizeXs)
        .arg(Theme::hex(Theme::colors().warning))));
    rt::UiScale::setScaledMinimumWidth(kit.shuttleSpeed, 50);
    kit.shuttleSpeed->setAlignment(Qt::AlignCenter);
    kit.shuttleSpeed->hide();
    kit.layout->addWidget(kit.shuttleSpeed, 0, Qt::AlignVCenter);

    return kit;
}

} // namespace monitorui
} // namespace rt
