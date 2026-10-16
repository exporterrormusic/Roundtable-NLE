/*
 * ComfyPanel.cpp — see header.
 */
#include "panels/comfy/ComfyPanel.h"
#include "AppPaths.h"

#include "panels/audio/AudioSync.h"
#include "panels/audio/VideoFrameGrabber.h"
#include "panels/comfy/ComfyRunner.h"
#include "panels/comfy/FramingEditor.h"
#include "widgets/VideoFrameView.h"
#include "Settings.h"
#include "PathUtils.h"
#include "Theme.h"
#include "decode/VideoDecoder.h"

#include <QApplication>
#include <QSaveFile>
#include <QButtonGroup>
#include <QComboBox>
#include <QCollator>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QHideEvent>
#include <QMouseEvent>
#include <QAbstractSpinBox>
#include <QKeyEvent>
#include <QPainter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QScrollArea>
#include <QRandomGenerator>
#include <QMenu>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <cmath>

namespace rt {

/// Timeline under the SHOTS big view: visible range, in/out region with
/// draggable handles (framing mode), playhead; click/drag elsewhere scrubs.
class ComfyTrimBar : public QWidget
{
public:
    std::function<void(double)> onScrub;                        ///< playhead dragged
    std::function<void(double, double, bool)> onTrim;           ///< in, out, final (mouse released)

    ComfyTrimBar() { setMinimumHeight(34); setMouseTracking(true); }

    void setRange(double viewStart, double viewEnd) { m_v0 = viewStart; m_v1 = std::max(viewStart + 0.01, viewEnd); update(); }
    void setInOut(double in, double out, bool handles) { m_in = in; m_out = out; m_handles = handles; update(); }
    void setPlayhead(double t) { m_head = t; update(); }
    [[nodiscard]] double in() const noexcept { return m_in; }
    [[nodiscard]] bool draggingIn() const noexcept { return m_drag == 1; }
    [[nodiscard]] double viewStart() const noexcept { return m_v0; }
    [[nodiscard]] double viewEnd() const noexcept { return m_v1; }
    [[nodiscard]] double out() const noexcept { return m_out; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const auto& tc = Theme::colors();
        QPainter g(this);
        const QRect track = rect().adjusted(0, 6, 0, -6);
        g.fillRect(track, tc.surface0);
        if (m_handles) {
            const int x0 = xOf(m_in), x1 = xOf(m_out);
            g.fillRect(QRect(QPoint(x0, track.top()), QPoint(x1, track.bottom())), tc.accentDim);
            for (int x : {x0, x1}) g.fillRect(QRect(x - 3, track.top() - 4, 6, track.height() + 8), tc.accent);
        } else {
            g.fillRect(track, tc.accentDim);
        }
        const int ph = xOf(m_head);
        g.fillRect(QRect(ph - 1, 0, 2, height()), tc.textBright);
        g.setPen(tc.border);
        g.drawRect(track.adjusted(0, 0, -1, -1));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int x = static_cast<int>(e->position().x());
        m_drag = 0;
        if (m_handles) {
            if (std::abs(x - xOf(m_in)) <= 7) m_drag = 1;
            else if (std::abs(x - xOf(m_out)) <= 7) m_drag = 2;
        }
        if (m_drag == 0) m_drag = 3;
        mouseMoveEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int x = static_cast<int>(e->position().x());
        if (m_handles && (std::abs(x - xOf(m_in)) <= 7 || std::abs(x - xOf(m_out)) <= 7))
            setCursor(Qt::SizeHorCursor);
        else
            setCursor(Qt::PointingHandCursor);
        if (!(e->buttons() & Qt::LeftButton) || m_drag == 0) return;
        const double t = std::clamp(tOf(x), m_v0, m_v1);
        if (m_drag == 1) { m_in = std::min(t, m_out - 0.1); if (onTrim) onTrim(m_in, m_out, false); }
        else if (m_drag == 2) { m_out = std::max(t, m_in + 0.1); if (onTrim) onTrim(m_in, m_out, false); }
        else { m_head = t; if (onScrub) onScrub(t); }
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && (m_drag == 1 || m_drag == 2) && onTrim) onTrim(m_in, m_out, true);
        m_drag = 0;
    }

private:
    int xOf(double t) const { return static_cast<int>(std::lround((t - m_v0) / (m_v1 - m_v0) * (width() - 1))); }
    double tOf(int x) const { return m_v0 + (m_v1 - m_v0) * x / std::max(1, width() - 1); }

    double m_v0{0.0}, m_v1{1.0}, m_in{0.0}, m_out{1.0}, m_head{0.0};
    bool   m_handles{true};
    int    m_drag{0};   ///< 1 in, 2 out, 3 playhead
};

namespace {

enum Column { ColOrder, ColShot, ColLine, ColTake, ColLength, ColPad, ColStatus, ColApprove, ColRetake, ColCount };
constexpr int kShotIndexRole = Qt::UserRole + 1;
constexpr int kSortValueRole = Qt::UserRole + 2;
const QSize kPreviewFrameSize(960, 540);

class ShotTreeItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override
    {
        // Segment headings stay in script order in either sort direction.
        if (!parent()) {
            const int a = data(0, kSortValueRole).toInt();
            const int b = other.data(0, kSortValueRole).toInt();
            return treeWidget()->header()->sortIndicatorOrder() == Qt::DescendingOrder ? a > b : a < b;
        }
        const int column = treeWidget()->sortColumn();
        static const QCollator collator = [] {
            QCollator c;
            c.setNumericMode(true);
            c.setCaseSensitivity(Qt::CaseInsensitive);
            return c;
        }();
        if (column == ColOrder || column == ColLength || column == ColPad) {
            const double a = data(column, kSortValueRole).toDouble();
            const double b = other.data(column, kSortValueRole).toDouble();
            if (a != b) return a < b;
        } else if (column == ColApprove || column == ColRetake) {
            if (checkState(column) != other.checkState(column))
                return checkState(column) < other.checkState(column);
        } else if (column == ColTake) {
            const int name = collator.compare(data(column, kSortValueRole).toString(),
                                              other.data(column, kSortValueRole).toString());
            if (name != 0) return name < 0;
            const double a = data(column, kSortValueRole + 1).toDouble();
            const double b = other.data(column, kSortValueRole + 1).toDouble();
            if (a != b) return a < b;
        } else {
            const int result = collator.compare(text(column), other.text(column));
            if (result != 0) return result < 0;
        }
        return data(0, kShotIndexRole).toInt() < other.data(0, kShotIndexRole).toInt();
    }
};

/// "<shot file name>": shotExportSignature() of the export that made it.
constexpr const char* kExportManifest = ".roundtable_shots.json";

QJsonObject readExportManifest(const QString& dir)
{
    QFile f(dir + QLatin1Char('/') + QLatin1String(kExportManifest));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeExportManifestEntry(const std::string& outputFile, const std::string& signature)
{
    const QFileInfo out(QString::fromStdString(outputFile));
    const QString dir = out.absolutePath();
    QJsonObject manifest = readExportManifest(dir);
    manifest.insert(out.fileName(), QString::fromStdString(signature));
    QSaveFile f(dir + QLatin1Char('/') + QLatin1String(kExportManifest));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

std::string exportSignatureOf(const ShotExportJob& job)
{
    const QFileInfo src(QString::fromStdString(job.sourceFile));
    return shotExportSignature(job, src.size(), src.lastModified().toMSecsSinceEpoch());
}

QString sectionLabelStyle()
{
    return QString("QLabel { color: %1; font-size: %2px; font-weight: 800; letter-spacing: 1px; }")
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::typography().sizeXs);
}

QString buttonStyle(bool primary)
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

} // namespace

ComfyPanel::ComfyPanel(AudioSync* audioSync, QWidget* parent)
    : QWidget(parent)
    , m_audioSync(audioSync)
    , m_grabber(std::make_unique<VideoFrameGrabber>())
{
    setupUi();
}

ComfyPanel::~ComfyPanel()
{
    if (m_ffmpeg) {
        m_ffmpeg->disconnect(this);
        m_ffmpeg->kill();
        m_ffmpeg->waitForFinished(3000);
    }
}

void ComfyPanel::hideEvent(QHideEvent* event)
{
    stopShotPlayback();   // don't keep playing on another page
    if (m_audioSync) m_audioSync->stopTakeAudio();   // hand the engine back
    if (m_keyFilterInstalled) {
        qApp->removeEventFilter(this);
        m_keyFilterInstalled = false;
    }
    QWidget::hideEvent(event);
}

bool ComfyPanel::eventFilter(QObject* watched, QEvent* event)
{
    const bool press = event->type() == QEvent::KeyPress;
    if ((press || event->type() == QEvent::KeyRelease) && isVisible() &&
        m_pages && m_pages->currentIndex() == kPageShots && !QApplication::activeModalWidget()) {
        QWidget* fw = QApplication::focusWidget();
        const bool inPanel = !fw || fw == this || isAncestorOf(fw);
        const bool typing = fw && (qobject_cast<QLineEdit*>(fw) || qobject_cast<QAbstractSpinBox*>(fw) ||
                                   (qobject_cast<QComboBox*>(fw) && static_cast<QComboBox*>(fw)->isEditable()));
        auto* ke = static_cast<QKeyEvent*>(event);
        // Real input reaches the WINDOW first, then Qt forwards it to the
        // focus widget. MainWindow's filter consumes Space/J/K/L at the window
        // stage, so claim them at whichever stage we see first — as long as
        // they're aimed at the window this panel lives in.
        QWidget* top = window();
        bool ourWindow = false;
        if (auto* qw = qobject_cast<QWindow*>(watched)) ourWindow = top && qw == top->windowHandle();
        else if (auto* w = qobject_cast<QWidget*>(watched)) ourWindow = w->window() == top;
        if (inPanel && ourWindow && !typing && !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) &&
            handleTransportKey(ke, press))
            return true;
    }
    if (watched == m_outputPreview && event->type() == QEvent::MouseButtonRelease) {
        setBigView(0);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ComfyPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_keyFilterInstalled) {   // after MainWindow's, so it runs first
        qApp->installEventFilter(this);
        m_keyFilterInstalled = true;
    }
    refresh();
}

// ═════════════════════════════════════════════════════════════════════════
//  UI
// ═════════════════════════════════════════════════════════════════════════

void ComfyPanel::setupUi()
{
    const auto& tc = Theme::colors();
    setStyleSheet(QString("QWidget#comfyPanel { background: %1; }").arg(Theme::hex(tc.surface1)));
    setObjectName("comfyPanel");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(10);

    // ── Header ─────────────────────────────────────────────────────────
    auto* header = new QHBoxLayout;
    m_titleLabel = new QLabel(QStringLiteral("COMFYUI"));
    m_titleLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 20px; font-weight: 800; }")
                                    .arg(Theme::hex(tc.textPrimary)));
    header->addWidget(m_titleLabel);
    // Pages in workflow order: SHOTS (frame + review) | QUEUE (batch + log) | SETTINGS
    auto* pageGroup = new QButtonGroup(this);
    m_pageButtons = pageGroup;
    pageGroup->setExclusive(true);
    const QString modeStyle = QString(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; padding: 5px 16px; font-weight: 700; }"
        "QPushButton:checked { background: %4; color: %5; border-color: %4; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.textSecondary), Theme::hex(tc.controlBorder),
             Theme::hex(tc.accent), Theme::hex(tc.textBright));
    const QStringList pageNames{QStringLiteral("SHOTS"), QStringLiteral("QUEUE"), QStringLiteral("SETTINGS")};
    for (int i = 0; i < pageNames.size(); ++i) {
        auto* b = new QPushButton(pageNames[i]);
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(modeStyle);
        pageGroup->addButton(b, i);
        header->addWidget(b);
    }
    connect(pageGroup, &QButtonGroup::idClicked, this, &ComfyPanel::showPage);
    header->addSpacing(12);
    m_hintLabel = new QLabel;
    m_hintLabel->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.textSecondary)));
    header->addWidget(m_hintLabel, 1);
    root->addLayout(header);

    auto* guide = new QLabel(QStringLiteral(
        "1  Pick a take for every line on the AUDIO page   ›   2  Check each shot's framing here   ›   "
        "3  RENDER (exports + renders in ComfyUI)   ›   4  ASSEMBLE ON TIMELINE"));
    guide->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.textTertiary)));
    root->addWidget(guide);

    m_pages = new QStackedWidget;
    root->addWidget(m_pages, 1);
    auto* shotsPage = new QWidget;
    auto* shotsLayout = new QVBoxLayout(shotsPage);
    shotsLayout->setContentsMargins(0, 0, 0, 0);
    shotsLayout->setSpacing(10);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    shotsLayout->addWidget(splitter, 1);

    // ── Shot list ──────────────────────────────────────────────────────
    m_shotTree = new QTreeWidget;
    m_shotTree->setColumnCount(ColCount);
    m_shotTree->setHeaderLabels({"#", "SHOT", "LINE", "TAKE", "LENGTH", "PAD", "STATUS", "APPROVE", "RETAKE"});
    m_shotTree->setRootIsDecorated(false);
    m_shotTree->setUniformRowHeights(true);
    m_shotTree->setAlternatingRowColors(true);
    // Several shots can be selected to render just those (right-click).
    m_shotTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_shotTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_shotTree, &QWidget::customContextMenuRequested, this, &ComfyPanel::showShotContextMenu);
    // Every column can be dragged wider/narrower or hidden; remembered.
    auto* shotHeader = m_shotTree->header();
    for (auto [col, width] : {std::pair{ColOrder, 44}, {ColShot, 86}, {ColLine, 360}, {ColTake, 150},
                              {ColLength, 110}, {ColPad, 44}, {ColApprove, 76}, {ColRetake, 76}})
        shotHeader->resizeSection(col, width);
    makeColumnsAdjustable(shotHeader, QStringLiteral("ComfyUI/shotColumns"));
    shotHeader->setSortIndicator(ColOrder, Qt::AscendingOrder);
    m_shotTree->setSortingEnabled(true);
    connect(m_shotTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        if (col != ColApprove && col != ColRetake) return;
        const QVariant v = item->data(0, kShotIndexRole);
        if (!v.isValid() || !m_audioSync) return;
        const int index = v.toInt();
        if (index < 0 || index >= static_cast<int>(m_shots.size())) return;
        auto& settings = m_audioSync->pipeline().shots[m_shots[static_cast<size_t>(index)].key];
        const RenderVersion* render = usableRender(settings);
        const bool checked = item->checkState(col) == Qt::Checked;
        if (col == ColApprove && checked && !render) {
            updateShotRow(index);
            return;
        }
        settings.review = checked ? (col == ColApprove ? ShotReview::Approved : ShotReview::Retake)
                                  : ShotReview::Unreviewed;
        settings.reviewedRender = checked && render ? render->file : std::string{};
        updateShotRow(index);
        updateExportSummary();
    });
    connect(m_shotTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int col) {
        if (col == ColApprove || col == ColRetake) return;
        const QVariant v = item ? item->data(0, kShotIndexRole) : QVariant();
        if (v.isValid()) playAndTrimTake(v.toInt());
    });
    m_shotTree->setStyleSheet(QString(
        "QTreeWidget { background: %1; alternate-background-color: %2; color: %3; border: 1px solid %4;"
        " border-radius: %5px; }"
        "QTreeWidget::item { padding: 4px 2px; }"
        "QTreeWidget::item:selected { background: %6; color: %7; }")
        .arg(Theme::hex(tc.surface0), Theme::hex(tc.surface2), Theme::hex(tc.textPrimary),
             Theme::hex(tc.border))
        .arg(Theme::metrics().radiusMd)
        .arg(Theme::hex(tc.accentDim), Theme::hex(tc.textBright)));
    connect(m_shotTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (!item) return;
        const QVariant v = item->data(0, kShotIndexRole);
        if (v.isValid()) selectShot(v.toInt());
    });
    splitter->addWidget(m_shotTree);

    // ── Framing column ─────────────────────────────────────────────────
    auto* framingPane = new QWidget;
    auto* fl = new QVBoxLayout(framingPane);
    fl->setContentsMargins(12, 0, 0, 0);
    fl->setSpacing(8);

    auto* framingLabel = new QLabel(QStringLiteral("FRAMING"));
    framingLabel->setStyleSheet(sectionLabelStyle());
    fl->addWidget(framingLabel);
    m_framingTitle = new QLabel;
    m_framingTitle->setWordWrap(true);
    m_framingTitle->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.textPrimary)));
    fl->addWidget(m_framingTitle);

    // Big view: the framing editor (the take) or the chosen render — pick
    // with the INPUT / RENDER thumbnails below.
    m_bigView = new QStackedWidget;
    m_bigView->setMinimumHeight(260);
    m_framingEditor = new FramingEditor;
    m_bigView->addWidget(m_framingEditor);
    m_bigRender = new VideoFrameView;
    m_bigView->addWidget(m_bigRender);
    fl->addWidget(m_bigView, 1);

    // Transport: play / pause, timeline (in/out handles on the take), time.
    auto* transport = new QHBoxLayout;
    m_playBtn = new QPushButton(QStringLiteral("▶"));
    m_playBtn->setFixedWidth(40);
    m_playBtn->setStyleSheet(buttonStyle(false));
    m_playBtn->setToolTip(QStringLiteral("Play / pause (the take with its audio, or the render)"));
    connect(m_playBtn, &QPushButton::clicked, this, &ComfyPanel::togglePlayback);
    transport->addWidget(m_playBtn);
    m_trimBar = new ComfyTrimBar;
    m_trimBar->setToolTip(QStringLiteral(
        "Drag the handles to set the in / out (the take, or the render shown big). Click or drag elsewhere to scrub.\n"
        "Keys: Space play · J/K/L shuttle · K+J/L step (hold = slow) · ←/→ frame · I/O set in/out · Home/End."));
    m_trimBar->setFocusPolicy(Qt::ClickFocus);
    m_bigView->setFocusPolicy(Qt::ClickFocus);
    m_trimBar->onScrub = [this](double t) { stopShotPlayback(); seekTo(t); scrubAudio(t); };
    m_trimBar->onTrim = [this](double in, double out, bool final) {
        stopShotPlayback();
        const double at = m_trimBar->draggingIn() ? in : out;
        seekTo(at);   // show the frame at the handle
        if (!final) { scrubAudio(at); return; }
        if (framingMode()) commitTrim(in, out);
        else commitRenderTrim(in, out);
    };
    transport->addWidget(m_trimBar, 1);
    m_timeLabel = new QLabel;
    m_timeLabel->setMinimumWidth(150);
    m_timeLabel->setStyleSheet(QString("QLabel { color: %1; font-family: Consolas, monospace; }")
                                   .arg(Theme::hex(tc.textSecondary)));
    transport->addWidget(m_timeLabel);
    fl->addLayout(transport);

    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(30);
    connect(m_playTimer, &QTimer::timeout, this, [this]() {
        const double in = m_trimBar->in(), out = m_trimBar->out();
        double t;
        if (m_playWithAudio) {
            t = m_audioSync->takeAudioTime();
            if (t < 0.0) t = m_speed > 0.0 ? out : in;            // audio ran off the loaded range
        } else {
            t = m_playStartHead + m_speed * (QDateTime::currentMSecsSinceEpoch() - m_playStartMs) / 1000.0;
        }
        if (m_speed > 0.0 && t >= out) { stopShotPlayback(); seekTo(out); return; }
        if (m_speed < 0.0 && t <= in)  { stopShotPlayback(); seekTo(in);  return; }
        seekTo(t);
    });
    // K held + J/L: a tap steps a frame; still held after this → slow motion.
    m_slowHoldTimer = new QTimer(this);
    m_slowHoldTimer->setSingleShot(true);
    m_slowHoldTimer->setInterval(300);
    connect(m_slowHoldTimer, &QTimer::timeout, this, [this]() {
        if (m_kHeld && m_slowDir != 0) startPlayback(0.25 * m_slowDir);
    });
    connect(m_framingEditor, &FramingEditor::framingChanged, this,
            [this](const ShotFraming& f) { applyFraming(f, false); });
    connect(m_framingEditor, &FramingEditor::framingCommitted, this,
            [this](const ShotFraming& f) { applyFraming(f, true); });

    // Applies-to toggle
    auto* modeRow = new QHBoxLayout;
    m_recordingModeBtn = new QRadioButton(QStringLiteral("All shots of this type in this recording"));
    m_shotModeBtn = new QRadioButton(QStringLiteral("This shot only"));
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_recordingModeBtn);
    modeGroup->addButton(m_shotModeBtn);
    for (auto* b : {m_recordingModeBtn, m_shotModeBtn}) {
        b->setStyleSheet(QString("QRadioButton { color: %1; }").arg(Theme::hex(tc.textPrimary)));
        modeRow->addWidget(b);
    }
    modeRow->addStretch();
    fl->addLayout(modeRow);
    connect(m_shotModeBtn, &QRadioButton::toggled, this, [this](bool on) {
        if (m_updatingControls || m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
        auto& state = m_audioSync->pipeline();
        const ShowShot& shot = m_shots[static_cast<size_t>(m_selected)];
        auto& settings = state.shots[shot.key];
        if (on) {
            // Start the override from what the shot uses now.
            settings.framing = state.framingFor(shot.key, shot.take.sourceFile, shot.shot == "CLOSE-UP");
            settings.hasFraming = true;
        } else {
            settings.hasFraming = false;
        }
        loadFramingForSelection();
    });

    // Numeric fields
    auto* numRow = new QHBoxLayout;
    auto makeSpin = [&](const QString& label, double lo, double hi, double step, const QString& suffix) {
        auto* l = new QLabel(label);
        l->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.textSecondary)));
        numRow->addWidget(l);
        auto* spin = new QDoubleSpinBox;
        spin->setRange(lo, hi);
        spin->setDecimals(1);
        spin->setSingleStep(step);
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        numRow->addWidget(spin);
        return spin;
    };
    m_scaleSpin = makeSpin(QStringLiteral("Scale"), 10.0, 400.0, 1.0, QStringLiteral(" %"));
    m_xSpin = makeSpin(QStringLiteral("Centre X"), -100.0, 200.0, 0.5, QStringLiteral(" %"));
    m_ySpin = makeSpin(QStringLiteral("Centre Y"), -100.0, 200.0, 0.5, QStringLiteral(" %"));
    numRow->addStretch();
    fl->addLayout(numRow);
    for (auto* spin : {m_scaleSpin, m_xSpin, m_ySpin}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() {
            if (m_updatingControls) return;
            ShotFraming f;
            f.scale = m_scaleSpin->value() / 100.0;
            f.centerX = m_xSpin->value() / 100.0;
            f.centerY = m_ySpin->value() / 100.0;
            m_framingEditor->setFraming(f);
            applyFraming(f, true);
        });
    }

    // Framed input (what ComfyUI gets) | the chosen render (what came back).
    auto* previewRow = new QHBoxLayout;
    auto previewColumn = [&](const QString& caption, QWidget* view) {
        auto* col = new QVBoxLayout;
        col->setSpacing(2);
        auto* l = new QLabel(caption);
        l->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; }").arg(Theme::hex(tc.textTertiary)));
        col->addWidget(l);
        col->addWidget(view);
        previewRow->addLayout(col);
    };
    m_outputPreview = new QLabel;
    m_outputPreview->setFixedSize(288, 162);
    m_outputPreview->setStyleSheet("QLabel { background: #000; }");
    m_outputPreview->setToolTip(QStringLiteral("The 1920×1080 frame sent to ComfyUI (green = outside the recording). "
                                               "Click to frame / play the take in the big view."));
    previewColumn(QStringLiteral("INPUT (framed)"), m_outputPreview);
    previewRow->addSpacing(4);
    // Every render of this shot, newest first.
    auto* stripScroll = new QScrollArea;
    stripScroll->setFixedHeight(162 + 34);
    stripScroll->setWidgetResizable(true);
    stripScroll->setFrameShape(QFrame::NoFrame);
    stripScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_renderStrip = new QWidget;
    m_renderStripLayout = new QHBoxLayout(m_renderStrip);
    m_renderStripLayout->setContentsMargins(0, 0, 0, 0);
    m_renderStripLayout->setSpacing(8);
    m_renderStripEmpty = new QLabel(QStringLiteral("No renders yet — right-click the shot › Render."));
    m_renderStripEmpty->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.textTertiary)));
    m_renderStripLayout->addWidget(m_renderStripEmpty);
    m_renderStripLayout->addStretch();
    stripScroll->setWidget(m_renderStrip);
    previewColumn(QStringLiteral("RENDERS — click to use & view · right-click for more"), stripScroll);
    previewRow->setStretch(previewRow->count() - 1, 1);
    fl->addLayout(previewRow);

    // Click the INPUT thumbnail to frame / play the take in the big view.
    m_outputPreview->setCursor(Qt::PointingHandCursor);
    m_outputPreview->installEventFilter(this);

    // ── This shot's settings (actions live in the list's right-click menu) ─
    auto* shotLabel = new QLabel(QStringLiteral("THIS SHOT"));
    shotLabel->setStyleSheet(sectionLabelStyle());
    fl->addWidget(shotLabel);
    m_expressionEdit = new QLineEdit;
    m_expressionEdit->setToolTip(QStringLiteral(
        "\"5. FACIAL EXPRESSION\" for this shot. Empty = the script's ACTION text, "
        "else the default expression on SETTINGS."));
    connect(m_expressionEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_updatingControls || m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
        m_audioSync->pipeline().shots[m_shots[static_cast<size_t>(m_selected)].key].expression =
            m_expressionEdit->text().trimmed().toStdString();
    });
    fl->addWidget(m_expressionEdit);
    auto* shotRow = new QHBoxLayout;
    m_fpsCombo = new QComboBox;
    m_fpsCombo->addItem(QStringLiteral("16 fps"), kScailDefaultFps);
    m_fpsCombo->addItem(QStringLiteral("12 fps (long shots)"), kScailReducedFps);
    m_fpsCombo->setToolTip(QStringLiteral("\"1. DRIVING VIDEO\" force_rate. Use 12 for long shots."));
    connect(m_fpsCombo, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_updatingControls || m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
        m_audioSync->pipeline().shots[m_shots[static_cast<size_t>(m_selected)].key].fps =
            m_fpsCombo->currentData().toInt();
        updateShotRow(m_selected);
    });
    shotRow->addWidget(m_fpsCombo);
    auto* padLabel = new QLabel(QStringLiteral("Tail pad"));
    padLabel->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.textSecondary)));
    shotRow->addWidget(padLabel);
    m_shotPadSpin = new QSpinBox;
    m_shotPadSpin->setRange(-1, 120);
    m_shotPadSpin->setSpecialValueText(QStringLiteral("Default"));
    m_shotPadSpin->setSuffix(QStringLiteral(" frames"));
    m_shotPadSpin->setToolTip(QStringLiteral(
        "Extra frames rendered past the end of the line (SCAIL end-of-clip bug), trimmed off again "
        "on the timeline. Default = the tail pad on SETTINGS."));
    connect(m_shotPadSpin, &QSpinBox::valueChanged, this, [this](int v) {
        if (m_updatingControls || m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
        m_audioSync->pipeline().shots[m_shots[static_cast<size_t>(m_selected)].key].padFrames = v;
        updateShotRow(m_selected);
    });
    shotRow->addWidget(m_shotPadSpin);
    m_seedLabel = new QLabel;
    m_seedLabel->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.textSecondary)));
    shotRow->addWidget(m_seedLabel);
    shotRow->addStretch();
    fl->addLayout(shotRow);

    auto* actionsHint = new QLabel(QStringLiteral(
        "Double-click a shot to play it and set its in / out. "
        "Right-click to render (select several with Ctrl / Shift)."));
    actionsHint->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::hex(tc.textTertiary)));
    fl->addWidget(actionsHint);

    splitter->addWidget(framingPane);
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 4);

    m_pages->addWidget(shotsPage);                 // kPageShots
    m_pages->addWidget(setupQueuePage());           // kPageQueue
    m_pages->addWidget(setupSettingsPage());        // kPageSettings
    root->addWidget(setupActionBar());              // RENDER / Stop / ASSEMBLE, on every page
    setupRunner();
}

void ComfyPanel::showPage(int id)
{
    m_pages->setCurrentIndex(id);
    if (auto* b = m_pageButtons ? m_pageButtons->button(id) : nullptr) b->setChecked(true);
    if (id == kPageSettings) refreshRenderPage();
}

std::vector<int> ComfyPanel::selectedShotIndices() const
{
    std::vector<int> out;
    for (auto* item : m_shotTree->selectedItems()) {
        const QVariant v = item->data(0, kShotIndexRole);
        if (v.isValid()) out.push_back(v.toInt());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ComfyPanel::showShotContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* clicked = m_shotTree->itemAt(pos);
    if (!clicked || !clicked->data(0, kShotIndexRole).isValid()) return;
    // Right-clicking outside the selection acts on that one shot (Explorer-style).
    if (!clicked->isSelected()) {
        m_shotTree->clearSelection();
        m_shotTree->setCurrentItem(clicked);
    }
    std::vector<int> picked;
    for (int i : selectedShotIndices())
        if (isExportable(m_shots[static_cast<size_t>(i)])) picked.push_back(i);

    if (picked.empty()) return;   // no picked video take: nothing to do from here

    const bool busy = m_ffmpeg || (m_runner && m_runner->isRunning());
    const QString what = picked.size() == 1 ? QStringLiteral("this shot")
                                            : QStringLiteral("%1 shots").arg(picked.size());
    auto& state = m_audioSync->pipeline();

    // Actions only; per-shot settings (fps, tail pad, framing, expression)
    // stay in the pane on the right, and Play is next to USE RENDER.
    QMenu menu(this);
    QAction* trim = nullptr;
    if (picked.size() == 1) {
        trim = menu.addAction(QStringLiteral("Play && trim take…"));
        trim->setEnabled(!busy);
        menu.addSeparator();
    }
    QAction* render = menu.addAction(QStringLiteral("Render %1").arg(what));
    QAction* reroll = menu.addAction(QStringLiteral("Re-roll seed && render %1").arg(what));
    reroll->setToolTip(QStringLiteral("New random seed; earlier renders are kept."));
    render->setEnabled(!busy);
    reroll->setEnabled(!busy);
    QAction* reveal = nullptr;
    const RenderVersion* active = nullptr;
    if (picked.size() == 1) {
        auto it = state.shots.find(m_shots[static_cast<size_t>(picked[0])].key);
        if (it != state.shots.end()) active = usableRender(it->second);
        if (active) {
            menu.addSeparator();
            reveal = menu.addAction(QStringLiteral("Show render in folder"));
        }
    }

    QAction* chosen = menu.exec(m_shotTree->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (trim && chosen == trim) {
        playAndTrimTake(picked[0]);
    } else if (chosen == render || chosen == reroll) {
        if (chosen == reroll) {
            for (int i : picked)
                state.shots[m_shots[static_cast<size_t>(i)].key].setFreshSeed(
                    QRandomGenerator::global()->generate(), state.seed);
            loadFramingForSelection();   // seed label
        }
        startRender(picked, /*includeRendered=*/true);
    } else if (active && chosen == reveal) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
            {QStringLiteral("/select,"), QDir::toNativeSeparators(QString::fromStdString(active->file))});
    }
}

void ComfyPanel::playAndTrimTake(int shotIndex)
{
    if (!m_audioSync || shotIndex < 0 || shotIndex >= static_cast<int>(m_shots.size())) return;
    const ShowShot& shot = m_shots[static_cast<size_t>(shotIndex)];
    if (!shot.hasTake) return;
    const std::string key = shot.key;
    if (m_audioSync->openTakePlayerForClipId(shot.take.id)) {
        refresh();   // new length / frames; RENDER re-exports it (signature changed)
        for (int i = 0; i < static_cast<int>(m_shots.size()); ++i)
            if (m_shots[static_cast<size_t>(i)].key == key) selectShot(i);
    }
}

const RenderVersion* ComfyPanel::usableRender(const ShotSettings& settings)
{
    auto onDisk = [](const RenderVersion& r) { return QFileInfo::exists(QString::fromStdString(r.file)); };
    if (const RenderVersion* chosen = settings.activeRender(); chosen && onDisk(*chosen)) return chosen;
    for (auto it = settings.renders.rbegin(); it != settings.renders.rend(); ++it)
        if (onDisk(*it)) return &*it;
    return nullptr;
}

int ComfyPanel::existingRenderCount(const ShotSettings& settings)
{
    // Distinct files still on disk: two records can name one file (a render
    // deleted outside the app, then re-rendered to the freed name).
    std::vector<std::string> seen;
    for (const auto& r : settings.renders)
        if (std::find(seen.begin(), seen.end(), r.file) == seen.end() &&
            QFileInfo::exists(QString::fromStdString(r.file)))
            seen.push_back(r.file);
    return static_cast<int>(seen.size());
}

QString ComfyPanel::batchStatusFor(const std::string& key) const
{
    if (m_ffmpeg) {
        if (m_currentExport.key == key) return QStringLiteral("PREPARING");
        for (const auto& item : m_exportQueue)
            if (item.key == key) return QStringLiteral("QUEUED");
        // Exported (or reused) already, waiting for the rest before ComfyUI.
        if (m_preparedKeys.count(key)) return QStringLiteral("QUEUED");
    }
    if (m_runner && m_runner->isRunning()) {
        for (const RenderJob& job : m_runner->jobs()) {
            if (job.key != key) continue;
            if (job.state == RenderJob::State::Pending) return QStringLiteral("QUEUED");
            if (job.state == RenderJob::State::WaitingForNextPass)
                return job.generated ? QStringLiteral("AWAITING CLEANUP") : QStringLiteral("PREPARED");
            if (job.state == RenderJob::State::Running) {
                if (job.staged && !job.prepared) return QStringLiteral("PREPARING");
                if (job.generated) return QStringLiteral("CLEANING BACKGROUND");
                return job.steps > 0 ? QStringLiteral("RENDERING %1/%2").arg(job.step).arg(job.steps)
                                     : QStringLiteral("RENDERING");
            }
        }
    }
    return {};
}

const RenderVersion* ComfyPanel::shownRender() const
{
    if (!m_audioSync || m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return nullptr;
    const auto& state = m_audioSync->pipeline();
    auto it = state.shots.find(m_shots[static_cast<size_t>(m_selected)].key);
    if (it == state.shots.end() || it->second.renders.empty()) return nullptr;
    const auto& rs = it->second.renders;
    if (m_bigRenderIndex >= 0 && m_bigRenderIndex < static_cast<int>(rs.size()) &&
        QFileInfo::exists(QString::fromStdString(rs[static_cast<size_t>(m_bigRenderIndex)].file)))
        return &rs[static_cast<size_t>(m_bigRenderIndex)];
    return usableRender(it->second);
}

double ComfyPanel::mediaDuration(const std::string& path)
{
    const QFileInfo file(QString::fromStdString(path));
    const auto revision = std::pair{file.size(), file.lastModified().toMSecsSinceEpoch()};
    auto it = m_durations.find(path);
    if (it != m_durations.end() && m_mediaRevisions[path] == revision) return it->second;
    m_mediaFps.erase(path);
    double dur = 0.0;
    VideoDecoder probe;
    if (probe.open(utf8ToPath(path), /*forceSoftware=*/true)) {
        dur = probe.info().duration;
        m_mediaFps[path] = probe.info().fps;
    }
    m_durations[path] = dur;
    m_mediaRevisions[path] = revision;
    return dur;
}

void ComfyPanel::rebuildRenderStrip()
{
    // Remove the per-render columns (everything but the empty label + stretch).
    while (m_renderStripLayout->count() > 2) {
        QLayoutItem* item = m_renderStripLayout->takeAt(0);
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    m_renderThumbs.clear();

    const ShotSettings* settings = nullptr;
    if (m_audioSync && m_selected >= 0 && m_selected < static_cast<int>(m_shots.size())) {
        const auto& state = m_audioSync->pipeline();
        auto it = state.shots.find(m_shots[static_cast<size_t>(m_selected)].key);
        if (it != state.shots.end()) settings = &it->second;
    }
    std::vector<std::string> seen;
    int column = 0;
    if (settings) {
        const auto& rs = settings->renders;
        for (int i = static_cast<int>(rs.size()) - 1; i >= 0; --i) {   // newest first
            const RenderVersion& r = rs[static_cast<size_t>(i)];
            if (!QFileInfo::exists(QString::fromStdString(r.file))) continue;
            if (std::find(seen.begin(), seen.end(), r.file) != seen.end()) continue;   // same file twice
            seen.push_back(r.file);

            auto* col = new QWidget;
            auto* v = new QVBoxLayout(col);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(2);
            auto* view = new VideoFrameView;
            view->setFixedSize(192, 108);
            view->setHoverScrubEnabled(true);
            view->setCursor(Qt::PointingHandCursor);
            view->setContextMenuPolicy(Qt::CustomContextMenu);
            QString expr = QString::fromStdString(r.expression);
            view->setToolTip(QStringLiteral("%1\nseed %2 · %3 fps%4\nClick to use & view big · hover to scrub · right-click for more")
                .arg(QFileInfo(QString::fromStdString(r.file)).fileName()).arg(r.seed).arg(r.fps)
                .arg(expr.isEmpty() ? QString() : QStringLiteral("\n") + expr));
            v->addWidget(view);
            auto* caption = new QLabel;
            caption->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; }").arg(Theme::hex(Theme::colors().textSecondary)));
            v->addWidget(caption);
            m_renderStripLayout->insertWidget(column++, col);

            const int index = i;
            QPointer<VideoFrameView> guard(view);
            const std::string file = r.file;
            m_grabber->requestStill(view, file, 0.35 * mediaDuration(file), view->requestSize(),
                                    [guard](const QImage& img) { if (guard) guard->setPoster(img); });
            connect(view, &VideoFrameView::clicked, this, [this, index]() { useRender(index, true); });
            connect(view, &VideoFrameView::hoverScrubbed, this, [this, guard, file](double f) {
                if (!guard) return;
                m_grabber->requestLive(guard, file, f * mediaDuration(file), guard->requestSize(),
                                       [guard](const QImage& img) { if (guard && guard->isHoverScrubbing()) guard->setFrame(img); },
                                       VideoFrameGrabber::HoverScrub);
            });
            connect(view, &VideoFrameView::hoverEnded, this, [guard]() { if (guard) guard->showPoster(); });
            connect(view, &QWidget::customContextMenuRequested, this, [this, guard, index](const QPoint& pos) {
                if (guard) showRenderMenu(index, guard->mapToGlobal(pos));
            });
            m_renderThumbs.push_back({view, caption, index});
        }
    }
    m_renderStripEmpty->setVisible(m_renderThumbs.empty());
    updateRenderStripMarks();
}

void ComfyPanel::updateRenderStripMarks()
{
    if (!m_audioSync || m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    const auto& state = m_audioSync->pipeline();
    auto it = state.shots.find(m_shots[static_cast<size_t>(m_selected)].key);
    if (it == state.shots.end()) return;
    const RenderVersion* used = usableRender(it->second);
    const RenderVersion* big = framingMode() ? nullptr : shownRender();
    const auto& tc = Theme::colors();
    const int total = static_cast<int>(m_renderThumbs.size());
    int n = total;
    for (auto& t : m_renderThumbs) {
        const RenderVersion& r = it->second.renders[static_cast<size_t>(t.index)];
        const bool isUsed = used && used->file == r.file;
        if (t.view) t.view->setOutline(big && big->file == r.file ? tc.accent : isUsed ? tc.success : QColor());
        if (t.caption)
            t.caption->setText(QStringLiteral("%1v%2 · seed %3 · %4 fps")
                .arg(isUsed ? QStringLiteral("✓ USED · ") : QString()).arg(n--).arg(r.seed).arg(r.fps));
    }
}

bool ComfyPanel::needsRender(const ShotSettings& settings)
{
    return settings.review == ShotReview::Retake || !usableRender(settings);
}

void ComfyPanel::useRender(int renderIndex, bool showBig)
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    auto& settings = m_audioSync->pipeline().shots[m_shots[static_cast<size_t>(m_selected)].key];
    if (renderIndex < 0 || renderIndex >= static_cast<int>(settings.renders.size())) return;
    const RenderVersion* previous = usableRender(settings);
    if (!previous || previous->file != settings.renders[static_cast<size_t>(renderIndex)].file) {
        settings.review = ShotReview::Unreviewed;
        settings.reviewedRender.clear();
    }
    settings.chosenRender = renderIndex;   // what ASSEMBLE puts on the timeline
    m_bigRenderIndex = renderIndex;
    if (showBig) setBigView(1);
    showRenderPreview();
    updateShotRow(m_selected);
    updateExportSummary();
}

void ComfyPanel::showRenderMenu(int renderIndex, const QPoint& globalPos)
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    const auto& settings = m_audioSync->pipeline().shots[m_shots[static_cast<size_t>(m_selected)].key];
    if (renderIndex < 0 || renderIndex >= static_cast<int>(settings.renders.size())) return;
    const QString file = QString::fromStdString(settings.renders[static_cast<size_t>(renderIndex)].file);
    const RenderVersion* used = usableRender(settings);
    const bool isUsed = used && used->file == settings.renders[static_cast<size_t>(renderIndex)].file;

    QMenu menu(this);
    QAction* use = menu.addAction(isUsed ? QStringLiteral("✓ Used on the timeline") : QStringLiteral("Use this render"));
    use->setEnabled(!isUsed);
    QAction* open = menu.addAction(QStringLiteral("Open in video player"));
    QAction* reveal = menu.addAction(QStringLiteral("Show in folder"));
    menu.addSeparator();
    QAction* del = menu.addAction(QStringLiteral("Delete from disk…"));
    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;
    if (chosen == use) {
        useRender(renderIndex, true);
    } else if (chosen == open) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
    } else if (chosen == reveal) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,"), QDir::toNativeSeparators(file)});
    } else if (chosen == del) {
        // Deferred: rebuilding the strip deletes the thumbnail this menu came from.
        QTimer::singleShot(0, this, [this, renderIndex]() { deleteRender(renderIndex); });
    }
}

void ComfyPanel::deleteRender(int renderIndex)
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    auto& settings = m_audioSync->pipeline().shots[m_shots[static_cast<size_t>(m_selected)].key];
    if (renderIndex < 0 || renderIndex >= static_cast<int>(settings.renders.size())) return;
    const std::string file = settings.renders[static_cast<size_t>(renderIndex)].file;
    const QString name = QFileInfo(QString::fromStdString(file)).fileName();
    if (QMessageBox::question(this, QStringLiteral("Delete render"),
            QStringLiteral("Move %1 to the Recycle Bin?").arg(name)) != QMessageBox::Yes)
        return;
    stopShotPlayback();
    if (QFileInfo::exists(QString::fromStdString(file)) && !QFile::moveToTrash(QString::fromStdString(file))) {
        QMessageBox::warning(this, QStringLiteral("Delete render"),
                             QStringLiteral("Could not delete %1 (is it open in another program?)").arg(name));
        return;
    }
    if (settings.reviewedRender == file) {
        settings.review = ShotReview::Unreviewed;
        settings.reviewedRender.clear();
    }
    // Drop every record of that file; keep the chosen one pointing at the same file.
    const std::string chosenFile = settings.chosenRender >= 0 &&
                                   settings.chosenRender < static_cast<int>(settings.renders.size())
        ? settings.renders[static_cast<size_t>(settings.chosenRender)].file : std::string{};
    settings.renders.erase(std::remove_if(settings.renders.begin(), settings.renders.end(),
                                          [&](const RenderVersion& r) { return r.file == file; }),
                           settings.renders.end());
    settings.chosenRender = -1;   // newest
    for (int i = 0; i < static_cast<int>(settings.renders.size()); ++i)
        if (!chosenFile.empty() && settings.renders[static_cast<size_t>(i)].file == chosenFile) settings.chosenRender = i;
    m_bigRenderIndex = -1;
    appendLog(QStringLiteral("Deleted render %1 (Recycle Bin)").arg(name));
    loadFramingForSelection();   // strip, seed line, big view
    updateShotRow(m_selected);
    updateExportSummary();
}

void ComfyPanel::showRenderPreview()
{
    const auto generation = ++m_renderPreviewGeneration;
    m_bigRender->clear();
    const RenderVersion* r = shownRender();
    if (!r) {
        if (!framingMode()) setBigView(0);   // nothing to show big
        updateTransport();
        return;
    }
    const std::string file = r->file;
    QPointer<VideoFrameView> view(m_bigRender);
    QPointer<ComfyPanel> self(this);
    m_grabber->requestStill(view, file, 0.35 * mediaDuration(file), view->requestSize(),
                            [self, view, generation](const QImage& img) {
        if (self && view && self->m_renderPreviewGeneration == generation) view->setPoster(img);
    });
    updateRenderStripMarks();
    updateTransport();
}

// ── Big view + transport ────────────────────────────────────────────────

void ComfyPanel::setBigView(int mode)
{
    if (mode == 1 && !shownRender()) mode = 0;
    stopShotPlayback();
    m_bigMode = mode;
    m_bigView->setCurrentIndex(mode);
    const QString on = QString("border: 2px solid %1;").arg(Theme::hex(Theme::colors().accent));
    m_outputPreview->setStyleSheet(QString("QLabel { background: #000; %1 }").arg(mode == 0 ? on : QString()));
    updateRenderStripMarks();
    updateTransport();
}

void ComfyPanel::updateTransport()
{
    const bool valid = m_selected >= 0 && m_selected < static_cast<int>(m_shots.size()) &&
                       isExportable(m_shots[static_cast<size_t>(m_selected)]);
    m_playBtn->setEnabled(valid);
    m_trimBar->setEnabled(valid);
    if (!valid) { m_timeLabel->clear(); return; }
    const ShowShot& shot = m_shots[static_cast<size_t>(m_selected)];
    if (framingMode()) {
        const double dur = mediaDuration(shot.take.sourceFile);
        const double pad = std::max(1.0, 0.25 * (shot.take.end - shot.take.start));
        m_trimBar->setRange(std::max(0.0, shot.take.start - pad),
                            dur > 0.0 ? std::min(dur, shot.take.end + pad) : shot.take.end + pad);
        m_trimBar->setInOut(shot.take.start, shot.take.end, /*handles=*/true);
        if (m_playhead < shot.take.start || m_playhead > shot.take.end) m_playhead = shot.take.start;
    } else if (const RenderVersion* r = shownRender()) {
        // The render's own in / out: drag the out handle over the tail pad
        // instead of typing a number (automatic = end − tail pad).
        const double dur = std::max(0.1, mediaDuration(r->file));
        m_trimBar->setRange(0.0, dur);
        m_trimBar->setInOut(std::clamp(r->trimIn, 0.0, dur), std::min(dur, r->effectiveOut(dur)), /*handles=*/true);
        if (m_playhead > dur) m_playhead = 0.0;
    }
    m_trimBar->setPlayhead(m_playhead);
    seekTo(m_playhead);
}

void ComfyPanel::seekTo(double t)
{
    m_playhead = t;
    m_trimBar->setPlayhead(t);
    auto fmt = [](double s) {
        const int ms = static_cast<int>(std::lround(std::max(0.0, s) * 1000.0));
        return QString("%1:%2.%3").arg(ms / 60000).arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
                                  .arg((ms % 1000) / 100);
    };
    if (m_selected >= 0 && m_selected < static_cast<int>(m_shots.size())) {
        if (framingMode())
            m_timeLabel->setText(QString("%1  (%2 s)").arg(fmt(t)).arg(m_trimBar->out() - m_trimBar->in(), 0, 'f', 2));
        else {
            m_timeLabel->setText(QString("%1 / %2").arg(fmt(t), fmt(m_trimBar->out())));
        }
    }
    showFrameAt(t);
}

void ComfyPanel::showFrameAt(double t)
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    QPointer<ComfyPanel> self(this);
    if (framingMode()) {
        const ShowShot& shot = m_shots[static_cast<size_t>(m_selected)];
        const QSize src = sourceSize(shot.take.sourceFile);
        const int selected = m_selected;
        m_grabber->requestLive(this, shot.take.sourceFile, t, kPreviewFrameSize,
                               [self, selected, src](const QImage& img) {
            if (!self || self->m_selected != selected || !self->framingMode()) return;
            self->m_sourceFrame = img;
            self->m_framingEditor->setSource(img, src.width(), src.height());
            self->updateOutputPreview();
        }, VideoFrameGrabber::Player);
    } else if (const RenderVersion* r = shownRender()) {
        QPointer<VideoFrameView> view(m_bigRender);
        const auto generation = m_renderPreviewGeneration;
        m_grabber->requestLive(view, r->file, t, view->requestSize(),
                               [self, view, generation](const QImage& img) {
            if (self && view && !self->framingMode() && self->m_renderPreviewGeneration == generation)
                view->setFrame(img);
        },
                               VideoFrameGrabber::Player);
    }
}

void ComfyPanel::togglePlayback()
{
    if (m_playing) stopShotPlayback();
    else startPlayback(1.0);
}

void ComfyPanel::startPlayback(double speed)
{
    stopShotPlayback();
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size()) || speed == 0.0) return;
    const double in = m_trimBar->in(), out = m_trimBar->out();
    // From the playhead; wrap to the far end when it's already at the end.
    if (speed > 0.0 && (m_playhead >= out - 0.05 || m_playhead < in)) m_playhead = in;
    if (speed < 0.0 && (m_playhead <= in + 0.05 || m_playhead > out)) m_playhead = out;
    m_speed = speed;
    // Sound at every speed: shuttle, reverse and slow motion are time-stretched.
    m_playWithAudio = ensurePreviewAudio() && m_audioSync->playPreviewAudio(m_playhead, speed);
    m_playStartHead = m_playhead;
    m_playStartMs = QDateTime::currentMSecsSinceEpoch();
    m_playing = true;
    m_playBtn->setText(QStringLiteral("⏸"));
    m_playTimer->start();
}

void ComfyPanel::stopShotPlayback()
{
    if (m_playTimer) m_playTimer->stop();
    if (m_playWithAudio && m_audioSync) m_audioSync->pausePreviewAudio();   // stays loaded for scrubbing
    m_playWithAudio = false;
    m_playing = false;
    m_speed = 1.0;
    if (m_playBtn) m_playBtn->setText(QStringLiteral("▶"));
}

double ComfyPanel::frameStep()
{
    std::string path;
    double fallback = 30.0;
    if (m_selected >= 0 && m_selected < static_cast<int>(m_shots.size())) {
        if (framingMode()) path = m_shots[static_cast<size_t>(m_selected)].take.sourceFile;
        else if (const RenderVersion* r = shownRender()) { path = r->file; fallback = r->fps > 0 ? r->fps : 16.0; }
    }
    if (!path.empty()) {
        (void)mediaDuration(path);   // called only to probe and cache the fps
        auto it = m_mediaFps.find(path);
        if (it != m_mediaFps.end() && it->second > 0.0) return 1.0 / it->second;
    }
    return 1.0 / fallback;
}

void ComfyPanel::stepFrames(int frames)
{
    stopShotPlayback();
    const double t = std::max(0.0, m_playhead + frames * frameStep());
    seekTo(t);
    scrubAudio(t);
}

bool ComfyPanel::ensurePreviewAudio()
{
    if (!m_audioSync || m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return false;
    if (framingMode())
        return m_audioSync->loadPreviewAudioForTake(m_shots[static_cast<size_t>(m_selected)].take.id,
                                                    m_trimBar->viewStart(), m_trimBar->viewEnd());
    const RenderVersion* r = shownRender();
    return r && m_audioSync->loadPreviewAudioForMedia(r->file);   // the render's own audio
}

void ComfyPanel::scrubAudio(double t)
{
    if (m_playing || !ensurePreviewAudio()) return;
    m_audioSync->scrubPreviewAudio(t);
}

bool ComfyPanel::handleTransportKey(QKeyEvent* e, bool press)
{
    const int key = e->key();
    const bool repeat = e->isAutoRepeat();
    if (!press) {
        if (repeat) return key == Qt::Key_K || key == Qt::Key_J || key == Qt::Key_L || key == Qt::Key_Space;
        if (key == Qt::Key_K) {
            m_kHeld = false;
            m_slowHoldTimer->stop();
            if (m_slowDir != 0) { m_slowDir = 0; stopShotPlayback(); }
            return true;
        }
        if ((key == Qt::Key_J && m_slowDir < 0) || (key == Qt::Key_L && m_slowDir > 0)) {
            m_slowHoldTimer->stop();
            m_slowDir = 0;
            stopShotPlayback();
            return true;
        }
        return key == Qt::Key_J || key == Qt::Key_L || key == Qt::Key_Space;
    }

    const bool shift = e->modifiers() & Qt::ShiftModifier;
    switch (key) {
    case Qt::Key_Space:
        if (!repeat) togglePlayback();
        return true;
    case Qt::Key_K:
        if (!repeat) { m_kHeld = true; stopShotPlayback(); }
        return true;
    case Qt::Key_J:
    case Qt::Key_L: {
        if (repeat) return true;
        const int dir = key == Qt::Key_L ? 1 : -1;
        if (m_kHeld) {                      // K+J / K+L: step, hold = slow motion
            m_slowDir = dir;
            stepFrames(dir);
            m_slowHoldTimer->start();
            return true;
        }
        if (m_playing && (m_speed > 0.0) == (dir > 0) && std::abs(m_speed) >= 1.0)
            startPlayback(std::clamp(m_speed * 2.0, -4.0, 4.0));    // L L = 2×, L L L = 4×
        else
            startPlayback(dir);
        return true;
    }
    case Qt::Key_Left:
    case Qt::Key_Right:
        stepFrames((key == Qt::Key_Right ? 1 : -1) * (shift ? 5 : 1));
        return true;
    case Qt::Key_Home:
        stopShotPlayback();
        seekTo(m_trimBar->in());
        return true;
    case Qt::Key_End:
        stopShotPlayback();
        seekTo(m_trimBar->out());
        return true;
    case Qt::Key_I:
    case Qt::Key_O: {
        stopShotPlayback();
        double in = m_trimBar->in(), out = m_trimBar->out();
        if (key == Qt::Key_I) in = std::min(m_playhead, out - 0.1);
        else                  out = std::max(m_playhead, in + 0.1);
        m_trimBar->setInOut(in, out, true);
        if (framingMode()) commitTrim(in, out);
        else commitRenderTrim(in, out);
        seekTo(m_playhead);
        return true;
    }
    default:
        return false;
    }
}

void ComfyPanel::commitRenderTrim(double in, double out)
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    const RenderVersion* shown = shownRender();
    if (!shown) return;
    auto& settings = m_audioSync->pipeline().shots[m_shots[static_cast<size_t>(m_selected)].key];
    for (auto& r : settings.renders) {   // every record of that file
        if (r.file != shown->file) continue;
        r.trimIn = std::max(0.0, in);
        r.trimOut = out;
    }
    appendLog(QStringLiteral("Render in/out set to %1 – %2 s").arg(in, 0, 'f', 2).arg(out, 0, 'f', 2));
    seekTo(m_playhead);
}

void ComfyPanel::commitTrim(double in, double out)
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    ShowShot& shot = m_shots[static_cast<size_t>(m_selected)];
    if (std::abs(in - shot.take.start) < 1e-6 && std::abs(out - shot.take.end) < 1e-6) return;
    if (!m_audioSync->setTakeInOut(shot.take.id, in, out)) { updateTransport(); return; }
    shot.take.start = in;    // our copy (the AUDIO page's clip is the source of truth)
    shot.take.end = out;
    updateShotRow(m_selected);
    updateExportSummary();
}

// ═════════════════════════════════════════════════════════════════════════
//  Shot list
// ═════════════════════════════════════════════════════════════════════════

bool ComfyPanel::isExportable(const ShowShot& shot) const
{
    return shot.hasTake && !shot.shot.empty() && VideoFrameGrabber::isVideoFile(shot.take.sourceFile);
}

void ComfyPanel::refresh()
{
    if (!m_audioSync) return;
    const std::string selectedKey = (m_selected >= 0 && m_selected < static_cast<int>(m_shots.size()))
        ? m_shots[static_cast<size_t>(m_selected)].key : std::string{};

    m_shots = m_audioSync->showShots();
    m_selected = -1;

    m_updatingControls = true;
    const auto& state = m_audioSync->pipeline();
    m_outputDirEdit->setText(QString::fromStdString(state.outputDir));
    m_outputDirEdit->setPlaceholderText(defaultOutputDir());
    m_defaultPadSpin->setValue(state.defaultPadFrames);
    m_updatingControls = false;

    const QString scriptName = m_audioSync->activeScriptName();
    m_titleLabel->setText(scriptName.isEmpty() ? QStringLiteral("COMFYUI")
                                               : QStringLiteral("COMFYUI — %1").arg(scriptName));

    QSignalBlocker blocker(m_shotTree);
    m_shotTree->setSortingEnabled(false);
    m_shotTree->clear();
    if (!m_audioSync->isShotScript()) {
        m_hintLabel->setText(QStringLiteral(
            "Load a SHOT | ACTION | SCRIPT script on the AUDIO page, import your green-screen video and pick takes."));
        m_framingEditor->setSource(QImage(), 0, 0);
        m_framingTitle->clear();
        m_shotTree->setSortingEnabled(true);
        updateExportSummary();
        return;
    }

    QTreeWidgetItem* segmentItem = nullptr;
    std::string currentSegment;
    QTreeWidgetItem* reselect = nullptr;
    for (int i = 0; i < static_cast<int>(m_shots.size()); ++i) {
        const ShowShot& shot = m_shots[static_cast<size_t>(i)];
        if (!segmentItem || shot.segment != currentSegment) {
            currentSegment = shot.segment;
            segmentItem = new ShotTreeItem(m_shotTree);
            segmentItem->setData(0, kSortValueRole, i);
            segmentItem->setText(ColLine, QString::fromStdString(currentSegment));
            QFont f = segmentItem->font(ColLine);
            f.setBold(true);
            segmentItem->setFont(ColLine, f);
            segmentItem->setFlags(Qt::ItemIsEnabled);
            segmentItem->setFirstColumnSpanned(true);
            segmentItem->setText(0, QString::fromStdString(currentSegment));
            segmentItem->setFont(0, f);
        }
        auto* item = new ShotTreeItem(segmentItem);
        item->setData(0, kShotIndexRole, i);
        if (shot.key == selectedKey) reselect = item;
    }
    m_shotTree->expandAll();
    for (int i = 0; i < static_cast<int>(m_shots.size()); ++i) updateShotRow(i);
    m_shotTree->setSortingEnabled(true);
    updateExportSummary();

    if (!reselect && m_shotTree->topLevelItemCount() > 0 && m_shotTree->topLevelItem(0)->childCount() > 0)
        reselect = m_shotTree->topLevelItem(0)->child(0);
    if (reselect) {
        m_shotTree->setCurrentItem(reselect);
        selectShot(reselect->data(0, kShotIndexRole).toInt());
    }
    refreshRenderPage();
}

void ComfyPanel::updateShotRow(int index)
{
    if (index < 0 || index >= static_cast<int>(m_shots.size())) return;
    QTreeWidgetItem* item = nullptr;
    for (int s = 0; s < m_shotTree->topLevelItemCount() && !item; ++s) {
        auto* seg = m_shotTree->topLevelItem(s);
        for (int c = 0; c < seg->childCount(); ++c) {
            if (seg->child(c)->data(0, kShotIndexRole).toInt() == index) { item = seg->child(c); break; }
        }
    }
    if (!item) return;
    const QSignalBlocker blocker(m_shotTree);

    const auto& tc = Theme::colors();
    const ShowShot& shot = m_shots[static_cast<size_t>(index)];
    const auto& state = m_audioSync->pipeline();
    item->setText(ColOrder, QString::number(index + 1));
    item->setData(ColOrder, kSortValueRole, index + 1);
    item->setText(ColShot, shot.shot.empty() ? QStringLiteral("SET SHOT") : QString::fromStdString(shot.shot));
    item->setForeground(ColShot, shot.shot.empty() ? QBrush(tc.error) : QBrush(tc.textPrimary));
    item->setText(ColLine, QString::fromStdString(shot.dialogue));
    item->setToolTip(ColLine, QString::fromStdString(shot.action.empty() ? shot.dialogue
                                                                        : shot.dialogue + "\nACTION: " + shot.action));

    const int fps = state.fpsFor(shot.key);
    const int pad = state.padFramesFor(shot.key);
    item->setData(ColPad, kSortValueRole, pad);
    item->setData(ColLength, kSortValueRole, shot.hasTake ? std::max(0.0, shot.take.end - shot.take.start) : -1.0);
    item->setData(ColTake, kSortValueRole, shot.hasTake
        ? QFileInfo(QString::fromStdString(shot.take.sourceFile)).fileName() : QString());
    item->setData(ColTake, kSortValueRole + 1, shot.hasTake ? shot.take.start : -1.0);
    if (shot.hasTake) {
        const int secs = static_cast<int>(shot.take.start);
        item->setText(ColTake, QString("%1  %2:%3")
            .arg(QFileInfo(QString::fromStdString(shot.take.sourceFile)).fileName())
            .arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0')));
        const double dur = std::max(0.0, shot.take.end - shot.take.start);
        const int frames = static_cast<int>(std::ceil(dur * fps)) + pad;
        item->setText(ColLength, QString("%1s · %2f@%3").arg(dur, 0, 'f', 1).arg(frames).arg(fps));
    } else {
        item->setText(ColTake, QStringLiteral("—"));
        item->setText(ColLength, QString());
    }
    item->setText(ColPad, pad > 0 ? QString::number(pad) : QStringLiteral("—"));

    QString status;
    QColor color = tc.textSecondary;
    auto it = state.shots.find(shot.key);
    const RenderVersion* render = it != state.shots.end() ? usableRender(it->second) : nullptr;
    const ShotReview review = it != state.shots.end() ? it->second.reviewFor(render) : ShotReview::Unreviewed;
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(ColApprove, review == ShotReview::Approved ? Qt::Checked : Qt::Unchecked);
    item->setCheckState(ColRetake, review == ShotReview::Retake ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(ColApprove, render ? QStringLiteral("Approve the selected render. Clears Retake.")
                                      : QStringLiteral("Render this shot before approving it."));
    item->setToolTip(ColRetake, QStringLiteral("Generate a new version with a fresh seed on the next Render All."));
    if (shot.shot.empty())                                     { status = "SET SHOT (AUDIO page)"; color = tc.error; }
    else if (!shot.hasTake)                                    { status = "NO TAKE PICKED";        color = tc.warning; }
    else if (!VideoFrameGrabber::isVideoFile(shot.take.sourceFile)) { status = "AUDIO ONLY";       color = tc.warning; }
    else if (const int onDisk = it != state.shots.end() ? existingRenderCount(it->second) : 0; onDisk > 0) {
        // Only renders still on disk (deleted files don't count).
        status = onDisk > 1 ? QString("RENDERED (%1)").arg(onDisk) : QStringLiteral("RENDERED");
        color = tc.success;
        if (review == ShotReview::Approved) status = QStringLiteral("APPROVED");
        else if (review == ShotReview::Retake) { status = QStringLiteral("RETAKE"); color = tc.warning; }
    }
    else                                                       { status = "READY"; }
    if (const QString batch = batchStatusFor(shot.key); !batch.isEmpty()) {
        status = batch;
        color = tc.accent;
    }
    item->setText(ColStatus, status);
    item->setForeground(ColStatus, QBrush(color));
}

void ComfyPanel::updateExportSummary()
{
    int ready = 0, toRender = 0, missingShot = 0, missingTake = 0;
    for (const auto& s : m_shots) {
        if (isExportable(s)) {
            ++ready;
            const auto& settings = m_audioSync->pipeline().shots;
            const auto it = settings.find(s.key);
            if (it == settings.end() || needsRender(it->second)) ++toRender;
        }
        else if (s.shot.empty()) ++missingShot;
        else if (!s.hasTake) ++missingTake;
    }
    if (m_renderAllBtn) m_renderAllBtn->setText(QStringLiteral("RENDER ALL (%1)").arg(toRender));
    updateRenderButtons();
    if (m_audioSync && m_audioSync->isShotScript()) {
        QStringList parts;
        parts << QString("%1 lines").arg(m_shots.size()) << QString("%1 ready").arg(ready);
        if (missingTake) parts << QString("%1 without a picked take").arg(missingTake);
        if (missingShot) parts << QString("%1 missing a SHOT").arg(missingShot);
        m_hintLabel->setText(parts.join(QStringLiteral(" · ")));
    }
}

// ═════════════════════════════════════════════════════════════════════════
//  Framing
// ═════════════════════════════════════════════════════════════════════════

void ComfyPanel::selectShot(int index)
{
    m_selected = index;
    loadFramingForSelection();
}

QSize ComfyPanel::sourceSize(const std::string& path)
{
    auto it = m_sourceSizes.find(path);
    if (it != m_sourceSizes.end()) return it->second;
    QSize size;
    VideoDecoder probe;
    if (probe.open(utf8ToPath(path), /*forceSoftware=*/true))
        size = QSize(static_cast<int>(probe.info().width), static_cast<int>(probe.info().height));
    if (size.isEmpty()) size = QSize(3840, 2160);
    m_sourceSizes[path] = size;
    return size;
}

void ComfyPanel::loadFramingForSelection()
{
    const bool valid = m_selected >= 0 && m_selected < static_cast<int>(m_shots.size()) &&
                       isExportable(m_shots[static_cast<size_t>(m_selected)]);
    for (QWidget* w : std::initializer_list<QWidget*>{m_framingEditor, m_recordingModeBtn, m_shotModeBtn,
                                                      m_scaleSpin, m_xSpin, m_ySpin, m_shotPadSpin,
                                                      m_expressionEdit, m_fpsCombo,
                                                      m_renderStrip})
        w->setEnabled(valid);
    if (!valid) {
        m_sourceFrame = QImage();
        m_sourcePath.clear();
        m_framingEditor->setSource(QImage(), 0, 0);
        m_outputPreview->clear();
        stopShotPlayback();
        m_bigRenderIndex = -1;
        rebuildRenderStrip();
        ++m_renderPreviewGeneration;
        m_bigRender->clear();
        m_framingTitle->setText(m_selected >= 0 ? QStringLiteral("This line needs a SHOT and a picked video take.")
                                                : QString());
        return;
    }

    const ShowShot& shot = m_shots[static_cast<size_t>(m_selected)];
    const auto& state = m_audioSync->pipeline();
    const bool closeUp = shot.shot == "CLOSE-UP";
    const QSize src = sourceSize(shot.take.sourceFile);
    const auto settingsIt = state.shots.find(shot.key);
    const bool override = settingsIt != state.shots.end() && settingsIt->second.hasFraming;

    m_updatingControls = true;
    m_recordingModeBtn->setText(QString("All %1 shots from this recording").arg(closeUp ? "CLOSE-UP" : "WIDE"));
    m_recordingModeBtn->setChecked(!override);
    m_shotModeBtn->setChecked(override);
    m_shotPadSpin->setValue(settingsIt != state.shots.end() ? settingsIt->second.padFrames : -1);
    m_expressionEdit->setText(settingsIt != state.shots.end() ? QString::fromStdString(settingsIt->second.expression)
                                                              : QString());
    m_expressionEdit->setPlaceholderText(!shot.action.empty()
        ? QStringLiteral("ACTION: %1").arg(QString::fromStdString(shot.action))
        : !state.defaultExpression.empty()
            ? QStringLiteral("Default: %1").arg(QString::fromStdString(state.defaultExpression))
            : QStringLiteral("Workflow's expression"));
    m_fpsCombo->setCurrentIndex(state.fpsFor(shot.key) == kScailReducedFps ? 1 : 0);
    {
        const int64_t seed = state.seedFor(shot.key);
        const bool rerolled = settingsIt != state.shots.end() && settingsIt->second.seed >= 0;
        const int renders = settingsIt != state.shots.end() ? existingRenderCount(settingsIt->second) : 0;
        m_seedLabel->setText(QStringLiteral("Seed %1%2 · %3 render(s)")
            .arg(seed >= 0 ? QString::number(seed) : QStringLiteral("(workflow)"))
            .arg(rerolled ? QStringLiteral(" (re-rolled)") : QString())
            .arg(renders));

        m_bigRenderIndex = -1;   // the used render, until another is clicked
        rebuildRenderStrip();
    }
    m_updatingControls = false;
    stopShotPlayback();
    m_playhead = shot.take.start;
    showRenderPreview();   // also updates the transport

    m_framingTitle->setText(QString("<b>%1</b> · %2 · %3×%4")
        .arg(QString::fromStdString(shot.shot).toHtmlEscaped(),
             QFileInfo(QString::fromStdString(shot.take.sourceFile)).fileName().toHtmlEscaped())
        .arg(src.width()).arg(src.height()));

    // Another recording: drop its frame now so the box is never drawn over
    // the wrong picture (or against the wrong source size) while decoding.
    if (m_sourcePath != shot.take.sourceFile) {
        m_sourcePath = shot.take.sourceFile;
        m_sourceFrame = QImage();
        m_framingEditor->setSource(QImage(), src.width(), src.height());
    }
    const ShotFraming framing = state.framingFor(shot.key, shot.take.sourceFile, closeUp);
    m_framingEditor->setFraming(framing);
    updateFramingControls(framing);

    // Poster frame of the take (the same moment the take chip shows).
    QPointer<ComfyPanel> self(this);
    const std::string path = shot.take.sourceFile;
    const int selected = m_selected;
    const double t = shot.take.start + 0.35 * std::max(0.0, shot.take.end - shot.take.start);
    m_grabber->requestStill(this, path, t, kPreviewFrameSize, [self, selected, src](const QImage& image) {
        if (!self || self->m_selected != selected) return;
        self->m_sourceFrame = image;
        self->m_framingEditor->setSource(image, src.width(), src.height());
        self->updateOutputPreview();
    });
    updateOutputPreview();
}

void ComfyPanel::applyFraming(const ShotFraming& framing, bool commit)
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    const ShowShot& shot = m_shots[static_cast<size_t>(m_selected)];
    auto& state = m_audioSync->pipeline();
    if (m_shotModeBtn->isChecked()) {
        auto& settings = state.shots[shot.key];
        settings.hasFraming = true;
        settings.framing = framing;
    } else {
        auto& rec = state.recordings[shot.take.sourceFile];
        (shot.shot == "CLOSE-UP" ? rec.closeUp : rec.wide) = framing;
    }
    updateFramingControls(framing);
    updateOutputPreview();
    (void)commit;
}

void ComfyPanel::updateFramingControls(const ShotFraming& framing)
{
    m_updatingControls = true;
    m_scaleSpin->setValue(framing.scale * 100.0);
    m_xSpin->setValue(framing.centerX * 100.0);
    m_ySpin->setValue(framing.centerY * 100.0);
    m_updatingControls = false;
}

void ComfyPanel::updateOutputPreview()
{
    if (m_selected < 0 || m_selected >= static_cast<int>(m_shots.size())) return;
    const ShowShot& shot = m_shots[static_cast<size_t>(m_selected)];
    const QSize src = sourceSize(shot.take.sourceFile);
    const QImage out = FramingEditor::renderOutput(m_sourceFrame, src.width(), src.height(),
                                                   m_framingEditor->framing(), m_outputPreview->size());
    m_outputPreview->setPixmap(QPixmap::fromImage(out));
}

// ═════════════════════════════════════════════════════════════════════════
//  Export
// ═════════════════════════════════════════════════════════════════════════

QString ComfyPanel::defaultOutputDir() const
{
    for (const auto& s : m_shots) {
        if (!s.hasTake) continue;
        const QString name = m_audioSync->activeScriptName().isEmpty()
            ? QStringLiteral("show") : m_audioSync->activeScriptName();
        QString safe = name;
        safe.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
        return QFileInfo(QString::fromStdString(s.take.sourceFile)).absolutePath()
             + QStringLiteral("/SCAIL shots - ") + safe + QStringLiteral("/source");
    }
    return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QStringLiteral("/SCAIL shots");
}

QString ComfyPanel::findFfmpeg()
{
    const QString bundled = QString::fromStdWString(rt::AppPaths::ffmpegExecutable().wstring());
    if (!bundled.isEmpty()) return bundled;
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

void ComfyPanel::prepareShots(const std::vector<int>& indices, std::function<void()> then)
{
    if (m_ffmpeg || !m_audioSync) return;
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Render"),
                             QStringLiteral("ffmpeg.exe was not found next to the application."));
        return;
    }
    QString outDir = m_outputDirEdit->text().trimmed();
    if (outDir.isEmpty()) outDir = defaultOutputDir();
    if (!QDir().mkpath(outDir)) {
        QMessageBox::warning(this, QStringLiteral("Render"),
                             QStringLiteral("Cannot create the shot folder:\n%1").arg(outDir));
        return;
    }
    auto& state = m_audioSync->pipeline();
    state.outputDir = outDir.toStdString();
    m_outputDirEdit->setText(outDir);

    m_exportQueue.clear();
    m_preparedKeys.clear();
    m_exportCancelled = false;
    const QJsonObject manifest = readExportManifest(outDir);
    int reused = 0;
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(m_shots.size())) continue;
        const ShowShot& shot = m_shots[static_cast<size_t>(index)];
        if (!isExportable(shot)) continue;
        const int order = index + 1;
        const int fps = state.fpsFor(shot.key);
        const int pad = state.padFramesFor(shot.key);
        const QSize src = sourceSize(shot.take.sourceFile);
        ExportItem item;
        item.key = shot.key;
        item.tailPadSec = static_cast<double>(pad) / fps;
        item.job.sourceFile = shot.take.sourceFile;
        item.job.start = shot.take.start;
        item.job.end = shot.take.end;
        item.job.tailPadSec = item.tailPadSec;
        item.job.srcW = src.width();
        item.job.srcH = src.height();
        item.job.framing = state.framingFor(shot.key, shot.take.sourceFile, shot.shot == "CLOSE-UP");
        item.job.outputFile = (outDir + "/" + QString::fromStdString(
            shotFileName(order, shot.segment, shot.shot, shot.lineNumber))).toStdString();
        item.signature = exportSignatureOf(item.job);

        // Unchanged since its last export (same take, in/out, framing, pad,
        // source file): reuse it instead of exporting again.
        const QFileInfo existing(QString::fromStdString(item.job.outputFile));
        if (existing.size() > 0 &&
            manifest.value(existing.fileName()).toString().toStdString() == item.signature) {
            auto& settings = state.shots[shot.key];
            settings.exportedFile = item.job.outputFile;
            settings.exportedTailPadSec = item.tailPadSec;
            m_preparedKeys.insert(shot.key);
            ++reused;
            continue;
        }
        m_exportQueue.push_back(std::move(item));
    }
    if (reused > 0)
        appendLog(QStringLiteral("%1 shot(s) unchanged since their last export — reused.").arg(reused));
    if (m_exportQueue.empty()) {
        if (then) then();
        return;
    }
    m_afterExport = std::move(then);

    m_exportTotal = static_cast<int>(m_exportQueue.size());
    m_exportDone = 0;
    m_exportFailed = 0;
    m_lastError.clear();
    m_progress->setRange(0, m_exportTotal);
    m_progress->setValue(0);
    m_progress->setVisible(true);

    m_ffmpeg = new QProcess(this);
    m_ffmpeg->setProgram(ffmpeg);
    m_ffmpeg->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_ffmpeg, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const QString output = QString::fromLocal8Bit(m_ffmpeg->readAll()).trimmed();
        const bool ok = status == QProcess::NormalExit && exitCode == 0 &&
                        QFileInfo(QString::fromStdString(m_currentExport.job.outputFile)).size() > 0;
        if (!ok && m_currentExport.job.videoEncoder == "h264_nvenc" && status == QProcess::NormalExit) {
            // No NVENC (other GPU / driver): fall back for this and all later shots.
            spdlog::warn("ComfyPanel: h264_nvenc failed ({}); falling back to libopenh264",
                         output.toStdString());
            m_encoder = "libopenh264";
            m_exportQueue.push_front(m_currentExport);
        } else if (ok) {
            ++m_exportDone;
            m_preparedKeys.insert(m_currentExport.key);
            writeExportManifestEntry(m_currentExport.job.outputFile, m_currentExport.signature);
            auto& settings = m_audioSync->pipeline().shots[m_currentExport.key];
            settings.exportedFile = m_currentExport.job.outputFile;
            settings.exportedTailPadSec = m_currentExport.tailPadSec;
            for (int i = 0; i < static_cast<int>(m_shots.size()); ++i)
                if (m_shots[static_cast<size_t>(i)].key == m_currentExport.key) updateShotRow(i);
        } else {
            ++m_exportFailed;
            m_lastError = output.isEmpty() ? QStringLiteral("ffmpeg exited with code %1").arg(exitCode) : output;
            spdlog::warn("ComfyPanel: export failed for {}: {}", m_currentExport.job.outputFile,
                         m_lastError.toStdString());
        }
        m_progress->setValue(m_exportDone + m_exportFailed);
        runNextExport();
    });
    runNextExport();
}

void ComfyPanel::runNextExport()
{
    if (m_exportQueue.empty()) {
        QString msg = QString("Exported %1 of %2 changed shot(s)").arg(m_exportDone).arg(m_exportTotal);
        if (m_exportFailed && !m_exportCancelled)
            msg += QString(" · %1 failed (skipped): %2").arg(m_exportFailed).arg(m_lastError.left(160));
        finishExport(msg);
        return;
    }
    m_currentExport = std::move(m_exportQueue.front());
    m_exportQueue.pop_front();
    for (int i = 0; i < static_cast<int>(m_shots.size()); ++i) updateShotRow(i);   // PREPARING / QUEUED
    m_currentExport.job.videoEncoder = m_encoder;

    QStringList args;
    for (const auto& a : buildShotExportArgs(m_currentExport.job)) args << QString::fromStdString(a);
    updateRenderButtons();
    m_renderStatus->setText(QString("Preparing shot %1 of %2 for ComfyUI · %3")
        .arg(m_exportDone + m_exportFailed + 1).arg(m_exportTotal)
        .arg(QFileInfo(QString::fromStdString(m_currentExport.job.outputFile)).fileName()));
    m_ffmpeg->setArguments(args);
    m_ffmpeg->start();
}

void ComfyPanel::finishExport(const QString& message)
{
    if (m_ffmpeg) {
        m_ffmpeg->deleteLater();
        m_ffmpeg = nullptr;
    }
    m_progress->setVisible(false);
    appendLog(message);
    for (int i = 0; i < static_cast<int>(m_shots.size()); ++i) updateShotRow(i);
    auto then = std::move(m_afterExport);
    m_afterExport = nullptr;
    updateExportSummary();
    if (m_exportCancelled) {
        m_renderStatus->setText(QStringLiteral("Stopped"));
        return;
    }
    m_renderStatus->setText(message);
    if (then) then();
}

} // namespace rt
