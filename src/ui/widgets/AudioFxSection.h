/*
 * AudioFxSection — Premiere-style editor for a clip's audio effect chain.
 *
 * Shows the AudioClip's audiofx::FxChain (Parametric EQ, Dynamics, Reverb,
 * Delay, Distortion, plus Essential Sound style presets) with
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

class QLabel;
class QPushButton;
class QVBoxLayout;

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

    /// Read / replace the chain being edited (any owner: a clip, a track).
    using ChainGetter = std::function<const audiofx::FxChain*()>;
    using ChainSetter = std::function<void(audiofx::FxChain)>;

    /// Edit an arbitrary chain. `get` may return nullptr once the owner is
    /// gone; `set` must resolve its owner at call time (it runs from undo /
    /// redo). `owner` only identifies the target for deferred refreshes.
    void setTarget(ChainGetter get, ChainSetter set, CommandStack* stack, const void* owner);

    [[nodiscard]] AudioClip* clip() const noexcept { return m_clip; }

    /// Hide the built-in "Audio Effects" title when the host already shows one
    /// (Effect Controls puts the editor under its own section header).
    void setTitleVisible(bool visible);

    /// Show only the processor at `index`, as a bare body (no title, Add
    /// button, enable checkbox or remove button): Effect Controls draws its
    /// own Premiere-style header for each audio effect. -1 = whole chain.
    void showOnly(int index);

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
    /// Generic editor for table-described effects (Reverb, Delay, Distortion…).
    QWidget* buildParamWidget(int index);

    /// The chain being edited right now (nullptr when none).
    [[nodiscard]] const audiofx::FxChain* chain() const { return m_get ? m_get() : nullptr; }

    AudioClip*    m_clip{nullptr};
    ChainGetter   m_get;
    ChainSetter   m_set;
    const void*   m_owner{nullptr};
    CommandStack* m_commandStack{nullptr};

    QVBoxLayout* m_chainLayout{nullptr};  ///< holds per-processor groups
    QLabel*      m_titleLabel{nullptr};
    QPushButton* m_addBtn{nullptr};
    int                m_onlyIndex{-1};   ///< >= 0: single-processor body mode
    bool m_rebuildQueued{false};
};

} // namespace rt
