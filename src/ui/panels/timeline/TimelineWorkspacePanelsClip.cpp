/*
 * TimelineWorkspacePanelsClip.cpp — the docks that edit the selected clip
 * or the timeline: Properties, Effect Controls, Color Correction, Effects,
 * History, Scopes, Graphics Editor, Captions and Tier List. Part of
 * createPanelWidgets().
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"
#include "PathUtils.h"
#include "panels/captions/CaptionsPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/monitors/ScopesPanel.h"
#include "panels/project/HistoryPanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/tierlist/TierListPanel.h"

#include <QDockWidget>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace rt {

void TimelineWorkspace::createClipPanelDocks()
{
    // -- Properties -------------------------------------------------------
    m_propertiesPanel = new PropertiesPanel(this);
    if (m_commandStack) m_propertiesPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_propertiesPanel->setTimeline(m_timeline);
    if (m_modelManager) m_propertiesPanel->setModelManager(m_modelManager);
    if (m_shotPresetManager) m_propertiesPanel->setShotPresetManager(m_shotPresetManager);
#ifdef ROUNDTABLE_HAS_SPINE
    m_propertiesPanel->setAnimationNamesProvider(
        [this](const std::string& charName, const std::string& outfit, int stance)
            -> std::vector<std::string> {
        if (m_destroying.load(std::memory_order_acquire)) return {};
        const std::string key = charName + "|" + outfit + "|" + std::to_string(stance);
        if (!m_compositeService) return {};
        // Keep borrowed entries/engines alive throughout this query.
        std::lock_guard lock(m_compositeService->compositeMutex());
        {
            auto* cached = m_compositeService->findAnimNames(key);
            if (cached) return *cached;
        }
        auto sharedData = m_compositeService->findSpineSharedData(key);
        for (auto& [cid, state] : m_compositeService->spineCache()) {
            if (state && state->shared && state->shared == sharedData
                && state->engine.isLoaded()) {
                auto anims = state->engine.animation().listAnimations();
                std::vector<std::string> names;
                names.reserve(anims.size());
                for (const auto& info : anims) {
                    if (info.name == "talk_end" || info.name == "talk_start")
                        continue;
                    names.push_back(info.name);
                }
                m_compositeService->storeAnimNames(key, names);
                return names;
            }
        }
        if (sharedData && !sharedData->skelBytes.empty() && !sharedData->atlasText.empty()) {
            SpineEngine tmpEngine;
            if (tmpEngine.loadSkeletonFromBuffers(
                    sharedData->skelBytes, sharedData->atlasText,
                    sharedData->atlasDir,
                    sharedData->skelPath, sharedData->atlasPath)) {
                auto anims = tmpEngine.animation().listAnimations();
                std::vector<std::string> names;
                names.reserve(anims.size());
                for (const auto& info : anims) {
                    if (info.name == "talk_end" || info.name == "talk_start")
                        continue;
                    names.push_back(info.name);
                }
                m_compositeService->storeAnimNames(key, names);
                return names;
            }
        }
        return {};
    });
#endif
    m_propertiesPanel->setVideoAnimNamesProvider(
        [](const std::string& charName, const std::string& outfit)
            -> std::vector<std::string> {
        namespace fs = std::filesystem;
        std::vector<std::string> names;
        static const char* fmtDirs[] = {"H264_Green", "H264_Blue", "H264_Custom", "ProRes"};
        fs::path dir;
        for (const auto* fmt : fmtDirs) {
            auto candidate = fs::path("assets/converted") / fmt / charName / outfit;
            std::error_code ec2;
            if (fs::exists(candidate, ec2)) { dir = candidate; break; }
        }
        if (dir.empty()) dir = fs::path("assets/converted") / "H264_Green" / charName / outfit;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string stem = pathToUtf8(entry.path().stem());
            if (stem.size() > 5 && stem.substr(stem.size() - 5) == "_talk")
                continue;
            names.push_back(stem);
        }
        std::sort(names.begin(), names.end());
        return names;
    });
    makeDock("Properties", m_propertiesPanel);

    // -- Effect Controls --------------------------------------------------
    m_effectControlsPanel = new EffectControlsPanel(this);
    if (m_commandStack) m_effectControlsPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_effectControlsPanel->setTimeline(m_timeline);
    makeDock("Effect Controls", m_effectControlsPanel);

    // -- Color Correction -------------------------------------------------
    m_ColorGradingPanel = new ColorGradingPanel(this);
    if (m_commandStack) m_ColorGradingPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_ColorGradingPanel->setTimeline(m_timeline);
    makeDock("Color Correction", m_ColorGradingPanel);

    // -- Effects ----------------------------------------------------------
    m_effectsPanel = new EffectsPanel(this);
    if (m_commandStack) m_effectsPanel->setCommandStack(m_commandStack);
    makeDock("Effects", m_effectsPanel);

    // -- History ----------------------------------------------------------
    m_historyPanel = new HistoryPanel(this);
    if (m_commandStack) m_historyPanel->setCommandStack(m_commandStack);
    makeDock("History", m_historyPanel);

    // -- Scopes -----------------------------------------------------------
    m_scopesPanel = new ScopesPanel(this);
    makeDock("Scopes", m_scopesPanel);

    // -- Graphics Editor --------------------------------------------------
    m_GraphicsEditorPanel = new GraphicsEditorPanel(this);
    if (m_commandStack) m_GraphicsEditorPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_GraphicsEditorPanel->setTimeline(m_timeline);
    makeDock("Graphics Editor", m_GraphicsEditorPanel);

    // -- Captions ---------------------------------------------------------
    m_captionsPanel = new CaptionsPanel(this);
    if (m_commandStack) m_captionsPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_captionsPanel->setTimeline(m_timeline);
    makeDock("Captions", m_captionsPanel);

    // -- Tier List --------------------------------------------------------
    m_tierListPanel = new TierListPanel(this);
    if (m_commandStack) m_tierListPanel->setCommandStack(m_commandStack);
    if (m_timeline) m_tierListPanel->setTimeline(m_timeline);
    makeDock("Tier List", m_tierListPanel);
}

} // namespace rt
