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
                float orderBonus = std::max(0.0f, 0.15f * (1.0f - orderDist * 2.0f));

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

        for (const auto& entry : allPairs) {
            if (assignedClips.count(entry.clipIdx)) continue;
            if (!allowRetakes && assignedLines.count(entry.lineIdx)) continue;

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

    updateSyncProgress(60, "Trimming silence from clips...");

    updateSyncProgress(60, "Trimming silence from clips...");

    // --- Step 5: Auto-trim silence from non-confirmed clips ---
    int trimCount = 0;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        auto& clip = m_clips[i];
        if (clip.matchState == 2) continue; // preserve confirmed boundaries
        if (!clipInScope(clip)) continue;   // don't touch other characters
        auto it = m_audioSamples.find(clip.sourceFile);
        if (it == m_audioSamples.end()) continue;

        const auto& audioData = it->second;
        const int windowSize = static_cast<int>(0.010 * audioData.sampleRate); // 10ms
        auto startSample = static_cast<int>(clip.start * audioData.sampleRate);
        auto endSample   = static_cast<int>(clip.end   * audioData.sampleRate);
        if (startSample >= static_cast<int>(audioData.samples.size())) continue;
        if (endSample > static_cast<int>(audioData.samples.size()))
            endSample = static_cast<int>(audioData.samples.size());
        if (endSample - startSample < windowSize * 3) continue;

        // Measure all window RMS values to compute energy profile
        std::vector<float> rmsValues;
        for (int pos = startSample; pos + windowSize <= endSample; pos += windowSize) {
            float sumSq = 0.0f;
            for (int j = 0; j < windowSize; ++j) {
                float s = audioData.samples[static_cast<size_t>(pos + j)];
                sumSq += s * s;
            }
            rmsValues.push_back(std::sqrt(sumSq / static_cast<float>(windowSize)));
        }
        if (rmsValues.size() < 3) continue;

        // Dynamic-range threshold: 8% of the way from noise floor to speech peak
        auto sortedRms = rmsValues;
        std::sort(sortedRms.begin(), sortedRms.end());
        float noiseFloor  = sortedRms[sortedRms.size() / 10];       // 10th percentile
        float speechLevel = sortedRms[sortedRms.size() * 9 / 10];   // 90th percentile
        float dynRange    = speechLevel - noiseFloor;
        float trimThreshold = (dynRange > 0.001f)
            ? noiseFloor + dynRange * 0.08f
            : std::max(0.003f, noiseFloor * 1.5f);

        // Scan forward for speech start � require 2 consecutive above-threshold windows
        int speechStart = startSample;
        for (int pos = startSample; pos + windowSize * 2 <= endSample; pos += windowSize) {
            auto rmsAt = [&](int p) {
                float sumSq = 0.0f;
                for (int j = 0; j < windowSize; ++j) {
                    float s = audioData.samples[static_cast<size_t>(p + j)];
                    sumSq += s * s;
                }
                return std::sqrt(sumSq / static_cast<float>(windowSize));
            };
            if (rmsAt(pos) > trimThreshold && rmsAt(pos + windowSize) > trimThreshold) {
                speechStart = pos; break;
            }
        }
        // Scan backward for speech end � require 2 consecutive above-threshold windows
        int speechEnd = endSample;
        for (int pos = endSample - windowSize * 2; pos >= startSample; pos -= windowSize) {
            auto rmsAt = [&](int p) {
                if (p + windowSize > endSample) return 0.0f;
                float sumSq = 0.0f;
                for (int j = 0; j < windowSize; ++j) {
                    float s = audioData.samples[static_cast<size_t>(p + j)];
                    sumSq += s * s;
                }
                return std::sqrt(sumSq / static_cast<float>(windowSize));
            };
            if (rmsAt(pos) > trimThreshold && rmsAt(pos + windowSize) > trimThreshold) {
                speechEnd = pos + windowSize * 2; break;
            }
        }
        int prepad  = static_cast<int>(0.060 * audioData.sampleRate); // 60ms
        int postpad = static_cast<int>(0.040 * audioData.sampleRate); // 40ms
        speechStart = std::max(startSample, speechStart - prepad);
        speechEnd   = std::min(endSample,   speechEnd + postpad);
        double newStart = static_cast<double>(speechStart) / audioData.sampleRate;
        double newEnd   = static_cast<double>(speechEnd)   / audioData.sampleRate;
        if (std::abs(newStart - clip.start) > 0.01 || std::abs(newEnd - clip.end) > 0.01) {
            clip.start = newStart;
            clip.end   = newEnd;
            ++trimCount;
        }
    updateSyncProgress(80, "Closing inter-clip gaps...");

    }
    if (trimCount > 0)
        spdlog::info("AudioSync: Auto-trimmed silence from {} clips", trimCount);

    updateSyncProgress(80, "Closing inter-clip gaps...");

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
    updateSyncProgress(90, "Confirming high-confidence matches...");

            ++gapsClosed;
        }
    }
    if (gapsClosed > 0)
        spdlog::info("AudioSync: Closed {} inter-clip gaps", gapsClosed);

    updateSyncProgress(90, "Confirming high-confidence matches...");

    updateSyncProgress(90, "Confirming high-confidence matches...");

    // --- Step 6: Auto-confirm high-confidence matches ---
    int autoConfirmed = 0;
    for (auto& clip : m_clips) {
        if (!clipInScope(clip)) continue;
        if (clip.matchState == 1 && clip.confidence >= 0.90f) {
            clip.matchState = 2;
            ++autoConfirmed;
        }
    }
    if (autoConfirmed > 0)
        spdlog::info("AudioSync: Auto-confirmed {} high-confidence matches", autoConfirmed);

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

