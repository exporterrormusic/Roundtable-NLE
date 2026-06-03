/*
 * AudioFxSection — Premiere-style editor for a clip's audio effect chain.
 *
 * Shows the AudioClip's audiofx::FxChain (Parametric EQ, Dynamics) with
 * add / remove / reorder and per-parameter controls. All edits are routed
 * through the CommandStack for undo/redo using whole-chain snapshots, so the
 * structure stays simple and bullet-proof regardless of which parameter or
 * structural change happened.
 *
 * The widget owns no audio state — it reads and writes the live chain on the
 * clip. The export path clones the chain at mix time, so editing here never
 * fights an in-flight render.
 */

#pragma once

#include <QWidget>
#include <functional>

namespace rt {

class AudioClip;
class CommandStack;

namespace audiofx { class FxChain; }

class AudioFxSection : public QWidget
{
    Q_OBJECT

public:
    explicit AudioFxSection(QWidget* parent = nullptr);
    ~AudioFxSection() override;

    /// Point the editor at a clip (nullptr clears it). Rebuilds the UI.
    void setClip(AudioClip* clip, CommandStack* stack);

    [[nodiscard]] AudioClip* clip() const noexcept { return m_clip; }

signals:
    /// Emitted after any edit (add/remove/parameter), for "modified" tracking.
    void changed();

private:
    /// Tear down and rebuild the per-processor controls from the live chain.
    void rebuild();

    /// Snapshot-based undo: clones the chain, applies `mutate` to the clone,
    /// and pushes a command that swaps whole chains on redo/undo.
    void commitEdit(const QString& name,
                    const std::function<void(audiofx::FxChain&)>& mutate);

    QWidget* buildEqWidget(int index);
    QWidget* buildDynamicsWidget(int index);

    AudioClip*    m_clip{nullptr};
    CommandStack* m_commandStack{nullptr};

    class QVBoxLayout* m_chainLayout{nullptr};  ///< holds per-processor groups
    bool m_rebuildQueued{false};
};

} // namespace rt
