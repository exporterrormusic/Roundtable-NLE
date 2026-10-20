/*
 * AudioSyncSession.cpp - Script session management for AudioSync.
 *
 * Each loaded script gets its own session storing clips, audio paths,
 * transcription state, etc.  Users can switch between sessions freely.
 */
#include "panels/audio/AudioSync.h"
#include "ai/ScriptMatcher.h"
#include "Theme.h"
#include <spdlog/spdlog.h>

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
//  Session save / restore / clear
// ═══════════════════════════════════════════════════════════════════════════

ScriptSession& AudioSync::session() noexcept
{
    if (!m_activeScriptKey.empty()) {
        auto it = m_scriptSessions.find(m_activeScriptKey);
        if (it != m_scriptSessions.end()) return it->second;
    }
    return m_scratchSession;
}

const ScriptSession& AudioSync::session() const noexcept
{
    if (!m_activeScriptKey.empty()) {
        auto it = m_scriptSessions.find(m_activeScriptKey);
        if (it != m_scriptSessions.end()) return it->second;
    }
    return m_scratchSession;
}

void AudioSync::saveCurrentSession()
{
    if (m_activeScriptKey.empty() || !session().script) return;
    // Waiting actions and the card build belong to the outgoing session.
    m_sampleLoader->dropWaitingActions();
    cancelCardBuild();

    // The open script's data already lives in its session. Only data entered
    // before that session existed (the scratch session) still has to move in.
    if (!m_scriptSessions.count(m_activeScriptKey)) {
        m_scriptSessions[m_activeScriptKey] = std::move(m_scratchSession);
        m_scratchSession = ScriptSession{};
    }

    const auto& saved = m_scriptSessions[m_activeScriptKey];
    spdlog::info("AudioSync: Saved session '{}' ({} clips, {} audio files)",
                 saved.displayName, saved.clips.size(), saved.audioPaths.size());
}

void AudioSync::restoreSession(const std::string& sessionKey)
{
    auto it = m_scriptSessions.find(sessionKey);
    if (it == m_scriptSessions.end()) return;

    // Data entered outside any session is dropped, as before (opening a
    // session used to overwrite the live copy it was in).
    m_scratchSession = ScriptSession{};
    m_activeScriptKey = sessionKey;

    spdlog::info("AudioSync: Restored session '{}'", it->second.displayName);
}

void AudioSync::clearCurrentSession()
{
    m_sampleLoader->dropWaitingActions();
    cancelCardBuild();
    // Only the scratch session is cleared: a stored session keeps its data
    // (callers save it first or have just removed it).
    m_scratchSession = ScriptSession{};
    // Also clear the audio file list widget so old entries don't linger
    if (m_audioFileList) {
        m_audioFileList->blockSignals(true);
        m_audioFileList->clear();
        m_audioFileList->blockSignals(false);
    }
    if (m_audioStatus)
        m_audioStatus->setText("No files imported");
    m_displayedTakeId.clear();
    m_cardWaveforms.clear();
    m_cardWidgets.clear();
    m_cardScriptLineNums.clear();
    m_cardClipIndices.clear();
    setHighlightedCard(nullptr);
    m_selectedLeftCard = nullptr;
    m_playingClipIdx = -1;
    m_selectedClipIdx = -1;
    m_activeScriptKey.clear();
    m_lastScriptSource.clear();
    if (m_scriptStatus)
        m_scriptStatus->setText("No script loaded");
}

void AudioSync::switchToScript(const std::string& sessionKey)
{
    if (sessionKey == m_activeScriptKey) return;

    // Save current session before switching
    saveCurrentSession();

    if (m_scriptSessions.count(sessionKey)) {
        restoreSession(sessionKey);
    } else {
        clearCurrentSession();
        m_activeScriptKey = sessionKey;
    }

    // Load audio samples FIRST so waveforms appear in the list items
    if (!session().audioPaths.empty())
        loadAudioSamples();

    // Repopulate audio file list from restored paths
    if (m_audioFileList) {
        m_audioFileList->blockSignals(true);
        m_audioFileList->clear();
        for (const auto& ap : session().audioPaths)
            addAudioFileListItem(QString::fromStdString(ap));
        m_audioFileList->blockSignals(false);
    }
    if (m_audioStatus)
        m_audioStatus->setText(session().audioPaths.empty() ? "No files imported"
            : QString("%1 file(s)").arg(session().audioPaths.size()));

    // Rebuild UI
    if (session().script) {
        populateScriptFilter();
        populateScriptList();
        if (!session().clips.empty())
            populateClipList();
        updateWorkflowState();
        m_scriptStatus->setText(QString("%1 lines, %2 characters")
            .arg(session().script->lineCount())
            .arg(session().script->characters.size()));
    }
    populateScriptSessionList();
    emit voiceContextChanged();
}

void AudioSync::resetForNewProject()
{
    // Save nothing — this is a hard reset for project switch
    detachTranscriptionRun();
    m_scriptSessions.clear();
    clearCurrentSession();

    // Rebuild UI widgets to show empty state
    populateScriptSessionList();
    if (m_scriptStatus)
        m_scriptStatus->setText("No script loaded");
    if (m_audioStatus)
        m_audioStatus->setText("No files imported");
    if (m_rightLayout) {
        QLayoutItem* child;
        while ((child = m_rightLayout->takeAt(0)) != nullptr) {
            if (auto* w = child->widget()) {
                w->setParent(nullptr);
                delete w;
            }
            delete child;
        }
        auto* placeholder = new QLabel("Load a script and import audio to begin.");
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet(
            QString("QLabel { color: %1; font-size: 13px; padding: 40px; border: none; }")
                .arg(Theme::hex(Theme::colors().textTertiary)));
        m_rightLayout->addWidget(placeholder);
        m_rightLayout->addStretch();
    }
    populateLeftList();
    updateWorkflowState();
    updateSmartBar();

    spdlog::info("AudioSync: Reset for new project");
    emit voiceContextChanged();
}

void AudioSync::populateScriptSessionList()
{
    if (!m_scriptSessionList) return;

    m_scriptSessionList->blockSignals(true);
    m_scriptSessionList->clear();

    for (const auto& [key, session] : m_scriptSessions) {
        QString displayName = QString::fromStdString(
            session.displayName.empty() ? key : session.displayName);

        // Build meta info string
        QString meta;
        const Script* scriptPtr = session.script.get();
        if (scriptPtr) {
            meta = QString("%1 lines \u00B7 %2 clips")
                .arg(scriptPtr->lineCount())
                .arg(session.clips.size());
        } else {
            meta = "No script loaded";
        }

        // Determine workflow state for the status dot
        // 0=empty, 1=loaded, 2=transcribed, 3=synced
        int state = 0;
        if (session.scriptLoaded) state = 1;
        if (session.transcriptionDone) state = 2;
        if (session.syncDone) state = 3;

        auto* item = new QListWidgetItem(displayName);
        item->setData(Qt::UserRole, QString::fromStdString(key));       // session key
        item->setData(Qt::UserRole + 1, meta);                         // meta info
        item->setData(Qt::UserRole + 2, key == m_activeScriptKey);     // is active
        item->setData(Qt::UserRole + 3, state);                        // workflow state
        m_scriptSessionList->addItem(item);
    }

    m_scriptSessionList->blockSignals(false);
}

} // namespace rt

// ═══════════════════════════════════════════════════════════════════════════
//  Update existing session with new script content (same URL reload)
// ═══════════════════════════════════════════════════════════════════════════

namespace rt {

void AudioSync::updateExistingSessionScript(const std::string& sessionKey,
                                            std::unique_ptr<Script> newScript,
                                            const std::string& rawContent)
{
    if (!newScript || newScript->isEmpty()) {
        spdlog::warn("AudioSync: updateExistingSessionScript called with empty script");
        return;
    }

    auto it = m_scriptSessions.find(sessionKey);
    if (it == m_scriptSessions.end()) return;

    auto& session = it->second;

    Script* oldScript = session.script.get();

    // ── Step 1: Remap existing clips to new script lines ───────────────
    // For each matched clip, locate the line it is currently matched to in
    // the NEW script.  The reference text is the dialogue of the OLD line it
    // points at — NOT the clip's transcript.  A matched clip's transcript
    // (raw Whisper output) is rarely a byte-exact match to the script line,
    // so comparing the transcript would wrongly drop matches for lines that
    // never changed.  Comparing old-line dialogue → new-line dialogue means
    // an unchanged line matches exactly (score 1.0) and keeps its match;
    // only genuinely edited/removed lines fall through to unmatched.
    //
    // IMPORTANT: clip.scriptLineNumber holds ScriptLine::lineNumber (the
    // source-text line number), NOT the index into Script::lines.  Blank
    // lines and segment markers advance lineNumber without producing a
    // ScriptLine, so the two are different.  Look lines up BY lineNumber and
    // store the matched line's lineNumber back — using the array index here
    // is what made re-synced clips point at the wrong (or no) line.
    auto dialogueForLineNumber = [](const Script* scr, int lineNumber) -> std::string {
        if (!scr) return {};
        for (const auto& ln : scr->lines)
            if (ln.lineNumber == lineNumber)
                return ln.dialogue;
        return {};
    };

    auto remapClips = [&](std::vector<SyncClip>& clips) {
        for (auto& clip : clips) {
            if (clip.scriptLineNumber < 0)
                continue;

            // Reference = dialogue of the line this clip is currently matched
            // to in the old script (looked up by lineNumber).  Fall back to
            // the clip's own text only when the old line can't be found.
            std::string refText = dialogueForLineNumber(oldScript, clip.scriptLineNumber);
            if (refText.empty())
                refText = clip.editedText.empty() ? clip.transcript : clip.editedText;
            if (refText.empty())
                continue;

            std::string refNorm = ScriptMatcher::normalize(refText);

            // ── Fast path: keep an UNCHANGED line exactly where it is ──────
            // If the clip's CURRENT line still exists in the new script with
            // the same dialogue, preserve its assignment (and confirmed
            // state). The fuzzy loop below picks the FIRST line with the best
            // score, so two identically-worded lines (e.g. two short "Hey!" /
            // "Marian!" lines) would let an EARLIER line steal a confirmed
            // clip from its real line — scrambling matches on a GDrive
            // re-sync even when nothing in the doc actually moved. Honoring the
            // existing line when unchanged keeps confirmed clips stable.
            {
                const std::string oldDlg =
                    dialogueForLineNumber(oldScript, clip.scriptLineNumber);
                const std::string newDlg =
                    dialogueForLineNumber(newScript.get(), clip.scriptLineNumber);
                if (!oldDlg.empty() && !newDlg.empty()
                    && ScriptMatcher::normalize(oldDlg)
                       == ScriptMatcher::normalize(newDlg)) {
                    for (const auto& ln : newScript->lines) {
                        if (ln.lineNumber == clip.scriptLineNumber) {
                            clip.scriptSegment = ln.character + ": " + ln.dialogue;
                            break;
                        }
                    }
                    continue;   // scriptLineNumber + matchState untouched
                }
            }

            int bestIdx = -1;
            float bestScore = 0.0f;

            // Case-insensitive speaker compare — never remap a clip onto a
            // DIFFERENT character's line. The audio is per-character, so a
            // Marian clip must only match Marian lines; matching purely on text
            // similarity (the auto-matcher groups by character, but this remap
            // didn't) is what dropped a speaker's audio onto another's line.
            auto sameSpeaker = [](const std::string& a, const std::string& b) {
                if (a.empty() || b.empty()) return true;   // unknown → don't block
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                        return false;
                return true;
            };

            for (size_t j = 0; j < newScript->lines.size(); ++j) {
                if (!sameSpeaker(clip.character, newScript->lines[j].character))
                    continue;   // different speaker — not a candidate
                std::string newDialogue = ScriptMatcher::normalize(newScript->lines[j].dialogue);

                float score;
                if (refNorm == newDialogue) {
                    score = 1.0f; // exact match — line unchanged
                } else {
                    score = ScriptMatcher::sequenceRatio(refNorm, newDialogue);
                }

                if (score > bestScore) {
                    bestScore = score;
                    bestIdx = static_cast<int>(j);
                }
            }

            if (bestScore >= 0.6f && bestIdx >= 0) {
                const auto& newLine = newScript->lines[bestIdx];
                clip.scriptLineNumber = newLine.lineNumber;   // line NUMBER, not index
                // Match runAutoSync's scriptSegment format ("CHAR: dialogue")
                // so a re-synced clip looks identical to a freshly synced one.
                clip.scriptSegment = newLine.character + ": " + newLine.dialogue;
            } else {
                // Could not remap — mark as unmatched
                clip.scriptLineNumber = -1;
                clip.matchState = 0;
                clip.confidence = 0.0f;
                clip.scriptSegment.clear();
            }
        }
    };

    remapClips(session.clips);

    // ── Step 2: Update the session with the new script ─────────────────
    // Retain display name and source URL from the old session
    std::string oldDisplayName = session.displayName;
    std::string oldSourceUrl = session.sourceUrl;

    session.script = std::move(newScript);
    session.scriptLoaded = true;
    session.displayName = oldDisplayName;
    session.sourceUrl = oldSourceUrl;
    // The latest text, so the session restores it offline.
    session.rawContent = rawContent;

    // Reset sync state since line numbers may have shifted;
    // transcription results are still valid but need re-syncing
    session.syncDone = false;

    spdlog::info("AudioSync: Updated session '{}' — new script has {} lines, {} clips remapped",
                 session.displayName,
                 session.script ? session.script->lineCount() : 0,
                 session.clips.size());
}

} // namespace rt
