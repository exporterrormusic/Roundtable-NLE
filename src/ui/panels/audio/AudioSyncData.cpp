/*
 * AudioSyncData.cpp - Data processing, algorithms, and persistence for AudioSync.
 * Split from AudioSync.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "PathUtils.h"
#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "spine/ShotPreset.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "widgets/MiniWaveformWidget.h"
#include "widgets/ManualMatchDialog.h"
#include "Theme.h"

#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <sstream>
#include <filesystem>

#include "audiosync/AutoSync.h"
#include "audiosync/SyncFileName.h"

namespace rt {

void AudioSync::runAutoSync()
{
    // Programmatic callers get complete audio now. The button waits for the
    // background decode first (onAutoSyncClicked), so this is a no-op there.
    loadAudioSamplesNow();
    runClipsMutationWithUndo(
        "Auto-sync audio matches",
        [this]() { runAutoSyncImpl(); },
        [this]() {
            populateClipList();
            updateWorkflowState();
            emit voiceContextChanged();
        });
}

void AudioSync::runAutoSyncImpl()
{
    if (!session().script || session().clips.empty()) return;

    // Optional character scope: when a specific character is selected in the
    // MATCH-side filter list (not "All", not "UNMATCHED"), restrict the entire
    // sync to that character — match only its clips/lines and leave every other
    // character's matches and clip boundaries untouched.
    QString filterCharQt;
    if (m_charFilterList && m_charFilterList->currentRow() > 0) {
        if (auto* item = m_charFilterList->currentItem()) {
            if (item->text() != "UNMATCHED")
                filterCharQt = item->text();
        }
    }
    const bool hasCharFilter = !filterCharQt.isEmpty();

    audiosync::AutoSyncOptions options;
    options.characterFilter = filterCharQt.toStdString();
    options.frontPadSec = m_syncFrontPaddingSec;
    options.endPadSec = m_syncEndPaddingSec;
    options.retakes = m_retakesCheck && m_retakesCheck->isChecked();
    options.progress = [this](int value, const std::string& text) {
        if (m_syncProgress) {
            m_syncProgress->setValue(value);
            m_syncProgress->setLabelText(QString::fromStdString(text));
            QCoreApplication::processEvents();
        }
    };
    using Path = audiosync::AutoSyncResult::Path;
    const auto result = audiosync::autoSyncClips(syncSource(), session().clips, options);
    if (result.path == Path::NothingToSync) return;
    if (result.path == Path::WordAligned) {
        if (session().script->isShotScript())
            m_displayedTakeId.clear();   // take ids were reassigned
        session().syncDone = true;
        populateClipList();
        updateWorkflowState();
        if (m_syncStatus)
            m_syncStatus->setText(
                QString("Aligned %1 clips to script lines").arg(session().clips.size()));
        return;
    }

    session().syncDone = true;
    populateClipList();
    updateWorkflowState();
    m_syncStatus->setText(
        hasCharFilter
            ? QString("Matched %1/%2 segments for %3")
                  .arg(result.matchCount).arg(result.scopeTotal).arg(filterCharQt)
            : QString("Matched %1/%2 segments").arg(result.matchCount).arg(result.scopeTotal));
    emit voiceContextChanged();
    emit syncCompleted(result.matchCount, result.scopeTotal);
}


// ─── Export to Timeline (ported from Python _export_timeline) ────────────────

void AudioSync::createClipsFromTranscription()
{
    // Legacy single-file version — delegates to multi-file
    createClipsFromAllTranscriptions();
}

void AudioSync::createClipsFromAllTranscriptions()
{
    session().clips = audiosync::clipsFromTranscriptions(syncSource());
}

audiosync::SyncSource AudioSync::syncSource() const
{
    return syncSourceOf(session());
}

audiosync::SyncSource AudioSync::syncSourceOf(const ScriptSession& data)
{
    return {data.script.get(), data.audioPaths, data.allTranscriptionResults, data.audioSamples};
}

} // namespace rt
