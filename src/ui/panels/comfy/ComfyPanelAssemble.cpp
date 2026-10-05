/*
 * ComfyPanelAssemble.cpp — lays the chosen ComfyUI renders onto a timeline:
 * script order, tail pad trimmed off, full-width bottom-aligned, keyed green,
 * WIDE / CLOSE-UP backgrounds underneath, render audio linked, and a gap +
 * named marker per segment for the stinger.
 *
 * Built on a detached clone and applied as ONE undoable step (same pattern as
 * AudioSync::exportToTimeline).
 */
#include "panels/comfy/ComfyPanel.h"

#include "Constants.h"
#include "PathUtils.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "decode/VideoDecoder.h"
#include "effects/ChromaKey.h"
#include "panels/audio/AudioSync.h"
#include "timeline/AudioClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QFileInfo>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace rt {

namespace {

struct AssemblyProbe
{
    bool   ok{false};
    bool   still{false};
    double durationSec{0.0};
    double fps{0.0};
    int    width{0};
    int    height{0};
    bool   hasAudio{false};
};

AssemblyProbe probeAssemblyMedia(const std::string& path)
{
    AssemblyProbe p;
    const QString ext = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    p.still = ext == QLatin1String("png") || ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") ||
              ext == QLatin1String("webp") || ext == QLatin1String("bmp");
    VideoDecoder dec;
    if (!dec.open(utf8ToPath(path), /*forceSoftware=*/true)) return p;
    const auto& info = dec.info();
    p.ok = true;
    p.durationSec = info.duration;
    p.fps = info.fps;
    p.width = static_cast<int>(info.width);
    p.height = static_cast<int>(info.height);
    p.hasAudio = info.hasAudio;
    return p;
}

/// Unlocked, clip-accepting video tracks, bottom layer (V1) LAST — the
/// timeline stacks higher indices underneath.
std::vector<Track*> assemblyVideoTracks(Timeline& tl)
{
    std::vector<Track*> out;
    for (size_t i = 0; i < tl.trackCount(); ++i) {
        Track* t = tl.track(i);
        if (t && t->type() == TrackType::Video && !t->isDivider() && !t->isCaptionTrack() && !t->isLocked())
            out.push_back(t);
    }
    return out;
}

Track* assemblyAudioTrack(Timeline& tl)
{
    for (size_t i = 0; i < tl.trackCount(); ++i) {
        Track* t = tl.track(i);
        if (t && t->type() == TrackType::Audio && !t->isDivider() && !t->isLocked()) return t;
    }
    return tl.addAudioTrack("A1");
}

} // namespace

int ComfyPanel::assembleIntoTimeline(Timeline* timeline, CommandStack* stack, QString* error)
{
    if (!timeline || !m_audioSync) return 0;
    refresh();
    const auto& state = m_audioSync->pipeline();

    // ── Collect the chosen render of every shot, in script order ─────────
    struct Item { const ShowShot* shot; const RenderVersion* render; AssemblyProbe probe; };
    std::vector<Item> items;
    QStringList missing;
    for (const auto& shot : m_shots) {
        if (shot.shot.empty() || !shot.hasTake) continue;
        auto it = state.shots.find(shot.key);
        const RenderVersion* rv = it != state.shots.end() ? ComfyPanel::usableRender(it->second) : nullptr;
        if (!rv || !QFileInfo::exists(QString::fromStdString(rv->file))) {
            missing << QString::fromStdString(shot.dialogue).left(40);
            continue;
        }
        AssemblyProbe probe = probeAssemblyMedia(rv->file);
        if (!probe.ok || probe.durationSec <= 0.0) {
            missing << QString::fromStdString(shot.dialogue).left(40);
            continue;
        }
        items.push_back({&shot, rv, probe});
    }
    if (items.empty()) {
        if (error) *error = QStringLiteral("No rendered shots yet. Press RENDER ALL first.");
        return 0;
    }

    const AssemblyProbe bgProbe = state.background.empty() ? AssemblyProbe{} : probeAssemblyMedia(state.background);

    // ── Staged build (nothing visible changes until it all succeeded) ────
    auto before = timeline->clone();
    auto staged = timeline->clone();
    Timeline& tl = *staged;

    // Start after everything already on the sequence.
    int64_t start = 0;
    for (size_t i = 0; i < tl.trackCount(); ++i) {
        const Track* t = tl.track(i);
        if (!t) continue;
        for (size_t c = 0; c < t->clipCount(); ++c)
            if (const Clip* clip = t->clip(c))
                start = std::max(start, clip->timelineIn() + clip->duration());
    }
    if (start > 0) start += static_cast<int64_t>(std::llround(state.segmentGapSec * kTicksPerSecond));

    std::vector<AssemblyShot> layout;
    for (const auto& item : items)
        layout.push_back({item.shot->segment, item.shot->shot == "CLOSE-UP",
                          item.probe.durationSec, item.render->tailPadSec,
                          item.render->trimIn, item.render->effectiveOut(item.probe.durationSec)});
    const AssemblyPlan plan = planAssembly(layout, start, state.segmentGapSec,
                                           bgProbe.still ? 0.0 : bgProbe.durationSec);

    // Tracks: backgrounds on V1 (bottom), renders on the layer above.
    auto videoTracks = assemblyVideoTracks(tl);
    while (videoTracks.size() < 2) {
        tl.addVideoTrack("");
        videoTracks = assemblyVideoTracks(tl);
    }
    Track* bgTrack = videoTracks[videoTracks.size() - 1];
    Track* shotTrack = videoTracks[videoTracks.size() - 2];
    Track* audioTrack = assemblyAudioTrack(tl);
    if (!bgTrack || !shotTrack || !audioTrack) {
        if (error) *error = QStringLiteral("Could not find or create the video/audio tracks.");
        return 0;
    }

    for (const auto& bg : plan.backgrounds) {
        const std::string& path = state.background;
        const AssemblyProbe& probe = bgProbe;
        if (path.empty() || !probe.ok) continue;
        // Full size under WIDE shots; zoomed under CLOSE-UPs so it looks as
        // close as the talent does (auto = that recording's CU / WIDE framing).
        double zoom = 1.0;
        if (bg.closeUp) {
            zoom = state.closeUpBackgroundZoom;
            if (zoom <= 0.0) {
                const ShowShot& s = *items[bg.firstShot].shot;
                const double wide = state.framingFor(s.key, s.take.sourceFile, false).scale;
                const double cu = state.framingFor(s.key, s.take.sourceFile, true).scale;
                zoom = wide > 0.0 ? cu / wide : 1.0;
            }
        }
        auto clip = std::make_unique<VideoClip>(path);
        clip->scaleX().setDefaultValue(static_cast<float>(zoom));
        clip->scaleY().setDefaultValue(static_cast<float>(zoom));
        clip->setTimelineIn(bg.timelineIn);
        clip->setDuration(bg.duration);
        clip->setSourceIn(bg.sourceIn);
        clip->setSourceDuration(probe.still ? 0 : static_cast<int64_t>(std::llround(probe.durationSec * kTicksPerSecond)));
        if (probe.fps > 0) clip->setSourceFps(probe.fps);
        if (probe.width > 0) clip->setSourceResolution(static_cast<uint32_t>(probe.width), static_cast<uint32_t>(probe.height));
        clip->setLabel(pathToUtf8(utf8ToPath(path).stem()));
        bgTrack->addClip(std::move(clip));
    }

    int placed = 0;
    for (const auto& ps : plan.shots) {
        const Item& item = items[ps.index];
        const ShowShot& shot = *item.shot;
        const std::string& file = item.render->file;
        const int64_t sourceLen = static_cast<int64_t>(std::llround(item.probe.durationSec * kTicksPerSecond));
        const QString label = QStringLiteral("%1 · %2 · %3")
            .arg(static_cast<int>(&shot - m_shots.data()) + 1, 3, 10, QLatin1Char('0'))
            .arg(QString::fromStdString(shot.segment),
                 shot.shot == "CLOSE-UP" ? QStringLiteral("CU") : QString::fromStdString(shot.shot));

        // The render's own in point (manual trim on SHOTS); out = in + duration.
        const int64_t renderIn = static_cast<int64_t>(std::llround(std::max(0.0, item.render->trimIn) * kTicksPerSecond));
        auto vc = std::make_unique<VideoClip>(file);
        vc->setTimelineIn(ps.timelineIn);
        vc->setSourceIn(renderIn);
        vc->setDuration(ps.duration);
        vc->setSourceDuration(sourceLen);
        if (item.probe.fps > 0) vc->setSourceFps(item.probe.fps);
        vc->setSourceResolution(static_cast<uint32_t>(item.probe.width), static_cast<uint32_t>(item.probe.height));
        vc->setLabel(label.toStdString());
        const FillWidthPlacement fit = fillWidthBottom(item.probe.width, item.probe.height);
        vc->scaleX().setDefaultValue(fit.scale);
        vc->scaleY().setDefaultValue(fit.scale);
        vc->positionY().setDefaultValue(fit.posYPx);
        vc->effects().addEffect(std::make_unique<ChromaKey>());   // SCAIL/ToonOut background is pure green
        const uint64_t linkId = vc->id();
        if (item.probe.hasAudio) vc->setLinkId(linkId);
        shotTrack->addClip(std::move(vc));

        if (item.probe.hasAudio) {
            auto ac = std::make_unique<AudioClip>(file);
            ac->setTimelineIn(ps.timelineIn);
            ac->setSourceIn(renderIn);
            ac->setDuration(ps.duration);
            ac->setSourceDuration(sourceLen);
            ac->setLabel(label.toStdString());
            ac->setLinkId(linkId);
            audioTrack->addClip(std::move(ac));
        }
        ++placed;
    }

    for (const auto& m : plan.markers)
        tl.addMarker(m.time, m.label, 0xFF40A0FF);

    // ── Apply as one undo step ───────────────────────────────────────────
    auto after = staged->clone();
    timeline->restoreFrom(*staged);
    if (stack) {
        auto beforeShared = std::shared_ptr<Timeline>(std::move(before));
        auto afterShared = std::shared_ptr<Timeline>(std::move(after));
        stack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Assemble SCAIL shots",
            [timeline, afterShared]() { timeline->restoreFrom(*afterShared); },
            [timeline, beforeShared]() { timeline->restoreFrom(*beforeShared); }));
    }

    if (!missing.isEmpty() && error)
        *error = QStringLiteral("%1 line(s) have no render yet and were skipped:\n• %2")
                     .arg(missing.size()).arg(missing.mid(0, 12).join(QStringLiteral("\n• ")));
    spdlog::info("ComfyPanel: assembled {} shot(s), {} background clip(s), {} marker(s)",
                 placed, plan.backgrounds.size(), plan.markers.size());
    return placed;
}

} // namespace rt
