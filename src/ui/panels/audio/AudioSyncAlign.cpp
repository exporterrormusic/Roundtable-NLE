/*
 * AudioSyncAlign.cpp — word-level forced alignment of transcript words to the
 * script.
 *
 * Replaces the fuzzy merge/resegment/match heuristics (when word timestamps are
 * available) with a Needleman-Wunsch alignment of each audio file's transcribed
 * word stream to its character's script-line words.  Each transcript word
 * inherits the script word's line number; consecutive words of the same line
 * become one clip spanning exactly that line's words.  Because the clip RANGES
 * now follow LINE boundaries, the "clip spans 1.5 lines / misses the tail"
 * problems (and "fixed file on wrong lines") go away by construction.
 *
 * See AudioSync::runAutoSync(), which calls this first and falls back to the
 * heuristic path when it returns false (no words, or no script-character match).
 */
#include "panels/audio/AudioSync.h"
#include "panels/audio/AudioSyncFileName.h"
#include "ai/ScriptMatcher.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rt {

// Shave only leading/trailing SILENCE from [clipStart, clipEnd], keeping ALL
// speech between — including the quiet onsets/tails of an expressive line.  The
// word span already brackets the line (first word → last word, whisper edges
// run loose), so here we just trim the loose silence: we do NOT anchor to a loud
// "speech core" (that collapsed long, dynamic lines down to their loudest middle
// chunk and cut the softer real speech on both sides).
static void refineSpeechBounds(const std::vector<float>& samples, int sr,
                               double& clipStart, double& clipEnd,
                               double wordStart, double wordEnd,
                               double frontPaddingSec, double endPaddingSec,
                               bool& refined)
{
    if (sr <= 0) return;
    const int N  = static_cast<int>(samples.size());
    int s0 = std::max(0, static_cast<int>(clipStart * sr));
    int s1 = std::min(N, static_cast<int>(clipEnd   * sr));
    const int win = std::max(1, static_cast<int>(0.020 * sr));
    const int hop = std::max(1, static_cast<int>(0.010 * sr));
    if (s1 - s0 < win * 3) return;

    std::vector<float> env;
    std::vector<int>   pos;
    for (int p = s0; p + win <= s1; p += hop) {
        float sum = 0.0f;
        for (int j = 0; j < win; ++j) { const float v = samples[static_cast<size_t>(p + j)]; sum += v * v; }
        env.push_back(std::sqrt(sum / static_cast<float>(win)));
        pos.push_back(p);
    }
    if (env.size() < 5) return;

    std::vector<float> sorted = env;
    std::sort(sorted.begin(), sorted.end());
    const float noise = sorted[sorted.size() / 10];
    const float peak  = sorted[sorted.size() * 9 / 10];
    const float range = peak - noise;
    if (range < 0.0008f) return;
    const float thresh = noise + 0.12f * range;   // above this = speech (incl. soft edges)

    // Speech runs (contiguous windows above threshold).
    const int M = static_cast<int>(env.size());
    std::vector<std::pair<int,int>> runs;          // [first, last] window indices
    for (int k = 0, rs = -1; k <= M; ++k) {
        const bool sp = k < M && env[static_cast<size_t>(k)] >= thresh;
        if (sp) { if (rs < 0) rs = k; }
        else if (rs >= 0) { runs.push_back({ rs, k - 1 }); rs = -1; }
    }
    if (runs.empty()) return;

    // Merge runs separated by a SHORT gap (natural intra-line pauses); a longer
    // gap marks a real break (breath / next-speaker bleed / silence) and splits
    // the body.  ~0.40 s in 10 ms hops.  Never bridge across a word-span edge,
    // though: a separated run lying wholly after the last word (or a body lying
    // wholly before the first) is a breath/lip-smack, and merging it used to
    // glue it onto the speech so the overlap filter below could no longer drop
    // it — the main "out point too late" mechanism.  ±0.10 s tolerance since
    // even DTW word edges are only good to a few hops.
    const int mergeGap = 40;
    std::vector<std::pair<int,int>> body;
    body.push_back(runs.front());
    for (size_t r = 1; r < runs.size(); ++r) {
        const double backE = static_cast<double>(pos[static_cast<size_t>(body.back().second)] + win) / sr;
        const double runS  = static_cast<double>(pos[static_cast<size_t>(runs[r].first)]) / sr;
        const bool acrossEdge = backE < wordStart - 0.10   // body so far = pre-word junk
                             || runS  > wordEnd   + 0.10;  // incoming run = post-word junk
        if (!acrossEdge && runs[r].first - body.back().second <= mergeGap)
            body.back().second = runs[r].second;
        else body.push_back(runs[r]);
    }

    // Keep EVERY connected segment that overlaps the line's word span — that's
    // the whole line, internal pauses included (a long line can split into
    // several segments, so picking just one would chop the line in half).  Drop
    // only segments that fall entirely OUTSIDE the words: a breath/bleed before
    // the first word or after the last.  An onset that starts a touch before the
    // first word still overlaps, so it's recovered.
    int firstKeep = -1, lastKeep = -1;
    for (int s = 0; s < static_cast<int>(body.size()); ++s) {
        const double segS = static_cast<double>(pos[static_cast<size_t>(body[s].first)])  / sr;
        const double segE = static_cast<double>(pos[static_cast<size_t>(body[s].second)] + win) / sr;
        if (segE >= wordStart && segS <= wordEnd) {   // intersects the word span
            if (firstKeep < 0) firstKeep = s;
            lastKeep = s;
        }
    }
    if (firstKeep < 0) return;   // nothing overlaps the words — keep word-span boundaries

    int bStart = body[firstKeep].first;    // env index of body start
    int bEnd   = body[lastKeep].second;    // env index of body end

    // Firm edges: snap each side to where energy crosses a fraction of that
    // word's OWN local peak (the loudest window within ~0.30 s of the edge).
    // Relative-to-local-peak means soft words aren't dropped — only the soft
    // attack/decay (breath/room slack) below the fraction gets trimmed.
    const float kEdgeFrac = 0.40f;
    const int   kEdgeWin  = 30;             // ~0.30 s in 10 ms hops
    {
        const int look = std::min(bEnd, bStart + kEdgeWin);
        float pk = 0.0f;
        for (int k = bStart; k <= look; ++k) pk = std::max(pk, env[static_cast<size_t>(k)]);
        const float lvl = kEdgeFrac * pk;
        for (int k = bStart; k <= look; ++k)
            if (env[static_cast<size_t>(k)] >= lvl) { bStart = k; break; }
    }
    {
        const int look = std::max(bStart, bEnd - kEdgeWin);
        float pk = 0.0f;
        for (int k = look; k <= bEnd; ++k) pk = std::max(pk, env[static_cast<size_t>(k)]);
        const float lvl = kEdgeFrac * pk;
        for (int k = bEnd; k >= look; --k)
            if (env[static_cast<size_t>(k)] >= lvl) { bEnd = k; break; }
    }

    const int ss = std::max(s0, pos[static_cast<size_t>(bStart)]
                                - static_cast<int>(frontPaddingSec * sr));
    // Apply the padding selected for this Auto-Sync run. The incoming clip
    // window is already clamped to the source and adjacent aligned lines.
    const int se = std::min(s1, pos[static_cast<size_t>(bEnd)] + win
                                + static_cast<int>(endPaddingSec * sr));
    if (se > ss) {
        clipStart = static_cast<double>(ss) / sr;
        clipEnd = static_cast<double>(se) / sr;
        refined = true;
    }
}

bool AudioSync::alignClipsToScript()
{
    if (!m_script || m_script->lines.empty()) return false;

    const double frontPaddingSec = std::max(0.0, m_syncFrontPaddingSec);
    const double endPaddingSec   = std::max(0.0, m_syncEndPaddingSec);

    // Require word timestamps on at least one file (else nothing to align with).
    size_t totalWords = 0, validTimed = 0, emptyText = 0;
    std::string sample;
    for (const auto& res : m_allTranscriptionResults)
        for (const auto& seg : res.segments)
            for (const auto& wd : seg.words) {
                ++totalWords;
                if (wd.end > wd.start) ++validTimed;
                if (wd.word.empty())   ++emptyText;
                if (sample.empty() && !wd.word.empty())
                    sample = "'" + wd.word + "' [" + std::to_string(wd.start)
                           + "," + std::to_string(wd.end) + "]";
            }
    spdlog::warn("[ALIGN] {} result(s), {} words (validTimed={}, emptyText={}), sample={}, {} clips, {} paths",
                 m_allTranscriptionResults.size(), totalWords, validTimed, emptyText,
                 sample, m_clips.size(), m_audioPaths.size());
    if (validTimed == 0) {
        spdlog::warn("[ALIGN] no VALID word timestamps → cannot align, falling back");
        return false;
    }

    auto norm = [](const std::string& s) { return ScriptMatcher::normalize(s); };
    auto normChar = [](std::string name) -> std::string {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        while (!name.empty() && name.back()  == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        if (name == "marian") return "modernia";   // alias (matches runAutoSync)
        return name;
    };
    auto tokenize = [](const std::string& s) {
        std::vector<std::string> out;
        std::istringstream ss(s);
        std::string w;
        while (ss >> w) out.push_back(std::move(w));
        return out;
    };

    // Preserve confirmed clips; their lines are off-limits to re-alignment.
    std::vector<SyncClip> result;
    std::unordered_set<int> confirmedLines;
    for (const auto& c : m_clips) {
        if (c.matchState == 2) {
            result.push_back(c);
            if (c.scriptLineNumber >= 0) confirmedLines.insert(c.scriptLineNumber);
        }
    }
    const size_t confirmedCount = result.size();

    // Per-character script word stream (line-tagged), in script order.
    struct SWord { std::string w; int line; };
    std::unordered_map<std::string, std::vector<SWord>> scriptByChar;
    std::unordered_map<int, const ScriptLine*> lineByNum;
    std::unordered_map<int, int> lineWordCount;   // line → #script words (for coverage)
    for (const auto& line : m_script->lines) {
        lineByNum[line.lineNumber] = &line;
        auto& vec = scriptByChar[normChar(line.character)];
        auto toks = tokenize(norm(line.dialogue));
        lineWordCount[line.lineNumber] = static_cast<int>(toks.size());
        for (auto& w : toks)
            vec.push_back({ std::move(w), line.lineNumber });
    }

    {
        std::string chars;
        for (const auto& [k, v] : scriptByChar) chars += k + "(" + std::to_string(v.size()) + "w) ";
        spdlog::warn("[ALIGN] script characters: {}", chars);
    }

    std::unordered_set<std::string> alignedFiles;

    for (size_t fi = 0; fi < m_audioPaths.size(); ++fi) {
        if (fi >= m_allTranscriptionResults.size()) continue;
        const std::string& file = m_audioPaths[fi];

        // Flatten this file's timestamped words (normalized).
        struct AWord { std::string w; double start, end; };
        std::vector<AWord> W;
        for (const auto& seg : m_allTranscriptionResults[fi].segments)
            for (const auto& wd : seg.words) {
                std::string nw = norm(wd.word);
                if (!nw.empty() && wd.end > wd.start)
                    W.push_back({ std::move(nw), wd.start, wd.end });
            }
        if (W.empty()) continue;

        const std::string charName = extractCharacterName(file);
        auto sit = scriptByChar.find(normChar(charName));
        const bool charMatched = (sit != scriptByChar.end() && !sit->second.empty());
        spdlog::warn("[ALIGN] file#{} char='{}' (norm='{}') fileWords={} scriptMatch={}",
                     fi, charName, normChar(charName), W.size(), charMatched);
        if (!charMatched) continue;
        const std::vector<SWord>& S = sit->second;

        const int n = static_cast<int>(W.size());
        const int m = static_cast<int>(S.size());
        if (static_cast<long long>(n) * m > 6'000'000LL) continue;   // guard pathological sizes

        // Needleman-Wunsch (match +2 / mismatch -1 / gap -1).  Full traceback
        // byte matrix; two rolling score rows.  dir: 0=diag, 1=up (W-only word),
        // 2=left (script-only word).
        std::vector<uint8_t> tb(static_cast<size_t>(n + 1) * (m + 1), 0);
        std::vector<int> prev(m + 1), cur(m + 1);
        for (int j = 0; j <= m; ++j) { prev[j] = -j; tb[static_cast<size_t>(j)] = 2; }
        tb[0] = 0;
        for (int i = 1; i <= n; ++i) {
            cur[0] = -i;
            tb[static_cast<size_t>(i) * (m + 1)] = 1;
            for (int j = 1; j <= m; ++j) {
                const int ms = (W[i - 1].w == S[j - 1].w) ? 2 : -1;
                int best = prev[j - 1] + ms;  uint8_t dir = 0;
                const int up = prev[j]   - 1;   if (up > best) { best = up; dir = 1; }
                const int lf = cur[j - 1] - 1;  if (lf > best) { best = lf; dir = 2; }
                cur[j] = best;
                tb[static_cast<size_t>(i) * (m + 1) + j] = dir;
            }
            std::swap(prev, cur);
        }

        // Traceback → line per W word (-1 = unmatched).  Also record which words
        // were a TRUE match (equal text) vs a forced mismatch — used for the
        // per-clip confidence below.
        std::vector<int>  wordLine(n, -1);
        std::vector<char> wordHit(n, 0);     // aligned AND text-equal
        std::vector<char> wordAligned(n, 0); // aligned to a script word (match OR mismatch)
        for (int i = n, j = m; i > 0 && j > 0; ) {
            const uint8_t dir = tb[static_cast<size_t>(i) * (m + 1) + j];
            if (dir == 0) {
                wordLine[i - 1]    = S[j - 1].line;
                wordAligned[i - 1] = 1;
                wordHit[i - 1]     = (W[i - 1].w == S[j - 1].w) ? 1 : 0;
                --i; --j;
            }
            else if (dir == 1) --i;
            else --j;
        }
        // Mistranscribed/unmatched words inherit the nearest matched line.
        for (int i = 0, last = -1; i < n; ++i) {
            if (wordLine[i] >= 0) last = wordLine[i];
            else if (last >= 0)   wordLine[i] = last;
        }
        for (int i = n - 1, nxt = -1; i >= 0; --i) {
            if (wordLine[i] >= 0) nxt = wordLine[i];
            else if (nxt >= 0)    wordLine[i] = nxt;
        }
        if (wordLine[0] < 0) continue;   // nothing aligned for this file

        // Group consecutive words by line.  Collect ALL groups first (including
        // confirmed / low-confidence ones) so each clip can be clamped against
        // its NEIGHBOURS — the previous/next line's words in the same file.
        // Without the clamp, the loose word edges plus the generous refine
        // window let a clip's tail run into the next line whenever the pause
        // between lines was shorter than the window (out point too late), and
        // let its head swallow the previous line's tail (in point too early).
        struct Group { int ln, i, j, fa, la, hits; };
        std::vector<Group> groups;
        for (int i = 0; i < n; ) {
            const int ln = wordLine[i];
            if (ln < 0) { ++i; continue; }
            int j = i;
            while (j < n && wordLine[j] == ln) ++j;
            // fa/la = first/last word that truly ALIGNED to this line; the clip
            // boundary uses these, NOT the trailing/leading FILLER words
            // (breaths/junk) that merely inherited the line — those stretched
            // the clip end out into the dead space before the next line.
            int hits = 0, fa = -1, la = -1;
            for (int k = i; k < j; ++k) {
                hits += wordHit[k];
                if (wordAligned[k]) { if (fa < 0) fa = k; la = k; }
            }
            if (fa >= 0) groups.push_back({ ln, i, j, fa, la, hits });
            i = j;
        }

        // One clip per (line, file), bounded by the adjacent groups.
        bool madeClip = false;
        std::vector<int> fileLines;   // diag: lines this file produced clips on
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const Group& g = groups[gi];
            if (confirmedLines.count(g.ln)) continue;
            // Confidence = COVERAGE of the script line: how many of the
            // line's words this clip actually matched, over the line's word
            // count.  (NOT matched/clip-words — that wrongly rated a 1-word
            // fragment 1.00 and let it beat the real full-line take in dedup.)
            const int   lwc  = lineWordCount.count(g.ln) ? std::max(1, lineWordCount[g.ln]) : (g.j - g.i);
            const float conf = std::min(1.0f, static_cast<float>(g.hits) / static_cast<float>(std::max(1, lwc)));
            if (conf < 0.15f) continue;   // need to cover enough of the line

            fileLines.push_back(g.ln);
            const ScriptLine* sl = lineByNum.count(g.ln) ? lineByNum[g.ln] : nullptr;
            SyncClip clip;
            clip.sourceFile       = file;
            clip.character        = charName;
            // Bound to the aligned-word extent, but give the connected-body
            // refine a GENEROUS window each side so it can recover an onset
            // whisper timed late and find the true speech end.  The refine
            // locks onto the line's connected segment, so this extra room
            // doesn't add dead space (disconnected breath/bleed is dropped).
            clip.start            = std::max(
                0.0, W[g.fa].start - std::max(0.25, frontPaddingSec));
            clip.end              = W[g.la].end + std::max(0.20, endPaddingSec);
            // Neighbour clamp: the window may not cross into the adjacent
            // line's words.  Midpoint of the gap when the lines sit closer
            // than the margin allows; and the head clamp never moves past
            // this line's own first word.
            if (gi > 0) {
                const double prevEnd = W[groups[gi - 1].la].end;
                double lim = std::min(prevEnd + 0.02, 0.5 * (prevEnd + W[g.fa].start));
                lim = std::min(lim, W[g.fa].start);
                clip.start = std::max(clip.start, lim);
            }
            if (gi + 1 < groups.size()) {
                const double nextStart = W[groups[gi + 1].fa].start;
                const double lim = std::max(nextStart - 0.02, 0.5 * (W[g.la].end + nextStart));
                clip.end = std::min(clip.end, lim);
            }
            if (clip.end < clip.start + 0.05) clip.end = clip.start + 0.05;
            // Word span for the refine's overlap test, kept inside the window.
            const double wSpanS = std::max(W[g.fa].start, clip.start);
            const double wSpanE = std::max(wSpanS, std::min(W[g.la].end, clip.end));
            // Snap the (loose) word-edge boundaries to the actual speech.
            bool refined = false;
            auto as = m_audioSamples.find(file);
            if (as != m_audioSamples.end())
                refineSpeechBounds(as->second.samples,
                                   static_cast<int>(as->second.sampleRate),
                                   clip.start, clip.end, wSpanS, wSpanE,
                                   frontPaddingSec, endPaddingSec, refined);
            if (!refined) {
                // If energy analysis is unavailable or inconclusive, apply the
                // selected padding directly to the aligned word timestamps.
                clip.start = std::max(0.0, W[g.fa].start - frontPaddingSec);
                clip.end = W[g.la].end + endPaddingSec;
                if (gi > 0) {
                    const double prevEnd = W[groups[gi - 1].la].end;
                    double lim = std::min(prevEnd + 0.02,
                                          0.5 * (prevEnd + W[g.fa].start));
                    lim = std::min(lim, W[g.fa].start);
                    clip.start = std::max(clip.start, lim);
                }
                if (gi + 1 < groups.size()) {
                    const double nextStart = W[groups[gi + 1].fa].start;
                    const double lim = std::max(
                        nextStart - 0.02,
                        0.5 * (W[g.la].end + nextStart));
                    clip.end = std::min(clip.end, lim);
                }
                if (as != m_audioSamples.end() && as->second.sampleRate > 0) {
                    const double duration = static_cast<double>(as->second.samples.size())
                                          / as->second.sampleRate;
                    clip.end = std::min(clip.end, duration);
                }
                if (clip.end < clip.start + 0.05)
                    clip.end = clip.start + 0.05;
            }
            clip.transcript       = sl ? sl->dialogue : std::string();
            clip.editedText       = clip.transcript;
            clip.scriptLineNumber = g.ln;
            clip.scriptSegment    = sl ? (sl->character + ": " + sl->dialogue) : std::string();
            clip.matchState       = 1;      // tentative — user reviews/confirms
            clip.confidence       = conf;
            if (charName == "Trony" || charName == "Kilo")
                spdlog::warn("[ALIGN-CLIP] {} line={} nw={} aligned=[{},{}] span=[{:.2f},{:.2f}] "
                             "final=[{:.2f},{:.2f}] conf={:.2f} '{}'..'{}'",
                             charName, g.ln, g.j - g.i, g.la - g.fa + 1, g.hits, wSpanS, wSpanE,
                             clip.start, clip.end, conf, W[g.fa].w, W[g.la].w);
            result.push_back(std::move(clip));
            madeClip = true;
        }
        if (madeClip) {
            alignedFiles.insert(file);
            std::string ls;
            for (int l : fileLines) ls += std::to_string(l) + " ";
            spdlog::warn("[ALIGN] file#{} '{}' → {} clips on lines: {}",
                         fi, charName, fileLines.size(), ls);
        }
    }

    // Dedup: when the same (character, line) got clips from more than one file
    // (e.g. a main take AND a pickup/retake re-recording it), keep only the
    // highest-confidence one — the take whose words actually match that line.
    {
        std::unordered_map<std::string, size_t> bestIdx;   // char#line → index of best clip
        std::vector<char> drop(result.size(), 0);
        std::string dups;
        for (size_t k = 0; k < result.size(); ++k) {
            const auto& c = result[k];
            if (c.matchState != 1 || c.scriptLineNumber < 0) continue;
            const std::string key = normChar(c.character) + "#" + std::to_string(c.scriptLineNumber);
            auto it = bestIdx.find(key);
            if (it == bestIdx.end()) { bestIdx[key] = k; continue; }
            const size_t prev = it->second;
            dups += std::to_string(c.scriptLineNumber) + " ";
            if (c.confidence > result[prev].confidence) { drop[prev] = 1; bestIdx[key] = k; }
            else                                         { drop[k]    = 1; }
        }
        size_t dropped = 0;
        std::vector<SyncClip> kept;
        kept.reserve(result.size());
        for (size_t k = 0; k < result.size(); ++k) {
            if (drop[k]) { ++dropped; continue; }
            kept.push_back(std::move(result[k]));
        }
        result = std::move(kept);
        if (dropped)
            spdlog::warn("[ALIGN] dedup: dropped {} duplicate clip(s) on lines: {}", dropped, dups);
    }

    // Safety: keep ORIGINAL (non-confirmed) clips for any file we couldn't align
    // (e.g. its character isn't in the script) so no audio silently vanishes.
    for (const auto& c : m_clips) {
        if (c.matchState == 2) continue;                 // confirmed already kept
        if (!alignedFiles.count(c.sourceFile)) result.push_back(c);
    }

    if (alignedFiles.empty()) {
        spdlog::warn("[ALIGN] 0 files aligned → falling back to heuristic path");
        return false;   // aligned nothing → use the heuristic path
    }

    m_clips = std::move(result);
    for (size_t k = 0; k < m_clips.size(); ++k) m_clips[k].id = static_cast<int>(k);
    spdlog::warn("[ALIGN] USED — word-aligned across {} file(s) → {} clips",
                 alignedFiles.size(), m_clips.size());
    return true;
}

} // namespace rt
