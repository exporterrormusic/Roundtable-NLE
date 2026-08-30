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

#include "panels/audio/AudioSyncFileName.h"

namespace rt {

void AudioSync::runAutoSync()
{
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
    if (!m_script || m_clips.empty()) return;

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

    // ── Word-level forced alignment (preferred) ─────────────────────────────
    // When transcription word timestamps exist, align each file's words to the
    // script lines so clip RANGES follow LINE boundaries (see AudioSyncAlign
    // .cpp) — far more accurate than the fuzzy merge/resegment/match heuristics
    // below.  Skipped for a scoped (single-character) sync, which stays
    // surgical and uses the heuristic path.  Returns false → fall through.
    spdlog::warn("[ALIGN] runAutoSync entered (hasCharFilter={}, clips={})",
                 hasCharFilter, m_clips.size());
    if (!hasCharFilter && alignClipsToScript()) {
        m_syncDone = true;
        populateClipList();
        updateWorkflowState();
        if (m_syncStatus)
            m_syncStatus->setText(
                QString("Aligned %1 clips to script lines").arg(m_clips.size()));
        return;
    }
    spdlog::warn("[ALIGN] falling through to HEURISTIC path (alignment not used)");

    // Build a list of script character names to match against filenames.
    // We search for each character name as a standalone word in the filename
    // (case-insensitive, delimited by non-alphanumeric chars) rather than
    // just taking the first word.  This way "OVERSPEC AD - ANIS.wav" matches
    // a script character named "Anis".
    std::vector<std::string> scriptChars;
    scriptChars.reserve(m_script->characters.size());
    for (const auto& ch : m_script->characters) {
        std::string lower;
        lower.reserve(ch.size());
        for (char c : ch)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        scriptChars.push_back(std::move(lower));
    }

    int recoveredCharacterCount = 0;
    for (auto& clip : m_clips) {
        std::string oldChar = clip.character;

        // Try matching against script character names embedded in the filename.
        // This runs for ALL clips (even ones already with a character) because
        // extractCharacterName() can't handle names embedded deeper in filenames
        // like "OVERSPEC AD - ANIS.wav".  The script names always take priority.
        std::filesystem::path fp(clip.sourceFile);
        std::string stem = pathToUtf8(fp.stem());
        std::string stemLower = stem;
        for (auto& c : stemLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        bool found = false;
        for (const auto& sc : scriptChars) {
            if (sc.empty()) continue;
            // Search for the character name as a standalone word within the filename.
            // A match is valid if it's preceded and followed by non-alphanumeric
            // characters (or start/end of string).
            size_t pos = 0;
            while ((pos = stemLower.find(sc, pos)) != std::string::npos) {
                bool leftOk  = (pos == 0) ||
                    !std::isalnum(static_cast<unsigned char>(stemLower[pos - 1]));
                bool rightOk = (pos + sc.size() >= stemLower.size()) ||
                    !std::isalnum(static_cast<unsigned char>(stemLower[pos + sc.size()]));
                if (leftOk && rightOk) {
                    // Match — use the original-cased character name from the script
                    clip.character = m_script->characters[&sc - scriptChars.data()];
                    if (clip.character != oldChar)
                        ++recoveredCharacterCount;
                    found = true;
                    break;
                }
                pos += sc.size();
            }
            if (found) break;
        }

        // Fall back to legacy extraction if no character name was found in the filename
        if (!found && oldChar.empty()) {
            clip.character = extractCharacterName(clip.sourceFile);
            if (!clip.character.empty())
                ++recoveredCharacterCount;
        }
    }
    if (recoveredCharacterCount > 0) {
        spdlog::warn("AudioSync: Recovered character names for {} clips before matching",
                     recoveredCharacterCount);
    }

    auto updateSyncProgress = [this](int value, const QString& text) {
        if (m_syncProgress) {
            m_syncProgress->setValue(value);
            m_syncProgress->setLabelText(text);
            QCoreApplication::processEvents();
        }
    };

    // The merge / re-segment pre-passes re-cut and merge clips across ALL
    // characters, so they're skipped when syncing a single filtered character
    // (otherwise a scoped sync would still disturb everyone else's clips).
    if (!hasCharFilter) {
    updateSyncProgress(5, "Merging short segments...");

    // Pre-pass: merge short segments that match script lines better combined
    mergeSegmentsToMatchScript();

    // Pre-pass 2: script-guided re-segmentation Ã¢â‚¬â€ re-cut clips so boundaries
    // better align with script lines (fixes whisper's arbitrary segmentation)
    updateSyncProgress(15, "Re-segmenting by script lines...");

    resegmentByScript();
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // GLOBAL OPTIMAL MATCHING
    //
    // Instead of greedy sequential matching with a small window, we build
    // an NxM cost matrix (clips Ãƒâ€” lines) per character and find the best
    // global assignment.  A sequential-order bonus is included so that
    // in-order recordings are preferred, but out-of-order clips can still
    // match correctly.
    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬

    // --- Helpers ---
    auto normalizeChar = [](const std::string& name) -> std::string {
        std::string lower;
        lower.reserve(name.size());
        for (char c : name) {
            if (c != ' ' || (!lower.empty() && lower.back() != ' '))
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        while (!lower.empty() && lower.back() == ' ') lower.pop_back();
        if (lower == "marian") return "modernia";
        return lower;
    };

    auto norm = [](const std::string& text) {
        return ScriptMatcher::normalize(text);
    };

    // Normalized name of the filtered character (empty when unfiltered), plus
    // a predicate for "this clip is in scope for the current sync".
    const std::string filterCharNorm =
        hasCharFilter ? normalizeChar(filterCharQt.toStdString()) : std::string{};
    auto clipInScope = [&](const SyncClip& c) -> bool {
        return !hasCharFilter || normalizeChar(c.character) == filterCharNorm;
    };

    auto wordOverlap = [](const std::string& a, const std::string& b) -> float {
        std::unordered_set<std::string> aWords, bWords;
        { std::istringstream ss(a); std::string w; while (ss >> w) aWords.insert(w); }
        { std::istringstream ss(b); std::string w; while (ss >> w) bWords.insert(w); }
        if (aWords.empty()) return 0.0f;
        size_t common = 0;
        for (const auto& w : bWords)
            if (aWords.count(w)) ++common;
        return static_cast<float>(common) / static_cast<float>(std::max(aWords.size(), size_t{1}));
    };

    // --- Step 1: Collect confirmed matches to preserve ---
    std::unordered_set<int> confirmedClipIds;
    std::unordered_set<int> confirmedLineNumbers;
    for (const auto& clip : m_clips) {
        if (clip.matchState == 2) {
            confirmedClipIds.insert(clip.id);
            if (clip.scriptLineNumber >= 0)
                confirmedLineNumbers.insert(clip.scriptLineNumber);
        }
    }

    // --- Step 2: Group by character (exclude confirmed; honour filter) ---
    std::unordered_map<std::string, std::vector<size_t>> clipsByChar;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        if (confirmedClipIds.count(m_clips[i].id)) continue;
        std::string ck = normalizeChar(m_clips[i].character);
        if (hasCharFilter && ck != filterCharNorm) continue;
        clipsByChar[ck].push_back(i);
    }

    std::unordered_map<std::string, std::vector<size_t>> linesByChar;
    for (size_t i = 0; i < m_script->lines.size(); ++i) {
        if (confirmedLineNumbers.count(m_script->lines[i].lineNumber)) continue;
        std::string ck = normalizeChar(m_script->lines[i].character);
        if (hasCharFilter && ck != filterCharNorm) continue;
        linesByChar[ck].push_back(i);
    }

    for (const auto& [ch, idx] : clipsByChar)
        spdlog::info("AudioSync: Clip group '{}' has {} clips", ch, idx.size());
    for (const auto& [ch, idx] : linesByChar)
        spdlog::info("AudioSync: Script group '{}' has {} lines", ch, idx.size());

    updateSyncProgress(30, "Matching clips to script lines...");

    updateSyncProgress(30, "Matching clips to script lines...");

    updateSyncProgress(30, "Matching clips to script lines...");

    // --- Step 3: Clear non-confirmed matches (only those in scope) ---
    for (auto& clip : m_clips) {
        if (confirmedClipIds.count(clip.id)) continue;
        if (!clipInScope(clip)) continue;  // keep other characters' matches
        clip.scriptLineNumber = -1;
        clip.confidence       = 0.0f;
        clip.matchState       = 0;
        clip.scriptSegment.clear();
    }

    // --- Step 4: Global matching within each character group ---
    int matchCount = 0;
    bool allowRetakes = m_retakesCheck && m_retakesCheck->isChecked();

    for (auto& [charKey, clipIndices] : clipsByChar) {
        auto lineIt = linesByChar.find(charKey);
        if (lineIt == linesByChar.end() || lineIt->second.empty()) {
            spdlog::debug("AudioSync: No script lines for character '{}'", charKey);
            continue;
        }
        const auto& lineIndices = lineIt->second;

        // Sort clips by start time
        std::sort(clipIndices.begin(), clipIndices.end(),
                  [&](size_t a, size_t b) { return m_clips[a].start < m_clips[b].start; });

        // Are this character's clips spread across MULTIPLE source files?  Each
        // recording starts at 0:00, so a start-time sort of clips from a base
        // file + a "fixed"/retake file does NOT reflect script order.  The
        // sequential-order bonus below assumes it does, so on multi-file
        // characters it actively rewards a retake clip lining up with a line it
        // doesn't belong to (and starves the correct in-order original).
        // Disable the order bonus there and rely on text similarity alone.
        bool multiFile = false;
        {
            const std::string* firstFile = nullptr;
            for (size_t idx : clipIndices) {
                const std::string& f = m_clips[idx].sourceFile;
                if (!firstFile) firstFile = &f;
                else if (f != *firstFile) { multiFile = true; break; }
            }
        }

        size_t N = clipIndices.size();
        size_t M = lineIndices.size();

        // Pre-normalize all texts
        std::vector<std::string> clipTexts(N), lineTexts(M);
        for (size_t i = 0; i < N; ++i) {
            const auto& c = m_clips[clipIndices[i]];
            clipTexts[i] = norm(c.editedText.empty() ? c.transcript : c.editedText);
        }
        for (size_t j = 0; j < M; ++j)
            lineTexts[j] = norm(m_script->lines[lineIndices[j]].dialogue);

        // Build NxM score matrix
        struct ScoreEntry {
            size_t clipIdx;  // index into clipIndices
            size_t lineIdx;  // index into lineIndices
            float  score;
            float  textScore;
        };
        std::vector<ScoreEntry> allPairs;
        allPairs.reserve(N * M);

        for (size_t ci = 0; ci < N; ++ci) {
            for (size_t li = 0; li < M; ++li) {
                float textScore = ScriptMatcher::sequenceRatio(clipTexts[ci], lineTexts[li]);
                float wo = wordOverlap(lineTexts[li], clipTexts[ci]);

                // Phonetic similarity
                float phoneticScore = 0.0f;
                {
                    std::vector<std::string> cWords, lWords;
                    { std::istringstream s(clipTexts[ci]); std::string w; while (s >> w) cWords.push_back(w); }
                    { std::istringstream s(lineTexts[li]); std::string w; while (s >> w) lWords.push_back(w); }
                    if (!lWords.empty() && !cWords.empty()) {
                        size_t matches = 0;
                        for (const auto& lw : lWords) {
                            std::string lm = ScriptMatcher::metaphone(lw);
                            for (const auto& cw : cWords)
                                if (ScriptMatcher::metaphone(cw) == lm) { ++matches; break; }
                        }
                        phoneticScore = static_cast<float>(matches) / static_cast<float>(lWords.size());
                    }
                }

                // Sequential order bonus: clips and lines that are in the same
                // relative position get a bonus.  The closer to the "expected"
                // sequential mapping (ci/N Ã¢â€°Ë† li/M), the larger the bonus.
                float expectedLinePos = (N > 1)
                    ? static_cast<float>(ci) / static_cast<float>(N - 1)
                    : 0.5f;
                float actualLinePos = (M > 1)
                    ? static_cast<float>(li) / static_cast<float>(M - 1)
                    : 0.5f;
                float orderDist = std::abs(expectedLinePos - actualLinePos);
                float orderBonus = multiFile ? 0.0f
                    : std::max(0.0f, 0.15f * (1.0f - orderDist * 2.0f));

                float combined = textScore * 0.40f + phoneticScore * 0.20f
                               + wo * 0.25f + orderBonus;

                // Short-text penalty: for very short transcripts (1-2 words),
                // word overlap and phonetic scores are unreliable (a single
                // common word gives 1.0).  Shift weight toward textScore to
                // prefer exact matches and reduce false positives.
                {
                    size_t clipWordCount = 0;
                    { std::istringstream s(clipTexts[ci]); std::string w; while (s >> w) ++clipWordCount; }
                    size_t lineWordCount = 0;
                    { std::istringstream s(lineTexts[li]); std::string w; while (s >> w) ++lineWordCount; }
                    size_t minWords = std::min(clipWordCount, lineWordCount);
                    if (minWords <= 2) {
                        // For 1-2 word texts, heavily weight textScore
                        combined = textScore * 0.70f + phoneticScore * 0.05f
                                 + wo * 0.10f + orderBonus * 0.5f;
                    } else if (minWords <= 4) {
                        // For 3-4 word texts, moderately shift toward textScore
                        combined = textScore * 0.55f + phoneticScore * 0.12f
                                 + wo * 0.18f + orderBonus * 0.75f;
                    }
                }

                if (combined >= 0.20f)
                    allPairs.push_back({ci, li, combined, textScore});
            }
        }

        // Sort by score descending Ã¢â‚¬â€ best assignments first
        std::sort(allPairs.begin(), allPairs.end(),
                  [](const ScoreEntry& a, const ScoreEntry& b) {
                      return a.score > b.score;
                  });

        // Greedy global assignment: each clip assigned to at most one line,
        // each line assigned to at most one clip (unless retakes are enabled).
        std::unordered_set<size_t> assignedClips;
        std::unordered_set<size_t> assignedLines;

        // Don't FORCE a clip onto a line it only weakly matches — leave it
        // unmatched instead.  When a character has extra takes (e.g. a base
        // recording + a "fixed" corrections file), there are more clips than
        // lines, and the surplus/displaced clips otherwise cascade onto
        // neighbouring lines they don't belong to.  A genuine match clears this
        // bar comfortably; anything left over is better placed by hand than put
        // somewhere wrong.  (Tunable — raise to be stricter, lower to assign more.)
        constexpr float kMinAssignScore = 0.40f;

        for (const auto& entry : allPairs) {
            if (assignedClips.count(entry.clipIdx)) continue;
            if (!allowRetakes && assignedLines.count(entry.lineIdx)) continue;
            if (entry.score < kMinAssignScore) continue;   // too weak → leave unmatched

            size_t clipGlobalIdx = clipIndices[entry.clipIdx];
            size_t lineGlobalIdx = lineIndices[entry.lineIdx];
            const auto& line = m_script->lines[lineGlobalIdx];

            auto& clip = m_clips[clipGlobalIdx];
            clip.confidence       = entry.textScore;
            clip.scriptLineNumber = line.lineNumber;
            clip.matchState       = 1; // tentative
            clip.scriptSegment    = line.character + ": " + line.dialogue;
            ++matchCount;

            assignedClips.insert(entry.clipIdx);
            if (!allowRetakes)
                assignedLines.insert(entry.lineIdx);

            spdlog::debug("AudioSync: Matched clip {} Ã¢â€ â€™ line {} (score={:.2f} text={:.2f}) '{}'",
                          clipGlobalIdx, line.lineNumber, entry.score, entry.textScore,
                          line.dialogue.substr(0, 40));
        }
    updateSyncProgress(60, "Trimming silence from clips...");

    }

    updateSyncProgress(60, "Trimming clips to speech...");

    // --- Step 5: Trim non-confirmed clips to speech (RMS energy, word-bounded) ---
    // Boundary detection combines two signals so neither's weakness dominates:
    //   * RMS energy edges land TIGHT on the real onset/offset, but a breath /
    //     room-tone / trailing noise can fool a scan over a whole loose segment
    //     (the "few seconds too long" failure).
    //   * Whisper word timestamps know roughly WHERE the words are, but their
    //     EDGES are loose — the first word's start runs early and the last
    //     word's end runs late — so they make a poor boundary on their own.
    // So we use the words only to BOUND the RMS search window (kills the gross
    // errors), then let RMS find the precise edge inside it (kills loose edges).
    // Clips with no word data (a re-sync after reload, before re-transcription)
    // just scan the full clip range, exactly as the original did.
    std::unordered_map<std::string, std::vector<const WordSegment*>> fileWords;
    for (size_t fi = 0; fi < m_allTranscriptionResults.size(); ++fi) {
        const std::string sf = (fi < m_audioPaths.size()) ? m_audioPaths[fi]
                                                          : std::string();
        if (sf.empty()) continue;
        auto& vec = fileWords[sf];
        for (const auto& seg : m_allTranscriptionResults[fi].segments)
            for (const auto& w : seg.words)
                if (w.end > w.start) vec.push_back(&w);
        std::sort(vec.begin(), vec.end(),
                  [](const WordSegment* a, const WordSegment* b) {
                      return a->start < b->start;
                  });
    }
    // ── Two-threshold (hysteresis) voice-activity trim ──────────────────────
    // The approach a human takes reading the waveform: find the bursts that are
    // CLEARLY speech, ignore the quiet breaths / room-tone, then span from the
    // first real speech to the last — INCLUDING the soft consonant edges
    // attached to them.  A single low threshold (the previous approach) cannot
    // do both: it either grabs a breath (huge leading gap) or misses a soft
    // onset (cuts into the word).
    //   edgeThresh   — "still part of speech": low, so soft onsets/offsets
    //                  (s/f/th, breathy releases) stay attached.
    //   speechThresh — "clearly speech": high enough that a breath / lip-smack /
    //                  room tone never reaches it, so those never anchor an edge.
    // A contiguous run of windows above edgeThresh is KEPT only if it peaks
    // above speechThresh AND overlaps the spoken word span; the clip becomes
    // [start of first kept run, end of last kept run].
    constexpr double kEnvWinSec  = 0.020;  // 20 ms analysis window
    constexpr double kEnvHopSec  = 0.010;  // 10 ms hop
    constexpr float  kEdgeFrac   = 0.12f;  // edgeThresh   = noise + 12% of range
    constexpr float  kSpeechFrac = 0.40f;  // speechThresh = noise + 40% of range
    const double kPrePadSec  = std::max(0.0, m_syncFrontPaddingSec);
    const double kPostPadSec = std::max(0.0, m_syncEndPaddingSec);
    constexpr double kWordPadSec = 0.20;   // word-span overlap slack

    int trimCount = 0;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        auto& clip = m_clips[i];
        if (clip.matchState == 2) continue; // preserve confirmed boundaries
        if (!clipInScope(clip)) continue;   // don't touch other characters

        auto it = m_audioSamples.find(clip.sourceFile);
        if (it == m_audioSamples.end()) continue;
        const auto& audioData = it->second;
        const int sr = static_cast<int>(audioData.sampleRate);
        const int N  = static_cast<int>(audioData.samples.size());
        if (sr <= 0 || N == 0) continue;

        // Analyse the FULL clip range so the noise floor is measured against the
        // leading/trailing silence whisper left around the utterance.
        const int s0  = std::max(0, static_cast<int>(clip.start * sr));
        const int s1  = std::min(N, static_cast<int>(clip.end   * sr));
        const int win = std::max(1, static_cast<int>(kEnvWinSec * sr));
        const int hop = std::max(1, static_cast<int>(kEnvHopSec * sr));
        if (s1 - s0 < win * 3) continue;

        // Energy envelope (RMS per hop) + the sample each window starts at.
        std::vector<float> env;
        std::vector<int>   envPos;
        env.reserve(static_cast<size_t>((s1 - s0) / hop + 1));
        for (int p = s0; p + win <= s1; p += hop) {
            float sumSq = 0.0f;
            for (int j = 0; j < win; ++j) {
                const float v = audioData.samples[static_cast<size_t>(p + j)];
                sumSq += v * v;
            }
            env.push_back(std::sqrt(sumSq / static_cast<float>(win)));
            envPos.push_back(p);
        }
        if (env.size() < 5) continue;

        // Robust levels: 10th-pct ~ silence floor, 90th-pct ~ speech level.
        std::vector<float> sorted = env;
        std::sort(sorted.begin(), sorted.end());
        const float noiseFloor = sorted[sorted.size() / 10];
        const float speechPeak = sorted[sorted.size() * 9 / 10];
        const float range = speechPeak - noiseFloor;
        const float edgeThresh   = noiseFloor + kEdgeFrac   * range;
        const float speechThresh = noiseFloor + kSpeechFrac * range;
        const bool  levelsUsable = (range >= 0.0008f);  // else ~uniform; the VAD can't help

        // Spoken-word span.  The words reliably bracket WHERE the speech is even
        // when the audio-level VAD fails (a loud transient — pop, breath, bleed —
        // can inflate speechThresh above the real, quieter speech, leaving NO
        // core).  We use the span as a HARD bound on the final boundary and let
        // the VAD only TIGHTEN within it; without it, a clip whose VAD skips
        // keeps whisper's over-extended segment end (the "huge gap").
        bool   hasWords    = false;
        double wFirstStart = 0.0, wLastEnd = 0.0;
        int    wordLo = s0, wordHi = s1;
        if (auto wit = fileWords.find(clip.sourceFile);
            wit != fileWords.end() && !wit->second.empty()) {
            const WordSegment* first = nullptr;
            const WordSegment* last  = nullptr;
            for (const WordSegment* w : wit->second) {
                const double mid = 0.5 * (w->start + w->end);
                if (mid < clip.start || mid > clip.end) continue;
                if (!first) first = w;
                last = w;
            }
            if (first && last) {
                hasWords    = true;
                wFirstStart = first->start;
                wLastEnd    = last->end;
                wordLo = static_cast<int>((wFirstStart - kWordPadSec) * sr);
                wordHi = static_cast<int>((wLastEnd   + kWordPadSec) * sr);
            }
        }

        // VAD: find the speech CORE (windows clearing speechThresh within the
        // word span), then extend outward a BOUNDED amount while above edgeThresh
        // to catch the soft onset/offset (s/f/th, breathy release) without
        // swallowing a long trailing breath / room-tone swell.  Produces a
        // tight boundary WHEN it finds a confident core; otherwise vadOk stays
        // false and the word-span clamp below does the bounding.
        double vadStart = 0.0, vadEnd = 0.0;
        bool   vadOk    = false;
        if (levelsUsable) {
            int coreStart = -1, coreEnd = -1;
            // Collect speech runs (consecutive windows >= speechThresh within the
            // word span), then drop TRAILING runs separated from the body by a
            // long silence — a breath / noise / next-line bleed AFTER the
            // dialogue.  Anchoring the end to the last single speech window
            // (ignoring the big silence before it) is what left the "huge empty
            // space at the end".  coreStart stays the first speech window (the
            // start was already correct).
            std::vector<std::pair<int,int>> runs;  // [startK, endK] env indices
            {
                int rs = -1;
                for (int k = 0; k < static_cast<int>(env.size()); ++k) {
                    const int sample = envPos[static_cast<size_t>(k)];
                    const bool sp = env[static_cast<size_t>(k)] >= speechThresh
                                    && sample + win > wordLo && sample < wordHi;
                    if (sp) { if (rs < 0) rs = k; }
                    else if (rs >= 0) { runs.push_back({rs, k - 1}); rs = -1; }
                }
                if (rs >= 0) runs.push_back({rs, static_cast<int>(env.size()) - 1});
            }
            if (!runs.empty()) {
                constexpr double kMaxBodyGapSec = 0.7;  // silence that ends the utterance body
                size_t lastBody = runs.size() - 1;
                while (lastBody > 0) {
                    const double gap = (envPos[runs[lastBody].first]
                                        - (envPos[runs[lastBody - 1].second] + win))
                                       / static_cast<double>(sr);
                    if (gap > kMaxBodyGapSec) --lastBody;  // detached trailing run → drop
                    else break;
                }
                coreStart = runs.front().first;
                coreEnd   = runs[lastBody].second;
            }
            if (coreStart >= 0) {
                constexpr double kMaxLeadSec  = 0.15;  // soft-onset extension cap
                constexpr double kMaxTrailSec = 0.10;  // soft-offset extension cap
                const int maxLeadWins  = std::max(1, static_cast<int>(kMaxLeadSec  / kEnvHopSec));
                const int maxTrailWins = std::max(1, static_cast<int>(kMaxTrailSec / kEnvHopSec));
                int startK = coreStart;
                for (int n = 0; n < maxLeadWins && startK - 1 >= 0 &&
                                env[static_cast<size_t>(startK - 1)] >= edgeThresh; ++n)
                    --startK;
                int endK = coreEnd;
                for (int n = 0; n < maxTrailWins && endK + 1 < static_cast<int>(env.size()) &&
                                env[static_cast<size_t>(endK + 1)] >= edgeThresh; ++n)
                    ++endK;
                int ss = std::max(s0, envPos[static_cast<size_t>(startK)]
                                          - static_cast<int>(kPrePadSec * sr));
                int se = std::min(s1, envPos[static_cast<size_t>(endK)] + win
                                          + static_cast<int>(kPostPadSec * sr));
                vadStart = static_cast<double>(ss) / sr;
                vadEnd   = static_cast<double>(se) / sr;
                vadOk    = true;
            }
        }

        // Combine.  Start from the VAD result if it found speech, else the clip's
        // current bounds; then clamp to the word span so the boundary ALWAYS
        // brackets the words (never cuts the first/last word) and never runs far
        // past them (a hugely over-extended segment is bounded to a modest tail).
        double newStart = vadOk ? vadStart : clip.start;
        double newEnd   = vadOk ? vadEnd   : clip.end;
        if (hasWords) {
            // Trust the (now word-bounded) energy VAD's end — it marks where the
            // SOUND actually stops — and only CAP it to the last word + a small
            // pad so a trailing breath / room-swell the VAD kept can't make a
            // huge gap.  whisper's last-word end runs LOOSE-LATE, so we must NOT
            // anchor UP to it: doing that left ~1-2 s of empty space past the
            // real speech on clips where the VAD had correctly found the end
            // earlier.  (The START stays with the VAD: whisper's first-word start
            // runs EARLY, so anchoring the in-point to it pulls every start too
            // early.)
            // Preserve the requested breathing room without changing the
            // transcriber's pinpoint word timestamps.  The VAD result and the
            // clip's existing source/neighbour bounds remain the other caps.
            newEnd = std::min(newEnd, wLastEnd + kPostPadSec);
        } else if (!vadOk) {
            continue;   // no words and no audio core — nothing reliable; leave as-is
        }

        if (newEnd > newStart &&
            (std::abs(newStart - clip.start) > 0.01 ||
             std::abs(newEnd   - clip.end)   > 0.01)) {
            clip.start = newStart;
            clip.end   = newEnd;
            ++trimCount;
        }
    }
    if (trimCount > 0)
        spdlog::info("AudioSync: VAD-trimmed {} clips", trimCount);

    updateSyncProgress(80, "Closing inter-clip gaps...");

    // --- Step 5b: Close gaps between consecutive clips from the same file ---
    // When two adjacent clips come from the same audio file, snap the
    // boundary so there's no dead gap between them.  The midpoint of
    // the gap is used as the shared boundary.
    // Use a sorted index list so m_clips retains its original order
    // (not scrambled by source-file sorting).
    std::vector<size_t> sortedIdx(m_clips.size());
    std::iota(sortedIdx.begin(), sortedIdx.end(), size_t{0});
    std::sort(sortedIdx.begin(), sortedIdx.end(),
              [&](size_t x, size_t y) {
                  if (m_clips[x].sourceFile != m_clips[y].sourceFile)
                      return m_clips[x].sourceFile < m_clips[y].sourceFile;
                  return m_clips[x].start < m_clips[y].start;
              });
    int gapsClosed = 0;
    for (size_t si = 0; si + 1 < sortedIdx.size(); ++si) {
        auto& a = m_clips[sortedIdx[si]];
        auto& b = m_clips[sortedIdx[si + 1]];
        if (a.sourceFile != b.sourceFile) continue;
        if (a.matchState == 2 || b.matchState == 2) continue; // preserve confirmed boundaries
        if (!clipInScope(a) || !clipInScope(b)) continue;     // scope to filter
        double gap = b.start - a.end;
        if (gap > 0.001 && gap < 0.050) { // only close tiny seam gaps (<50ms)
            // Find the quietest point in the gap to place the boundary
            double boundary = (a.end + b.start) / 2.0;
            auto sampIt = m_audioSamples.find(a.sourceFile);
            if (sampIt != m_audioSamples.end()) {
                const auto& ad = sampIt->second;
                int sr = static_cast<int>(ad.sampleRate);
                int winSz = static_cast<int>(0.010 * sr);
                int gapStart = static_cast<int>(a.end * sr);
                int gapEnd = static_cast<int>(b.start * sr);
                if (gapEnd - gapStart > winSz) {
                    float bestRms = 1e9f;
                    int bestPos = (gapStart + gapEnd) / 2;
                    for (int pos = gapStart; pos + winSz <= gapEnd; pos += winSz / 2) {
                        float sumSq = 0.0f;
                        for (int j = 0; j < winSz && static_cast<size_t>(pos + j) < ad.samples.size(); ++j) {
                            float s = ad.samples[static_cast<size_t>(pos + j)];
                            sumSq += s * s;
                        }
                        float rms = std::sqrt(sumSq / static_cast<float>(winSz));
                        if (rms < bestRms) { bestRms = rms; bestPos = pos; }
                    }
                    boundary = static_cast<double>(bestPos) / sr;
                }
            }
            a.end = boundary;
            b.start = boundary;
            ++gapsClosed;
        }
    }
    if (gapsClosed > 0)
        spdlog::info("AudioSync: Closed {} inter-clip gaps", gapsClosed);

    // Auto-Sync only proposes matches.  Confidence affects which line is
    // proposed, never whether that proposal is approved; approval always
    // requires an explicit user action.  Existing confirmed clips were
    // excluded above and remain confirmed.
    updateSyncProgress(90, "Finalizing tentative matches...");

    m_syncDone = true;
    populateClipList();
    updateWorkflowState();

    // When filtering, report against the number of in-scope clips so the
    // ratio isn't diluted by every other character's clips.
    int scopeTotal = static_cast<int>(m_clips.size());
    if (hasCharFilter) {
        scopeTotal = 0;
        for (const auto& clip : m_clips)
            if (clipInScope(clip)) ++scopeTotal;
    }
    m_syncStatus->setText(
        hasCharFilter
            ? QString("Matched %1/%2 segments for %3")
                  .arg(matchCount).arg(scopeTotal).arg(filterCharQt)
            : QString("Matched %1/%2 segments").arg(matchCount).arg(scopeTotal));

    spdlog::info("AudioSync: Auto-sync matched {}/{} segments{}",
                 matchCount, scopeTotal,
                 hasCharFilter ? " (filtered: " + filterCharQt.toStdString() + ")" : "");
    emit voiceContextChanged();
    emit syncCompleted(matchCount, scopeTotal);
}


std::vector<std::pair<double,double>> AudioSync::getEffectiveRanges(const SyncClip& clip)
{
    if (clip.deletedRegions.empty())
        return {{clip.start, clip.end}};

    // Sort deleted regions by start time
    auto deleted = clip.deletedRegions;
    std::sort(deleted.begin(), deleted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<double,double>> ranges;
    double currentStart = clip.start;

    for (const auto& [delStart, delEnd] : deleted) {
        double ds = std::max(delStart, clip.start);
        double de = std::min(delEnd, clip.end);

        // Add range before deleted region (at least 50ms)
        if (currentStart < ds - 0.05)
            ranges.emplace_back(currentStart, ds);

        currentStart = de;
    }

    // Final range after all deleted regions
    if (currentStart < clip.end - 0.05)
        ranges.emplace_back(currentStart, clip.end);

    return ranges;
}

// Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Export to Timeline (ported from Python _export_timeline) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬

void AudioSync::createClipsFromTranscription()
{
    // Legacy single-file version Ã¢â‚¬â€ delegates to multi-file
    createClipsFromAllTranscriptions();
}

void AudioSync::createClipsFromAllTranscriptions()
{
    m_clips.clear();

    int globalId = 0;
    for (size_t fileIdx = 0; fileIdx < m_allTranscriptionResults.size(); ++fileIdx) {
        const auto& result = m_allTranscriptionResults[fileIdx];
        std::string sourceFile = (fileIdx < m_audioPaths.size()) ? m_audioPaths[fileIdx] : "";

        // Derive character name from audio filename (like Python's _extract_character_name)
        std::string charName;
        if (!sourceFile.empty()) {
            charName = extractCharacterName(sourceFile);
        }

        for (const auto& seg : result.segments) {
            SyncClip clip;
            clip.id         = globalId++;
            clip.sourceFile = sourceFile;
            clip.start      = seg.start;
            clip.end        = seg.end;
            clip.transcript = seg.text;
            clip.editedText = seg.text;
            // Use segment character if set, otherwise derive from filename
            clip.character  = seg.character.empty() ? charName : seg.character;
            m_clips.push_back(std::move(clip));
        }
    }

    spdlog::info("AudioSync: Created {} clips from {} file(s)",
                 m_clips.size(), m_allTranscriptionResults.size());
}

void AudioSync::appendClipsFromNewTranscriptions()
{
    // Build a set of source files that already have clips so we don't
    // duplicate them.  This preserves existing clips with their
    // matchState, scriptLineNumber, character, confidence, etc.
    std::unordered_set<std::string> existingSourceFiles;
    for (const auto& c : m_clips)
        existingSourceFiles.insert(c.sourceFile);

    int nextId = m_clips.empty() ? 0 : m_clips.back().id + 1;

    size_t added = 0;
    for (size_t fileIdx = 0; fileIdx < m_allTranscriptionResults.size(); ++fileIdx) {
        const auto& result = m_allTranscriptionResults[fileIdx];
        std::string sourceFile = (fileIdx < m_audioPaths.size()) ? m_audioPaths[fileIdx] : "";

        // Skip files that already have clips (already transcribed + synced)
        if (existingSourceFiles.count(sourceFile))
            continue;

        std::string charName;
        if (!sourceFile.empty())
            charName = extractCharacterName(sourceFile);

        for (const auto& seg : result.segments) {
            SyncClip clip;
            clip.id         = nextId++;
            clip.sourceFile = sourceFile;
            clip.start      = seg.start;
            clip.end        = seg.end;
            clip.transcript = seg.text;
            clip.editedText = seg.text;
            clip.character  = seg.character.empty() ? charName : seg.character;
            m_clips.push_back(std::move(clip));
            ++added;
        }
    }

    spdlog::info("AudioSync: Appended {} new clips (preserved {} existing)",
                 added, existingSourceFiles.size());
}

} // namespace rt
