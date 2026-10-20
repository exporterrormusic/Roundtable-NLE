/*
 * AudioSyncResegmentAndManual.cpp - Script-guided resegmentation and manual match.
 * Extracted from AudioSyncData.cpp for modularity.
 */
#include "panels/audio/AudioSync.h"
#include "audiosync/AutoSync.h"
#include "audiosync/SyncFileName.h"
#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "audio/AudioEngine.h"
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

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace rt {

void AudioSync::resegmentByScript()
{
    audiosync::resegmentByScript(syncSource(), session().clips);
}

void AudioSync::openManualMatch(int lineNumber)
{
    if (!session().script || session().audioPaths.empty()) return;
    if (!audioReadyOrNotify({}, QStringLiteral("Manual match"))) return;

    // QDialog::exec() runs a nested event loop.  Stop both AudioSync preview
    // modes before entering it so their timers cannot observe the dialog's
    // transport state and clear its shared AudioEngine sources.
    stopPlayback();
    stopFilePlayback();

    // Find the script line
    std::string character, dialogue;
    for (const auto& sl : session().script->lines) {
        if (sl.lineNumber == lineNumber) {
            character = sl.character;
            dialogue  = sl.dialogue;
            break;
        }
    }

    // Collect per-file confirmed & tentative regions
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> confirmedByFile;
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> tentativeByFile;
    for (const auto& clip : session().clips) {
        if (clip.scriptLineNumber < 0) continue;
        if (clip.matchState == 2)
            confirmedByFile[clip.sourceFile].emplace_back(clip.start, clip.end);
        else if (clip.matchState == 1)
            tentativeByFile[clip.sourceFile].emplace_back(clip.start, clip.end);
    }

    // Build audio data map using same type as ManualMatchDialog expects
    std::unordered_map<std::string, ManualMatchDialog::AudioData> audioDataMap;
    for (const auto& [path, sampleData] : session().audioSamples) {
        ManualMatchDialog::AudioData data;
        data.sampleRate = sampleData.sampleRate;
        data.pcm = sampleData.samples;   // shared, not copied
        audioDataMap[path] = data;
    }

    // Pre-select audio file: user-assigned file takes priority,
    // otherwise fall back to character name matching.
    std::string preselectedFile;
    auto assignIt = session().lineAudioFile.find(lineNumber);
    if (assignIt != session().lineAudioFile.end() && !assignIt->second.empty()) {
        preselectedFile = assignIt->second;
    } else {
        for (const auto& p : session().audioPaths) {
            std::string charFromFile = extractCharacterName(p);
            std::string charLower = character;
            std::transform(charLower.begin(), charLower.end(), charLower.begin(), ::tolower);
            std::transform(charFromFile.begin(), charFromFile.end(), charFromFile.begin(), ::tolower);
            if (charLower == charFromFile) {
                preselectedFile = p;
                break;
            }
        }
    }
    if (preselectedFile.empty() && !session().audioPaths.empty())
        preselectedFile = session().audioPaths[0];

    ManualMatchDialog dlg(
        character, dialogue, lineNumber,
        session().audioPaths, audioDataMap,
        {}, {},  // global regions not used — per-file via m_confirmedByFile
        preselectedFile,
        m_audioEngine,
        this);

    // Set per-file region maps, then load initial file (must be after regions)
    dlg.m_confirmedByFile = confirmedByFile;
    dlg.m_tentativeByFile = tentativeByFile;
    dlg.setVideoFrameGrabber(frameGrabber());   // video files show their picture
    dlg.loadInitialFile();

    m_manualMatchOpen = true;
    int dialogResult = dlg.exec();
    m_manualMatchOpen = false;

    if (dialogResult == QDialog::Accepted) {
        auto result = dlg.result();
        bool found = false;
        runClipsMutationWithUndo(
            "Manually match audio",
            [this, lineNumber, character, dialogue, result, &found]() {
                // Check if this line already has a clip; update it.
                for (auto& clip : session().clips) {
                    if (clip.scriptLineNumber == lineNumber) {
                        clip.sourceFile = result.audioFile;
                        clip.start      = result.start;
                        clip.end        = result.end;
                        clip.matchState = 2;
                        clip.confidence = 1.0f;
                        found = true;
                        break;
                    }
                }

                // Otherwise create a new clip
                if (!found) {
                    SyncClip newClip;
                    newClip.id              = static_cast<int>(session().clips.size());
                    newClip.sourceFile      = result.audioFile;
                    newClip.character       = character;
                    newClip.start           = result.start;
                    newClip.end             = result.end;
                    newClip.matchState      = 2;
                    newClip.confidence      = 1.0f;
                    newClip.scriptLineNumber = lineNumber;
                    newClip.scriptSegment   = character + ": " + dialogue;
                    session().clips.push_back(std::move(newClip));
                }
            },
            [this, lineNumber]() {
                // Only this line's card changes (a newly created clip is
                // appended, so no other card's clip index moves).
                QTimer::singleShot(0, this, [this, lineNumber]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    rebuildCardsForLines({lineNumber});
                    emit voiceContextChanged();
                });
            });

        spdlog::info("AudioSync: Manual match for line {} → {}  {:.3f}-{:.3f}",
                      lineNumber, result.audioFile, result.start, result.end);
    }
}


bool AudioSync::setTakeInOut(int clipId, double start, double end)
{
    if (end <= start + 0.05) return false;
    int lineNumber = -1;
    for (const auto& c : session().clips) if (c.id == clipId) { lineNumber = c.scriptLineNumber; break; }
    if (lineNumber == -1 && std::none_of(session().clips.begin(), session().clips.end(),
                                         [clipId](const SyncClip& c) { return c.id == clipId; }))
        return false;
    runClipsMutationWithUndo(
        "Adjust take in/out",
        [this, clipId, start, end]() {
            for (auto& c : session().clips) {
                if (c.id != clipId) continue;
                c.start = start;
                c.end = end;
                c.deletedRegions.erase(
                    std::remove_if(c.deletedRegions.begin(), c.deletedRegions.end(),
                                   [&](const auto& r) { return r.second <= c.start || r.first >= c.end; }),
                    c.deletedRegions.end());
                break;
            }
        },
        [this, lineNumber]() {
            QTimer::singleShot(0, this, [this, lineNumber]() {
                if (!m_destroying.load(std::memory_order_acquire)) rebuildCardsForLines({lineNumber});
            });
        });
    return true;
}

bool AudioSync::openTakePlayerForClipId(int clipId)
{
    for (size_t i = 0; i < session().clips.size(); ++i) {
        if (session().clips[i].id != clipId) continue;
        const double start = session().clips[i].start, end = session().clips[i].end;
        openTakePlayer(i);
        for (const auto& c : session().clips)   // index may not survive an undo-stack edit; re-find
            if (c.id == clipId) return c.start != start || c.end != end;
        return false;
    }
    return false;
}

void AudioSync::openTakePlayer(size_t clipIdx)
{
    if (clipIdx >= session().clips.size()) return;
    if (!audioReadyOrNotify(session().clips[clipIdx].sourceFile, QStringLiteral("Take player"))) return;
    const SyncClip clip = session().clips[clipIdx];
    auto sampleIt = session().audioSamples.find(clip.sourceFile);
    if (sampleIt == session().audioSamples.end()) return;

    // Same nested-event-loop rule as openManualMatch().
    stopPlayback();
    stopFilePlayback();

    std::string character = clip.character, dialogue;
    if (session().script) {
        for (const auto& sl : session().script->lines) {
            if (sl.lineNumber == clip.scriptLineNumber) {
                character = sl.character;
                dialogue  = sl.dialogue;
                break;
            }
        }
    }

    // Darken the file's other matched regions (not this take itself).
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> confirmedByFile;
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> tentativeByFile;
    for (size_t i = 0; i < session().clips.size(); ++i) {
        const auto& c = session().clips[i];
        if (i == clipIdx || c.sourceFile != clip.sourceFile || c.scriptLineNumber < 0) continue;
        if (c.matchState == 2)      confirmedByFile[c.sourceFile].emplace_back(c.start, c.end);
        else if (c.matchState == 1) tentativeByFile[c.sourceFile].emplace_back(c.start, c.end);
    }

    // Only this take's file: copying every imported file's samples into the
    // dialog would be slow for long recordings.
    std::unordered_map<std::string, ManualMatchDialog::AudioData> audioDataMap;
    ManualMatchDialog::AudioData takeData;
    takeData.sampleRate = sampleIt->second.sampleRate;
    takeData.pcm = sampleIt->second.samples;   // shared, not copied
    audioDataMap[clip.sourceFile] = takeData;

    ManualMatchDialog dlg(character, dialogue, clip.scriptLineNumber,
                          {clip.sourceFile}, audioDataMap, {}, {},
                          clip.sourceFile, m_audioEngine, this);
    dlg.m_confirmedByFile = confirmedByFile;
    dlg.m_tentativeByFile = tentativeByFile;
    dlg.setVideoFrameGrabber(frameGrabber());
    dlg.loadInitialFile();
    dlg.setEditRange(QStringLiteral("Take Player"),
                     QStringLiteral("✓  Apply In/Out"), clip.start, clip.end);

    m_manualMatchOpen = true;
    const int dialogResult = dlg.exec();
    m_manualMatchOpen = false;
    if (dialogResult != QDialog::Accepted) return;

    const auto result = dlg.result();
    if (result.end <= result.start) return;
    const int clipId = clip.id;
    const int lineNumber = clip.scriptLineNumber;
    runClipsMutationWithUndo(
        "Adjust take in/out",
        [this, clipId, result]() {
            for (auto& c : session().clips) {
                if (c.id != clipId) continue;
                if (std::abs(c.start - result.start) < 1e-6 && std::abs(c.end - result.end) < 1e-6)
                    break;
                c.start = result.start;
                c.end   = result.end;
                // Drop cut regions that no longer fall inside the take.
                c.deletedRegions.erase(
                    std::remove_if(c.deletedRegions.begin(), c.deletedRegions.end(),
                                   [&](const auto& r) { return r.second <= c.start || r.first >= c.end; }),
                    c.deletedRegions.end());
                break;
            }
        },
        [this, lineNumber]() {
            QTimer::singleShot(0, this, [this, lineNumber]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                rebuildCardsForLines({lineNumber});
            });
        });
    spdlog::info("AudioSync: take {} in/out → {:.3f}-{:.3f}", clipId, result.start, result.end);
}


// Split/merge + persistence methods moved to AudioSyncState.cpp


// ═══════════════════════════════════════════════════════════════════════════
//  Script history (recent URLs dropdown)
// ═══════════════════════════════════════════════════════════════════════════



} // namespace rt
