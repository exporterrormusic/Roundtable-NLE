/*
 * AudioSyncPersistence.cpp - Split/merge and project state persistence for AudioSync.
 * Split from AudioSyncData.cpp for maintainability.
 *
 * Contains: splitClip, mergeClipWithNext, saveProjectState, restoreProjectState.
 */

#include "panels/audio/AudioSync.h"

#include <spdlog/spdlog.h>

#include <QFile>
#include <QSettings>

#include "Settings.h"

#include "ai/ScriptMatcher.h"     // For Script (complete type needed by ScriptSession dtor)
#include "project/ClipSerialization.h"

namespace rt {
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
//  Split & merge
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â

void AudioSync::splitClip(size_t clipIdx)
{
    if (clipIdx >= m_clips.size()) return;
    // Default: split at midpoint
    auto& clip = m_clips[clipIdx];
    double mid = (clip.start + clip.end) / 2.0;
    SyncClip newClip = clip;
    newClip.start = mid;
    newClip.id = static_cast<int>(m_clips.size());
    clip.end = mid;
    m_clips.insert(m_clips.begin() + static_cast<ptrdiff_t>(clipIdx) + 1, newClip);
    populateClipList();
}

void AudioSync::mergeClipWithNext(size_t clipIdx)
{
    if (clipIdx + 1 >= m_clips.size()) return;
    auto& clip = m_clips[clipIdx];
    const auto& next = m_clips[clipIdx + 1];
    clip.end = next.end;
    if (!next.editedText.empty()) {
        if (!clip.editedText.empty())
            clip.editedText += " ";
        clip.editedText += next.editedText;
    }
    if (!next.transcript.empty()) {
        if (!clip.transcript.empty())
            clip.transcript += " ";
        clip.transcript += next.transcript;
    }
    m_clips.erase(m_clips.begin() + static_cast<ptrdiff_t>(clipIdx) + 1);
    populateClipList();
}

// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
//  Project state persistence
/// Save a single SyncClip to QSettings under a given prefix+index.
static void saveClipToSettings(QSettings& settings, const QString& prefix, size_t clipIdx, const SyncClip& c)
{
    QString cp = prefix + "clip/" + QString::number(static_cast<int>(clipIdx)) + "/";
    settings.setValue(cp + "id",              c.id);
    settings.setValue(cp + "sourceFile",      QString::fromStdString(c.sourceFile));
    settings.setValue(cp + "character",       QString::fromStdString(c.character));
    settings.setValue(cp + "start",           c.start);
    settings.setValue(cp + "end",             c.end);
    settings.setValue(cp + "transcript",      QString::fromStdString(c.transcript));
    settings.setValue(cp + "editedText",      QString::fromStdString(c.editedText));
    settings.setValue(cp + "matchState",      c.matchState);
    settings.setValue(cp + "confidence",      static_cast<double>(c.confidence));
    settings.setValue(cp + "scriptLineNumber", c.scriptLineNumber);
    settings.setValue(cp + "scriptSegment",   QString::fromStdString(c.scriptSegment));

    // Deleted regions
    QVariantList regions;
    for (const auto& [rs, re] : c.deletedRegions) {
        QVariantList pair;
        pair.append(rs);
        pair.append(re);
        regions.append(QVariant(pair));
    }
    settings.setValue(cp + "deletedRegions", regions);
}

/// Restore a single SyncClip from QSettings given a prefix+index.
static SyncClip loadClipFromSettings(QSettings& settings, const QString& prefix, int clipIdx)
{
    QString cp = prefix + "clip/" + QString::number(clipIdx) + "/";
    SyncClip c;
    c.id              = settings.value(cp + "id", clipIdx).toInt();
    c.sourceFile      = settings.value(cp + "sourceFile").toString().toStdString();
    c.character       = settings.value(cp + "character").toString().toStdString();
    c.start           = settings.value(cp + "start", 0.0).toDouble();
    c.end             = settings.value(cp + "end", 0.0).toDouble();
    c.transcript      = settings.value(cp + "transcript").toString().toStdString();
    c.editedText      = settings.value(cp + "editedText").toString().toStdString();
    c.matchState      = settings.value(cp + "matchState", 0).toInt();
    c.confidence      = static_cast<float>(settings.value(cp + "confidence", 0.0).toDouble());
    c.scriptLineNumber = settings.value(cp + "scriptLineNumber", -1).toInt();
    c.scriptSegment   = settings.value(cp + "scriptSegment").toString().toStdString();

    QVariantList regions = settings.value(cp + "deletedRegions").toList();
    for (const auto& r : regions) {
        QVariantList pair = r.toList();
        if (pair.size() >= 2)
            c.deletedRegions.emplace_back(pair[0].toDouble(), pair[1].toDouble());
    }
    return c;
}

void AudioSync::saveProjectState(const QString& projectName)
{
    if (projectName.isEmpty()) return;

    auto settings = rt::appSettings();
    QString prefix = "Project/" + projectName + "/AudioSync/";

    // Save active session key
    settings.setValue(prefix + "activeSessionKey", QString::fromStdString(m_activeScriptKey));

    // Save number of sessions
    settings.setValue(prefix + "sessionCount", static_cast<int>(m_scriptSessions.size()));

    int sessionIdx = 0;
    for (const auto& [key, session] : m_scriptSessions) {
        QString sp = prefix + "session/" + QString::number(sessionIdx) + "/";

        settings.setValue(sp + "key",         QString::fromStdString(key));
        settings.setValue(sp + "displayName", QString::fromStdString(session.displayName));
        settings.setValue(sp + "sourceUrl",   QString::fromStdString(session.sourceUrl));
        // For the active session, use m_scriptRawContent (may have been
        // updated by sync without session.rawContent being refreshed).
        const std::string& rawForSave = (key == m_activeScriptKey)
            ? m_scriptRawContent : session.rawContent;
        settings.setValue(sp + "rawContent",  QString::fromStdString(rawForSave));

        if (key == m_activeScriptKey) {
            // Active session — data lives in member variables
            QStringList audioPaths;
            for (const auto& p : m_audioPaths)
                audioPaths.append(QString::fromStdString(p));
            settings.setValue(sp + "audioPaths", audioPaths);

            settings.setValue(sp + "audioPath", QString::fromStdString(m_audioPath));
            settings.setValue(sp + "scriptLoaded",      m_scriptLoaded);
            settings.setValue(sp + "audioImported",      m_audioImported);
            settings.setValue(sp + "transcriptionDone",  m_transcriptionDone);
            settings.setValue(sp + "syncDone",           m_syncDone);

            // Save active session clips
            settings.setValue(sp + "clipCount", static_cast<int>(m_clips.size()));
            for (size_t i = 0; i < m_clips.size(); ++i) {
                saveClipToSettings(settings, sp, i, m_clips[i]);
            }

            // lineAudioFile
            settings.setValue(sp + "lineAudioFileCount", static_cast<int>(m_lineAudioFile.size()));
            int lafIdx = 0;
            for (const auto& [lineNum, audioFile] : m_lineAudioFile) {
                settings.setValue(sp + "laf/" + QString::number(lafIdx) + "/line", lineNum);
                settings.setValue(sp + "laf/" + QString::number(lafIdx) + "/file",
                                  QString::fromStdString(audioFile));
                ++lafIdx;
            }
        } else {
            // Stored session — data lives in the session object
            QStringList audioPaths;
            for (const auto& p : session.audioPaths)
                audioPaths.append(QString::fromStdString(p));
            settings.setValue(sp + "audioPaths", audioPaths);

            settings.setValue(sp + "audioPath", QString::fromStdString(session.audioPath));
            settings.setValue(sp + "scriptLoaded",      session.scriptLoaded);
            settings.setValue(sp + "audioImported",      session.audioImported);
            settings.setValue(sp + "transcriptionDone",  session.transcriptionDone);
            settings.setValue(sp + "syncDone",           session.syncDone);

            // Save stored session clips
            settings.setValue(sp + "clipCount", static_cast<int>(session.clips.size()));
            for (size_t i = 0; i < session.clips.size(); ++i) {
                saveClipToSettings(settings, sp, i, session.clips[i]);
            }

            // lineAudioFile
            settings.setValue(sp + "lineAudioFileCount", static_cast<int>(session.lineAudioFile.size()));
            int lafIdx = 0;
            for (const auto& [lineNum, audioFile] : session.lineAudioFile) {
                settings.setValue(sp + "laf/" + QString::number(lafIdx) + "/line", lineNum);
                settings.setValue(sp + "laf/" + QString::number(lafIdx) + "/file",
                                  QString::fromStdString(audioFile));
                ++lafIdx;
            }
        }

        ++sessionIdx;
    }

    spdlog::info("AudioSync::saveProjectState: saved {} sessions (active='{}')",
                 m_scriptSessions.size(), m_activeScriptKey);
}

void AudioSync::restoreProjectState(const QString& projectName)
{
    if (projectName.isEmpty()) return;

    m_restoring = true;  // Suppress redundant UI rebuilds inside loadScript

    auto settings = rt::appSettings();
    QString prefix = "Project/" + projectName + "/AudioSync/";

    // ── Check for multi-session format ──────────────────────────────────
    int sessionCount = settings.value(prefix + "sessionCount", 0).toInt();

    if (sessionCount > 0) {
        // Multi-session format — restore all sessions
        m_scriptSessions.clear();
        clearCurrentSession();

        QString activeKey = settings.value(prefix + "activeSessionKey").toString();

        for (int si = 0; si < sessionCount; ++si) {
            QString sp = prefix + "session/" + QString::number(si) + "/";

            QString key         = settings.value(sp + "key").toString();
            QString dispName    = settings.value(sp + "displayName").toString();
            QString sourceUrl   = settings.value(sp + "sourceUrl").toString();
            QString rawContent  = settings.value(sp + "rawContent").toString();

            if (key.isEmpty()) continue;

            ScriptSession session;
            session.displayName = dispName.toStdString();
            session.sourceUrl   = sourceUrl.toStdString();
            session.rawContent  = rawContent.toStdString();

            // Parse the script from raw content so the Script object is available
            if (!rawContent.isEmpty()) {
                try {
                    auto parsed = Script::load(rawContent.toStdString());
                    if (!parsed.isEmpty())
                        session.script = std::make_unique<Script>(std::move(parsed));
                } catch (const std::exception& e) {
                    spdlog::warn("  session '{}': failed to parse script: {}", key.toStdString(), e.what());
                }
            }

            // Restore audio paths
            QStringList audioPaths = settings.value(sp + "audioPaths").toStringList();
            for (const auto& ap : audioPaths) {
                if (!ap.isEmpty())
                    session.audioPaths.push_back(ap.toStdString());
            }
            session.audioPath = settings.value(sp + "audioPath").toString().toStdString();
            session.scriptLoaded      = settings.value(sp + "scriptLoaded", false).toBool();
            session.audioImported     = settings.value(sp + "audioImported", false).toBool();
            session.transcriptionDone = settings.value(sp + "transcriptionDone", false).toBool();
            session.syncDone          = settings.value(sp + "syncDone", false).toBool();

            // Restore clips
            int clipCount = settings.value(sp + "clipCount", 0).toInt();
            for (int ci = 0; ci < clipCount; ++ci)
                session.clips.push_back(loadClipFromSettings(settings, sp, ci));

            // Restore lineAudioFile
            int lafCount = settings.value(sp + "lineAudioFileCount", 0).toInt();
            for (int li = 0; li < lafCount; ++li) {
                int lineNum = settings.value(sp + "laf/" + QString::number(li) + "/line", -1).toInt();
                QString audioFile = settings.value(sp + "laf/" + QString::number(li) + "/file").toString();
                if (lineNum >= 0 && !audioFile.isEmpty())
                    session.lineAudioFile[lineNum] = audioFile.toStdString();
            }

            // Only keep if it has a source URL or raw content
            if (!session.sourceUrl.empty() || !session.rawContent.empty())
                m_scriptSessions[key.toStdString()] = std::move(session);
        }

        spdlog::info("AudioSync::restoreProjectState: restored {} sessions, active='{}'",
                     m_scriptSessions.size(), activeKey.toStdString());

        // Restore the active session
        std::string targetKey = activeKey.toStdString();
        if (!m_scriptSessions.count(targetKey)) {
            // Fall back to first available session
            if (!m_scriptSessions.empty())
                targetKey = m_scriptSessions.begin()->first;
            else
                targetKey.clear();
        }

        if (!targetKey.empty()) {
            restoreSession(targetKey);

            // Load audio samples FIRST so waveforms appear in the list items
            if (!m_audioPaths.empty())
                loadAudioSamples();

            // Build audio file list UI
            if (m_audioFileList) {
                m_audioFileList->blockSignals(true);
                m_audioFileList->clear();
                for (const auto& ap : m_audioPaths) {
                    if (QFile::exists(QString::fromStdString(ap)))
                        addAudioFileListItem(QString::fromStdString(ap));
                }
                m_audioFileList->blockSignals(false);
            }
            if (m_audioStatus)
                m_audioStatus->setText(m_audioPaths.empty() ? "No files imported"
                    : QString("%1 file(s)").arg(m_audioPaths.size()));
        }

        // Rebuild transcription results for each session
        // Build a sourceFile→transcript map for O(clips + audioFiles) instead of O(clips × audioFiles)
        for (auto& [key, session] : m_scriptSessions) {
            if (session.transcriptionDone && !session.audioPaths.empty()) {
                std::unordered_map<std::string, std::string> fileToTranscript;
                fileToTranscript.reserve(session.audioPaths.size());
                for (const auto& c : session.clips) {
                    if (!c.transcript.empty()) {
                        auto& t = fileToTranscript[c.sourceFile];
                        if (!t.empty()) t += ' ';
                        t += c.transcript;
                    }
                }
                session.allTranscriptionResults.resize(session.audioPaths.size());
                for (size_t fi = 0; fi < session.audioPaths.size(); ++fi) {
                    auto it = fileToTranscript.find(session.audioPaths[fi]);
                    if (it != fileToTranscript.end() && !it->second.empty()) {
                        TranscriptionSegment seg;
                        seg.text = it->second;
                        session.allTranscriptionResults[fi].segments.push_back(std::move(seg));
                    }
                }
            }
        }

        populateScriptSessionList();
        m_restoring = false;
        updateWorkflowState();
        if (m_script) {
            // Rebuild the Match-tab character filter list; restoreSession()
            // does not do this, so without it the Match tab shows no characters.
            populateScriptFilter();
            if (isVisible()) {
                populateCards();      // <-- also calls populateLeftList() internally
            } else {
                populateLeftList();   // <-- need explicit left-list when cards deferred
                m_cardsDirty = true;
            }
        } else {
            populateLeftList();
        }
        if (!m_clips.empty() && m_syncDone)
            showAudioSidePanel(3);

        return;
    }

    // ═══════════════════════════════════════════════════════════════════
    //  Legacy single-session format (backward compat)
    // ═══════════════════════════════════════════════════════════════════

    // Restore script source
    QString scriptSource = settings.value(prefix + "scriptSource").toString();
    if (!scriptSource.isEmpty()) {
        m_lastScriptSource = scriptSource;

        // Restore display name so it persists across sessions
        QString savedName = settings.value(prefix + "scriptDisplayName").toString();
        if (!savedName.isEmpty())
            m_pendingSessionName = savedName.toStdString();

        // Restore raw script content to avoid auto-fetching from URL on project open.
        QString rawContent = settings.value(prefix + "scriptRawContent").toString();
        m_scriptRawContent = rawContent.toStdString();

        if (!m_scriptRawContent.empty()) {
            // Restore from saved content instead of re-fetching
            loadScript(m_scriptRawContent, scriptSource.toStdString());
        } else if (scriptSource.startsWith("http://") || scriptSource.startsWith("https://")) {
            fetchScriptFromUrl(scriptSource);
        } else {
            loadScript(scriptSource.toStdString());
        }
    }

    // Restore audio paths — batch import
    m_audioPaths.clear();
    if (m_audioFileList) m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    QStringList audioPaths = settings.value(prefix + "audioPaths").toStringList();
    for (const auto& path : audioPaths) {
        if (!path.isEmpty() && QFile::exists(path))
            m_audioPaths.push_back(path.toStdString());
    }
    if (!m_audioPaths.empty()) {
        m_audioImported = true;
        m_audioPath = m_audioPaths.back();
        if (m_audioPathEdit) m_audioPathEdit->setText(QString::fromStdString(m_audioPath));
        m_audioStatus->setText(QString("%1 file(s) imported").arg(m_audioPaths.size()));
        loadAudioSamples();
        if (m_audioFileList) {
            for (const auto& p : m_audioPaths)
                addAudioFileListItem(QString::fromStdString(p));
        }
    }

    // Restore clips
    int clipCount = settings.value(prefix + "clipCount", 0).toInt();
    if (clipCount > 0) {
        m_clips.clear();
        m_clips.reserve(static_cast<size_t>(clipCount));
        for (int i = 0; i < clipCount; ++i)
            m_clips.push_back(loadClipFromSettings(settings, prefix, i));
        spdlog::info("AudioSync: Restored {} clips from legacy project state", m_clips.size());
    }

    // Restore workflow state flags
    m_scriptLoaded      = settings.value(prefix + "scriptLoaded",     m_scriptLoaded).toBool();
    m_audioImported     = settings.value(prefix + "audioImported",    m_audioImported).toBool();
    m_transcriptionDone = settings.value(prefix + "transcriptionDone", m_transcriptionDone).toBool();
    m_syncDone          = settings.value(prefix + "syncDone",         m_syncDone).toBool();

    // Rebuild transcription results from clips
    // Build a sourceFile→transcript map for O(clips + audioFiles) instead of O(clips × audioFiles)
    if (m_transcriptionDone && !m_audioPaths.empty()) {
        std::unordered_map<std::string, std::string> fileToTranscript;
        fileToTranscript.reserve(m_audioPaths.size());
        for (const auto& c : m_clips) {
            if (!c.transcript.empty()) {
                auto& t = fileToTranscript[c.sourceFile];
                if (!t.empty()) t += ' ';
                t += c.transcript;
            }
        }
        m_allTranscriptionResults.resize(m_audioPaths.size());
        for (size_t i = 0; i < m_audioPaths.size(); ++i) {
            if (!m_allTranscriptionResults[i].segments.empty())
                continue;
            auto it = fileToTranscript.find(m_audioPaths[i]);
            if (it != fileToTranscript.end() && !it->second.empty()) {
                TranscriptionSegment seg;
                seg.text = it->second;
                m_allTranscriptionResults[i].segments.push_back(std::move(seg));
            }
        }
    }

    // Persist restored data into the current session
    saveCurrentSession();
    m_restoring = false;

    updateWorkflowState();
    if (m_script) {
        // Rebuild the Match-tab character filter list (see restoreProjectState
        // multi-session path above for why this is required).
        populateScriptFilter();
        if (isVisible()) {
            populateCards();      // <-- also calls populateLeftList() internally
        } else {
            populateLeftList();   // <-- need explicit left-list when cards deferred
            m_cardsDirty = true;
        }
    } else {
        populateLeftList();
    }
    if (!m_clips.empty() && m_syncDone)
        showAudioSidePanel(3);
}

void AudioSync::restoreAudioPaths(const QString& projectName)
{
    if (projectName.isEmpty() || !m_audioPaths.empty()) return;

    auto settings = rt::appSettings();
    QString prefix = "Project/" + projectName + "/AudioSync/";

    QStringList audioPaths = settings.value(prefix + "audioPaths").toStringList();
    if (audioPaths.isEmpty()) return;

    m_audioPaths.clear();
    if (m_audioFileList) m_audioFileList->clear();
    for (const auto& path : audioPaths) {
        if (!path.isEmpty() && QFile::exists(path))
            m_audioPaths.push_back(path.toStdString());
    }
    if (!m_audioPaths.empty()) {
        m_audioImported = true;
        m_audioPath = m_audioPaths.back();
        if (m_audioPathEdit) m_audioPathEdit->setText(QString::fromStdString(m_audioPath));
        m_audioStatus->setText(QString("%1 file(s) imported").arg(m_audioPaths.size()));
        loadAudioSamples();
        if (m_audioFileList) {
            for (const auto& p : m_audioPaths)
                addAudioFileListItem(QString::fromStdString(p));
        }
        spdlog::info("AudioSync::restoreAudioPaths: supplemented {} paths from QSettings",
                     m_audioPaths.size());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Binary blob serialization (for .rtp project file)
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uint32_t AUDIO_SYNC_BLOB_VERSION = 4;

/// Write a single session's clip/audio data into the writer.
/// Used by serializeToBlob for each session (active or stored).
static void writeSessionData(BinaryWriter& w,
                             const std::vector<SyncClip>& clips,
                             const std::vector<std::string>& audioPaths,
                             const std::string& audioPath,
                             bool scriptLoaded, bool audioImported,
                             bool transcriptionDone, bool syncDone,
                             const std::unordered_map<int, std::string>& lineAudioFile)
{
    // Audio paths
    w.writeU32(static_cast<uint32_t>(audioPaths.size()));
    for (const auto& p : audioPaths)
        w.writeString(p);

    // Workflow flags
    w.writeU8(scriptLoaded      ? 1 : 0);
    w.writeU8(audioImported     ? 1 : 0);
    w.writeU8(transcriptionDone ? 1 : 0);
    w.writeU8(syncDone          ? 1 : 0);

    // Currently-transcribing audio path
    w.writeString(audioPath);

    // Clips
    w.writeU32(static_cast<uint32_t>(clips.size()));
    for (const auto& c : clips) {
        w.writeU32(static_cast<uint32_t>(c.id));
        w.writeString(c.sourceFile);
        w.writeString(c.character);
        w.writeF64(c.start);
        w.writeF64(c.end);
        w.writeString(c.transcript);
        w.writeString(c.editedText);
        w.writeU32(static_cast<uint32_t>(c.matchState));
        w.writeF32(c.confidence);
        w.writeU32(static_cast<uint32_t>(c.scriptLineNumber));
        w.writeString(c.scriptSegment);

        // Deleted regions
        w.writeU32(static_cast<uint32_t>(c.deletedRegions.size()));
        for (const auto& [rs, re] : c.deletedRegions) {
            w.writeF64(rs);
            w.writeF64(re);
        }
    }

    // lineAudioFile per-line mapping
    w.writeU32(static_cast<uint32_t>(lineAudioFile.size()));
    for (const auto& [lineNum, audioFile] : lineAudioFile) {
        w.writeI64(static_cast<int64_t>(lineNum));
        w.writeString(audioFile);
    }
}

std::vector<uint8_t> AudioSync::serializeToBlob() const
{
    BinaryWriter w;
    w.writeU32(AUDIO_SYNC_BLOB_VERSION);  // v4

    //── Write all sessions ──────────────────────────────────────────────
    // The active session's data is in member variables (m_clips, etc.),
    // while stored sessions have their data in m_scriptSessions entries.
    //
    // We iterate m_scriptSessions: for the active key we use member data,
    // for other keys we use the session's stored data.

    w.writeU32(static_cast<uint32_t>(m_scriptSessions.size()));
    w.writeString(m_activeScriptKey);

    for (const auto& [key, session] : m_scriptSessions) {
        // Session identity
        w.writeString(key);                // session key (source URL / identifier)
        w.writeString(session.displayName);
        w.writeString(session.sourceUrl);
        // For the active session, use m_scriptRawContent (may have been
        // updated by sync without session.rawContent being refreshed).
        w.writeString(key == m_activeScriptKey ? m_scriptRawContent : session.rawContent);

        if (key == m_activeScriptKey) {
            // Active session — data lives in member variables
            writeSessionData(w, m_clips, m_audioPaths, m_audioPath,
                             m_scriptLoaded, m_audioImported,
                             m_transcriptionDone, m_syncDone,
                             m_lineAudioFile);
        } else {
            // Stored session — data lives in the session object
            writeSessionData(w, session.clips, session.audioPaths, session.audioPath,
                             session.scriptLoaded, session.audioImported,
                             session.transcriptionDone, session.syncDone,
                             session.lineAudioFile);
        }
    }

    spdlog::info("AudioSync::serializeToBlob (v4): {} sessions, {} bytes",
                 m_scriptSessions.size(), w.data().size());
    return w.data();
}

/// Read a single session's clip/audio data from the reader (v4 per-session layout).
static void readSessionData(BinaryReader& r,
                            std::vector<SyncClip>& outClips,
                            std::vector<std::string>& outAudioPaths,
                            std::string& outAudioPath,
                            bool& outScriptLoaded, bool& outAudioImported,
                            bool& outTranscriptionDone, bool& outSyncDone,
                            std::unordered_map<int, std::string>& outLineAudioFile)
{
    // Audio paths
    uint32_t pathCount = r.readU32();
    outAudioPaths.clear();
    outAudioPaths.reserve(pathCount);
    for (uint32_t i = 0; i < pathCount; ++i) {
        std::string p = r.readString();
        if (!p.empty())
            outAudioPaths.push_back(std::move(p));
    }

    // Workflow flags
    outScriptLoaded      = (r.readU8() != 0);
    outAudioImported     = (r.readU8() != 0);
    outTranscriptionDone = (r.readU8() != 0);
    outSyncDone          = (r.readU8() != 0);

    // Currently-transcribing audio path
    outAudioPath = r.readString();

    // Clips
    uint32_t clipCount = r.readU32();
    outClips.clear();
    outClips.reserve(clipCount);
    for (uint32_t i = 0; i < clipCount; ++i) {
        SyncClip c;
        c.id              = static_cast<int>(r.readU32());
        c.sourceFile      = r.readString();
        c.character       = r.readString();
        c.start           = r.readF64();
        c.end             = r.readF64();
        c.transcript      = r.readString();
        c.editedText      = r.readString();
        c.matchState      = static_cast<int>(r.readU32());
        c.confidence      = r.readF32();
        c.scriptLineNumber = static_cast<int>(r.readU32());
        c.scriptSegment   = r.readString();

        uint32_t delCount = r.readU32();
        c.deletedRegions.reserve(delCount);
        for (uint32_t d = 0; d < delCount; ++d) {
            double rs = r.readF64();
            double re = r.readF64();
            c.deletedRegions.emplace_back(rs, re);
        }
        outClips.push_back(std::move(c));
    }

    // lineAudioFile per-line mapping
    uint32_t lafCount = r.readU32();
    outLineAudioFile.clear();
    for (uint32_t i = 0; i < lafCount; ++i) {
        int64_t lineNum = r.readI64();
        std::string audioFile = r.readString();
        outLineAudioFile[static_cast<int>(lineNum)] = std::move(audioFile);
    }
}

void AudioSync::deserializeFromBlob(const std::vector<uint8_t>& blob)
{
    if (blob.empty()) {
        spdlog::warn("AudioSync::deserializeFromBlob: blob is EMPTY");
        return;
    }
    spdlog::info("AudioSync::deserializeFromBlob: blob size={}", blob.size());

    BinaryReader r(blob.data(), blob.size());

    uint32_t version = r.readU32();
    if (version > AUDIO_SYNC_BLOB_VERSION) {
        spdlog::warn("AudioSync::deserializeFromBlob: version {} > supported {}",
                     version, AUDIO_SYNC_BLOB_VERSION);
        return;
    }

    m_restoring = true;

    // ═══════════════════════════════════════════════════════════════════
    //  v4+ Multi-session format
    // ═══════════════════════════════════════════════════════════════════
    if (version >= 4) {
        uint32_t sessionCount = r.readU32();
        std::string activeKey = r.readString();

        spdlog::info("AudioSync::deserializeFromBlob (v4): {} sessions, active='{}'",
                     sessionCount, activeKey);

        // Clear existing sessions before restoring
        m_scriptSessions.clear();
        clearCurrentSession();

        std::string firstActiveKey;  // track first valid session to activate

        for (uint32_t si = 0; si < sessionCount; ++si) {
            std::string key       = r.readString();
            std::string dispName  = r.readString();
            std::string sourceUrl = r.readString();
            std::string rawContent = r.readString();

            ScriptSession session;
            session.displayName = dispName;
            session.sourceUrl   = sourceUrl;
            session.rawContent  = rawContent;

            readSessionData(r, session.clips, session.audioPaths, session.audioPath,
                            session.scriptLoaded, session.audioImported,
                            session.transcriptionDone, session.syncDone,
                            session.lineAudioFile);

            // Parse the script from raw content so the Script object is available
            if (!rawContent.empty()) {
                try {
                    auto parsed = Script::load(rawContent);
                    if (!parsed.isEmpty()) {
                        session.script = std::make_unique<Script>(std::move(parsed));
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("  session[{}]: failed to parse script: {}", si, e.what());
                }
            }

            // Only keep sessions where the source file still exists (for local files)
            // or that have a URL or raw content (can be re-fetched/re-parsed).
            bool keep = true;
            if (!sourceUrl.empty() && !rawContent.empty()) {
                keep = true;  // have raw content, can restore offline
            } else if (sourceUrl.empty()) {
                keep = false; // no source at all
            }

            if (keep) {
                m_scriptSessions[key] = std::move(session);
                spdlog::info("  session[{}]: '{}' ({} clips, {} audio files, script={})",
                             si, dispName,
                             m_scriptSessions[key].clips.size(),
                             m_scriptSessions[key].audioPaths.size(),
                             m_scriptSessions[key].script ? "loaded" : "null");

                // Remember the first valid key to activate if activeKey doesn't match
                if (firstActiveKey.empty())
                    firstActiveKey = key;
            } else {
                spdlog::warn("  session[{}]: SKIPPED (no source URL or raw content)", si);
            }
        }

        // Restore the active session
        std::string targetKey;
        if (m_scriptSessions.count(activeKey))
            targetKey = activeKey;
        else if (!firstActiveKey.empty())
            targetKey = firstActiveKey;

        if (!targetKey.empty()) {
            restoreSession(targetKey);

            // Load audio samples FIRST so waveforms appear in the list items
            if (!m_audioPaths.empty()) {
                loadAudioSamples();
                m_audioImported = true;
            }

            // Build audio file list UI for restored paths
            if (m_audioFileList) {
                m_audioFileList->blockSignals(true);
                m_audioFileList->clear();
                for (const auto& ap : m_audioPaths) {
                    if (QFile::exists(QString::fromStdString(ap)))
                        addAudioFileListItem(QString::fromStdString(ap));
                }
                m_audioFileList->blockSignals(false);
            }
            if (m_audioStatus)
                m_audioStatus->setText(m_audioPaths.empty() ? "No files imported"
                    : QString("%1 file(s)").arg(m_audioPaths.size()));
        }

        // Rebuild transcription results from clips for each session
        // Build a sourceFile→transcript map for O(clips + audioFiles) instead of O(clips × audioFiles)
        for (auto& [key, session] : m_scriptSessions) {
            if (session.transcriptionDone && !session.audioPaths.empty()) {
                std::unordered_map<std::string, std::string> fileToTranscript;
                fileToTranscript.reserve(session.audioPaths.size());
                for (const auto& c : session.clips) {
                    if (!c.transcript.empty()) {
                        auto& t = fileToTranscript[c.sourceFile];
                        if (!t.empty()) t += ' ';
                        t += c.transcript;
                    }
                }
                session.allTranscriptionResults.resize(session.audioPaths.size());
                for (size_t fi = 0; fi < session.audioPaths.size(); ++fi) {
                    auto it = fileToTranscript.find(session.audioPaths[fi]);
                    if (it != fileToTranscript.end() && !it->second.empty()) {
                        TranscriptionSegment seg;
                        seg.text = it->second;
                        session.allTranscriptionResults[fi].segments.push_back(std::move(seg));
                    }
                }
            }
        }

        m_restoring = false;

        spdlog::info("AudioSync::deserializeFromBlob (v4): restored {} sessions, active='{}'",
                     m_scriptSessions.size(), m_activeScriptKey);

        // Refresh UI — populateCards() calls populateLeftList() internally
        populateScriptSessionList();
        updateWorkflowState();
        if (m_script) {
            // Rebuild the Match-tab character filter list; restoreSession()
            // does not, so the Match tab would otherwise show no characters.
            populateScriptFilter();
            if (isVisible())
                populateCards();
            else
                m_cardsDirty = true;
        }
        if (!m_clips.empty() && m_syncDone) showAudioSidePanel(3);

        return;
    }

    // ═══════════════════════════════════════════════════════════════════
    //  Legacy v1-v3 single-session format (backward compat)
    // ═══════════════════════════════════════════════════════════════════

    // Clear sessions so the restored single session is the only one
    m_scriptSessions.clear();
    clearCurrentSession();

    // Script source
    std::string scriptSource = r.readString();

    // Script display name (v2+)
    if (version >= 2 && r.remaining() > 0) {
        std::string savedName = r.readString();
        if (!savedName.empty())
            m_pendingSessionName = savedName;
    }

    // Script raw content (v3+)
    if (version >= 3 && r.remaining() > 0) {
        m_scriptRawContent = r.readString();
    }

    if (!scriptSource.empty()) {
        m_lastScriptSource = QString::fromStdString(scriptSource);
        if (!m_scriptRawContent.empty()) {
            loadScript(m_scriptRawContent, scriptSource);
        } else if (m_lastScriptSource.startsWith("http://") || m_lastScriptSource.startsWith("https://")) {
            fetchScriptFromUrl(m_lastScriptSource);
        } else {
            loadScript(scriptSource);
        }
    }

    // Audio paths
    uint32_t pathCount = r.readU32();
    spdlog::info("AudioSync::deserializeFromBlob (v{}): reading {} audio paths",
                 version, pathCount);
    m_audioPaths.clear();
    if (m_audioFileList) m_audioFileList->clear();
    m_fileWaveforms.clear();
    m_filePlayBtns.clear();
    m_fileTimeLabels.clear();
    for (uint32_t i = 0; i < pathCount; ++i) {
        std::string p = r.readString();
        bool exists = QFile::exists(QString::fromStdString(p));
        spdlog::info("  audio[{}]: exists={} path={}", i, exists, p);
        if (!p.empty() && exists)
            m_audioPaths.push_back(std::move(p));
    }
    spdlog::info("AudioSync::deserializeFromBlob: kept {} audio paths", m_audioPaths.size());
    if (!m_audioPaths.empty()) {
        m_audioImported = true;
        m_audioPath = m_audioPaths.back();
        if (m_audioPathEdit)
            m_audioPathEdit->setText(QString::fromStdString(m_audioPath));
        m_audioStatus->setText(QString("%1 file(s) imported").arg(m_audioPaths.size()));
        loadAudioSamples();
        if (m_audioFileList) {
            for (const auto& ap : m_audioPaths)
                addAudioFileListItem(QString::fromStdString(ap));
        }
    }

    // Workflow flags
    m_scriptLoaded      = (r.readU8() != 0);
    m_audioImported     = (r.readU8() != 0);
    m_transcriptionDone = (r.readU8() != 0);
    m_syncDone          = (r.readU8() != 0);

    // Clips
    uint32_t clipCount = r.readU32();
    m_clips.clear();
    m_clips.reserve(clipCount);
    for (uint32_t i = 0; i < clipCount; ++i) {
        SyncClip c;
        c.id              = static_cast<int>(r.readU32());
        c.sourceFile      = r.readString();
        c.character       = r.readString();
        c.start           = r.readF64();
        c.end             = r.readF64();
        c.transcript      = r.readString();
        c.editedText      = r.readString();
        c.matchState      = static_cast<int>(r.readU32());
        c.confidence      = r.readF32();
        c.scriptLineNumber = static_cast<int>(r.readU32());
        c.scriptSegment   = r.readString();

        uint32_t delCount = r.readU32();
        c.deletedRegions.reserve(delCount);
        for (uint32_t d = 0; d < delCount; ++d) {
            double rs = r.readF64();
            double re = r.readF64();
            c.deletedRegions.emplace_back(rs, re);
        }
        m_clips.push_back(std::move(c));
    }

    // Rebuild transcription results from clips
    // Build a sourceFile→transcript map for O(clips + audioFiles) instead of O(clips × audioFiles)
    if (m_transcriptionDone && !m_audioPaths.empty()) {
        std::unordered_map<std::string, std::string> fileToTranscript;
        fileToTranscript.reserve(m_audioPaths.size());
        for (const auto& c : m_clips) {
            if (!c.transcript.empty()) {
                auto& t = fileToTranscript[c.sourceFile];
                if (!t.empty()) t += ' ';
                t += c.transcript;
            }
        }
        m_allTranscriptionResults.resize(m_audioPaths.size());
        for (size_t fi = 0; fi < m_audioPaths.size(); ++fi) {
            if (!m_allTranscriptionResults[fi].segments.empty())
                continue;
            auto it = fileToTranscript.find(m_audioPaths[fi]);
            if (it != fileToTranscript.end() && !it->second.empty()) {
                TranscriptionSegment seg;
                seg.text = it->second;
                m_allTranscriptionResults[fi].segments.push_back(std::move(seg));
            }
        }
    }

    // Save the restored single session into the sessions map so that
    // subsequent session-switching works correctly.
    saveCurrentSession();

    m_restoring = false;

    spdlog::info("AudioSync::deserializeFromBlob (v{}): restored {} clips, {} audio paths",
                 version, m_clips.size(), m_audioPaths.size());

    // Refresh session list so restored display names appear
    populateScriptSessionList();
    updateWorkflowState();
    if (m_script) {
        // Rebuild the Match-tab character filter list (see v4 path above).
        populateScriptFilter();
        if (isVisible())
            populateCards();
        else
            m_cardsDirty = true;
    }
    if (!m_clips.empty() && m_syncDone) showAudioSidePanel(3);
}

} // namespace rt

