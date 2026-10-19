/*
 * TimelineWorkspacePanelsCentral.cpp — the inner main window's central
 * widget: sequence tab bar, timeline toolbar and timeline panel, plus the
 * tool column (its own "Tools" dock). Part of createPanelWidgets().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"
#include "Theme.h"
#include "command/CommandStack.h"
#include "command/commands/MarkerCommands.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/timeline/SequenceTabsController.h"
#include "panels/timeline/TimelinePanel.h"
#include "playback/PlaybackController.h"
#include "spine/AnimationVideoCache.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "widgets/ToolButton.h"
#include "widgets/ToolGroupButton.h"

#include <QAction>
#include <QDockWidget>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QSizePolicy>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <memory>

namespace rt {

void TimelineWorkspace::createCentralWidget()
{
    auto* centralContainer = new QWidget;
    centralContainer->setMinimumHeight(200);
    auto* centralLayout = new QHBoxLayout(centralContainer);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    QWidget* toolColumn = createToolColumn();

    // -- Center: Toolbar + Timeline Panel ---------------------------------
    auto* centerContainer = new QWidget;
    auto* centerLayout = new QVBoxLayout(centerContainer);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);

    // -- Sequence tab bar row ---------------------------------------------
    centerLayout->addWidget(m_sequenceTabs->createTabBar());

    // -- Timeline toolbar row ---------------------------------------------
    centerLayout->addWidget(createTimelineToolbar());

    // -- Timeline Panel ----------------------------------------------------
    m_timelinePanel = new TimelinePanel(this);
    if (m_timeline) m_timelinePanel->setTimeline(m_timeline);
    if (m_compositeService) m_timelinePanel->setCompositeService(m_compositeService.get());
    if (m_commandStack) m_timelinePanel->setCommandStack(m_commandStack);
    if (m_shortcutManager) m_timelinePanel->setShortcutManager(m_shortcutManager);
    if (m_mediaPool) m_timelinePanel->setMediaPool(m_mediaPool);
    connect(m_timelinePanel, &TimelinePanel::mediaRelinkRequested,
            this, &TimelineWorkspace::relinkMedia);
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_compositeService && m_compositeService->animVideoCache())
        m_timelinePanel->setAnimVideoCache(m_compositeService->animVideoCache());
#endif
    centerLayout->addWidget(m_timelinePanel, 1);

    centralLayout->addWidget(centerContainer);

    // Wrap tool column in its own dock widget
    makeDock("Tools", toolColumn, /*closable=*/false);

    m_innerMainWindow->setCentralWidget(centralContainer);
}

QWidget* TimelineWorkspace::createToolColumn()
{
    const auto& tc = Theme::colors();

    // -- Left: Vertical Tool Column (Premiere Pro style) ------------------
    auto* toolColumn = new QWidget;
    toolColumn->setMinimumWidth(48);
    toolColumn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    toolColumn->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; }")
        .arg(Theme::hex(tc.surface0)));
    auto* toolColumnLayout = new QVBoxLayout(toolColumn);
    toolColumnLayout->setContentsMargins(4, 6, 4, 0);
    toolColumnLayout->setSpacing(0);
    toolColumnLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto* btnSelection = new ToolButton(ToolButton::Selection);
    btnSelection->setFixedSize(40, 34);
    btnSelection->setToolTip(QStringLiteral("Selection Tool (A)"));
    btnSelection->setChecked(true);

    auto* btnRazor = new ToolButton(ToolButton::Razor);
    btnRazor->setFixedSize(40, 34);
    btnRazor->setToolTip(QStringLiteral("Razor Tool (B)"));

    auto* btnRipple = new ToolButton(ToolButton::Ripple);
    btnRipple->setFixedSize(40, 34);
    btnRipple->setToolTip(QStringLiteral("Ripple Edit Tool (N)"));

    auto* btnRolling = new ToolButton(ToolButton::Rolling);
    btnRolling->setFixedSize(40, 34);
    btnRolling->setToolTip(QStringLiteral("Rolling Edit Tool (R)"));

    auto* btnSlip = new ToolButton(ToolButton::Slip);
    btnSlip->setFixedSize(40, 34);
    btnSlip->setToolTip(QStringLiteral("Slip Tool (S)"));

    auto* btnSlide = new ToolButton(ToolButton::Slide);
    btnSlide->setFixedSize(40, 34);
    btnSlide->setToolTip(QStringLiteral("Slide Tool (U)"));

    auto* btnText = new ToolButton(ToolButton::Text);
    btnText->setFixedSize(40, 34);
    btnText->setToolTip(QStringLiteral("Text Tool (T)"));

    auto* btnZoom = new ToolButton(ToolButton::Zoom);
    btnZoom->setFixedSize(40, 34);
    btnZoom->setToolTip(QStringLiteral("Zoom Tool (Z) — Click: Zoom In, Alt+Click: Zoom Out"));

    // Pen / Ellipse / Rectangle Mask share one slot, as in Premiere: click
    // uses the last-picked tool, press-and-hold or right-click picks another.
    auto* btnMaskTools = new ToolGroupButton({
        {ToolButton::PenMask, QStringLiteral("Pen Mask Tool (P)"),
         static_cast<int>(EditTool::PenMask)},
        {ToolButton::EllipseMask,
         QStringLiteral("Ellipse Mask Tool (Shift: circle, Alt: from center)"),
         static_cast<int>(EditTool::EllipseMask)},
        {ToolButton::RectangleMask,
         QStringLiteral("Rectangle Mask Tool (Shift: square, Alt: from center)"),
         static_cast<int>(EditTool::RectangleMask)},
    });
    btnMaskTools->setObjectName(QStringLiteral("penMaskToolButton"));
    btnMaskTools->setFixedSize(40, 34);

    auto addToolSep = [&]() {
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Plain);
        sep->setFixedHeight(1);
        sep->setStyleSheet(QStringLiteral(
            "QFrame { color: %1; }")
            .arg(Theme::hex(tc.border)));
        toolColumnLayout->addSpacing(3);
        toolColumnLayout->addWidget(sep);
        toolColumnLayout->addSpacing(3);
    };

    toolColumnLayout->addWidget(btnSelection);
    addToolSep();
    toolColumnLayout->addWidget(btnRazor);
    addToolSep();
    toolColumnLayout->addWidget(btnRipple);
    addToolSep();
    toolColumnLayout->addWidget(btnRolling);
    addToolSep();
    toolColumnLayout->addWidget(btnSlip);
    addToolSep();
    toolColumnLayout->addWidget(btnSlide);
    addToolSep();
    toolColumnLayout->addWidget(btnText);
    addToolSep();
    toolColumnLayout->addWidget(btnZoom);
    addToolSep();
    toolColumnLayout->addWidget(btnMaskTools);
    toolColumnLayout->addStretch();

    // Store tool buttons for sync with shortcuts
    m_toolButtons[0] = btnSelection;
    m_toolButtons[1] = btnRipple;
    m_toolButtons[2] = btnRolling;
    m_toolButtons[3] = btnRazor;
    m_toolButtons[4] = btnSlip;
    m_toolButtons[5] = btnSlide;
    m_toolButtons[6] = btnText;
    m_toolButtons[7] = btnZoom;
    m_toolButtons[8] = btnMaskTools;

    return toolColumn;
}

QWidget* TimelineWorkspace::createTimelineToolbar()
{
    const auto& tc = Theme::colors();

    auto* toolbar = new QWidget;
    toolbar->setFixedHeight(30);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget#timelineToolbar { background: %1; border-top: 1px solid %2; "
        "border-bottom: 1px solid %3; }")
        .arg(Theme::hex(tc.surface2))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.border)));
    toolbar->setObjectName(QStringLiteral("timelineToolbar"));

    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(6, 0, 6, 0);
    toolbarLayout->setSpacing(2);

    m_timelineTimecode = new QLabel(QStringLiteral("00;00;00;00"));
    m_timelineTimecode->setAlignment(Qt::AlignCenter);
    m_timelineTimecode->setFixedWidth(100);
    m_timelineTimecode->setFixedHeight(22);
    m_timelineTimecode->setStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 12px; "
        "font-weight: bold; color: %1; background: %2; "
        "padding: 1px 4px; border-radius: 2px; }")
        .arg(Theme::hex(tc.textBright))
        .arg(Theme::hex(tc.surface0)));
    toolbarLayout->addWidget(m_timelineTimecode);
    toolbarLayout->addSpacing(8);

    QString tbIconStyle = QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: none; "
        "padding: 2px; font-size: 14px; min-width: 24px; min-height: 24px; }"
        "QToolButton:hover { background: %2; border-radius: 3px; }"
        "QToolButton:checked { color: %3; background: %4; border-radius: 3px; }")
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.accent))
        .arg(Theme::hex(tc.accentSubtle));

    auto* btnNest = new QToolButton;
    btnNest->setText(QStringLiteral("\u29C9"));
    btnNest->setToolTip(tr("Insert or overwrite sequences as nests or individual clips"));
    btnNest->setCheckable(true);
    btnNest->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnNest);

    auto* btnSnap = new QToolButton;
    btnSnap->setText(QStringLiteral("\U0001F9F2"));
    btnSnap->setToolTip(tr("Snap in Timeline (N)"));
    btnSnap->setCheckable(true);
    btnSnap->setChecked(true);
    btnSnap->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnSnap);

    auto* btnLinked = new QToolButton;
    btnLinked->setText(QStringLiteral("\U0001F517"));
    btnLinked->setToolTip(tr("Linked Selection"));
    btnLinked->setCheckable(true);
    btnLinked->setChecked(true);
    btnLinked->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnLinked);

    auto* btnMarker = new QToolButton;
    btnMarker->setText(QStringLiteral("\u2666"));
    btnMarker->setToolTip(tr("Add Marker (M)"));
    btnMarker->setStyleSheet(tbIconStyle);
    toolbarLayout->addWidget(btnMarker);

    auto* btnCC = new QToolButton;
    btnCC->setText(QStringLiteral("CC"));
    btnCC->setToolTip(tr("Caption Track Options"));
    btnCC->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: 1px solid %1; "
        "padding: 1px 3px; font-size: 11px; font-weight: bold; min-width: 22px; "
        "min-height: 16px; border-radius: 2px; }"
        "QToolButton:hover { background: %2; color: %3; border-color: %3; }")
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.surface3))
        .arg(Theme::hex(tc.textPrimary)));
    toolbarLayout->addWidget(btnCC);

    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarLayout->addWidget(spacer);

    auto* zoomLabel = new QLabel(QStringLiteral("\u2212"));
    zoomLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 0 2px;")
        .arg(Theme::hex(tc.textTertiary)));
    toolbarLayout->addWidget(zoomLabel);

    auto* zoomSlider = new QSlider(Qt::Horizontal);
    zoomSlider->setRange(10, 500);
    zoomSlider->setValue(100);
    zoomSlider->setFixedWidth(120);
    zoomSlider->setToolTip("Timeline Zoom");
    zoomSlider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { background: %1; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %2; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::handle:horizontal:hover { background: %3; }")
        .arg(Theme::hex(tc.border))
        .arg(Theme::hex(tc.textSecondary))
        .arg(Theme::hex(tc.textPrimary)));
    toolbarLayout->addWidget(zoomSlider);

    auto* zoomPlusLabel = new QLabel(QStringLiteral("+"));
    zoomPlusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 0 2px;")
        .arg(Theme::hex(tc.textTertiary)));
    toolbarLayout->addWidget(zoomPlusLabel);

    // Connect zoom slider to timeline layout engine
    connect(zoomSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_timelinePanel) {
            m_timelinePanel->layoutEngine().setPixelsPerSecond(static_cast<double>(value));
            m_timelinePanel->update();
            for (auto* child : m_timelinePanel->findChildren<QWidget*>())
                child->update();
        }
    });

    // Connect linked selection toggle
    connect(btnLinked, &QToolButton::toggled, this, [this](bool checked) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_timelinePanel) {
            m_timelinePanel->setLinkedSelectionEnabled(checked);
        }
    });

    // Connect Add Marker button
    connect(btnMarker, &QToolButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_timeline && m_playbackController && m_commandStack) {
            int64_t tick = m_playbackController->currentTick();
            m_commandStack->execute(
                std::make_unique<AddMarkerCommand>(m_timeline, tick, "Marker"));
            if (m_timelinePanel) m_timelinePanel->update();
        }
    });

    // Connect Nest Sequences toggle
    connect(btnNest, &QToolButton::toggled, this, [this, btnNest](bool checked) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        btnNest->setToolTip(checked
            ? tr("Insert sequences as nested clips")
            : tr("Insert sequences as individual clips"));
        if (m_timelinePanel)
            m_timelinePanel->setNestSequencesEnabled(checked);
    });

    // Connect Caption Track Options button
    connect(btnCC, &QToolButton::clicked, this, [this, btnCC]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        QMenu menu(btnCC);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3; }"
            "QMenu::item:selected { background: %4; }")
            .arg(Theme::hex(Theme::colors().surface1),
                 Theme::hex(Theme::colors().textPrimary),
                 Theme::hex(Theme::colors().border),
                 Theme::hex(Theme::colors().accent)));
        // The caption track is created from the Captions panel
        // ("Add Captions to Timeline"); this button only toggles its
        // visibility. Disable the toggles when no caption track exists.
        const bool hasCaptionTrack = m_timeline && m_timeline->captionTrack();
        QAction* aHide = menu.addAction(tr("Hide Captions Track"), this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelinePanel) m_timelinePanel->setCaptionTrackVisible(false);
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        QAction* aShow = menu.addAction(tr("Show Captions Track"), this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelinePanel) m_timelinePanel->setCaptionTrackVisible(true);
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        aHide->setEnabled(hasCaptionTrack);
        aShow->setEnabled(hasCaptionTrack);
        menu.exec(btnCC->mapToGlobal(QPoint(0, btnCC->height())));
    });

    return toolbar;
}

} // namespace rt
