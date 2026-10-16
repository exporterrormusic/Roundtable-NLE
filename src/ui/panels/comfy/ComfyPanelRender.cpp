/*
 * ComfyPanelRender.cpp — the COMFYUI page's RENDER side: ComfyUI settings,
 * the pre-flight frame-count check, the batch queue and its log. The batch
 * itself runs in ComfyRunner.
 */
#include "panels/comfy/ComfyPanel.h"

#include "panels/audio/AudioSync.h"
#include "panels/audio/VideoFrameGrabber.h"
#include "panels/comfy/ComfyRunner.h"
#include "widgets/VideoFrameView.h"
#include "Settings.h"
#include "Theme.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QThreadPool>
#include <QPointer>
#include <QMenu>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace rt {

namespace {

constexpr const char* kUrlKey      = "ComfyUI/url";
constexpr const char* kLauncherKey = "ComfyUI/launcher";
constexpr const char* kSplitterKey = "ComfyUI/renderSplitter";

enum QueueColumn { QOrder, QShot, QLine, QFps, QTry, QStatus, QOutput, QCount };

QString renderLabelStyle()
{
    return QString("QLabel { color: %1; }").arg(Theme::hex(Theme::colors().textSecondary));
}

QString renderButtonStyle(bool primary)
{
    const auto& tc = Theme::colors();
    return QString("QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: %4px;"
                   " padding: 6px 14px; font-weight: %5; }"
                   "QPushButton:hover { background: %6; }"
                   "QPushButton:disabled { color: %7; }")
        .arg(Theme::hex(primary ? tc.primaryBtnBg : tc.surface2),
             Theme::hex(primary ? tc.textBright : tc.textPrimary),
             Theme::hex(primary ? tc.primaryBtnBg : tc.controlBorder))
        .arg(Theme::metrics().radiusSm)
        .arg(primary ? 700 : 500)
        .arg(Theme::hex(primary ? tc.primaryBtnHover : tc.surface3),
             Theme::hex(tc.textDisabled));
}

/// dev_defaults/comfy/ folder above the running roundtable.exe, or empty.
QString devDefaultsDir()
{
    // Test executables share the app's output folder; keep them hermetic.
    if (QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName().compare(
            QStringLiteral("roundtable"), Qt::CaseInsensitive) != 0)
        return {};
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        const QString candidate = dir.absoluteFilePath(QStringLiteral("dev_defaults/comfy"));
        if (QFileInfo::exists(candidate + QStringLiteral("/comfy_defaults.json"))) return candidate;
        if (!dir.cdUp()) break;
    }
    return {};
}

QString settingsSectionStyle()
{
    return QString("QLabel { color: %1; font-weight: 800; letter-spacing: 1px; padding-top: 6px; }")
        .arg(Theme::hex(Theme::colors().accent));
}

QString shotTag(const std::string& shot)
{
    return shot == "CLOSE-UP" ? QStringLiteral("CU") : QString::fromStdString(shot);
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════
//  UI
// ═════════════════════════════════════════════════════════════════════════

// SETTINGS: everything set once per show, grouped in workflow order.
QWidget* ComfyPanel::setupSettingsPage()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    int row = 0;
    auto section = [&](const QString& title, const QString& note) {
        auto* t = new QLabel(title);
        t->setStyleSheet(settingsSectionStyle());
        grid->addWidget(t, row, 0, 1, 2);
        if (!note.isEmpty()) {
            auto* n = new QLabel(note);
            n->setStyleSheet(renderLabelStyle());
            n->setWordWrap(true);
            grid->addWidget(n, row, 2, 1, 5);
        }
        ++row;
    };
    auto label = [&](const QString& text, int r, int c) {
        auto* l = new QLabel(text);
        l->setStyleSheet(renderLabelStyle());
        grid->addWidget(l, r, c);
    };
    auto pathRow = [&](const QString& text, QLineEdit*& edit, int r, int c, const QString& filter,
                       std::function<void(const QString&)> onSet) {
        label(text, r, c);
        edit = new QLineEdit;
        auto* e = edit;
        connect(e, &QLineEdit::editingFinished, this, [e, onSet]() { onSet(e->text().trimmed()); });
        grid->addWidget(e, r, c + 1);
        auto* browse = new QPushButton(QStringLiteral("…"));
        browse->setFixedWidth(32);
        browse->setStyleSheet(renderButtonStyle(false));
        connect(browse, &QPushButton::clicked, this, [this, e, filter, onSet, text]() {
            const QString start = e->text().isEmpty() ? QString() : QFileInfo(e->text()).absolutePath();
            const QString f = QFileDialog::getOpenFileName(this, text, start, filter);
            if (f.isEmpty()) return;
            e->setText(QDir::toNativeSeparators(f));
            onSet(e->text());
        });
        grid->addWidget(browse, r, c + 2);
    };

    // ── ComfyUI server ─────────────────────────────────────────────────
    section(QStringLiteral("COMFYUI"),
            QStringLiteral("RENDER starts ComfyUI with the launcher if it isn't running. No browser tab is opened."));
    label(QStringLiteral("Address"), row, 0);
    m_urlEdit = new QLineEdit;
    m_urlEdit->setPlaceholderText(QStringLiteral("http://127.0.0.1:8188"));
    connect(m_urlEdit, &QLineEdit::editingFinished, this, [this]() {
        appSettings().setValue(kUrlKey, m_urlEdit->text().trimmed());
    });
    grid->addWidget(m_urlEdit, row, 1);
    pathRow(QStringLiteral("Launcher (.bat)"), m_launcherEdit, row, 3, QStringLiteral("Batch files (*.bat *.cmd)"),
            [](const QString& p) { appSettings().setValue(kLauncherKey, p); });
    auto* serverRow = new QHBoxLayout;
    auto* startServerBtn = new QPushButton(QStringLiteral("Start ComfyUI now"));
    startServerBtn->setStyleSheet(renderButtonStyle(false));
    startServerBtn->setToolTip(QStringLiteral("Optional: warm ComfyUI up before rendering."));
    connect(startServerBtn, &QPushButton::clicked, this, [this]() {
        const QString url = m_urlEdit->text().trimmed().isEmpty() ? m_urlEdit->placeholderText()
                                                                  : m_urlEdit->text().trimmed();
        showPage(kPageQueue);
        m_runner->launchServer(url, m_launcherEdit->text().trimmed());
    });
    serverRow->addWidget(startServerBtn);
    auto* openUiBtn = new QPushButton(QStringLiteral("Open ComfyUI in browser"));
    openUiBtn->setStyleSheet(renderButtonStyle(false));
    openUiBtn->setToolTip(QStringLiteral("ComfyUI's own editor (only when you click this)."));
    connect(openUiBtn, &QPushButton::clicked, this, [this]() {
        const QString url = m_urlEdit->text().trimmed().isEmpty() ? m_urlEdit->placeholderText()
                                                                  : m_urlEdit->text().trimmed();
        QDesktopServices::openUrl(QUrl(url));
    });
    serverRow->addWidget(openUiBtn);
    serverRow->addStretch();
    ++row;
    grid->addLayout(serverRow, row, 1, 1, 5);
    ++row;

    // ── Workflow + what goes into it ───────────────────────────────────
    section(QStringLiteral("WORKFLOW"),
            QStringLiteral("The SCAIL workflow and the inputs RENDER fills in for every shot."));
    pathRow(QStringLiteral("Workflow"), m_workflowEdit, row, 0, QStringLiteral("ComfyUI workflow (*.json)"),
            [this](const QString& p) {
                m_audioSync->pipeline().workflowFile = p.toStdString();
                m_workflowLoadedFrom.clear();
                refreshRenderPage();
            });
    m_workflowStatus = new QLabel;
    m_workflowStatus->setWordWrap(true);
    grid->addWidget(m_workflowStatus, row, 3, 1, 4);
    ++row;
    const QString imgFilter = QStringLiteral("Images (*.png *.jpg *.jpeg *.webp)");
    pathRow(QStringLiteral("WIDE reference"), m_wideRefEdit, row, 0, imgFilter,
            [this](const QString& p) { m_audioSync->pipeline().wideReferenceImage = p.toStdString(); });
    pathRow(QStringLiteral("CLOSE-UP reference"), m_closeRefEdit, row, 3, imgFilter,
            [this](const QString& p) { m_audioSync->pipeline().closeUpReferenceImage = p.toStdString(); });
    ++row;
    label(QStringLiteral("Default expression"), row, 0);
    m_defaultExprEdit = new QLineEdit;
    m_defaultExprEdit->setToolTip(QStringLiteral(
        "Used when a shot has no expression of its own and no ACTION text in the script. "
        "Empty = the workflow's \"5. FACIAL EXPRESSION\"."));
    connect(m_defaultExprEdit, &QLineEdit::editingFinished, this, [this]() {
        m_audioSync->pipeline().defaultExpression = m_defaultExprEdit->text().trimmed().toStdString();
    });
    grid->addWidget(m_defaultExprEdit, row, 1, 1, 6);
    ++row;
    label(QStringLiteral("Seed"), row, 0);
    m_seedEdit = new QLineEdit;
    m_seedEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{0,15}")), m_seedEdit));
    m_seedEdit->setToolTip(QStringLiteral("Same seed for every shot. Empty = the workflow's own seed."));
    connect(m_seedEdit, &QLineEdit::editingFinished, this, [this]() {
        const QString t = m_seedEdit->text().trimmed();
        m_audioSync->pipeline().seed = t.isEmpty() ? -1 : t.toLongLong();
    });
    grid->addWidget(m_seedEdit, row, 1);
    label(QStringLiteral("Frame limit @16 fps"), row, 3);
    m_frameLimitSpin = new QSpinBox;
    m_frameLimitSpin->setRange(0, 10000);
    m_frameLimitSpin->setSpecialValueText(QStringLiteral("Off"));
    m_frameLimitSpin->setToolTip(QStringLiteral(
        "Before rendering, shots that would render more frames than this at 16 fps are listed "
        "so you can switch them to 12 fps."));
    connect(m_frameLimitSpin, &QSpinBox::valueChanged, this, [this](int value) {
        if (!m_updatingControls) m_audioSync->pipeline().frameCapThreshold = value;
    });
    grid->addWidget(m_frameLimitSpin, row, 4);
    ++row;

    // ── Shot files ─────────────────────────────────────────────────────
    section(QStringLiteral("SHOT FILES"),
            QStringLiteral("RENDER exports each shot (framed, trimmed, tail-padded 1920×1080) here, "
                           "then sends it to ComfyUI. Renders go in the \"renders\" folder next to it."));
    label(QStringLiteral("Folder"), row, 0);
    auto* folderRow = new QHBoxLayout;
    m_outputDirEdit = new QLineEdit;
    connect(m_outputDirEdit, &QLineEdit::editingFinished, this, [this]() {
        m_audioSync->pipeline().outputDir = m_outputDirEdit->text().trimmed().toStdString();
    });
    folderRow->addWidget(m_outputDirEdit, 1);
    auto* browse = new QPushButton(QStringLiteral("…"));
    browse->setFixedWidth(32);
    browse->setStyleSheet(renderButtonStyle(false));
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString start = m_outputDirEdit->text().isEmpty() ? defaultOutputDir() : m_outputDirEdit->text();
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Shot folder"), start);
        if (dir.isEmpty()) return;
        m_outputDirEdit->setText(dir);
        m_audioSync->pipeline().outputDir = dir.toStdString();
    });
    folderRow->addWidget(browse);
    auto* openFolderBtn = new QPushButton(QStringLiteral("Open folder"));
    openFolderBtn->setStyleSheet(renderButtonStyle(false));
    connect(openFolderBtn, &QPushButton::clicked, this, [this]() {
        const QString dir = m_outputDirEdit->text().isEmpty() ? defaultOutputDir() : m_outputDirEdit->text();
        if (QDir(dir).exists()) QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    folderRow->addWidget(openFolderBtn);
    grid->addLayout(folderRow, row, 1, 1, 6);
    ++row;
    label(QStringLiteral("Tail pad"), row, 0);
    m_defaultPadSpin = new QSpinBox;
    m_defaultPadSpin->setRange(0, 120);
    m_defaultPadSpin->setSuffix(QStringLiteral(" frames"));
    m_defaultPadSpin->setToolTip(QStringLiteral(
        "Extra frames rendered past the end of every line (SCAIL end-of-clip bug), trimmed off again "
        "on the timeline. 0 = none. A shot can override it on the SHOTS page."));
    connect(m_defaultPadSpin, &QSpinBox::valueChanged, this, [this](int v) {
        if (m_updatingControls) return;
        m_audioSync->pipeline().defaultPadFrames = v;
        for (int i = 0; i < static_cast<int>(m_shots.size()); ++i) updateShotRow(i);
    });
    grid->addWidget(m_defaultPadSpin, row, 1);
    ++row;

    // ── Timeline assembly ──────────────────────────────────────────────
    section(QStringLiteral("TIMELINE ASSEMBLY"),
            QStringLiteral("Used by ASSEMBLE ON TIMELINE."));
    pathRow(QStringLiteral("Background"), m_backgroundEdit, row, 0,
            QStringLiteral("Video or image (*.mp4 *.mov *.mkv *.webm *.png *.jpg *.jpeg);;All files (*)"),
            [this](const QString& p) { m_audioSync->pipeline().background = p.toStdString(); });
    m_backgroundEdit->setPlaceholderText(QStringLiteral("video or image under every shot"));
    ++row;
    label(QStringLiteral("CLOSE-UP zoom"), row, 0);
    m_cuZoomSpin = new QDoubleSpinBox;
    m_cuZoomSpin->setRange(0.0, 8.0);
    m_cuZoomSpin->setSingleStep(0.05);
    m_cuZoomSpin->setDecimals(2);
    m_cuZoomSpin->setSuffix(QStringLiteral("×"));
    m_cuZoomSpin->setSpecialValueText(QStringLiteral("Auto"));
    m_cuZoomSpin->setToolTip(QStringLiteral(
        "How far the background zooms in under CLOSE-UP shots (it stays full size under WIDE). "
        "Auto = the recording's CLOSE-UP scale ÷ WIDE scale (e.g. 90% ÷ 50% = 1.8×)."));
    connect(m_cuZoomSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updatingControls) m_audioSync->pipeline().closeUpBackgroundZoom = v;
    });
    grid->addWidget(m_cuZoomSpin, row, 1);
    label(QStringLiteral("Stinger gap"), row, 3);
    m_gapSpin = new QDoubleSpinBox;
    m_gapSpin->setRange(0.0, 60.0);
    m_gapSpin->setSingleStep(0.5);
    m_gapSpin->setDecimals(1);
    m_gapSpin->setSuffix(QStringLiteral(" s"));
    m_gapSpin->setToolTip(QStringLiteral("Gap left before each segment (STORY, SKINS, …) for its stinger; "
                                         "a marker names each segment."));
    connect(m_gapSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updatingControls) m_audioSync->pipeline().segmentGapSec = v;
    });
    grid->addWidget(m_gapSpin, row, 4);
    ++row;

    label(QStringLiteral("CLOSE-UP position X"), row, 0);
    label(QStringLiteral("Position Y"), row, 3);
    m_cuBackgroundXSpin = new QDoubleSpinBox;
    m_cuBackgroundYSpin = new QDoubleSpinBox;
    m_cuBackgroundXSpin->setObjectName(QStringLiteral("comfyBackgroundPositionX"));
    m_cuBackgroundYSpin->setObjectName(QStringLiteral("comfyBackgroundPositionY"));
    for (auto* spin : {m_cuBackgroundXSpin, m_cuBackgroundYSpin}) {
        spin->setRange(-10000.0, 10000.0);
        spin->setDecimals(1);
        spin->setSingleStep(0.1);
        spin->setSuffix(QStringLiteral(" px"));
        spin->setToolTip(QStringLiteral("Background centre in a 1920 x 1080 frame, measured from the top left. "
                                       "960, 540 is centred. WIDE shots always stay centred at 100%."));
    }
    connect(m_cuBackgroundXSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updatingControls) m_audioSync->pipeline().closeUpBackgroundOffsetX = v - 960.0;
    });
    connect(m_cuBackgroundYSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updatingControls) m_audioSync->pipeline().closeUpBackgroundOffsetY = v - 540.0;
    });
    grid->addWidget(m_cuBackgroundXSpin, row, 1);
    grid->addWidget(m_cuBackgroundYSpin, row, 4);
    ++row;

    pathRow(QStringLiteral("Timeline template"), m_timelineTemplateEdit, row, 0,
            QStringLiteral("Timeline assembly template (*.json)"),
            [this](const QString& p) { m_audioSync->pipeline().timelineTemplate = p.toStdString(); });
    m_timelineTemplateEdit->setObjectName(QStringLiteral("comfyTimelineTemplate"));
    m_timelineTemplateEdit->setPlaceholderText(QStringLiteral("Optional: character framing, stingers, music and overlays"));
    m_gapSpin->setToolTip(QStringLiteral("Gap between segments when no timeline template is selected. "
                                         "A template uses its own stinger durations."));
    ++row;
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(4, 1);
    v->addLayout(grid);
    v->addStretch();
    return page;
}

// QUEUE: the batch (left) and the ComfyUI console (right).
QWidget* ComfyPanel::setupQueuePage()
{
    const auto& tc = Theme::colors();
    m_queueTable = new QTableWidget(0, QCount);
    m_queueTable->setHorizontalHeaderLabels({"#", "SHOT", "LINE", "FPS", "TRY", "STATUS", "OUTPUT"});
    m_queueTable->verticalHeader()->setVisible(false);
    m_queueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto* queueHeader = m_queueTable->horizontalHeader();
    for (auto [col, width] : {std::pair{QOrder, 36}, {QShot, 80}, {QLine, 300}, {QFps, 44},
                              {QTry, 40}, {QStatus, 200}})
        queueHeader->resizeSection(col, width);
    makeColumnsAdjustable(queueHeader, QStringLiteral("ComfyUI/queueColumns"));
    m_queueTable->setStyleSheet(QString("QTableWidget { background: %1; color: %2; border: 1px solid %3; }")
                                    .arg(Theme::hex(tc.surface0), Theme::hex(tc.textPrimary), Theme::hex(tc.border)));

    m_logView = new QPlainTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(3000);
    m_logView->setStyleSheet(QString("QPlainTextEdit { background: %1; color: %2; border: 1px solid %3;"
                                     " font-family: Consolas, monospace; font-size: 12px; }")
                                 .arg(Theme::hex(tc.surface0), Theme::hex(tc.textSecondary), Theme::hex(tc.border)));

    // Queue | log side by side (ComfyUI-style console on the right), both
    // full height; drag the handle to resize, remembered across sessions.
    m_renderSplitter = new QSplitter(Qt::Horizontal);
    m_renderSplitter->setChildrenCollapsible(false);
    m_renderSplitter->setHandleWidth(6);
    // Right: ComfyUI's live preview of the shot rendering now, over the log.
    auto* previewPane = new QWidget;
    auto* pv = new QVBoxLayout(previewPane);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(2);
    m_livePreviewLabel = new QLabel(QStringLiteral("LIVE PREVIEW — appears while a shot renders"));
    m_livePreviewLabel->setStyleSheet(renderLabelStyle());
    pv->addWidget(m_livePreviewLabel);
    m_livePreview = new VideoFrameView;
    m_livePreview->setMinimumSize(160, 90);
    pv->addWidget(m_livePreview, 1);
    m_consoleSplitter = new QSplitter(Qt::Vertical);
    m_consoleSplitter->setChildrenCollapsible(false);
    m_consoleSplitter->addWidget(previewPane);
    m_consoleSplitter->addWidget(m_logView);
    m_consoleSplitter->setSizes({400, 300});
    if (const QByteArray saved = appSettings().value(QStringLiteral("ComfyUI/consoleSplitter")).toByteArray();
        !saved.isEmpty())
        m_consoleSplitter->restoreState(saved);
    connect(m_consoleSplitter, &QSplitter::splitterMoved, this, [this]() {
        appSettings().setValue(QStringLiteral("ComfyUI/consoleSplitter"), m_consoleSplitter->saveState());
    });

    m_renderSplitter->addWidget(m_queueTable);
    m_renderSplitter->addWidget(m_consoleSplitter);
    m_renderSplitter->setStretchFactor(0, 1);
    m_renderSplitter->setStretchFactor(1, 1);
    m_renderSplitter->setSizes({700, 700});
    if (const QByteArray saved = appSettings().value(kSplitterKey).toByteArray(); !saved.isEmpty())
        m_renderSplitter->restoreState(saved);
    connect(m_renderSplitter, &QSplitter::splitterMoved, this, [this]() {
        appSettings().setValue(kSplitterKey, m_renderSplitter->saveState());
    });
    return m_renderSplitter;
}

// The one action row, below every page.
QWidget* ComfyPanel::setupActionBar()
{
    auto* bar = new QWidget;
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 4, 0, 0);
    h->setSpacing(8);

    m_renderAllBtn = new QPushButton(QStringLiteral("RENDER ALL"));
    m_renderAllBtn->setStyleSheet(renderButtonStyle(true));
    m_renderAllBtn->setToolTip(QStringLiteral(
        "Render shots with no video on disk and shots marked Retake, using fresh seeds. "
        "Approved renders and renders awaiting review are kept. "
        "To render only some shots, select them on SHOTS and right-click."));
    connect(m_renderAllBtn, &QPushButton::clicked, this, [this]() {
        std::vector<int> all(m_shots.size());
        for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
        startRender(all, /*includeRendered=*/false, /*freshSeeds=*/true);
    });
    h->addWidget(m_renderAllBtn);
    auto* renderScope = new QLabel(QStringLiteral("Unrendered + retakes · new seeds"));
    renderScope->setStyleSheet(renderLabelStyle());
    h->addWidget(renderScope);
    m_stopRenderBtn = new QPushButton(QStringLiteral("Stop"));
    m_stopRenderBtn->setStyleSheet(renderButtonStyle(false));
    connect(m_stopRenderBtn, &QPushButton::clicked, this, [this]() {
        if (m_ffmpeg) {                // still preparing shots
            m_exportCancelled = true;
            m_exportQueue.clear();
            m_ffmpeg->kill();
        }
        if (m_runner) m_runner->stop();
    });
    h->addWidget(m_stopRenderBtn);

    m_progress = new QProgressBar;
    m_progress->setFixedSize(120, 8);
    m_progress->setTextVisible(false);
    m_progress->setVisible(false);
    h->addWidget(m_progress);
    m_renderStatus = new QLabel;
    m_renderStatus->setStyleSheet(renderLabelStyle());
    h->addWidget(m_renderStatus, 1);

    m_assembleBtn = new QPushButton(QStringLiteral("ASSEMBLE ON TIMELINE"));
    m_assembleBtn->setStyleSheet(renderButtonStyle(true));
    m_assembleBtn->setToolTip(QStringLiteral(
        "Put the chosen render of every shot on a sequence in script order: tail pad trimmed, "
        "keyed with a fresh background for each shot. A timeline template also adds character framing, "
        "stingers, music, transitions and overlays. One undo step. Configure on SETTINGS."));
    connect(m_assembleBtn, &QPushButton::clicked, this, [this]() { emit assembleRequested(); });
    h->addWidget(m_assembleBtn);
    return bar;
}

void ComfyPanel::setupRunner()
{
    m_runner = new ComfyRunner(this);
    connect(m_runner, &ComfyRunner::log, this, &ComfyPanel::appendLog);
    connect(m_runner, &ComfyRunner::serverOutput, this, &ComfyPanel::appendServerLine);
    connect(m_runner, &ComfyRunner::jobChanged, this, [this](int jobIndex) {
        updateQueueRow(jobIndex);
        if (jobIndex < 0 || jobIndex >= static_cast<int>(m_runner->jobs().size())) return;
        const RenderJob& job = m_runner->jobs()[static_cast<size_t>(jobIndex)];
        for (int i = 0; i < static_cast<int>(m_shots.size()); ++i)
            if (m_shots[static_cast<size_t>(i)].key == job.key) updateShotRow(i);   // QUEUED / RENDERING n/8
        if (job.state == RenderJob::State::Running)
            m_livePreviewLabel->setText(QStringLiteral("LIVE PREVIEW — %1").arg(job.label));
    });
    connect(m_runner, &ComfyRunner::preview, this, &ComfyPanel::showLivePreviewFrame);
    connect(m_runner, &ComfyRunner::previewClip, this, &ComfyPanel::showLivePreviewClip);
    connect(m_runner, &ComfyRunner::jobRendered, this, [this](int index, const QString& file) {
        if (index < 0 || index >= static_cast<int>(m_jobShotKeys.size())) return;
        const RenderJob& job = m_runner->jobs()[static_cast<size_t>(index)];
        RenderVersion rv;
        rv.file = file.toStdString();
        rv.seed = job.seed >= 0 ? job.seed : comfy::workflowSeed(m_workflowPrompt, m_workflowTargets);
        rv.expression = job.expression.toStdString();
        rv.fps = job.fpsUsed;
        rv.tailPadSec = job.tailPadSec;
        rv.renderedAt = QDateTime::currentSecsSinceEpoch();
        auto& settings = m_audioSync->pipeline().shots[m_jobShotKeys[static_cast<size_t>(index)]];
        // A file name freed by a deletion can be reused: drop stale records of it.
        settings.renders.erase(std::remove_if(settings.renders.begin(), settings.renders.end(),
                                              [&](const RenderVersion& r) { return r.file == rv.file; }),
                               settings.renders.end());
        settings.renders.push_back(std::move(rv));
        settings.chosenRender = -1;   // newest
        settings.review = ShotReview::Unreviewed;
        settings.reviewedRender.clear();
        for (int i = 0; i < static_cast<int>(m_shots.size()); ++i) {
            if (m_shots[static_cast<size_t>(i)].key != m_jobShotKeys[static_cast<size_t>(index)]) continue;
            updateShotRow(i);
            if (i == m_selected) loadFramingForSelection();   // new render in the USE RENDER list
        }
        updateExportSummary();
    });
    connect(m_runner, &ComfyRunner::finished, this, [this](int done, int failed, bool stopped) {
        m_renderStatus->setText(QString("%1 · %2 rendered · %3 failed")
            .arg(stopped ? QStringLiteral("Stopped") : QStringLiteral("Finished")).arg(done).arg(failed));
        m_livePreviewLabel->setText(QStringLiteral("LIVE PREVIEW — last frame of the batch"));
        for (int i = 0; i < static_cast<int>(m_shots.size()); ++i) updateShotRow(i);   // clear QUEUED
        updateRenderButtons();
    });
    updateRenderButtons();
}

void ComfyPanel::makeColumnsAdjustable(QHeaderView* header, const QString& settingsKey)
{
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setStretchLastSection(true);
    header->setMinimumSectionSize(28);
    header->setSectionsClickable(true);
    if (const QByteArray saved = appSettings().value(settingsKey).toByteArray(); !saved.isEmpty())
        header->restoreState(saved);
    auto save = [header, settingsKey]() { appSettings().setValue(settingsKey, header->saveState()); };
    QObject::connect(header, &QHeaderView::sectionResized, header, save);
    // Right-click the header: tick columns on/off (the last visible one stays).
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(header, &QWidget::customContextMenuRequested, header, [header, save](const QPoint& pos) {
        QMenu menu(header);
        auto* model = header->model();
        const int visible = header->count() - header->hiddenSectionCount();
        for (int c = 0; c < header->count(); ++c) {
            QString label = model ? model->headerData(c, header->orientation()).toString() : QString();
            if (label.isEmpty()) label = QString::number(c + 1);
            QAction* act = menu.addAction(label);
            act->setCheckable(true);
            act->setChecked(!header->isSectionHidden(c));
            act->setEnabled(header->isSectionHidden(c) || visible > 1);
            QObject::connect(act, &QAction::toggled, header, [header, c, save](bool on) {
                header->setSectionHidden(c, !on);
                if (on && header->sectionSize(c) < 40) header->resizeSection(c, 80);
                save();
            });
        }
        menu.addSeparator();
        QObject::connect(menu.addAction(QStringLiteral("Show all columns")), &QAction::triggered, header,
                         [header, save]() {
            for (int c = 0; c < header->count(); ++c) {
                if (!header->isSectionHidden(c)) continue;
                header->setSectionHidden(c, false);
                if (header->sectionSize(c) < 40) header->resizeSection(c, 80);
            }
            save();
        });
        menu.exec(header->mapToGlobal(pos));
    });
}

void ComfyPanel::showLivePreviewFrame(const QImage& image)
{
    ++m_livePreviewGen;   // a still replaces any clip (and pending decode)
    if (m_livePreviewTimer) m_livePreviewTimer->stop();
    m_livePreviewFrames.clear();
    m_livePreview->setFrame(image);
}

void ComfyPanel::showLivePreviewClip(const QByteArray& clip, const QString& mime, double fps)
{
    // The decoder reads files: write the clip to a temp file, decode every
    // frame off the GUI thread, then loop it until the next preview arrives.
    const int gen = ++m_livePreviewGen;
    const QString ext = mime.contains(QLatin1String("webp")) ? QStringLiteral("webp") : QStringLiteral("mp4");
    const QString path = QDir(QDir::tempPath()).filePath(
        QStringLiteral("roundtable_comfy_preview_%1_%2.%3").arg(QCoreApplication::applicationPid()).arg(gen).arg(ext));
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(clip) != clip.size()) return;
    }
    QPointer<ComfyPanel> guard(this);
    QThreadPool::globalInstance()->start([guard, path, gen, fps]() {
        std::vector<QImage> frames = VideoFrameGrabber::decodeAllFrames(path.toStdString(), QSize(1024, 1024));
        QFile::remove(path);
        QMetaObject::invokeMethod(qApp, [guard, gen, fps, frames = std::move(frames)]() mutable {
            if (!guard || gen != guard->m_livePreviewGen || frames.empty()) return;
            ComfyPanel* self = guard.data();
            if (!self->m_livePreviewTimer) {
                self->m_livePreviewTimer = new QTimer(self);
                connect(self->m_livePreviewTimer, &QTimer::timeout, self, [self]() {
                    if (self->m_livePreviewFrames.empty()) return;
                    self->m_livePreviewIndex = (self->m_livePreviewIndex + 1) % self->m_livePreviewFrames.size();
                    self->m_livePreview->setFrame(self->m_livePreviewFrames[self->m_livePreviewIndex]);
                });
            }
            self->m_livePreviewFrames = std::move(frames);
            self->m_livePreviewIndex = 0;
            self->m_livePreview->setFrame(self->m_livePreviewFrames.front());
            const double rate = std::clamp(fps > 0.0 ? fps : 2.0, 0.5, 30.0);
            self->m_livePreviewTimer->setInterval(static_cast<int>(std::lround(1000.0 / rate)));
            if (self->m_livePreviewFrames.size() > 1) self->m_livePreviewTimer->start();
            else self->m_livePreviewTimer->stop();
        });
    });
}

void ComfyPanel::appendLog(const QString& line)
{
    m_logView->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss  ")) + line);
    m_lastLogWasProgress = false;
}

void ComfyPanel::appendServerLine(const QString& line, bool progress)
{
    if (progress && m_lastLogWasProgress && m_logView->document()->blockCount() > 0) {
        // Redraw the bar in place, keeping the view pinned to the bottom if it was.
        auto* bar = m_logView->verticalScrollBar();
        const bool atBottom = !bar || bar->value() >= bar->maximum() - 2;
        QTextCursor cursor(m_logView->document()->lastBlock());
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.removeSelectedText();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText((m_logView->document()->isEmpty() ? QString() : QStringLiteral("\n")) + line);
        if (atBottom && bar) bar->setValue(bar->maximum());
    } else {
        m_logView->appendPlainText(line);
    }
    m_lastLogWasProgress = progress;
}

void ComfyPanel::updateRenderButtons()
{
    // Busy = preparing shots (ffmpeg) or rendering (ComfyUI).
    const bool running = (m_runner && m_runner->isRunning()) || m_ffmpeg;
    if (m_renderAllBtn) m_renderAllBtn->setEnabled(!running);
    if (m_stopRenderBtn) m_stopRenderBtn->setEnabled(running);
}

bool ComfyPanel::loadWorkflow(QString* error)
{
    const QString path = QString::fromStdString(m_audioSync->pipeline().workflowFile);
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("Choose the ComfyUI workflow (.json) on the SETTINGS page first.");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot read %1").arg(path);
        return false;
    }
    const QJsonObject ui = QJsonDocument::fromJson(f.readAll()).object();
    const auto result = comfy::convertUiWorkflow(ui);
    if (!result.ok()) {
        if (error) *error = result.error;
        return false;
    }
    m_workflowUi = ui;
    m_workflowPrompt = result.prompt;
    m_workflowTargets = result.targets;
    m_workflowLoadedFrom = path;
    return true;
}

void ComfyPanel::applyDevDefaults()
{
    if (!m_audioSync) return;
    const QString dir = devDefaultsDir();
    if (dir.isEmpty()) return;
    QFile f(dir + QStringLiteral("/comfy_defaults.json"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject json = QJsonDocument::fromJson(f.readAll()).object();
    auto pathOf = [&](const char* key) -> std::string {
        const QString v = json.value(QLatin1String(key)).toString();
        if (v.isEmpty()) return {};
        const QString abs = QDir::isAbsolutePath(v) ? v : QDir(dir).absoluteFilePath(v);
        return QFileInfo::exists(abs) ? QDir::toNativeSeparators(abs).toStdString() : std::string{};
    };
    auto& state = m_audioSync->pipeline();
    auto fill = [](std::string& field, const std::string& value) {
        if (field.empty() && !value.empty()) field = value;
    };
    fill(state.workflowFile, pathOf("workflow"));
    fill(state.wideReferenceImage, pathOf("wideReference"));
    fill(state.closeUpReferenceImage, pathOf("closeUpReference"));
    fill(state.timelineTemplate, pathOf("timelineTemplate"));
    fill(state.defaultExpression, json.value(QLatin1String("defaultExpression")).toString().trimmed().toStdString());
    // A revised local preset updates this background once per show. Persist
    // its revision so subsequent manual adjustments survive refresh/reload.
    const std::string background = pathOf("background");
    const int revision = std::max(0, json.value(QLatin1String("backgroundFramingRevision")).toInt(0));
    const bool newerFraming = static_cast<uint32_t>(revision) > state.backgroundFramingRevision
        && QFileInfo(QString::fromStdString(state.background)) == QFileInfo(QString::fromStdString(background));
    if (!background.empty() && (state.background.empty() || newerFraming)) {
        state.background = background;
        state.closeUpBackgroundZoom = json.value(QLatin1String("closeUpBackgroundZoom")).toDouble(0.0);
        state.closeUpBackgroundOffsetX = json.value(QLatin1String("closeUpBackgroundPositionX")).toDouble(960.0) - 960.0;
        state.closeUpBackgroundOffsetY = json.value(QLatin1String("closeUpBackgroundPositionY")).toDouble(540.0) - 540.0;
        state.backgroundFramingRevision = static_cast<uint32_t>(revision);
    }
    auto settings = appSettings();
    if (settings.value(kLauncherKey).toString().isEmpty()) {
        const std::string launcher = pathOf("launcher");
        if (!launcher.empty()) settings.setValue(kLauncherKey, QString::fromStdString(launcher));
    }
}

void ComfyPanel::refreshRenderPage()
{
    if (!m_audioSync || !m_urlEdit) return;
    applyDevDefaults();
    const auto& state = m_audioSync->pipeline();
    auto settings = appSettings();
    m_updatingControls = true;
    m_urlEdit->setText(settings.value(kUrlKey).toString());
    m_launcherEdit->setText(settings.value(kLauncherKey).toString());
    m_workflowEdit->setText(QString::fromStdString(state.workflowFile));
    m_wideRefEdit->setText(QString::fromStdString(state.wideReferenceImage));
    m_closeRefEdit->setText(QString::fromStdString(state.closeUpReferenceImage));
    m_seedEdit->setText(state.seed >= 0 ? QString::number(state.seed) : QString());
    m_defaultExprEdit->setText(QString::fromStdString(state.defaultExpression));
    m_frameLimitSpin->setValue(state.frameCapThreshold);
    m_backgroundEdit->setText(QString::fromStdString(state.background));
    m_timelineTemplateEdit->setText(QString::fromStdString(state.timelineTemplate));
    m_cuZoomSpin->setValue(state.closeUpBackgroundZoom);
    m_cuBackgroundXSpin->setValue(960.0 + state.closeUpBackgroundOffsetX);
    m_cuBackgroundYSpin->setValue(540.0 + state.closeUpBackgroundOffsetY);
    m_gapSpin->setValue(state.segmentGapSec);
    m_updatingControls = false;

    const auto& tc = Theme::colors();
    QString error;
    if (state.workflowFile.empty()) {
        m_workflowStatus->setText(QStringLiteral("Choose the SCAIL workflow (e.g. SCAIL 2 FINAL V10.json)."));
        m_workflowStatus->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.warning)));
    } else if (!loadWorkflow(&error)) {
        m_workflowStatus->setText(error);
        m_workflowStatus->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.error)));
    } else {
        QStringList missing;
        const auto& t = m_workflowTargets;
        if (t.drivingVideo.isEmpty()) missing << "1. DRIVING VIDEO";
        if (t.referenceImage.isEmpty()) missing << "2. REFERENCE IMAGE";
        if (t.expression.isEmpty()) missing << "5. FACIAL EXPRESSION";
        if (t.runSettings.isEmpty()) missing << "6. RUN SETTINGS";
        if (t.save.isEmpty()) missing << "7. SAVE";
        const qint64 seed = comfy::workflowSeed(m_workflowPrompt, t);
        m_seedEdit->setPlaceholderText(seed >= 0 ? QStringLiteral("workflow seed (%1)").arg(seed)
                                                 : QStringLiteral("workflow seed"));
        m_defaultExprEdit->setPlaceholderText(comfy::workflowExpression(m_workflowPrompt, t));
        m_workflowStatus->setText(missing.isEmpty()
            ? QStringLiteral("✓ %1 nodes · all inputs found").arg(m_workflowPrompt.size())
            : QStringLiteral("Missing nodes: %1").arg(missing.join(QStringLiteral(", "))));
        m_workflowStatus->setStyleSheet(QString("QLabel { color: %1; }")
            .arg(Theme::hex(missing.isEmpty() ? tc.success : tc.error)));
    }

    updateExportSummary();   // RENDER ALL (n)
}

// ═════════════════════════════════════════════════════════════════════════
//  Batch
// ═════════════════════════════════════════════════════════════════════════

QString ComfyPanel::expressionFor(const ShowShot& shot) const
{
    const auto& state = m_audioSync->pipeline();
    auto it = state.shots.find(shot.key);
    if (it != state.shots.end() && !it->second.expression.empty())
        return QString::fromStdString(it->second.expression);
    if (!shot.action.empty()) return QString::fromStdString(shot.action);
    return QString::fromStdString(state.defaultExpression);   // empty → workflow's own
}

QString ComfyPanel::rendersDir() const
{
    QString source = QString::fromStdString(m_audioSync->pipeline().outputDir);
    if (source.isEmpty()) source = defaultOutputDir();
    QDir dir(source);
    if (dir.dirName().compare(QLatin1String("source"), Qt::CaseInsensitive) == 0) {
        dir.cdUp();
        return dir.absoluteFilePath(QStringLiteral("renders"));
    }
    return dir.absoluteFilePath(QStringLiteral("renders"));
}

void ComfyPanel::startRender(const std::vector<int>& shotIndices, bool includeRendered, bool freshSeeds)
{
    if (!m_audioSync || !m_runner || m_runner->isRunning()) return;
    applyDevDefaults();
    auto& state = m_audioSync->pipeline();

    QString error;
    if (!loadWorkflow(&error)) {
        QMessageBox::warning(this, QStringLiteral("Render"), error);
        return;
    }

    std::vector<int> picked;
    bool needWide = false, needCloseUp = false;
    for (int i : shotIndices) {
        if (i < 0 || i >= static_cast<int>(m_shots.size())) continue;
        const ShowShot& shot = m_shots[static_cast<size_t>(i)];
        if (!isExportable(shot)) continue;
        auto it = state.shots.find(shot.key);
        if (!includeRendered && it != state.shots.end() && !needsRender(it->second)) continue;
        picked.push_back(i);
        (shot.shot == "CLOSE-UP" ? needCloseUp : needWide) = true;
    }
    if (picked.empty()) {
        QMessageBox::information(this, QStringLiteral("Render"),
            QStringLiteral("Nothing to render — no unfinished shots or retakes have a picked video take. "
                           "Mark a shot Retake on SHOTS to generate a new version."));
        return;
    }
    const auto refOk = [](const std::string& p) { return !p.empty() && QFileInfo::exists(QString::fromStdString(p)); };
    if ((needWide && !refOk(state.wideReferenceImage)) || (needCloseUp && !refOk(state.closeUpReferenceImage))) {
        QMessageBox::warning(this, QStringLiteral("Render"),
            QStringLiteral("Set the %1 reference image on the SETTINGS page.")
                .arg(needWide && !refOk(state.wideReferenceImage) ? QStringLiteral("WIDE") : QStringLiteral("CLOSE-UP")));
        return;
    }

    // ── Pre-flight: shots over the frame limit at 16 fps ─────────────────
    if (state.frameCapThreshold > 0) {
        std::vector<int> over;
        for (int i : picked) {
            const ShowShot& shot = m_shots[static_cast<size_t>(i)];
            const double dur = shot.take.end - shot.take.start;
            if (state.fpsFor(shot.key) == kScailDefaultFps &&
                state.renderFramesFor(shot.key, dur) > state.frameCapThreshold)
                over.push_back(i);
        }
        if (!over.empty()) {
            QDialog dlg(this);
            dlg.setWindowTitle(QStringLiteral("Shots over the frame limit"));
            auto* lay = new QVBoxLayout(&dlg);
            auto* intro = new QLabel(QStringLiteral(
                "These shots render more than %1 frames at 16 fps. Ticked shots switch to 12 fps "
                "(saved on the shot). Untick any you want to keep at 16.").arg(state.frameCapThreshold));
            intro->setWordWrap(true);
            lay->addWidget(intro);
            auto* list = new QListWidget;
            for (int i : over) {
                const ShowShot& shot = m_shots[static_cast<size_t>(i)];
                const double dur = shot.take.end - shot.take.start;
                const int f16 = state.renderFramesFor(shot.key, dur);
                const int f12 = static_cast<int>(std::ceil(dur * kScailReducedFps - 1e-6)) + state.padFramesFor(shot.key);
                auto* item = new QListWidgetItem(QStringLiteral("%1 · %2 · %3 — %4 frames @16 → %5 @12")
                    .arg(i + 1).arg(QString::fromStdString(shot.segment), shotTag(shot.shot))
                    .arg(f16).arg(f12));
                item->setToolTip(QString::fromStdString(shot.dialogue));
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Checked);
                item->setData(Qt::UserRole, i);
                list->addItem(item);
            }
            lay->addWidget(list);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Start rendering"));
            connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            lay->addWidget(buttons);
            dlg.resize(620, 420);
            if (dlg.exec() != QDialog::Accepted) return;
            for (int r = 0; r < list->count(); ++r) {
                if (list->item(r)->checkState() != Qt::Checked) continue;
                const int i = list->item(r)->data(Qt::UserRole).toInt();
                state.shots[m_shots[static_cast<size_t>(i)].key].fps = kScailReducedFps;
                updateShotRow(i);
            }
        }
    }

    // ── Export the shots (always fresh), then hand them to ComfyUI ───────
    if (picked.size() > 1) showPage(kPageQueue);
    appendLog(QStringLiteral("Preparing %1 shot(s) for ComfyUI…").arg(picked.size()));
    prepareShots(picked, [this, picked, freshSeeds]() { launchRender(picked, freshSeeds); });
}

void ComfyPanel::launchRender(const std::vector<int>& shotIndices, bool freshSeeds)
{
    if (!m_audioSync || !m_runner || m_runner->isRunning()) return;
    auto& state = m_audioSync->pipeline();
    std::vector<int> picked;
    for (int i : shotIndices)
        if (m_preparedKeys.count(m_shots[static_cast<size_t>(i)].key)) picked.push_back(i);
    if (picked.empty()) {
        m_renderStatus->setText(QStringLiteral("No shot could be prepared — see the log on QUEUE."));
        return;
    }

    // ── Jobs ─────────────────────────────────────────────────────────────
    std::vector<RenderJob> jobs;
    m_jobShotKeys.clear();
    for (int i : picked) {
        const ShowShot& shot = m_shots[static_cast<size_t>(i)];
        auto& settings = state.shots[shot.key];
        if (freshSeeds) {
            do {
                settings.setFreshSeed(QRandomGenerator::global()->generate(),
                                     comfy::workflowSeed(m_workflowPrompt, m_workflowTargets));
            } while (settings.seed == state.seed);
        }
        RenderJob job;
        job.key = shot.key;
        job.order = i + 1;
        job.label = QStringLiteral("%1 · %2 · %3").arg(i + 1, 3, 10, QLatin1Char('0'))
                        .arg(QString::fromStdString(shot.segment), shotTag(shot.shot));
        job.drivingVideo = QString::fromStdString(settings.exportedFile);
        job.closeUp = shot.shot == "CLOSE-UP";
        job.expression = expressionFor(shot);
        job.seed = state.seedFor(shot.key);
        job.fps = state.fpsFor(shot.key);
        job.tailPadSec = settings.exportedTailPadSec;
        job.frames = state.renderFramesFor(shot.key, shot.take.end - shot.take.start);
        jobs.push_back(std::move(job));
        m_jobShotKeys.push_back(shot.key);
    }
    if (freshSeeds) loadFramingForSelection();

    ComfyRunner::Config config;
    const QString url = m_urlEdit->text().trimmed();
    if (!url.isEmpty()) config.url = url;
    config.launcher = m_launcherEdit->text().trimmed();
    config.prompt = m_workflowPrompt;
    config.uiWorkflow = m_workflowUi;
    config.targets = m_workflowTargets;
    config.wideReference = QString::fromStdString(state.wideReferenceImage);
    config.closeUpReference = QString::fromStdString(state.closeUpReferenceImage);
    config.rendersDir = rendersDir();
    QString safe = m_audioSync->activeScriptName();
    safe.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9 _-])")), QString());
    config.filenamePrefix = QStringLiteral("roundtable/") + (safe.trimmed().isEmpty() ? QStringLiteral("show") : safe.trimmed());

    m_queueTable->setRowCount(0);
    m_queueTable->setRowCount(static_cast<int>(jobs.size()));
    showLivePreviewFrame(QImage());
    m_runner->start(config, std::move(jobs));
    for (int r = 0; r < m_queueTable->rowCount(); ++r) updateQueueRow(r);
    for (int i = 0; i < static_cast<int>(m_shots.size()); ++i) updateShotRow(i);
    m_renderStatus->setText(QStringLiteral("Rendering %1 shot(s)… you can keep editing.").arg(m_queueTable->rowCount()));
    updateRenderButtons();
}

void ComfyPanel::updateQueueRow(int jobIndex)
{
    if (!m_runner || jobIndex < 0 || jobIndex >= static_cast<int>(m_runner->jobs().size())) return;
    if (jobIndex >= m_queueTable->rowCount()) m_queueTable->setRowCount(jobIndex + 1);
    const RenderJob& job = m_runner->jobs()[static_cast<size_t>(jobIndex)];
    const auto& tc = Theme::colors();
    auto set = [&](int col, const QString& text, const QColor& color = QColor()) {
        auto* item = m_queueTable->item(jobIndex, col);
        if (!item) {
            item = new QTableWidgetItem;
            m_queueTable->setItem(jobIndex, col, item);
        }
        item->setText(text);
        item->setForeground(color.isValid() ? QBrush(color) : QBrush(tc.textPrimary));
    };
    QString line;
    for (const auto& s : m_shots) if (s.key == job.key) line = QString::fromStdString(s.dialogue);
    set(QOrder, QString::number(job.order));
    set(QShot, job.label.section(QStringLiteral(" · "), 1));
    set(QLine, line);
    set(QFps, QString::number(job.fpsUsed > 0 ? job.fpsUsed : job.fps));
    set(QTry, job.attempt > 0 ? QString::number(job.attempt + 1) : QString());
    QColor color;
    switch (job.state) {
    case RenderJob::State::Done:    color = tc.success; break;
    case RenderJob::State::Failed:  color = tc.error; break;
    case RenderJob::State::Running: color = tc.accent; break;
    default:                        color = tc.textSecondary; break;
    }
    set(QStatus, job.state == RenderJob::State::Pending && job.status.isEmpty() ? QStringLiteral("Waiting") : job.status,
        color);
    set(QOutput, QFileInfo(job.outputFile).fileName());
}

} // namespace rt
