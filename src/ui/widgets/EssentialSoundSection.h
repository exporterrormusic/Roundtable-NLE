/*
 * EssentialSoundSection — Premiere-style Essential Sound controls for the
 * selected audio clip (Properties panel, above the audio effects).
 *
 * Pick a type (Dialogue / Music / SFX / Ambience); the section shows that
 * type's task controls. Every change writes the clip's EssentialSoundSettings
 * as one undo step; the settings compile to processors that run before the
 * clip's own effects (AudioClip::buildProcessingChain()).
 *
 * One-shot actions:
 *   Auto-Match — measures the clip (BS.1770 LUFS) and sets its volume to hit
 *                the target loudness.
 *   Ducking    — (Music / Ambience) writes volume keyframes that dip this
 *                clip wherever a Dialogue-tagged clip plays.
 */

#pragma once

#include <QWidget>

#include <cstdint>
#include <functional>

namespace rt {

class AudioClip;
class CommandStack;
class Timeline;
namespace audiofx { struct EssentialSoundSettings; }

class EssentialSoundSection : public QWidget
{
    Q_OBJECT

public:
    explicit EssentialSoundSection(QWidget* parent = nullptr);
    ~EssentialSoundSection() override;

    /// Point at a clip (nullptr hides the section). `timeline` holds the
    /// clip; edits resolve the clip by id in it when they run (undo safe).
    void setClip(AudioClip* clip, Timeline* timeline, CommandStack* stack);

signals:
    /// Any edit (settings, Auto-Match, ducking keyframes).
    void changed();

private:
    void rebuild();
    void scheduleRebuild();
    [[nodiscard]] AudioClip* resolveClip() const;

    /// One undo step replacing the clip's settings with `mutate`d ones.
    void commitSettings(const QString& name,
                        const std::function<void(audiofx::EssentialSoundSettings&)>& mutate);
    void autoMatchLoudness();
    void generateDucking(float duckDb, int fadeMs);

    QWidget* buildTypeRow();
    QWidget* buildLoudness();
    QWidget* buildRepair();
    QWidget* buildClarity();
    QWidget* buildCreative();
    QWidget* buildAmbience();
    QWidget* buildDucking();

    Timeline*     m_timeline{nullptr};
    CommandStack* m_commandStack{nullptr};
    uint64_t      m_clipId{0};
    class QVBoxLayout* m_body{nullptr};
    bool m_rebuildQueued{false};
};

} // namespace rt
