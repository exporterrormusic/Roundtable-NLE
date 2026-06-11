/*
 * AudioSyncResegmentAndManual.cpp - Script-guided resegmentation and manual match.
 * Extracted from AudioSyncData.cpp for modularity.
 */
#include "panels/audio/AudioSync.h"
#include "panels/audio/AudioSyncFileName.h"
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

void AudioSync::mergeSegmentsToMatchScript()
{
    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Post-transcription merge Ã¢â‚¬â€ mirrors Python _merge_segments_to_match_script().
    // Consecutive clips from the same character are merged when the combined
    // text matches a script line significantly better than each clip alone.
    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (m_clips.empty() || !m_script || m_script->lines.empty()) return;

    auto normalizeChar = [](const std::string& name) -> std::string {
        std::string lower;
        for (char c : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // trim
        while (!lower.empty() && lower.back() == ' ') lower.pop_back();
        while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
        if (lower == "marian") return "modernia";
        return lower;
    };

    auto norm = [](const std::string& text) {
        return ScriptMatcher::normalize(text);
    };

    // Group clips by character
    std::unordered_map<std::string, std::vector<size_t>> clipsByChar;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        std::string ch = normalizeChar(m_clips[i].character);
        clipsByChar[ch].push_back(i);
    }

    // Group script lines by character
    std::unordered_map<std::string, std::vector<size_t>> linesByChar;
    for (size_t i = 0; i < m_script->lines.size(); ++i) {
        std::string ch = normalizeChar(m_script->lines[i].character);
        linesByChar[ch].push_back(i);
    }

    std::unordered_set<int> clipsToRemove;
    int mergedCount = 0;

    for (auto& [charKey, clipIndices] : clipsByChar) {
        auto lineIt = linesByChar.find(charKey);
        if (lineIt == linesByChar.end()) continue;
        const auto& lineIndices = lineIt->second;

        // Sort clips by start time
        std::sort(clipIndices.begin(), clipIndices.end(),
                  [&](size_t a, size_t b) { return m_clips[a].start < m_clips[b].start; });

        // Pre-normalize all script lines for this character
        std::vector<std::string> normLines;
        normLines.reserve(lineIndices.size());
        for (size_t li : lineIndices)
            normLines.push_back(norm(m_script->lines[li].dialogue));

        size_t i = 0;
        while (i < clipIndices.size()) {
            size_t ci = clipIndices[i];
            const auto& clip = m_clips[ci];
            if (clip.matchState == 2) { ++i; continue; } // preserve confirmed clips
            std::string clipText = norm(clip.editedText.empty() ? clip.transcript : clip.editedText);

            // Best single-clip score against any script line
            float bestSingleScore = 0.0f;
            for (const auto& lt : normLines) {
                float s = ScriptMatcher::sequenceRatio(clipText, lt);
                if (s > bestSingleScore) bestSingleScore = s;
            }

            // Try merging with next 1Ã¢â‚¬â€œ10 clips (extended for long monologues)
            std::string mergedText = clipText;
            float bestMergeScore = bestSingleScore;
            size_t bestMergeCount = 0;
            size_t lastMergedIdx = ci;

            for (size_t j = 1; j <= 10 && i + j < clipIndices.size(); ++j) {
                size_t nextCi = clipIndices[i + j];
                const auto& nextClip = m_clips[nextCi];
                // Never merge into a confirmed (manually assigned) clip — doing so
                // would destroy the user's manual match.
                if (nextClip.matchState == 2)
                    break;
                // Merge if clips are close together Ã¢â‚¬â€ 2.5s gap tolerance for monologues
                if (nextClip.start - m_clips[lastMergedIdx].end > 2.5)
                    break;

                lastMergedIdx = nextCi;
                std::string nextText = norm(
                    nextClip.editedText.empty() ? nextClip.transcript : nextClip.editedText);
                mergedText = mergedText + " " + nextText;

                // Check if merged text matches any line better
                for (const auto& lt : normLines) {
                    float s = ScriptMatcher::sequenceRatio(mergedText, lt);
                    if (s > bestMergeScore + 0.05f) { // Lower threshold for long passages
                        bestMergeScore = s;
                        bestMergeCount = j;
                    }
                }
            }

            if (bestMergeCount > 0 && bestMergeScore > bestSingleScore + 0.05f) {
                // Merge clips: combine text and time range into first clip
                auto& firstClip = m_clips[ci];
                std::string combined;
                for (size_t j = 0; j <= bestMergeCount; ++j) {
                    size_t idx = clipIndices[i + j];
                    const auto& c = m_clips[idx];
                    std::string t = c.editedText.empty() ? c.transcript : c.editedText;
                    // Trim
                    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    if (!combined.empty()) combined += ' ';
                    combined += t;
                }
                firstClip.editedText = combined;
                firstClip.transcript = combined;
                firstClip.end = m_clips[clipIndices[i + bestMergeCount]].end;

                // Mark merged clips for removal
                for (size_t j = 1; j <= bestMergeCount; ++j)
                    clipsToRemove.insert(m_clips[clipIndices[i + j]].id);

                spdlog::debug("AudioSync: Merged {} clips for '{}': score {:.2f} -> {:.2f}",
                              bestMergeCount + 1, charKey, bestSingleScore, bestMergeScore);
                ++mergedCount;
                i += bestMergeCount + 1;
            } else {
                ++i;
            }
        }
    }

    // Remove merged clips
    if (!clipsToRemove.empty()) {
        m_clips.erase(
            std::remove_if(m_clips.begin(), m_clips.end(),
                           [&](const SyncClip& c) { return clipsToRemove.count(c.id) > 0; }),
            m_clips.end());
        // Re-number IDs
        for (size_t i = 0; i < m_clips.size(); ++i)
            m_clips[i].id = static_cast<int>(i);

        spdlog::info("AudioSync: Merged {} segment groups, removed {} clips (now {} clips)",
                     mergedCount, clipsToRemove.size(), m_clips.size());
    }
}

void AudioSync::resegmentByScript()
{
    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Script-guided re-segmentation: split clips whose transcript spans
    // multiple script lines.  Whisper often produces one long segment that
    // actually covers 2-3 script lines Ã¢â‚¬â€ this step re-cuts those clips so
    // each new clip aligns with a single script line.
    //
    // The algorithm iterates up to 3 rounds so that a segment spanning 3+
    // lines is progressively split (first into 2, then the longer half
    // may split again).
    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (m_clips.empty() || !m_script || m_script->lines.empty()) return;

    auto normalizeChar = [](const std::string& name) -> std::string {
        std::string lower;
        for (char c : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!lower.empty() && lower.back() == ' ') lower.pop_back();
        while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
        if (lower == "marian") return "modernia";
        return lower;
    };

    auto norm = [](const std::string& text) {
        return ScriptMatcher::normalize(text);
    };

    int totalSplits = 0;

    // Iterate up to 3 rounds to handle multi-line segments
    for (int round = 0; round < 3; ++round) {
        // Group clips by character
        std::unordered_map<std::string, std::vector<size_t>> clipsByChar;
        for (size_t i = 0; i < m_clips.size(); ++i)
            clipsByChar[normalizeChar(m_clips[i].character)].push_back(i);

        // Group script lines by character
        std::unordered_map<std::string, std::vector<size_t>> linesByChar;
        for (size_t i = 0; i < m_script->lines.size(); ++i)
            linesByChar[normalizeChar(m_script->lines[i].character)].push_back(i);

        // Collect splits to apply
        struct SplitOp {
            size_t clipIdx;     // index in m_clips
            size_t splitWord;   // split after this many words
        };
        std::vector<SplitOp> splits;

        for (auto& [charKey, clipIndices] : clipsByChar) {
            auto lineIt = linesByChar.find(charKey);
            if (lineIt == linesByChar.end() || lineIt->second.empty()) continue;
            const auto& lineIndices = lineIt->second;

            // Pre-normalize script lines
            std::vector<std::string> normLines;
            normLines.reserve(lineIndices.size());
            for (size_t li : lineIndices)
                normLines.push_back(norm(m_script->lines[li].dialogue));

            for (size_t ci : clipIndices) {
                const auto& clip = m_clips[ci];
                if (clip.matchState == 2) continue; // confirmed Ã¢â‚¬â€ don't touch

                std::string clipText = norm(
                    clip.editedText.empty() ? clip.transcript : clip.editedText);

                // Split clip text into words
                std::vector<std::string> words;
                { std::istringstream ss(clipText); std::string w; while (ss >> w) words.push_back(w); }

                if (words.size() < 4) continue; // too short to split meaningfully

                // Best single-line match score
                float bestSingleScore = 0.0f;
                for (const auto& lt : normLines) {
                    float s = ScriptMatcher::sequenceRatio(clipText, lt);
                    bestSingleScore = std::max(bestSingleScore, s);
                }
                if (bestSingleScore >= 0.85f) continue; // already matches well

                // Try every split point: find the split that produces two halves
                // where the LONGER half matches a script line better than the
                // whole clip does.  We weight each half's score by its proportion
                // of the total word count so short garbage prefixes/suffixes
                // don't dominate.
                float bestSplitScore = -1.0f;
                size_t bestSplitPoint = 0;

                for (size_t sp = 2; sp <= words.size() - 2; ++sp) {
                    // Build both halves
                    std::string half1, half2;
                    for (size_t w = 0; w < sp; ++w) {
                        if (!half1.empty()) half1 += ' ';
                        half1 += words[w];
                    }
                    for (size_t w = sp; w < words.size(); ++w) {
                        if (!half2.empty()) half2 += ' ';
                        half2 += words[w];
                    }

                    // Find best matching script line for each half
                    float bestH1 = 0.0f, bestH2 = 0.0f;
                    size_t bestL1 = SIZE_MAX, bestL2 = SIZE_MAX;
                    for (size_t li = 0; li < normLines.size(); ++li) {
                        float s1 = ScriptMatcher::sequenceRatio(half1, normLines[li]);
                        float s2 = ScriptMatcher::sequenceRatio(half2, normLines[li]);
                        if (s1 > bestH1) { bestH1 = s1; bestL1 = li; }
                        if (s2 > bestH2) { bestH2 = s2; bestL2 = li; }
                    }

                    // At least the better half must match well (> 0.50)
                    float bestHalf = std::max(bestH1, bestH2);
                    if (bestHalf < 0.50f) continue;

                    // Weight-averaged score: each half weighted by word proportion
                    float frac1 = static_cast<float>(sp) / static_cast<float>(words.size());
                    float weighted = bestH1 * frac1 + bestH2 * (1.0f - frac1);

                    // Accept if weighted score beats single AND the better half
                    // improves over the single-clip match
                    if (weighted > bestSingleScore && bestHalf > bestSingleScore + 0.02f
                        && weighted > bestSplitScore) {
                        bestSplitScore = weighted;
                        bestSplitPoint = sp;
                    }
                }

                if (bestSplitPoint > 0) {
                    splits.push_back({ci, bestSplitPoint});
                    spdlog::debug("AudioSync: Splitting clip {} at word {} "
                                  "(single={:.2f} split={:.2f})",
                                  ci, bestSplitPoint, bestSingleScore, bestSplitScore);
                }
            }
        }

        if (splits.empty()) break; // no more splits needed

        // Apply splits in reverse order so indices stay valid
        std::sort(splits.begin(), splits.end(),
                  [](const SplitOp& a, const SplitOp& b) {
                      return a.clipIdx > b.clipIdx;
                  });

        for (const auto& op : splits) {
            auto& clip = m_clips[op.clipIdx];
            std::string text = clip.editedText.empty() ? clip.transcript : clip.editedText;

            // Split the text by words
            std::vector<std::string> words;
            { std::istringstream ss(ScriptMatcher::normalize(text)); std::string w; while (ss >> w) words.push_back(w); }

            // Build first half text (use original-ish words from normalized)
            std::string h1, h2;
            for (size_t w = 0; w < op.splitWord; ++w) {
                if (!h1.empty()) h1 += ' ';
                h1 += words[w];
            }
            for (size_t w = op.splitWord; w < words.size(); ++w) {
                if (!h2.empty()) h2 += ' ';
                h2 += words[w];
            }

            // Estimate split time proportionally, then refine to nearest silence
            double totalWords = static_cast<double>(words.size());
            double splitFrac = static_cast<double>(op.splitWord) / totalWords;
            double splitTimeEst = clip.start + (clip.end - clip.start) * splitFrac;
            double splitTime = splitTimeEst;

            // Refine: search �0.5s around the estimate for the quietest
            // 20ms window in the audio � that's the natural breath/pause.
            auto sampIt = m_audioSamples.find(clip.sourceFile);
            if (sampIt != m_audioSamples.end()) {
                const auto& ad = sampIt->second;
                int sr = static_cast<int>(ad.sampleRate);
                int winSz = static_cast<int>(0.020 * sr);
                int center = static_cast<int>(splitTimeEst * sr);
                int searchRadius = static_cast<int>(0.5 * sr);
                int lo = std::max(static_cast<int>(clip.start * sr) + winSz, center - searchRadius);
                int hi = std::min(static_cast<int>(clip.end * sr) - winSz, center + searchRadius);
                float bestRms = 1e9f;
                int bestPos = center;
                for (int pos = lo; pos + winSz <= hi; pos += winSz / 2) {
                    float sumSq = 0.0f;
                    for (int j = 0; j < winSz && static_cast<size_t>(pos + j) < ad.samples.size(); ++j) {
                        float s = ad.samples[static_cast<size_t>(pos + j)];
                        sumSq += s * s;
                    }
                    float rms = std::sqrt(sumSq / static_cast<float>(winSz));
                    if (rms < bestRms) { bestRms = rms; bestPos = pos; }
                }
                splitTime = static_cast<double>(bestPos) / sr;
                if (splitTime < clip.start + 0.1) splitTime = clip.start + 0.1;
                if (splitTime > clip.end - 0.1) splitTime = clip.end - 0.1;
            }

            // Create second clip (insert after current)
            SyncClip clip2;
            clip2.id         = 0; // will renumber later
            clip2.sourceFile = clip.sourceFile;
            clip2.character  = clip.character;
            clip2.start      = splitTime;
            clip2.end        = clip.end;
            clip2.transcript = h2;
            clip2.editedText = h2;
            clip2.matchState = 0;
            clip2.confidence = 0.0f;
            clip2.scriptLineNumber = -1;

            // Trim current clip
            clip.end        = splitTime;
            clip.transcript = h1;
            clip.editedText = h1;

            // Insert clip2 after current clip
            m_clips.insert(m_clips.begin() + static_cast<int>(op.clipIdx) + 1, clip2);
            ++totalSplits;
        }

        // Re-number IDs
        for (size_t i = 0; i < m_clips.size(); ++i)
            m_clips[i].id = static_cast<int>(i);
    }

    if (totalSplits > 0)
        spdlog::info("AudioSync: Re-segmentation split {} clips (now {} clips)",
                     totalSplits, m_clips.size());
}

void AudioSync::openManualMatch(int lineNumber)
{
    if (!m_script || m_audioPaths.empty()) return;

    // Find the script line
    std::string character, dialogue;
    for (const auto& sl : m_script->lines) {
        if (sl.lineNumber == lineNumber) {
            character = sl.character;
            dialogue  = sl.dialogue;
            break;
        }
    }

    // Collect per-file confirmed & tentative regions
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> confirmedByFile;
    std::unordered_map<std::string, std::vector<std::pair<double,double>>> tentativeByFile;
    for (const auto& clip : m_clips) {
        if (clip.scriptLineNumber < 0) continue;
        if (clip.matchState == 2)
            confirmedByFile[clip.sourceFile].emplace_back(clip.start, clip.end);
        else if (clip.matchState == 1)
            tentativeByFile[clip.sourceFile].emplace_back(clip.start, clip.end);
    }

    // Build audio data map using same type as ManualMatchDialog expects
    std::unordered_map<std::string, ManualMatchDialog::AudioData> audioDataMap;
    for (const auto& [path, sampleData] : m_audioSamples) {
        audioDataMap[path] = { sampleData.samples, sampleData.sampleRate };
    }

    // Pre-select audio file: user-assigned file takes priority,
    // otherwise fall back to character name matching.
    std::string preselectedFile;
    auto assignIt = m_lineAudioFile.find(lineNumber);
    if (assignIt != m_lineAudioFile.end() && !assignIt->second.empty()) {
        preselectedFile = assignIt->second;
    } else {
        for (const auto& p : m_audioPaths) {
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
    if (preselectedFile.empty() && !m_audioPaths.empty())
        preselectedFile = m_audioPaths[0];

    ManualMatchDialog dlg(
        character, dialogue, lineNumber,
        m_audioPaths, audioDataMap,
        {}, {},  // global regions not used Ã¢â‚¬â€ per-file via m_confirmedByFile
        preselectedFile,
        m_audioEngine,
        this);

    // Set per-file region maps, then load initial file (must be after regions)
    dlg.m_confirmedByFile = confirmedByFile;
    dlg.m_tentativeByFile = tentativeByFile;
    dlg.loadInitialFile();

    m_manualMatchOpen = true;
    int dialogResult = dlg.exec();
    m_manualMatchOpen = false;

    if (dialogResult == QDialog::Accepted) {
        auto result = dlg.result();

        // Check if this line already has a clip Ã¢â‚¬â€ update it
        bool found = false;
        for (auto& clip : m_clips) {
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
            newClip.id              = static_cast<int>(m_clips.size());
            newClip.sourceFile      = result.audioFile;
            newClip.character       = character;
            newClip.start           = result.start;
            newClip.end             = result.end;
            newClip.matchState      = 2;
            newClip.confidence      = 1.0f;
            newClip.scriptLineNumber = lineNumber;
            newClip.scriptSegment   = character + ": " + dialogue;
            m_clips.push_back(std::move(newClip));
        }

        spdlog::info("AudioSync: Manual match for line {} Ã¢â€ â€™ {}  {:.3f}-{:.3f}",
                      lineNumber, result.audioFile, result.start, result.end);
        // Defer heavy UI rebuild so the dialog closes instantly
        // For existing clips, do lightweight in-place UI update;
        // for new clips, full rebuild since card count changed.
        if (found) {
            QTimer::singleShot(0, this, [this, lineNumber]() {
                // Find the clip index for this line
                for (size_t ci = 0; ci < m_clips.size(); ++ci) {
                    if (m_clips[ci].scriptLineNumber == lineNumber) {
                        updateCardMatchStyle(ci);
                        // Update waveform widget if it exists
                        for (size_t wi = 0; wi < m_cardScriptLineNums.size(); ++wi) {
                            if (m_cardScriptLineNums[wi] == lineNumber && wi < m_cardWaveforms.size()) {
                                if (auto wv = m_cardWaveforms[wi]) {
                                    auto sit = m_audioSamples.find(m_clips[ci].sourceFile);
                                    if (sit != m_audioSamples.end()) {
                                        wv->setAudioShared(
                                            &sit->second.samples,
                                            sit->second.sampleRate,
                                            m_clips[ci].start,
                                            m_clips[ci].end);
                                        wv->update();
                                    }
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
                populateLeftList();
                updateWorkflowState(); // also sets m_syncDone + enables export when all confirmed
            });
        } else {
            QTimer::singleShot(0, this, [this]() {
                populateCards();
            });
        }
    }
}


// Split/merge + persistence methods moved to AudioSyncState.cpp


// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Script history (recent URLs dropdown)
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â



} // namespace rt
