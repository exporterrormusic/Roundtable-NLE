/*
 * PropertiesPanelApply.cpp - Data binding & property application extracted from PropertiesPanel.cpp.
 *
 * Contains: refreshEffects, updateShotSection, onShotChanged, populateFromClip,
 * populateFromTransition, and all apply*() methods.
 */


#include "panels/properties/PropertiesPanel.h"
#include "widgets/ScrubbySpinBox.h"
#include "widgets/AudioFxSection.h"
#include "Theme.h"

#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "audio/AudioFile.h"
#include "AudioStreamLabels.h"
#include "PathUtils.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/Track.h"
#include "timeline/Timeline.h"
#include "timeline/Transition.h"
#include "spine/ShotPreset.h"
#include "spine/ModelManager.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "command/commands/EffectCommands.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QComboBox>
#include <QColorDialog>
#include <QSet>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

namespace rt {

void PropertiesPanel::refreshEffects()
{
    if (!m_fxParamsLayout) return;
    const auto& m = Theme::metrics();

    // â”€â”€ Clear previous parameter widgets â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    while (QLayoutItem* item = m_fxParamsLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->deleteLater();
        } else if (item->layout()) {
            // Recursively clean up nested layouts
            while (QLayoutItem* child = item->layout()->takeAt(0)) {
                if (child->widget()) child->widget()->deleteLater();
                delete child;
            }
        }
        delete item;
    }

    // Also sync the hidden list (used for remove operations)
    if (m_fxList) m_fxList->clear();

    if (!m_clip) return;

    auto& stack = m_clip->effects();
    if (stack.effectCount() == 0) {
        auto* emptyLabel = new QLabel(tr("No effects applied"), m_effectsSection);
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; padding: 12px;")
            .arg(Theme::hex(Theme::colors().textTertiary)));
        m_fxParamsLayout->addWidget(emptyLabel);
        return;
    }

    // â”€â”€ Premiere Proâ€“style collapsible header + parameter rows â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Style constants â€” derived from universal Theme
    const auto& tc = Theme::colors();
    const QString kFxHeaderStyle = QStringLiteral(
        "QWidget {"
        "  background: %1;"
        "  border: none;"
        "  border-bottom: 1px solid %2;"
        "}")
    .arg(Theme::hex(tc.surface2))
    .arg(Theme::hex(tc.border));
    const QString kFxParamRowStyle = QStringLiteral(
        "QWidget { background: %1; }")
    .arg(Theme::hex(tc.surface1));
    const QString kFxLabelStyle = QStringLiteral(
        "QLabel { color: %1; font-size: 11px; font-weight: normal; "
        "         padding: 0; background: transparent; }")
    .arg(Theme::hex(tc.textSecondary));
    const QString kFxNameStyle = QStringLiteral(
        "QLabel { color: %1; font-size: 12px; font-weight: bold; "
        "         padding: 0; background: transparent; }")
    .arg(Theme::hex(tc.textPrimary));
    const QString kFxCheckStyle = QStringLiteral(
        "QCheckBox { spacing: 4px; background: transparent; }"
        "QCheckBox::indicator { width: 12px; height: 12px; border-radius: 2px;"
        "  border: 1px solid %1; background: %2; }"
        "QCheckBox::indicator:checked { background: %3; border-color: %4; }")
    .arg(Theme::hex(tc.controlBorder))
    .arg(Theme::hex(tc.controlBg))
    .arg(Theme::hex(tc.accent))
    .arg(Theme::hex(tc.controlBorderFocus));
    const QString kFxRemoveBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none; color: %1;"
        "  font-size: 12px; font-weight: bold; padding: 0; }"
        "QPushButton:hover { color: %2; }")
    .arg(Theme::hex(tc.textTertiary))
    .arg(Theme::hex(tc.error));

    for (size_t i = 0; i < stack.effectCount(); ++i) {
        auto& fx = stack.effect(i);

        // Sync the hidden list
        if (m_fxList) {
            QString text = QString::fromUtf8(fx.name());
            if (!fx.isEnabled()) text += " (disabled)";
            m_fxList->addItem(text);
        }

        // â”€â”€ Container for this effect â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* fxGroup = new QWidget(m_effectsSection);
        auto* fxGroupLayout = new QVBoxLayout(fxGroup);
        fxGroupLayout->setContentsMargins(0, 0, 0, 0);
        fxGroupLayout->setSpacing(0);

        // â”€â”€ Header row: [â–¸] [Enable] [Effect Name] â”€â”€â”€â”€â”€â”€â”€ [âœ•] â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* header = new QWidget(fxGroup);
        header->setFixedHeight(24);
        header->setStyleSheet(kFxHeaderStyle);
        auto* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(m.spacingSm, 0, m.spacingXs, 0);
        headerLayout->setSpacing(m.spacingXs);

        // Expand/collapse toggle (â–¸ / â–¾)
        auto* toggleBtn = new QPushButton(QStringLiteral("\u25B8"), header);
        toggleBtn->setFixedSize(14, 14);
        toggleBtn->setFlat(true);
        toggleBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; font-size: 11px; background: transparent;"
            "  border: none; padding: 0; }"
            "QPushButton:hover { color: %2; }")
            .arg(Theme::hex(tc.textSecondary))
            .arg(Theme::hex(tc.textBright)));
        headerLayout->addWidget(toggleBtn);

        // Enable checkbox
        auto* enableCheck = new QCheckBox(header);
        enableCheck->setChecked(fx.isEnabled());
        enableCheck->setStyleSheet(kFxCheckStyle);
        enableCheck->setToolTip(tr("Enable / Disable"));
        headerLayout->addWidget(enableCheck);

        // Effect name label
        auto* nameLabel = new QLabel(QString::fromUtf8(fx.name()), header);
        nameLabel->setStyleSheet(kFxNameStyle);
        headerLayout->addWidget(nameLabel, 1);

        // Remove button (âœ•)
        auto* removeBtn = new QPushButton(QStringLiteral("\u2715"), header);
        removeBtn->setFixedSize(16, 16);
        removeBtn->setToolTip(tr("Remove Effect"));
        removeBtn->setStyleSheet(kFxRemoveBtnStyle);
        headerLayout->addWidget(removeBtn);

        fxGroupLayout->addWidget(header);

        // â”€â”€ Parameter rows container (collapsible) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* paramsContainer = new QWidget(fxGroup);
        auto* paramsLayout = new QVBoxLayout(paramsContainer);
        paramsLayout->setContentsMargins(18, m.spacingXs, m.spacingSm, m.spacingSm);
        paramsLayout->setSpacing(3);

        for (size_t pi = 0; pi < fx.paramCount(); ++pi) {
            const auto& param = fx.param(pi);

            auto* row = new QWidget(paramsContainer);
            row->setStyleSheet(kFxParamRowStyle);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 1, 0, 1);
            rowLayout->setSpacing(m.spacingSm);

            // Parameter name label
            auto* paramLabel = new QLabel(QString::fromStdString(param.name), row);
            paramLabel->setFixedWidth(90);
            paramLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            paramLabel->setStyleSheet(kFxLabelStyle);
            rowLayout->addWidget(paramLabel);

            // Scrubby spin box for the parameter value
            auto* spin = createScrubby(
                static_cast<double>(param.minVal),
                static_cast<double>(param.maxVal),
                static_cast<double>((param.maxVal - param.minVal) / 100.0f),
                2);
            spin->setValue(static_cast<double>(param.track.evaluate(0)));
            rowLayout->addWidget(spin, 1);

            // Connect spin to update the effect parameter
            size_t effectIdx = i;
            size_t paramIdx  = pi;

            // Live preview during scrub (no undo)
            connect(spin, &ScrubbySpinBox::valueScrubbed,
                    this, [this, effectIdx, paramIdx](double val) {
                if (!m_clip) return;
                auto& st = m_clip->effects();
                if (effectIdx >= st.effectCount()) return;
                auto& ef = st.effect(effectIdx);
                if (paramIdx >= ef.paramCount()) return;
                ef.param(paramIdx).track.addKeyframe(0, static_cast<float>(val));
                emit propertyChanged();
            });

            // Commit with undo support
            connect(spin, &ScrubbySpinBox::valueCommitted,
                    this, [this, effectIdx, paramIdx](double oldVal, double newVal) {
                if (!m_clip) return;
                auto& st = m_clip->effects();
                if (effectIdx >= st.effectCount()) return;
                auto& ef = st.effect(effectIdx);
                if (paramIdx >= ef.paramCount()) return;
                ef.param(paramIdx).track.addKeyframe(0, static_cast<float>(newVal));
                emit propertyChanged();
                if (m_commandStack) {
                    auto fOld = static_cast<float>(oldVal);
                    auto fNew = static_cast<float>(newVal);
                    auto* stack = &st;
                    auto fxId = ef.id();
                    auto pi = paramIdx;
                    m_commandStack->pushWithoutExecute(
                        std::make_unique<LambdaCommand>(
                            "Set Effect Parameter",
                            [stack, fxId, pi, fNew]() {
                                if (auto* fx = stack->effectById(fxId))
                                    if (pi < fx->paramCount())
                                        fx->param(pi).track.addKeyframe(0, fNew);
                            },
                            [stack, fxId, pi, fOld]() {
                                if (auto* fx = stack->effectById(fxId))
                                    if (pi < fx->paramCount())
                                        fx->param(pi).track.addKeyframe(0, fOld);
                            }));
                }
            });

            paramsLayout->addWidget(row);
        }

        fxGroupLayout->addWidget(paramsContainer);

        // â”€â”€ Toggle collapse â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        connect(toggleBtn, &QPushButton::clicked, this,
                [toggleBtn, paramsContainer]() {
            bool visible = paramsContainer->isVisible();
            paramsContainer->setVisible(!visible);
            toggleBtn->setText(visible ? QStringLiteral("\u25B8")    // â–¸
                                       : QStringLiteral("\u25BE")); // â–¾
        });

        // â”€â”€ Enable/disable â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        size_t effectIdx = i;
        connect(enableCheck, &QCheckBox::toggled, this,
                [this, effectIdx, nameLabel](bool checked) {
            if (!m_clip) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            auto& fx = st.effect(effectIdx);
            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<SetEffectEnabledCommand>(&st, fx.id(), checked));
            } else {
                fx.setEnabled(checked);
            }
            // Dim the name when disabled
            const auto& c = Theme::colors();
            nameLabel->setStyleSheet(QStringLiteral(
                "QLabel { color: %1; font-size: 12px; font-weight: bold;"
                " padding: 0; background: transparent; }")
                .arg(Theme::hex(checked ? c.textPrimary : c.textDisabled)));
            emit propertyChanged();
        });

        // â”€â”€ Remove effect â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        connect(removeBtn, &QPushButton::clicked, this,
                [this, effectIdx]() {
            if (!m_clip) return;
            auto& st = m_clip->effects();
            if (effectIdx >= st.effectCount()) return;
            if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<RemoveEffectCommand>(&st, effectIdx));
            } else {
                (void)st.removeEffect(effectIdx);
            }
            refreshEffects();
            emit propertyChanged();
        });

        m_fxParamsLayout->addWidget(fxGroup);
    }
}

void PropertiesPanel::updateShotSection()
{
    if (!m_clip) {
        m_shotSection->setVisible(false);
        return;
    }

    m_shotSection->setVisible(true);

    m_shotInfoLabel->setText(
        QString("Group %1 — Layer: %2")
            .arg(m_clip->groupId())
            .arg(QString::fromStdString(m_clip->layerId())));

    if (!m_shotManager) {
        // No manager — fall back to just listing the clip's own shot.
        m_shotShowCombo->setVisible(false);
        m_shotCharCombo->setVisible(false);
        m_shotCombo->blockSignals(true);
        m_shotCombo->clear();
        if (!m_clip->shotName().empty())
            m_shotCombo->addItem(QString::fromStdString(m_clip->shotName()));
        m_shotCombo->blockSignals(false);
        return;
    }

    m_shotShowCombo->setVisible(true);
    m_shotCharCombo->setVisible(true);

    // Gather distinct shows + characters across all presets, and find the
    // current clip's shot so the navigation filters can default to it.
    QSet<QString> showSet, charSet;
    QString defShow, defChar;
    const QString clipShot = QString::fromStdString(m_clip->shotName());
    for (const auto& name : m_shotManager->presetNames()) {
        auto preset = m_shotManager->load(name);
        if (!preset) continue;
        for (const auto& s : preset->shows())
            showSet.insert(QString::fromStdString(s));
        for (const auto& ch : preset->characters())
            charSet.insert(QString::fromStdString(ch.characterName));
        if (QString::fromStdString(name) == clipShot) {
            if (!preset->shows().empty())
                defShow = QString::fromStdString(preset->shows().front());
            // Default the character filter to the shot's NAMESAKE — the part of
            // the shot name before " (...)", e.g. "Wells (Default)" → "Wells" —
            // rather than whichever character happens to be first in the layer
            // stack (characters().front()). The namesake is the character the
            // shot is "about" and the one the user wants to keep working with,
            // so changing the shot stays filtered to that character instead of
            // a co-star (e.g. Chime). Falls back to the first character if the
            // name doesn't match any character in the preset.
            std::string showPart, barePart;
            ShotPresetManager::splitKey(name, showPart, barePart);
            const QString bare = QString::fromStdString(barePart);
            const int paren = bare.indexOf(QStringLiteral(" ("));
            const QString namesake = (paren > 0 ? bare.left(paren) : bare).trimmed();
            for (const auto& ch : preset->characters()) {
                if (QString::fromStdString(ch.characterName)
                        .compare(namesake, Qt::CaseInsensitive) == 0) {
                    defChar = QString::fromStdString(ch.characterName);
                    break;
                }
            }
            if (defChar.isEmpty() && !preset->characters().empty())
                defChar = QString::fromStdString(preset->characters().front().characterName);
        }
    }

    auto sortedList = [](const QSet<QString>& set) {
        QStringList list(set.begin(), set.end());
        std::sort(list.begin(), list.end(),
            [](const QString& a, const QString& b) {
                return a.compare(b, Qt::CaseInsensitive) < 0;
            });
        return list;
    };

    {
        // Block the combo signals during population so currentIndexChanged
        // doesn't repopulate the shot combo mid-build; we call it once below.
        QSignalBlocker blockShow(m_shotShowCombo);
        QSignalBlocker blockChar(m_shotCharCombo);

        m_shotShowCombo->clear();
        m_shotShowCombo->addItem(tr("All Shows"), QString());
        for (const QString& s : sortedList(showSet))
            m_shotShowCombo->addItem(s, s);
        if (!defShow.isEmpty()) {
            int i = m_shotShowCombo->findData(defShow);
            if (i >= 0) m_shotShowCombo->setCurrentIndex(i);
        }

        m_shotCharCombo->clear();
        m_shotCharCombo->addItem(tr("All Characters"), QString());
        for (const QString& cn : sortedList(charSet))
            m_shotCharCombo->addItem(cn, cn);
        if (!defChar.isEmpty()) {
            int i = m_shotCharCombo->findData(defChar);
            if (i >= 0) m_shotCharCombo->setCurrentIndex(i);
        }
    }

    populateShotCombo(clipShot);
}

void PropertiesPanel::populateShotCombo(const QString& selectName)
{
    if (!m_shotCombo) return;

    QString target = selectName;
    if (target.isEmpty() && m_clip)
        target = QString::fromStdString(m_clip->shotName());

    const QString showF = m_shotShowCombo ? m_shotShowCombo->currentData().toString() : QString();
    const QString charF = m_shotCharCombo ? m_shotCharCombo->currentData().toString() : QString();

    m_shotCombo->blockSignals(true);
    m_shotCombo->clear();

    if (m_shotManager) {
        auto names = m_shotManager->presetNames();
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            auto preset = m_shotManager->load(name);
            if (!preset) continue;
            // Show filter
            if (!showF.isEmpty() && !preset->hasShow(showF.toStdString()))
                continue;
            // Character filter
            if (!charF.isEmpty()) {
                bool has = false;
                for (const auto& ch : preset->characters()) {
                    if (QString::fromStdString(ch.characterName)
                            .compare(charF, Qt::CaseInsensitive) == 0) {
                        has = true;
                        break;
                    }
                }
                if (!has) continue;
            }
            // Display the bare shot name (append the show when listing across
            // all shows) but carry the full "show/name" key as item data.
            std::string s, bare;
            ShotPresetManager::splitKey(name, s, bare);
            QString display = QString::fromStdString(bare);
            if (showF.isEmpty() && !s.empty())
                display += QStringLiteral("  ·  ") + QString::fromStdString(s);
            m_shotCombo->addItem(display, QString::fromStdString(name));
        }

        // Select by key (data); fall back to display text for legacy bare refs.
        int idx = m_shotCombo->findData(target);
        if (idx < 0) idx = m_shotCombo->findText(target);
        if (idx >= 0) {
            m_shotCombo->setCurrentIndex(idx);
        } else {
            // Current shot not in the filtered list — show a neutral placeholder
            // so the first listed shot isn't mistaken for the active one.
            m_shotCombo->insertItem(0, QStringLiteral("-- Choose a shot --"));
            m_shotCombo->setCurrentIndex(0);
        }
    } else if (!target.isEmpty()) {
        m_shotCombo->addItem(target);
    }

    m_shotCombo->blockSignals(false);
}

void PropertiesPanel::refreshShotDropdown()
{
    if (!m_clip || !m_shotSection->isVisible()) return;
    // Re-read presets from the manager (COMPOSE may have saved a new shot) and
    // repopulate, preserving the current selection where possible.
    QString current = m_shotCombo->currentText();
    if (current == QStringLiteral("-- Choose a shot --"))
        current.clear();
    populateShotCombo(current);
}

void PropertiesPanel::onShotChanged(const std::string& newShotName)
{
    if (!m_clip) return;
    // Placeholder item — ignore
    if (newShotName.empty() || newShotName == "-- Choose a shot --") return;

    // Determine the set of visual clips this switch applies to. For a
    // multi-clip mixed selection we operate on every visual clip the user
    // picked; for a single-clip selection it's just m_clip. Audio clips are
    // intentionally left alone — shot presets carry only visual layers.
    std::vector<Clip*> visualClips;
    if (m_multiSelection.size() > 1) {
        visualClips.reserve(m_multiSelection.size());
        for (auto* c : m_multiSelection) {
            if (c && c->isVisual())
                visualClips.push_back(c);
        }
    } else {
        visualClips.push_back(m_clip);
    }
    if (visualClips.empty()) return;

    // Pick / assign a shared groupId so applyShotSwitch can find every clip
    // that should be replaced. If the visual clips already share a non-zero
    // groupId, reuse it (preserves an existing shot group).
    uint64_t groupId = visualClips.front()->groupId();
    bool allSameNonZero = (groupId != 0);
    for (size_t i = 1; i < visualClips.size() && allSameNonZero; ++i) {
        if (visualClips[i]->groupId() != groupId) allSameNonZero = false;
    }
    if (!allSameNonZero) {
        // Generate a groupId that is guaranteed not to collide with any
        // existing clip id or groupId anywhere on the timeline. Using a
        // clip's own id (the previous behaviour) can collide with a
        // groupId already stamped on an unrelated clip — applyShotSwitch
        // scans by groupId equality, so a collision sweeps that clip into
        // the swap, replacing far more than the selection AND stretching
        // the new shot's layers across the inflated span (whose duration
        // exceeds the source media, painting black frames on the timeline
        // scrub strip). Pick max(id, groupId)+1 across the whole timeline
        // so the new id is strictly fresh.
        uint64_t freshGid = 1;
        if (m_timeline) {
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                const Track* trk = m_timeline->track(ti);
                if (!trk) continue;
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                    const Clip* c = trk->clip(ci);
                    if (!c) continue;
                    if (c->groupId() >= freshGid) freshGid = c->groupId() + 1;
                    if (c->id()      >= freshGid) freshGid = c->id() + 1;
                }
            }
        } else {
            freshGid = visualClips.front()->id() + 1;
        }
        groupId = freshGid;
        for (auto* c : visualClips) c->setGroupId(groupId);
    }

    // Let TimelineWorkspace handle the actual switch with undo support
    emit shotSwitchRequested(groupId, newShotName);
    emit propertyChanged();

    spdlog::info("PropertiesPanel: shot switch requested for group {} ({} visual clip(s))",
                 groupId, visualClips.size());
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Clip binding
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


void PropertiesPanel::populateFromClip()
{
    if (!m_clip) return;

    m_updating = true; // block signals from triggering apply methods

    // Header
    QString typeName;
    switch (m_clip->clipType())
    {
    case ClipType::Spine:      typeName = "Spine";      break;
    case ClipType::Video:      typeName = "Video";      break;
    case ClipType::Audio:      typeName = "Audio";      break;
    case ClipType::Title:      typeName = "Title";      break;
    case ClipType::Adjustment: typeName = "Adjustment"; break;
    case ClipType::Image:      typeName = "Image";      break;
    case ClipType::Graphic:    typeName = "Graphic";    break;
    case ClipType::Caption:    typeName = "Caption";    break;
    case ClipType::PngPuppet:  typeName = "Puppet";     break;
    default:                   break;
    }
    m_headerLabel->setText(QString::fromStdString(m_clip->label()));
    m_typeLabel->setText(typeName);

    // Colored type pill badge
    QColor badgeColor;
    switch (m_clip->clipType()) {
    case ClipType::Video:      badgeColor = QColor(80, 140, 220);  break;
    case ClipType::Audio:      badgeColor = QColor(80, 180, 100);  break;
    case ClipType::Spine:      badgeColor = QColor(160, 100, 220); break;
    case ClipType::Title:      badgeColor = QColor(220, 180, 60);  break;
    case ClipType::Graphic:    badgeColor = QColor(220, 140, 60);  break;
    case ClipType::Image:      badgeColor = QColor(100, 180, 200); break;
    case ClipType::Adjustment: badgeColor = QColor(180, 180, 180); break;
    case ClipType::Caption:    badgeColor = QColor(70, 200, 170);  break;
    case ClipType::PngPuppet:  badgeColor = QColor(220, 110, 160); break;
    default:                   badgeColor = QColor(150, 150, 150); break;
    }
    {
        const auto& bm = Theme::metrics();
        m_typeLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 11px; padding: 1px 6px; "
                           "border-radius: %2px; background: %3; border: none; }")
                .arg(Theme::hex(badgeColor.lighter(180)),
                     QString::number(bm.radiusSm),
                     Theme::hex(badgeColor.darker(200))));
    }

    // Update status bar
    if (m_statusLabel)
        m_statusLabel->setText(typeName + QStringLiteral(" clip"));

    // Identity
    m_labelEdit->setText(QString::fromStdString(m_clip->label()));
    m_enabledCheck->setChecked(m_clip->isEnabled());
    m_speedSpin->setValue(m_clip->speed() * 100.0);

    // Transform (using keyframe at t=0)
    // UI shows percentage for scale (100 = 1.0x) and opacity (100 = 1.0)
    m_posXSpin->setValue(m_clip->positionX().evaluate(0));
    m_posYSpin->setValue(m_clip->positionY().evaluate(0));
    m_scaleXSpin->setValue(m_clip->scaleX().evaluate(0) * 100.0);
    m_scaleYSpin->setValue(m_clip->scaleY().evaluate(0) * 100.0);
    // Reflect flip state from the sign of the scale (block signals so
    // syncing the UI doesn't re-commit a transform / create undo noise).
    if (m_flipHCheck) {
        QSignalBlocker b(m_flipHCheck);
        m_flipHCheck->setChecked(m_clip->scaleX().evaluate(0) < 0.0f);
    }
    if (m_flipVCheck) {
        QSignalBlocker b(m_flipVCheck);
        m_flipVCheck->setChecked(m_clip->scaleY().evaluate(0) < 0.0f);
    }
    m_rotationSpin->setValue(m_clip->rotation().evaluate(0));
    m_opacitySpin->setValue(m_clip->opacity().evaluate(0) * 100.0);

    // Crop — only Spine/Video surface it; the base accessors return 0 for
    // everything else, matching the old zero defaults.
    const bool hasCrop = m_clip->supportsCrop();
    m_tfCropLeftSpin->setValue(hasCrop ? static_cast<double>(m_clip->cropLeft()) : 0.0);
    m_tfCropRightSpin->setValue(hasCrop ? static_cast<double>(m_clip->cropRight()) : 0.0);
    m_tfCropTopSpin->setValue(hasCrop ? static_cast<double>(m_clip->cropTop()) : 0.0);
    m_tfCropBottomSpin->setValue(hasCrop ? static_cast<double>(m_clip->cropBottom()) : 0.0);

    // Type-specific sections — per-type populate helpers (exhaustive
    // dispatch stays a switch, per fable_cleanup.txt §3.5)
    switch (m_clip->clipType())
    {
    case ClipType::Spine:     populateFromSpine();   break;
    case ClipType::Video:     populateFromVideo();   break;
    case ClipType::PngPuppet: populateFromPuppet();  break;
    case ClipType::Audio:     populateFromAudio();   break;
    case ClipType::Caption:
        populateFromCaption();
        // The header showed the whole transcript line (CaptionClip label ==
        // its text); trim it so it doesn't dominate the title bar.
        {
            QString hdr = m_headerLabel->text();
            if (hdr.length() > 40)
                m_headerLabel->setText(hdr.left(37) + QStringLiteral("..."));
        }
        break;
    case ClipType::Title:     populateFromTitle();   break;
    case ClipType::Graphic:   populateFromGraphic(); break;
    default: break; // Adjustment/Image/Sequence: no type-specific section
    }

    m_updating = false;

    // Refresh applied effects list
    refreshEffects();
}

void PropertiesPanel::populateFromSpine()
{
    auto* sc = static_cast<SpineClip*>(m_clip);
    m_spineClip = sc;

    // Restore animation section defaults (may have been hidden for video chars)
    m_animationSection->setTitle("Animation");
    m_loopingCheck->setVisible(true);
    m_animationCombo->setVisible(true);
    m_animSpeedSpin->setVisible(true);
    m_continuityCheck->setVisible(true);
    m_outfitCombo->setVisible(true);
    m_stanceCombo->setVisible(true);
    if (m_characterSection)
        m_characterSection->setTitle("Character");

    // Populate dropdowns (order matters: character → outfit → stance → animation)
    populateCharacterDropdown();
    {
        // Find by folder name stored in item data
        int idx = m_characterCombo->findData(QString::fromStdString(sc->characterName()));
        if (idx >= 0)
            m_characterCombo->setCurrentIndex(idx);
        else
            m_characterCombo->setCurrentText(QString::fromStdString(sc->characterName()));
    }
    populateOutfitDropdown();
    m_outfitCombo->setCurrentText(QString::fromStdString(sc->outfit()));
    populateStanceDropdown();
    m_stanceCombo->setCurrentIndex(static_cast<int>(sc->stance()));
    populateAnimationDropdown();
    m_animationCombo->setCurrentText(QString::fromStdString(sc->animationName()));

    m_loopingCheck->setChecked(sc->isLooping());
    m_talkingCheck->setChecked(sc->isTalking());
    m_animSpeedSpin->setValue(sc->animationSpeed());
    m_continuityCheck->setChecked(sc->useGlobalTime());
}

void PropertiesPanel::populateFromVideo()
{
    auto* vc = static_cast<VideoClip*>(m_clip);
    m_mediaPathLabel->setText(QString::fromStdString(vc->mediaPath()));
    m_volumeSpin->setValue(vc->volume());

    // Video character controls
    if (vc->isVideoCharacter()) {
        // Character section — show name + outfit dropdown
        m_characterCombo->blockSignals(true);
        m_characterCombo->clear();
        m_characterCombo->addItem(QString::fromStdString(vc->characterName()));
        m_characterCombo->setCurrentIndex(0);
        m_characterCombo->blockSignals(false);

        // Populate outfit dropdown from ModelManager
        m_outfitCombo->setVisible(true);
        m_outfitCombo->blockSignals(true);
        m_outfitCombo->clear();
        if (m_modelManager) {
            auto outfits = m_modelManager->getMetadataOutfits(vc->characterName());
            for (const auto& outfit : outfits)
                m_outfitCombo->addItem(QString::fromStdString(outfit.key));
        }
        if (m_outfitCombo->count() == 0)
            m_outfitCombo->addItem(QString::fromStdString(
                vc->outfit().empty() ? "default" : vc->outfit()));
        {
            QString cur = QString::fromStdString(
                vc->outfit().empty() ? "default" : vc->outfit());
            int idx = m_outfitCombo->findText(cur);
            if (idx >= 0) m_outfitCombo->setCurrentIndex(idx);
            else m_outfitCombo->setCurrentIndex(0);
        }
        m_outfitCombo->blockSignals(false);

        m_stanceCombo->setVisible(false);
        if (m_characterSection)
            m_characterSection->setTitle(
                QString("Character: %1").arg(
                    QString::fromStdString(vc->characterName())));

        // Animation section — talking toggle + animation dropdown
        m_talkingCheck->setChecked(vc->isTalking());
        m_loopingCheck->setVisible(false);
        m_animSpeedSpin->setVisible(false);
        m_continuityCheck->setVisible(false);

        // Populate animation dropdown from video cache directory
        m_animationCombo->setVisible(true);
        m_animationCombo->blockSignals(true);
        m_animationCombo->clear();
        if (m_videoAnimNamesProvider) {
            std::string outfit = vc->outfit().empty() ? "default" : vc->outfit();
            auto anims = m_videoAnimNamesProvider(vc->characterName(), outfit);
            for (const auto& a : anims)
                m_animationCombo->addItem(QString::fromStdString(a));
        }
        if (m_animationCombo->count() == 0 && !vc->animationName().empty())
            m_animationCombo->addItem(QString::fromStdString(vc->animationName()));
        {
            QString cur = QString::fromStdString(vc->animationName());
            int idx = m_animationCombo->findText(cur);
            if (idx >= 0) m_animationCombo->setCurrentIndex(idx);
            else if (m_animationCombo->count() > 0) m_animationCombo->setCurrentIndex(0);
        }
        m_animationCombo->blockSignals(false);

        m_animationSection->setTitle("Animation");
    }
}

void PropertiesPanel::populateFromAudio()
{
    auto* ac = static_cast<AudioClip*>(m_clip);

    // Audio stream picker: enumerate the source file's audio streams (header
    // probe, no decode) and select the clip's current choice. "Auto" = -1 =
    // legacy best-stream behavior. Disabled when there's only one stream.
    if (m_audioStreamCombo) {
        m_audioStreamCombo->blockSignals(true);
        m_audioStreamCombo->clear();
        const auto streams = AudioFile::enumerateAudioStreams(utf8ToPath(ac->mediaPath()));
        m_audioStreamCombo->addItem(tr("Auto (best)"), -1);
        for (const auto& s : streams)
            m_audioStreamCombo->addItem(audioStreamLabel(s), s.ordinal);
        const int want = ac->audioStreamIndex();
        int sel = 0;  // default to "Auto"
        for (int i = 0; i < m_audioStreamCombo->count(); ++i) {
            if (m_audioStreamCombo->itemData(i).toInt() == want) { sel = i; break; }
        }
        m_audioStreamCombo->setCurrentIndex(sel);
        m_audioStreamCombo->setEnabled(streams.size() > 1);
        m_audioStreamCombo->blockSignals(false);
    }

    m_audioVolumeSpin->setValue(ac->volume().evaluate(0));
    m_panSpin->setValue(ac->pan().evaluate(0));
    m_fadeInSpin->setValue(static_cast<double>(ac->fadeInDuration()));
    m_fadeOutSpin->setValue(static_cast<double>(ac->fadeOutDuration()));
    if (m_audioFxSection) m_audioFxSection->setClip(ac, m_commandStack);
}

void PropertiesPanel::populateFromTitle()
{
    auto* tc = static_cast<TitleClip*>(m_clip);
    m_textEdit->setText(QString::fromStdString(tc->text()));
    m_fontFamilyEdit->setText(QString::fromStdString(tc->fontFamily()));
    m_fontSizeSpin->setValue(tc->fontSize());
    m_boldCheck->setChecked(tc->isBold());
    m_italicCheck->setChecked(tc->isItalic());
    m_alignCombo->setCurrentIndex(static_cast<int>(tc->alignment()));
}

void PropertiesPanel::populateFromGraphic()
{
    auto* gc = static_cast<GraphicClip*>(m_clip);
    // Populate from the first text layer (if any)
    TextLayer* tl = nullptr;
    for (size_t i = 0; i < gc->layerCount(); ++i) {
        if (gc->layer(i)->layerType() == GraphicLayerType::Text) {
            tl = static_cast<TextLayer*>(gc->layer(i));
            break;
        }
    }
    if (tl) {
        m_gfxTextEdit->setText(QString::fromStdString(tl->text()));
        m_gfxFontFamilyEdit->setText(QString::fromStdString(tl->fontFamily()));
        m_gfxFontSizeSpin->setValue(static_cast<double>(tl->fontSize()));
        m_gfxFontWeightSpin->setValue(tl->fontWeight());
        m_gfxItalicCheck->setChecked(tl->isItalic());
        m_gfxAllCapsCheck->setChecked(tl->allCaps());
        m_gfxSmallCapsCheck->setChecked(tl->smallCaps());
        m_gfxAlignCombo->setCurrentIndex(static_cast<int>(tl->alignment()));

        const auto& app = tl->appearance();
        // Fill color button
        if (!app.fills.empty()) {
            uint32_t fc = app.fills[0].color;
            QColor fillCol(static_cast<int>((fc>>16)&0xFF),
                           static_cast<int>((fc>>8)&0xFF),
                           static_cast<int>(fc&0xFF));
            m_gfxFillColorBtn->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; border: 1px solid #555; min-width: 40px; min-height: 18px; }")
                .arg(fillCol.name()));
        }
        // Stroke
        if (!app.strokes.empty()) {
            m_gfxStrokeCheck->setChecked(app.strokes[0].enabled);
            m_gfxStrokeWidthSpin->setValue(static_cast<double>(app.strokes[0].width));
            uint32_t sc = app.strokes[0].color;
            QColor strokeCol(static_cast<int>((sc>>16)&0xFF),
                             static_cast<int>((sc>>8)&0xFF),
                             static_cast<int>(sc&0xFF));
            m_gfxStrokeColorBtn->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; border: 1px solid #555; min-width: 40px; min-height: 18px; }")
                .arg(strokeCol.name()));
        }
        // Shadow
        if (!app.shadows.empty()) {
            m_gfxShadowCheck->setChecked(app.shadows[0].enabled);
        }
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Apply property changes
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void PropertiesPanel::applyLabel()
{
    if (m_updating || !m_clip) return;
    std::string newLabel = m_labelEdit->text().toStdString();
    if (newLabel == m_clip->label()) return;
    auto oldLabel = m_clip->label();
    auto* clip = m_clip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change label",
            [clip, newLabel, this]() { clip->setLabel(newLabel); populateFromClip(); emit propertyChanged(); },
            [clip, oldLabel, this]() { clip->setLabel(oldLabel); populateFromClip(); emit propertyChanged(); }));
    } else {
        clip->setLabel(newLabel);
        m_headerLabel->setText(m_labelEdit->text());
        emit propertyChanged();
    }
}

void PropertiesPanel::applyEnabled()
{
    if (m_updating || !m_clip) return;
    bool newVal = m_enabledCheck->isChecked();
    if (newVal == m_clip->isEnabled()) return;
    bool oldVal = m_clip->isEnabled();
    auto* clip = m_clip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Toggle enabled",
            [clip, newVal, this]() { clip->setEnabled(newVal); populateFromClip(); emit propertyChanged(); },
            [clip, oldVal, this]() { clip->setEnabled(oldVal); populateFromClip(); emit propertyChanged(); }));
    } else {
        clip->setEnabled(newVal);
        emit propertyChanged();
    }
}

void PropertiesPanel::applySpeed()
{
    if (m_updating || !m_clip) return;
    double newPct = m_speedSpin->value();
    double newVal = newPct / 100.0;
    if (newVal <= 0.0) newVal = 0.01;
    if (newVal == m_clip->speed()) return;
    double oldVal = m_clip->speed();
    int64_t oldDur = m_clip->duration();
    int64_t newDur = static_cast<int64_t>(std::llround(oldDur * oldVal / newVal));
    if (newDur < kMinClipDuration) newDur = kMinClipDuration;

    // Clamp to next clip's start so we don't overlap
    if (m_track) {
        size_t ci = m_track->findClipIndexById(m_clip->id());
        for (size_t i = ci + 1; i < m_track->clipCount(); ++i) {
            const Clip* next = m_track->clip(i);
            if (next && next->timelineIn() > m_clip->timelineIn()) {
                int64_t maxDur = next->timelineIn() - m_clip->timelineIn();
                if (newDur > maxDur && maxDur >= kMinClipDuration) newDur = maxDur;
                break;
            }
        }
    }

    auto* clip = m_clip;
    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Change speed",
            [clip, newVal, newDur, this]() { clip->setSpeed(newVal); clip->setDuration(newDur); populateFromClip(); emit propertyChanged(); },
            [clip, oldVal, oldDur, this]() { clip->setSpeed(oldVal); clip->setDuration(oldDur); populateFromClip(); emit propertyChanged(); }));
    } else {
        clip->setSpeed(newVal);
        clip->setDuration(newDur);
        emit propertyChanged();
    }
}

int64_t PropertiesPanel::clipRelativeTick() const noexcept
{
    if (!m_clip) return 0;
    const int64_t playhead = m_timeline ? m_timeline->playheadPosition() : 0;
    return playhead - m_clip->timelineIn();
}

void PropertiesPanel::applyTransformLive()
{
    // Live preview during scrub drag — writes current spinbox values to
    // clip properties without creating undo commands, so the program
    // monitor recomposites on the next poll cycle.
    if (m_updating || !m_clip) return;

    const int64_t t = clipRelativeTick();
    auto* clip = m_clip;
    clip->positionX().writeValue(t, static_cast<float>(m_posXSpin->value()));
    clip->positionY().writeValue(t, static_cast<float>(m_posYSpin->value()));
    clip->scaleX().writeValue(t, static_cast<float>(m_scaleXSpin->value() / 100.0));
    clip->scaleY().writeValue(t, static_cast<float>(m_scaleYSpin->value() / 100.0));
    clip->rotation().writeValue(t, static_cast<float>(m_rotationSpin->value()));
    clip->opacity().writeValue(t, static_cast<float>(m_opacitySpin->value() / 100.0));
    if (clip->supportsCrop()) {
        clip->setCrop(
            static_cast<float>(m_tfCropLeftSpin->value()),
            static_cast<float>(m_tfCropRightSpin->value()),
            static_cast<float>(m_tfCropTopSpin->value()),
            static_cast<float>(m_tfCropBottomSpin->value()));
    }
    emit propertyChanged();
}

void PropertiesPanel::applyTransform(ScrubbySpinBox* src, double oldUi, double newUi)
{
    // Called on scrub-end / typed-commit (valueCommitted) for a SINGLE
    // spinbox.  applyTransformLive() already wrote the new value during the
    // drag, so we only record an undo command here.  Crucially this command
    // touches ONLY the field that changed — bundling every transform track
    // into one command (and restoring each from its spinbox's stale
    // scrubStartValue) used to clobber unrelated fields on undo, e.g.
    // undoing a scale change would also revert a Flip H/V, since the flip
    // lives in the scaleX/scaleY default value.
    if (m_updating || !m_clip || !src || !m_commandStack) return;
    auto* clip = m_clip;
    const int64_t t = clipRelativeTick();

    // ── Crop edges (not keyframe-tracked; Spine/Video only) ─────────────
    // Each crop spinbox drives one edge; rebuild the crop tuple from the
    // clip's current edges and flip just this one between old/new so the
    // other three are never disturbed.
    auto cropEdgeCmd = [&](int edge) {
        const float oldV = static_cast<float>(oldUi);
        const float newV = static_cast<float>(newUi);
        auto setEdge = [clip, edge](float v) {
            if (!clip->supportsCrop())
                return;
            float l = clip->cropLeft(), r = clip->cropRight();
            float tp = clip->cropTop(), b = clip->cropBottom();
            switch (edge) { case 0: l = v; break; case 1: r = v; break;
                            case 2: tp = v; break; default: b = v; break; }
            clip->setCrop(l, r, tp, b);
        };
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Change crop",
            [setEdge, newV, this]() { setEdge(newV); populateFromClip(); emit propertyChanged(); },
            [setEdge, oldV, this]() { setEdge(oldV); populateFromClip(); emit propertyChanged(); }));
    };

    if      (src == m_tfCropLeftSpin)   { cropEdgeCmd(0); return; }
    else if (src == m_tfCropRightSpin)  { cropEdgeCmd(1); return; }
    else if (src == m_tfCropTopSpin)    { cropEdgeCmd(2); return; }
    else if (src == m_tfCropBottomSpin) { cropEdgeCmd(3); return; }

    // ── Keyframe-tracked fields ─────────────────────────────────────────
    // Map the source spinbox to its track index and the UI→internal scale.
    enum { kPosX, kPosY, kScaleX, kScaleY, kRot, kOpacity };
    int field = -1;
    double uiScale = 1.0;
    const char* desc = "Change transform";
    if      (src == m_posXSpin)     { field = kPosX;    desc = "Change position"; }
    else if (src == m_posYSpin)     { field = kPosY;    desc = "Change position"; }
    else if (src == m_scaleXSpin)   { field = kScaleX;  uiScale = 0.01; desc = "Change scale"; }
    else if (src == m_scaleYSpin)   { field = kScaleY;  uiScale = 0.01; desc = "Change scale"; }
    else if (src == m_rotationSpin) { field = kRot;     desc = "Change rotation"; }
    else if (src == m_opacitySpin)  { field = kOpacity; uiScale = 0.01; desc = "Change opacity"; }
    if (field < 0) return;

    const float oldVal = static_cast<float>(oldUi * uiScale);
    const float newVal = static_cast<float>(newUi * uiScale);

    // Stateless resolver so the command lambdas re-fetch the live track from
    // the clip (mirrors the Flip command pattern) rather than capturing a
    // raw KeyframeTrack reference.
    auto trackOf = [](Clip* c, int f) -> KeyframeTrack<float>& {
        switch (f) {
            case kPosX:   return c->positionX();
            case kPosY:   return c->positionY();
            case kScaleX: return c->scaleX();
            case kScaleY: return c->scaleY();
            case kRot:    return c->rotation();
            default:      return c->opacity();
        }
    };

    // writeValue matches the live-write semantics: on a static track it
    // updates the default value (no keyframe); on an animated track it
    // adds/updates the keyframe at t.
    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
        desc,
        [clip, field, t, newVal, trackOf, this]() {
            trackOf(clip, field).writeValue(t, newVal);
            populateFromClip();
            emit propertyChanged();
        },
        [clip, field, t, oldVal, trackOf, this]() {
            trackOf(clip, field).writeValue(t, oldVal);
            populateFromClip();
            emit propertyChanged();
        }));
}

// ── Spine methods are in PropertiesPanelSpine.cpp ───────────────────────────

// â”€â”€ Video â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// -- Video methods are in PropertiesPanelVideo.cpp --
// -- Audio methods are in PropertiesPanelAudio.cpp --


// â”€â”€ Audio â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€


// -- Audio methods are in PropertiesPanelAudio.cpp --







// â”€â”€ Title â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€


// -- Title methods are in PropertiesPanelTitle.cpp --











// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Graphic property changes
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// -- Graphic methods are in PropertiesPanelGraphic.cpp --
// (their firstTextLayer() helper lives there too)

} // namespace rt

