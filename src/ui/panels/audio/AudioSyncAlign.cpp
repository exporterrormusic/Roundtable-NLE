/*
 * AudioSyncAlign.cpp -- the panel side of word-level alignment; the algorithm
 * is audiosync::alignClipsToScript / alignShotTakes (core/audiosync).
 */
#include "panels/audio/AudioSync.h"
#include "ai/ScriptMatcher.h"
#include "audiosync/AutoSync.h"

namespace rt {

bool AudioSync::alignClipsToScript()
{
    const bool aligned = audiosync::alignClipsToScript(
        syncSource(), session().clips, m_syncFrontPaddingSec, m_syncEndPaddingSec);
    if (aligned && session().script && session().script->isShotScript())
        m_displayedTakeId.clear();   // take ids were reassigned
    return aligned;
}

bool AudioSync::alignShotTakes()
{
    const bool aligned = audiosync::alignShotTakes(
        syncSource(), session().clips, m_syncFrontPaddingSec, m_syncEndPaddingSec);
    if (aligned)
        m_displayedTakeId.clear();   // take ids were reassigned
    return aligned;
}

} // namespace rt
