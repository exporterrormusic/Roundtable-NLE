/*
 * MonitorUiKit.cpp — shared widget builders for the monitor panels.
 * See MonitorUiKit.h.
 */

#include "panels/monitors/MonitorUiKit.h"

#include "ShortcutManager.h"
#include "Theme.h"
#include "UiScale.h"
#include "widgets/TransportButton.h"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QWidget>

namespace rt {
namespace monitorui {

namespace {
// Row-button properties read by applyShortcutTooltips().
constexpr const char* kMonitorButtonNameProperty = "rtMonitorButtonName";
constexpr const char* kMonitorButtonActionProperty   = "rtMonitorButtonAction";
constexpr const char* kMonitorButtonKeyProperty   = "rtMonitorButtonKey";
constexpr const char* kMonitorTooltipsWatchedProperty = "rtMonitorTooltipsWatched";

QString monitorRowStyle(const char* objectName)
{
    return QStringLiteral("#%1 { background: %2; }")
        .arg(QLatin1String(objectName))
        .arg(Theme::hex(Theme::colors().surface0));
}
} // namespace

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

QWidget* makeViewSpacer(QWidget* parent, int heightPx)
{
    auto* spacer = new QWidget(parent);
    rt::UiScale::setScaledFixedHeight(spacer, heightPx);
    spacer->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; }")
        .arg(Theme::hex(Theme::colors().surface0)));
    return spacer;
}

QWidget* makeInfoRow(QWidget* parent, QHBoxLayout** outLayout)
{
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("MonitorInfoRow"));
    rt::UiScale::setScaledFixedHeight(row, 30);
    row->setStyleSheet(monitorRowStyle("MonitorInfoRow"));

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(rt::UiScale::px(8), rt::UiScale::px(3),
                               rt::UiScale::px(8), rt::UiScale::px(3));
    layout->setSpacing(rt::UiScale::px(6));
    if (outLayout) *outLayout = layout;
    return row;
}

QWidget* makeButtonRow(QWidget* parent, QHBoxLayout** outLayout)
{
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("MonitorButtonRow"));
    rt::UiScale::setScaledFixedHeight(row, 34);
    row->setStyleSheet(monitorRowStyle("MonitorButtonRow"));

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(rt::UiScale::px(8), rt::UiScale::px(4),
                               rt::UiScale::px(8), rt::UiScale::px(4));
    layout->setSpacing(rt::UiScale::px(2));
    layout->addStretch();
    if (outLayout) *outLayout = layout;
    return row;
}

void addButtonGroupGap(QHBoxLayout* layout)
{
    layout->addSpacing(rt::UiScale::px(20));
}

void finishButtonRow(QHBoxLayout* layout)
{
    layout->addStretch();
}

TransportButton* makeRowButton(QWidget* parent, int icon, const QString& name,
                               const QString& actionId, const QString& fixedKey)
{
    auto* btn = new TransportButton(static_cast<TransportButton::Icon>(icon), parent);
    rt::UiScale::setScaledFixedSize(btn, 30, 24);
    btn->setProperty(kMonitorButtonNameProperty, name);
    btn->setProperty(kMonitorButtonActionProperty, actionId);
    btn->setProperty(kMonitorButtonKeyProperty, fixedKey);
    btn->setToolTip(fixedKey.isEmpty() ? name
                                       : QStringLiteral("%1 (%2)").arg(name, fixedKey));
    return btn;
}

void applyShortcutTooltips(QWidget* root, const ShortcutManager* shortcuts)
{
    if (!root) return;
    for (auto* btn : root->findChildren<QPushButton*>()) {
        const QString name = btn->property(kMonitorButtonNameProperty).toString();
        if (name.isEmpty()) continue;
        QString key = btn->property(kMonitorButtonKeyProperty).toString();
        const QString actionId = btn->property(kMonitorButtonActionProperty).toString();
        if (shortcuts && !actionId.isEmpty()) {
            if (const ShortcutAction* a = shortcuts->action(actionId))
                key = a->currentKey.toString(QKeySequence::NativeText);
        }
        btn->setToolTip(key.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, key));
    }

    if (shortcuts && !root->property(kMonitorTooltipsWatchedProperty).toBool()) {
        root->setProperty(kMonitorTooltipsWatchedProperty, true);
        QObject::connect(shortcuts, &ShortcutManager::shortcutChanged, root,
                         [root, shortcuts]() { applyShortcutTooltips(root, shortcuts); });
    }
}

SettingsMenuKit makeSettingsMenu(QWidget* parent)
{
    SettingsMenuKit kit;
    kit.button = makeRowButton(parent, TransportButton::Settings, QObject::tr("Settings"));
    kit.menu = new QMenu(kit.button);
    kit.loop = kit.menu->addAction(QObject::tr("Loop"));
    kit.loop->setCheckable(true);
    kit.safeMargins = kit.menu->addAction(QObject::tr("Safe Margins"));
    kit.safeMargins->setCheckable(true);
    kit.button->setMenu(kit.menu);
    return kit;
}

QLabel* makeShuttleSpeedLabel(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: %1px; "
        "font-weight: 600; color: %2; background: transparent; "
        "padding: 0px 8px; }")
        .arg(Theme::typography().sizeXs)
        .arg(Theme::hex(Theme::colors().warning))));
    rt::UiScale::setScaledMinimumWidth(label, 50);
    label->setAlignment(Qt::AlignCenter);
    label->hide();
    return label;
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
    rt::UiScale::setScaledFixedWidth(label, 112);   // 11 monospace digits + padding
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
    rt::UiScale::setScaledFixedWidth(edit, 112);
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
    combo->setToolTip(QObject::tr("Select Zoom Level"));
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
    combo->setToolTip(QObject::tr("Select Playback Resolution"));
    combo->setStyleSheet(comboStyle());
    rt::UiScale::setScaledMinimumWidth(combo, 58);
    rt::UiScale::setScaledFixedHeight(combo, 24);
    return combo;
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
        "color: %1; background: transparent; padding: 0px 0px 0px 6px; }")
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::typography().sizeSmall)));
    rt::UiScale::setScaledFixedWidth(label, 112);   // 11 monospace digits + padding
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

PlaybackGroupKit addPlaybackGroup(QWidget* parent, QHBoxLayout* layout)
{
    PlaybackGroupKit kit;
    kit.goToIn = makeRowButton(parent, TransportButton::GoToIn, QObject::tr("Go to In"),
                               QString::fromLatin1(ShortcutManager::kGoIn));
    kit.stepBack = makeRowButton(parent, TransportButton::StepBack,
                                 QObject::tr("Step Back 1 Frame"),
                                 QString::fromLatin1(ShortcutManager::kFrameBack));
    kit.playPause = makeRowButton(parent, TransportButton::Play, QObject::tr("Play-Stop Toggle"),
                                  QString::fromLatin1(ShortcutManager::kPlayPause));
    kit.stepForward = makeRowButton(parent, TransportButton::StepForward,
                                    QObject::tr("Step Forward 1 Frame"),
                                    QString::fromLatin1(ShortcutManager::kFrameForward));
    kit.goToOut = makeRowButton(parent, TransportButton::GoToOut, QObject::tr("Go to Out"),
                                QString::fromLatin1(ShortcutManager::kGoOut));
    for (auto* b : {kit.goToIn, kit.stepBack, kit.playPause, kit.stepForward, kit.goToOut})
        layout->addWidget(b, 0, Qt::AlignVCenter);
    return kit;
}

MarkGroupKit addMarkGroup(QWidget* parent, QHBoxLayout* layout)
{
    // M, I and O are fixed keys (ShortcutControllerKeys), not rebindable.
    MarkGroupKit kit;
    kit.addMarker = makeRowButton(parent, TransportButton::AddMarker, QObject::tr("Add Marker"),
                                  {}, QStringLiteral("M"));
    kit.markIn = makeRowButton(parent, TransportButton::MarkIn, QObject::tr("Mark In"),
                               {}, QStringLiteral("I"));
    kit.markOut = makeRowButton(parent, TransportButton::MarkOut, QObject::tr("Mark Out"),
                                {}, QStringLiteral("O"));
    for (auto* b : {kit.addMarker, kit.markIn, kit.markOut})
        layout->addWidget(b, 0, Qt::AlignVCenter);
    return kit;
}

} // namespace monitorui
} // namespace rt
