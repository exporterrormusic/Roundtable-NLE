/*
 * AudioSyncFileList.cpp - Script history, filtering, and audio file list management.
 * Split from AudioSyncState.cpp.
 */

#include "panels/audio/AudioSync.h"
#include "QtHelpers.h"
#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "audio/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"
#include "Theme.h"

#include <spdlog/spdlog.h>

#include <QFileInfo>
#include <QSaveFile>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QMenu>
#include <QRegularExpression>
#include <regex>
#include <QScrollBar>
#include <QTabBar>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <unordered_set>
#include <algorithm>
#include <numeric>


namespace rt {

// ── Helper: derive a friendly display name from a URL / file path ────────
QString AudioSync::displayNameForScriptUrl(const QString& url)
{
    // Google Docs URL: extract doc ID and use short form
    if (url.contains("docs.google.com/document/d/")) {
        QRegularExpression re(R"(/d/([^/\?]+))");
        auto match = re.match(url);
        if (match.hasMatch()) {
            QString docId = match.captured(1);
            if (docId.length() > 12)
                return QString("Google Doc (%1...)").arg(docId.left(10));
            return QString("Google Doc (%1)").arg(docId);
        }
    }

    // Plain URL: show domain + short path
    if (url.startsWith("http://") || url.startsWith("https://")) {
        QUrl parsed(url);
        QString host = parsed.host();
        QString path = parsed.path();
        // Remove leading slash, truncate if long
        if (path.startsWith('/')) path = path.mid(1);
        if (path.length() > 40) path = path.left(37) + "...";
        if (!path.isEmpty())
            return QString("%1/%2").arg(host, path);
        return host;
    }

    // Local file: just the filename
    QFileInfo fi(url);
    if (!fi.fileName().isEmpty())
        return fi.fileName();

    // Fallback: truncate URL itself
    if (url.length() > 50)
        return url.left(47) + "...";
    return url;
}

// ── Helper: extract <title> from Google Docs HTML export ──────────────────
std::string AudioSync::extractHtmlTitle(const std::string& html)
{
    // Google Docs HTML export puts the document title in a <title> tag.
    // Look for <title>...</title> and extract the content.
    const std::string openTag = "<title>";
    const std::string closeTag = "</title>";

    auto startPos = html.find(openTag);
    if (startPos == std::string::npos) {
        // Try case-insensitive
        std::string lower = html;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        startPos = lower.find(openTag);
        if (startPos == std::string::npos) return {};
    }

    startPos += openTag.size();
    auto endPos = html.find(closeTag, startPos);
    if (endPos == std::string::npos) return {};

    std::string title = html.substr(startPos, endPos - startPos);

    // Trim whitespace
    auto first = title.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = title.find_last_not_of(" \t\r\n");
    title = title.substr(first, last - first + 1);

    // Decode common HTML entities
    auto replaceAll = [](std::string& s, const std::string& what, const std::string& with) {
        size_t pos = 0;
        while ((pos = s.find(what, pos)) != std::string::npos) {
            s.replace(pos, what.size(), with);
            pos += with.size();
        }
    };
    replaceAll(title, "&amp;",  "&");
    replaceAll(title, "&nbsp;", " ");
    replaceAll(title, "&quot;", "\"");
    replaceAll(title, "&lt;",   "<");
    replaceAll(title, "&gt;",   ">");
    replaceAll(title, "&apos;", "'");

    // Decode numeric references &#NNNN;
    {
        std::regex numericRef(R"(&#(\d+);)");
        std::smatch m;
        while (std::regex_search(title, m, numericRef)) {
            int code = std::stoi(m[1].str());
            std::string encoded;
            if (code < 128) {
                encoded = {static_cast<char>(code)};
            } else if (code < 0x800) {
                encoded = {static_cast<char>(0xC0 | (code >> 6)),
                           static_cast<char>(0x80 | (code & 0x3F))};
            } else {
                encoded = {static_cast<char>(0xE0 | (code >> 12)),
                           static_cast<char>(0x80 | ((code >> 6) & 0x3F)),
                           static_cast<char>(0x80 | (code & 0x3F))};
            }
            title.replace(static_cast<size_t>(m.position()), static_cast<size_t>(m.length()), encoded);
        }
    }

    if (title.empty()) return {};
    return title;
}

// ── Helper: extract display name and URL from a history line ──────────────
// Format: "display_name|url"  or legacy "url" (backward compat)
static void parseHistoryLine(const QString& line, QString& outName, QString& outUrl)
{
    int sep = line.indexOf('|');
    if (sep > 0 && sep < line.size() - 1) {
        outName = line.left(sep).trimmed();
        outUrl  = line.mid(sep + 1).trimmed();
    } else {
        // Legacy — line is just the URL
        outUrl  = line.trimmed();
        outName = AudioSync::displayNameForScriptUrl(outUrl);
    }
}

void AudioSync::loadScriptHistory()
{
    // History is now maintained by addToScriptHistory / deleteScriptHistoryEntry
    // for persistence of recent URLs. The old combo-box UI has been removed.
    // Entries are stored at: <userDataDir>/script_history.txt
}

void AudioSync::addToScriptHistory(const QString& url)
{
    if (url.isEmpty()) return;

    QString historyPath = rt::userDataDir() + QStringLiteral("/script_history.txt");
    QString displayName = displayNameForScriptUrl(url);

    // Read existing history — store as {displayName, url} pairs
    struct ScriptEntry {
        QString displayName;
        QString url;
    };
    std::vector<ScriptEntry> entries;

    QFile readFile(historyPath);
    if (readFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&readFile);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty()) continue;
            ScriptEntry e;
            parseHistoryLine(line, e.displayName, e.url);
            entries.push_back(std::move(e));
        }
        readFile.close();
    }

    // Remove duplicate URL, then insert at top with current display name
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [&](const ScriptEntry& e) { return e.url == url; }), entries.end());
    entries.insert(entries.begin(), {displayName, url});

    // Limit to 20 entries
    while (static_cast<int>(entries.size()) > 20)
        entries.pop_back();

    // Write back
    QSaveFile writeFile(historyPath);
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&writeFile);
        for (const auto& e : entries)
            out << e.displayName << "|" << e.url << "\n";
        out.flush();
        writeFile.commit();
    }

    spdlog::info("AudioSync: Added script to history (total: {})", entries.size());
}

void AudioSync::deleteScriptHistoryEntry(int index)
{
    if (index < 0) return;

    QString historyPath = rt::userDataDir() + QStringLiteral("/script_history.txt");

    // Read existing entries
    struct ScriptEntry { QString displayName; QString url; };
    std::vector<ScriptEntry> entries;
    QFile readFile(historyPath);
    if (readFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&readFile);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty()) continue;
            ScriptEntry e;
            parseHistoryLine(line, e.displayName, e.url);
            entries.push_back(std::move(e));
        }
        readFile.close();
    }

    if (index >= static_cast<int>(entries.size())) return;

    entries.erase(entries.begin() + index);

    // Write back
    QSaveFile writeFile(historyPath);
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&writeFile);
        for (const auto& e : entries)
            out << e.displayName << "|" << e.url << "\n";
        out.flush();
        writeFile.commit();
    }

    spdlog::info("AudioSync: Deleted script history entry at index {}", index);
}

void AudioSync::renameScriptHistoryEntry(int index)
{
    if (index < 0) return;

    QString historyPath = rt::userDataDir() + QStringLiteral("/script_history.txt");

    // Read existing entries
    struct ScriptEntry { QString displayName; QString url; };
    std::vector<ScriptEntry> entries;
    QFile readFile(historyPath);
    if (readFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&readFile);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty()) continue;
            ScriptEntry e;
            parseHistoryLine(line, e.displayName, e.url);
            entries.push_back(std::move(e));
        }
        readFile.close();
    }

    if (index >= static_cast<int>(entries.size())) return;

    QString oldName = entries[index].displayName;
    QString url = entries[index].url;
    if (url.isEmpty()) return;

    // Ask user for new name
    bool ok = false;
    QString newName = QInputDialog::getText(
        this, "Rename Script",
        "Enter a new name for this script:",
        QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.trimmed().isEmpty()) return;

    // Rename matching entry
    for (auto& e : entries) {
        if (e.url == url) {
            e.displayName = newName.trimmed();
            break;
        }
    }

    // Write back
    QSaveFile writeFile(historyPath);
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&writeFile);
        for (const auto& e : entries)
            out << e.displayName << "|" << e.url << "\n";
        out.flush();
        writeFile.commit();
    }

    // Also update the in-memory session display name if this session is loaded
    for (auto& [sKey, session] : m_scriptSessions) {
        if (QString::fromStdString(sKey) == url ||
            QString::fromStdString(session.sourceUrl) == url) {
            session.displayName = newName.trimmed().toStdString();
            break;
        }
    }

    spdlog::info("AudioSync: Renamed script '{}' -> '{}'",
                 oldName.toStdString(), newName.trimmed().toStdString());

    // Refresh session list
    populateScriptSessionList();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Script character filter
// ═══════════════════════════════════════════════════════════════════════════

void AudioSync::populateScriptFilter()
{
    if (m_scriptFilterCombo && session().script) {
        m_scriptFilterCombo->blockSignals(true);
        m_scriptFilterCombo->clear();
        m_scriptFilterCombo->addItem("All Characters");

        for (const auto& character : session().script->characters)
            m_scriptFilterCombo->addItem(QString::fromStdString(character));

        m_scriptFilterCombo->setCurrentIndex(0);
        m_scriptFilterCombo->blockSignals(false);
    }

    // Always populate the MATCH character filter list
    populateCharacterTabs();
}

void AudioSync::onScriptFilterChanged(int /*index*/)
{
    populateScriptList();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Visual helpers
// ═══════════════════════════════════════════════════════════════════════════

void AudioSync::updateSelectedClipHighlight()
{
    // The QListWidget stylesheet already handles the selection highlight
    // via :selected pseudo-state.  Just ensure the transport bar reflects
    // the current selection.
    updateTransportBar();
}

void AudioSync::updateTransportBar()
{
    if (!m_transportTimeLabel || !m_transportClipLabel) return;

    if (m_playingClipIdx >= 0 && static_cast<size_t>(m_playingClipIdx) < session().clips.size()) {
        const auto& clip = session().clips[static_cast<size_t>(m_playingClipIdx)];
        QString charStr = clip.character.empty()
            ? QStringLiteral("Clip #%1").arg(m_playingClipIdx + 1)
            : QString::fromStdString(clip.character);
        m_transportClipLabel->setText(charStr);

        // Show current playhead position
        if (auto* wf = waveformForClip(m_playingClipIdx)) {
            double ph = wf->playhead();
            int mins = static_cast<int>(ph) / 60;
            double secs = ph - mins * 60;
            m_transportTimeLabel->setText(
                QString("%1:%2").arg(mins, 2, 10, QChar('0'))
                               .arg(secs, 05, 'f', 2, QChar('0')));
        }
        if (m_transportPlayBtn)
            m_transportPlayBtn->setText(QStringLiteral("\u23F8"));  // pause icon
    } else {
        m_transportClipLabel->setText(QStringLiteral("No clip playing"));
        m_transportTimeLabel->setText(QStringLiteral("00:00.00"));
        if (m_transportPlayBtn)
            m_transportPlayBtn->setText(QStringLiteral("\u25B6"));  // play icon
    }
}

QColor AudioSync::characterColor(const std::string& character) const
{
    if (character.empty()) return Theme::colors().textSecondary;

    auto it = session().characterColors.find(character);
    if (it != session().characterColors.end()) return it->second;

    // Deterministic color from character name hash
    static const QColor palette[] = {
        QColor(90, 158, 233),   // blue
        QColor(233, 150, 90),   // orange
        QColor(130, 200, 130),  // green
        QColor(200, 130, 200),  // purple
        QColor(200, 200, 100),  // yellow
        QColor(130, 200, 200),  // cyan
        QColor(233, 130, 130),  // red/pink
        QColor(180, 160, 130),  // tan
    };
    constexpr size_t paletteSize = sizeof(palette) / sizeof(palette[0]);

    size_t hash = 5381;
    for (char c : character) hash = hash * 33 + static_cast<unsigned char>(c);
    QColor color = palette[hash % paletteSize];

    session().characterColors[character] = color;
    return color;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Audio file list helpers — rich items with file details
// ═══════════════════════════════════════════════════════════════════════════

void AudioSync::refreshAudioFileListItem(const std::string& path)
{
    if (!m_audioFileList) return;
    const QString fullPath = QString::fromStdString(path);
    for (int row = 0; row < m_audioFileList->count(); ++row) {
        QListWidgetItem* item = m_audioFileList->item(row);
        if (!item || item->data(Qt::UserRole).toString() != fullPath) continue;
        // Rebuilt in place (same row, same selection) now that its samples
        // can show a duration and waveform.
        const bool wasCurrent = m_audioFileList->currentRow() == row;
        const bool wasSelected = item->isSelected();
        m_audioFileList->blockSignals(true);
        delete m_audioFileList->takeItem(row);
        addAudioFileListItem(fullPath, row);
        if (QListWidgetItem* rebuilt = m_audioFileList->item(row)) {
            if (wasCurrent) m_audioFileList->setCurrentItem(rebuilt);
            rebuilt->setSelected(wasSelected);
        }
        m_audioFileList->blockSignals(false);
        return;
    }
}

void AudioSync::addAudioFileListItem(const QString& fullPath, int row)
{
    if (!m_audioFileList) return;

    QFileInfo fi(fullPath);
    QString fileName = fi.fileName();
    QString fileSize;
    qint64 bytes = fi.size();
    if (bytes < 1024)
        fileSize = QString("%1 B").arg(bytes);
    else if (bytes < 1024 * 1024)
        fileSize = QString("%1 KB").arg(bytes / 1024);
    else
        fileSize = QString("%1 MB").arg(bytes / (1024 * 1024));

    QString createdDate = fi.birthTime().isValid()
        ? fi.birthTime().toString("yyyy-MM-dd")
        : fi.lastModified().toString("yyyy-MM-dd");

    // Compute duration from loaded samples if available
    QString durationStr = "\u2014";
    auto sampIt = session().audioSamples.find(fullPath.toStdString());
    if (sampIt != session().audioSamples.end() && sampIt->second.sampleRate > 0 &&
        !sampIt->second.samples.empty()) {
        double dur = static_cast<double>(sampIt->second.samples.size()) /
                     static_cast<double>(sampIt->second.sampleRate);
        int mins = static_cast<int>(dur) / 60;
        int secs = static_cast<int>(dur) % 60;
        durationStr = QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
    }

    [[maybe_unused]] const auto& c = Theme::colors();
    const auto _rad = QString::number(Theme::metrics().radiusSm);

    // Find this file's index in session().audioPaths for playback
    size_t fileIdx = SIZE_MAX;
    auto pathStr = fullPath.toStdString();
    for (size_t i = 0; i < session().audioPaths.size(); ++i) {
        if (session().audioPaths[i] == pathStr) { fileIdx = i; break; }
    }

    // Build custom widget: info row + full waveform
    auto* widget = new QWidget;
    widget->setStyleSheet("background: transparent; border: none;");
    auto* vlay = new QVBoxLayout(widget);
    vlay->setContentsMargins(4, 4, 4, 4);
    vlay->setSpacing(4);

    // Top row: info label + play button
    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    auto* infoLabel = new QLabel(QString("<b>%1</b><br><span style='font-size:%6px;color:%5'>%2  │  %3  │  %4</span>")
        .arg(fileName.toHtmlEscaped(), fileSize, createdDate, durationStr,
             Theme::rgb(c.textTertiary),
             QString::number(Theme::typography().sizeXxs)));
    infoLabel->setTextFormat(Qt::RichText);
    infoLabel->setStyleSheet("border: none;");
    topRow->addWidget(infoLabel, 1);
    vlay->addLayout(topRow);

    // Waveform inside a container frame
    if (fileIdx != SIZE_MAX) {
        auto sampIt2 = session().audioSamples.find(fullPath.toStdString());
        if (sampIt2 != session().audioSamples.end() && !sampIt2->second.samples.empty()) {
            double dur = static_cast<double>(sampIt2->second.samples.size()) /
                         static_cast<double>(sampIt2->second.sampleRate);

            auto* waveFrame = new QFrame;
            waveFrame->setStyleSheet(
                QString("QFrame { background: %1; border: 1px solid %2; border-radius: %3px; }")
                    .arg(Theme::hex(c.inputBg), Theme::hex(c.controlBorder), _rad));
            auto* frameLay = new QVBoxLayout(waveFrame);
            frameLay->setContentsMargins(4, 4, 4, 4);
            frameLay->setSpacing(0);

            auto* waveform = new MiniWaveformWidget;
            waveform->setContextMenuPolicy(Qt::PreventContextMenu);
            waveform->setFixedHeight(40);
            waveform->setAudioShared(sampIt2->second.samples,
                                     sampIt2->second.sampleRate, 0.0, dur);
            waveform->setTrimHandlesVisible(false);

            // Click to seek
            connect(waveform, &MiniWaveformWidget::seekRequested,
                    this, [this, fileIdx, waveform](double t) {
                waveform->setPlayhead(t);
                waveform->setPlayheadVisible(true);
                // If this file is currently playing, seek the engine
                if (m_previewFileIdx == static_cast<int>(fileIdx) && m_audioEngine) {
                    int64_t frame = static_cast<int64_t>(
                        t * session().audioSamples[session().audioPaths[fileIdx]].sampleRate);
                    m_audioEngine->seekToFrame(frame);
                }
            });

            // Spacebar / double-click play toggle
            connect(waveform, &MiniWaveformWidget::playToggleRequested,
                    this, [this, fileIdx]() { playAudioFile(fileIdx); });

            // JKL shuttle
            connect(waveform, &MiniWaveformWidget::shuttleSpeedChanged,
                    this, [this, fileIdx](double speed) {
                if (!m_audioEngine) return;
                if (speed == 0.0) {
                    if (m_previewFileIdx == static_cast<int>(fileIdx))
                        m_audioEngine->pause();
                } else {
                    // Start playback if not already playing this file
                    if (m_previewFileIdx != static_cast<int>(fileIdx))
                        playAudioFile(fileIdx);
                    if (m_previewFileIdx == static_cast<int>(fileIdx)) {
                        m_audioEngine->setPlaybackSpeed(speed);
                        if (m_audioEngine->transportState() != TransportState::Playing)
                            m_audioEngine->play();
                    }
                }
            });

            frameLay->addWidget(waveform);
            vlay->addWidget(waveFrame);
            m_fileWaveforms[fileIdx] = waveform;

            // Transport controls row under waveform
            auto* ctrlRow = new QHBoxLayout;
            ctrlRow->setContentsMargins(0, 0, 0, 0);
            ctrlRow->setSpacing(6);

            auto* playBtn = new QPushButton(QStringLiteral("\u25B6"));
            playBtn->setContextMenuPolicy(Qt::PreventContextMenu);
            playBtn->setFixedSize(22, 22);
            playBtn->setStyleSheet(
                QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                        "border-radius: %4px; font-size: %5px; padding: 0; }"
                        "QPushButton:hover { background: %6; }")
                    .arg(Theme::hex(c.inputBg), Theme::hex(c.text),
                         Theme::hex(c.controlBorder), _rad,
                         QString::number(Theme::typography().sizeSmall), Theme::hex(c.surface3)));
            connect(playBtn, &QPushButton::clicked, this, [this, fileIdx]() {
                playAudioFile(fileIdx);
            });
            ctrlRow->addWidget(playBtn);
            m_filePlayBtns[fileIdx] = playBtn;

            auto* timeLabel = new QLabel(QStringLiteral("00:00.00"));
            timeLabel->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; font-family: monospace; border: none; }")
                    .arg(Theme::rgb(c.textSecondary)));
            ctrlRow->addWidget(timeLabel);
            m_fileTimeLabels[fileIdx] = timeLabel;

            int dMins = static_cast<int>(dur) / 60;
            int dSecs = static_cast<int>(dur) % 60;
            int dCs   = static_cast<int>(dur * 100) % 100;
            auto* durLabel = new QLabel(QString("/ %1:%2.%3")
                .arg(dMins).arg(dSecs, 2, 10, QChar('0')).arg(dCs, 2, 10, QChar('0')));
            durLabel->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; font-family: monospace; border: none; }")
                    .arg(Theme::rgb(c.textTertiary)));
            ctrlRow->addWidget(durLabel);
            ctrlRow->addStretch();

            vlay->addLayout(ctrlRow);
        }
    }

    auto* item = new QListWidgetItem;
    item->setData(Qt::UserRole, fullPath);       // for sorting by path
    item->setData(Qt::UserRole + 1, fi.birthTime().isValid()
        ? fi.birthTime() : fi.lastModified());   // for sorting by date
    item->setData(Qt::UserRole + 2, bytes);      // for sorting by size
    item->setSizeHint(QSize(0, std::max(widget->sizeHint().height(), 160) + 8));
    if (row >= 0 && row <= m_audioFileList->count())
        m_audioFileList->insertItem(row, item);
    else
        m_audioFileList->addItem(item);
    m_audioFileList->setItemWidget(item, widget);
}

void AudioSync::sortAudioFileList(int criterion)
{
    if (!m_audioFileList || session().audioPaths.empty()) return;

    const std::string activeTranscriptionPath =
        session().currentTranscriptionIndex < session().audioPaths.size()
            ? session().audioPaths[session().currentTranscriptionIndex]
            : std::string{};
    std::vector<std::string> pendingPaths;
    pendingPaths.reserve(session().pendingTranscriptionIndices.size());
    for (size_t index : session().pendingTranscriptionIndices)
        if (index < session().audioPaths.size()) pendingPaths.push_back(session().audioPaths[index]);

    if (m_previewFileIdx >= 0)
        stopFilePlayback();

        if (m_audioFileList) {
            spdlog::debug("AudioSync: Clearing audio file list (sort)");
            m_audioFileList->blockSignals(true);
            m_audioFileList->clear();
            m_audioFileList->blockSignals(false);
        }
    std::vector<size_t> indices(session().audioPaths.size());
    std::iota(indices.begin(), indices.end(), 0);

    switch (criterion) {
    case 0: // Name
        std::sort(indices.begin(), indices.end(), [this](size_t a, size_t b) {
            return QFileInfo(QString::fromStdString(session().audioPaths[a])).fileName().toLower()
                 < QFileInfo(QString::fromStdString(session().audioPaths[b])).fileName().toLower();
        });
        break;
    case 1: // Date
        std::sort(indices.begin(), indices.end(), [this](size_t a, size_t b) {
            QFileInfo fa(QString::fromStdString(session().audioPaths[a]));
            QFileInfo fb(QString::fromStdString(session().audioPaths[b]));
            return fa.lastModified() > fb.lastModified();  // newest first
        });
        break;
    case 2: // Size
        std::sort(indices.begin(), indices.end(), [this](size_t a, size_t b) {
            return QFileInfo(QString::fromStdString(session().audioPaths[a])).size()
                 > QFileInfo(QString::fromStdString(session().audioPaths[b])).size();  // largest first
        });
        break;
    }

    // Keep transcription results paired with their audio paths while sorting.
    session().allTranscriptionResults.resize(session().audioPaths.size());
    std::vector<std::string> sorted;
    std::vector<TranscriptionResult> sortedResults;
    sorted.reserve(session().audioPaths.size());
    sortedResults.reserve(session().allTranscriptionResults.size());
    for (size_t i : indices) {
        sorted.push_back(session().audioPaths[i]);
        sortedResults.push_back(std::move(session().allTranscriptionResults[i]));
    }
    session().audioPaths = std::move(sorted);
    session().allTranscriptionResults = std::move(sortedResults);

    auto indexForPath = [this](const std::string& path) -> size_t {
        auto it = std::find(session().audioPaths.begin(), session().audioPaths.end(), path);
        return it == session().audioPaths.end()
            ? session().audioPaths.size()
            : static_cast<size_t>(std::distance(session().audioPaths.begin(), it));
    };
    if (!activeTranscriptionPath.empty())
        session().currentTranscriptionIndex = indexForPath(activeTranscriptionPath);
    session().pendingTranscriptionIndices.clear();
    for (const auto& path : pendingPaths) {
        const size_t index = indexForPath(path);
        if (index < session().audioPaths.size())
            session().pendingTranscriptionIndices.push_back(index);
    }

    // Rebuild the list widget
    m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    for (const auto& p : session().audioPaths)
        addAudioFileListItem(QString::fromStdString(p));
    refreshTranscribeFileList();
}

void AudioSync::refreshTranscribeFileList()
{
    if (!m_transcribeFileList) return;
    spdlog::debug("AudioSync: Clearing transcribe file list");
    m_transcribeFileList->blockSignals(true);
    m_transcribeFileList->clear();
    m_transcribeFileList->blockSignals(false);
    m_transcribeWaveforms.clear();
    m_transcribePlayBtns.clear();
    m_transcribeTimeLabels.clear();

    const auto& tc = Theme::colors();
    const auto _rad = QString::number(Theme::metrics().radiusSm);

    // One result slot per imported path. Shrinking here also repairs stale
    // entries saved by older builds.
    session().allTranscriptionResults.resize(session().audioPaths.size());

    for (size_t i = 0; i < session().audioPaths.size(); ++i) {
        QFileInfo fi(QString::fromStdString(session().audioPaths[i]));
        // "Transcribed" if this file has segments OR any clip references it.
        // The per-file results aren't fully serialized, so after a project
        // reload session().allTranscriptionResults is empty while the clips persist —
        // a segments-only check wrongly shows "Pending".  Matches the context-
        // menu + startTranscription skip logic (clips outlive segments).
        bool done = !session().allTranscriptionResults[i].segments.empty();
        if (!done) {
            const std::string& p = session().audioPaths[i];
            for (const auto& c : session().clips)
                if (c.sourceFile == p) { done = true; break; }
        }
        QString icon = done ? QStringLiteral("\u2713") : QStringLiteral("\u25CB");
        QString status = done ? "Transcribed" : "Pending";
        QColor fg = done ? tc.success : tc.textTertiary;

        auto* widget = new QWidget;
        widget->setStyleSheet("background: transparent; border: none;");
        auto* vlay = new QVBoxLayout(widget);
        vlay->setContentsMargins(4, 4, 4, 4);
        vlay->setSpacing(4);

        // Top row: status icon + filename + status + play button
        auto* topRow = new QHBoxLayout;
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(8);

        auto* label = new QLabel(QString("<span style='color:%4'>%1</span>  <b>%2</b>  \u2502  <span style='font-size:%5px;color:%4'>%3</span>")
            .arg(icon, fi.fileName().toHtmlEscaped(), status, fg.name(),
                 QString::number(Theme::typography().sizeXxs)));
        label->setTextFormat(Qt::RichText);
        label->setStyleSheet("border: none;");
        topRow->addWidget(label, 1);
        vlay->addLayout(topRow);

        // Waveform inside a container frame
        auto sampIt = session().audioSamples.find(session().audioPaths[i]);
        if (sampIt != session().audioSamples.end() && !sampIt->second.samples.empty()) {
            double dur = static_cast<double>(sampIt->second.samples.size()) /
                         static_cast<double>(sampIt->second.sampleRate);

            auto* waveFrame = new QFrame;
            waveFrame->setStyleSheet(
                QString("QFrame { background: %1; border: 1px solid %2; border-radius: %3px; }")
                    .arg(Theme::hex(tc.inputBg), Theme::hex(tc.controlBorder), _rad));
            auto* frameLay = new QVBoxLayout(waveFrame);
            frameLay->setContentsMargins(4, 4, 4, 4);
            frameLay->setSpacing(0);

            auto* waveform = new MiniWaveformWidget;
            waveform->setContextMenuPolicy(Qt::PreventContextMenu);
            waveform->setFixedHeight(40);
            waveform->setAudioShared(sampIt->second.samples,
                                     sampIt->second.sampleRate, 0.0, dur);
            waveform->setTrimHandlesVisible(false);

            connect(waveform, &MiniWaveformWidget::seekRequested,
                    this, [this, i, waveform](double t) {
                waveform->setPlayhead(t);
                waveform->setPlayheadVisible(true);
                if (m_previewFileIdx == static_cast<int>(i) && m_audioEngine) {
                    int64_t frame = static_cast<int64_t>(
                        t * session().audioSamples[session().audioPaths[i]].sampleRate);
                    m_audioEngine->seekToFrame(frame);
                }
            });

            connect(waveform, &MiniWaveformWidget::playToggleRequested,
                    this, [this, i]() { playAudioFile(i); });

            connect(waveform, &MiniWaveformWidget::shuttleSpeedChanged,
                    this, [this, i](double speed) {
                if (!m_audioEngine) return;
                if (speed == 0.0) {
                    if (m_previewFileIdx == static_cast<int>(i))
                        m_audioEngine->pause();
                } else {
                    if (m_previewFileIdx != static_cast<int>(i))
                        playAudioFile(i);
                    if (m_previewFileIdx == static_cast<int>(i)) {
                        m_audioEngine->setPlaybackSpeed(speed);
                        if (m_audioEngine->transportState() != TransportState::Playing)
                            m_audioEngine->play();
                    }
                }
            });

            frameLay->addWidget(waveform);
            vlay->addWidget(waveFrame);
            m_transcribeWaveforms[i] = waveform;

            // Transport controls row under waveform
            auto* ctrlRow = new QHBoxLayout;
            ctrlRow->setContentsMargins(0, 0, 0, 0);
            ctrlRow->setSpacing(6);

            auto* playBtn = new QPushButton(QStringLiteral("\u25B6"));
            playBtn->setContextMenuPolicy(Qt::PreventContextMenu);
            playBtn->setFixedSize(22, 22);
            playBtn->setStyleSheet(
                QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                        "border-radius: %4px; font-size: %5px; padding: 0; }"
                        "QPushButton:hover { background: %6; }")
                    .arg(Theme::hex(tc.inputBg), Theme::hex(tc.text),
                         Theme::hex(tc.controlBorder), _rad,
                         QString::number(Theme::typography().sizeSmall), Theme::hex(tc.surface3)));
            connect(playBtn, &QPushButton::clicked, this, [this, i]() {
                playAudioFile(i);
            });
            ctrlRow->addWidget(playBtn);
            m_transcribePlayBtns[i] = playBtn;

            auto* timeLabel = new QLabel(QStringLiteral("00:00.00"));
            timeLabel->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; font-family: monospace; border: none; }")
                    .arg(Theme::rgb(tc.textSecondary)));
            ctrlRow->addWidget(timeLabel);
            m_transcribeTimeLabels[i] = timeLabel;

            int dMins = static_cast<int>(dur) / 60;
            int dSecs = static_cast<int>(dur) % 60;
            int dCs   = static_cast<int>(dur * 100) % 100;
            auto* durLabel = new QLabel(QString("/ %1:%2.%3")
                .arg(dMins).arg(dSecs, 2, 10, QChar('0')).arg(dCs, 2, 10, QChar('0')));
            durLabel->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; font-family: monospace; border: none; }")
                    .arg(Theme::rgb(tc.textTertiary)));
            ctrlRow->addWidget(durLabel);
            ctrlRow->addStretch();

            vlay->addLayout(ctrlRow);
        }

        auto* item = new QListWidgetItem;
        item->setSizeHint(QSize(0, std::max(widget->sizeHint().height(), 140) + 8));
        m_transcribeFileList->addItem(item);
        m_transcribeFileList->setItemWidget(item, widget);
    }
}

void AudioSync::relinkAudioFile(int fileIdx)
{
    if (fileIdx < 0 || static_cast<size_t>(fileIdx) >= session().audioPaths.size()) return;

    int importScrollPos = (m_audioFileList && m_audioFileList->verticalScrollBar())
        ? m_audioFileList->verticalScrollBar()->value()
        : 0;
    int transcribeScrollPos = (m_transcribeFileList && m_transcribeFileList->verticalScrollBar())
        ? m_transcribeFileList->verticalScrollBar()->value()
        : 0;

    QList<int> selectedImportRows;
    if (m_audioFileList) {
        const auto selectedItems = m_audioFileList->selectedItems();
        selectedImportRows.reserve(selectedItems.size());
        for (auto* item : selectedItems)
            selectedImportRows.push_back(m_audioFileList->row(item));
    }

    QString oldPath = QString::fromStdString(session().audioPaths[static_cast<size_t>(fileIdx)]);
    QString oldDir  = QFileInfo(oldPath).absolutePath();
    QString oldName = QFileInfo(oldPath).fileName();

    QString newPath = QFileDialog::getOpenFileName(
        this,
        tr("Re-link: %1").arg(oldName),
        oldDir,
        tr("Audio or Video (*.wav *.mp3 *.flac *.aac *.m4a *.ogg *.aiff *.aif "
           "*.mp4 *.mov *.mxf *.mkv *.m4v *.avi *.webm);;All Files (*.*)"));
    if (newPath.isEmpty()) return;

    std::string newPathStr = newPath.toStdString();
    std::string oldPathStr = oldPath.toStdString();

    if (newPathStr == oldPathStr) return;

    // Update the path in session().audioPaths
    session().audioPaths[static_cast<size_t>(fileIdx)] = newPathStr;

    // Update SyncClip::sourceFile for every clip pointing at the old path
    int relinkCount = 0;
    for (auto& clip : session().clips) {
        if (clip.sourceFile == oldPathStr) {
            clip.sourceFile = newPathStr;
            ++relinkCount;
        }
    }

    // Move cached audio samples to the new key
    auto sampIt = session().audioSamples.find(oldPathStr);
    if (sampIt != session().audioSamples.end()) {
        session().audioSamples[newPathStr] = std::move(sampIt->second);
        session().audioSamples.erase(sampIt);
    }

    spdlog::info("AudioSync: Re-linked file [{}] '{}' -> '{}' ({} clips updated)",
                 fileIdx, oldPathStr, newPathStr, relinkCount);

    // Rebuild the audio file list to reflect the new path
    m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    for (const auto& p : session().audioPaths)
        addAudioFileListItem(QString::fromStdString(p));

    if (m_audioFileList) {
        m_audioFileList->clearSelection();
        for (int row : selectedImportRows) {
            if (auto* item = m_audioFileList->item(row))
                item->setSelected(true);
        }
        if (auto* item = m_audioFileList->item(fileIdx))
            m_audioFileList->setCurrentItem(item);
    }

    // Rebuild dependent views whose waveform widgets are bound to the old
    // clip/file source path keys.
    refreshTranscribeFileList();
    if (session().script)
        populateCards();

    QTimer::singleShot(0, this, [this, importScrollPos, transcribeScrollPos]() {
        if (m_audioFileList && m_audioFileList->verticalScrollBar())
            m_audioFileList->verticalScrollBar()->setValue(importScrollPos);
        if (m_transcribeFileList && m_transcribeFileList->verticalScrollBar())
            m_transcribeFileList->verticalScrollBar()->setValue(transcribeScrollPos);
    });
}

void AudioSync::removeFileFromSync(int fileIdx)
{
    removeFilesFromSync({fileIdx});
}

void AudioSync::removeFilesFromSync(std::vector<int> fileIndices)
{
    if (fileIndices.empty()) return;

    // Index-based worker callbacks require the file vectors to remain stable
    // for the entire run, including cancellation cleanup.
    if (m_transcriptionState != TranscriptionState::Idle
        || (m_workerThread && m_workerThread->isRunning())) {
        if (m_transcribeStatus)
            m_transcribeStatus->setText("Cancel or wait for transcription to finish before removing files");
        return;
    }

    fileIndices.erase(
        std::remove_if(fileIndices.begin(), fileIndices.end(), [this](int index) {
            return index < 0 || static_cast<size_t>(index) >= session().audioPaths.size();
        }),
        fileIndices.end());
    std::sort(fileIndices.begin(), fileIndices.end(), std::greater<int>());
    fileIndices.erase(std::unique(fileIndices.begin(), fileIndices.end()), fileIndices.end());
    if (fileIndices.empty()) return;

    std::unordered_set<std::string> removedPaths;
    removedPaths.reserve(fileIndices.size());
    for (int index : fileIndices)
        removedPaths.insert(session().audioPaths[static_cast<size_t>(index)]);

    if (m_previewFileIdx >= 0)
        stopFilePlayback();
    // Also clears a one-shot scrub source, which can hold a raw pointer into
    // session().audioSamples even when no clip is marked as playing.
    stopPlayback();
    if (m_trimDebounceTimer)
        m_trimDebounceTimer->stop();

    // Rebuilt cards must not inherit indices or pointers into the old clip set.
    m_selectedClipIdx = -1;
    setHighlightedCard(nullptr);
    m_selectedLeftCard = nullptr;

    // Restore the one-path/one-result invariant before erasing paired entries.
    session().allTranscriptionResults.resize(session().audioPaths.size());

    // Descending indices keep each erase valid while both vectors stay paired.
    for (int index : fileIndices) {
        const std::string& path = session().audioPaths[static_cast<size_t>(index)];
        session().audioSamples.erase(path);
        session().audioPaths.erase(session().audioPaths.begin() + index);
        session().allTranscriptionResults.erase(session().allTranscriptionResults.begin() + index);
    }

    // No queued work may survive an idle-state file-list mutation.
    session().pendingTranscriptionIndices.clear();
    session().currentTranscriptionIndex = 0;
    session().transcriptionRunTotal = 0;
    session().transcriptionRunCompleted = 0;

    // Remove every derived clip and script-line assignment for these files.
    auto clipsEnd = std::remove_if(session().clips.begin(), session().clips.end(),
        [&removedPaths](const SyncClip& clip) {
            return removedPaths.find(clip.sourceFile) != removedPaths.end();
        });
    session().clips.erase(clipsEnd, session().clips.end());

    for (auto it = session().lineAudioFile.begin(); it != session().lineAudioFile.end(); ) {
        if (removedPaths.find(it->second) != removedPaths.end())
            it = session().lineAudioFile.erase(it);
        else ++it;
    }

    session().audioImported = !session().audioPaths.empty();
    if (session().audioPaths.empty()) {
        session().audioPath.clear();
    } else if (std::find(session().audioPaths.begin(), session().audioPaths.end(), session().audioPath)
               == session().audioPaths.end()) {
        session().audioPath = session().audioPaths.front();
    }
    session().transcriptionDone = !session().clips.empty();
    if (!session().transcriptionDone) {
        for (const auto& result : session().allTranscriptionResults) {
            if (!result.segments.empty()) {
                session().transcriptionDone = true;
                break;
            }
        }
    }
    if (session().clips.empty()) session().syncDone = false;

    // Rebuild the import list so every remaining waveform callback captures
    // its new index rather than the index it had before the erase.
    if (m_audioFileList) {
        m_audioFileList->blockSignals(true);
        m_audioFileList->clear();
        m_fileWaveforms.clear();
        m_filePlayBtns.clear();
        m_fileTimeLabels.clear();
        for (const auto& path : session().audioPaths)
            addAudioFileListItem(QString::fromStdString(path));
        m_audioFileList->blockSignals(false);
    }

    if (m_audioStatus) {
        m_audioStatus->setText(session().audioPaths.empty() ? "No files imported"
            : QString("%1 file(s) imported").arg(session().audioPaths.size()));
    }

    // Update every dependent view immediately, including the Transcribe page
    // even when it is not the currently visible side-panel page.
    refreshTranscribeFileList();
    populateClipList();
    populateLeftList();
    updateWorkflowState();

    if (m_transcribeStatus) {
        m_transcribeStatus->setText(fileIndices.size() == 1
            ? "Removed 1 imported file"
            : QString("Removed %1 imported files").arg(fileIndices.size()));
    }
    spdlog::info("AudioSync: Cleanly removed {} audio file(s) from sync",
                 fileIndices.size());
}

} // namespace rt
