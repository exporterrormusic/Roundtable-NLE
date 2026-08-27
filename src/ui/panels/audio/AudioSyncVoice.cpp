#include "panels/audio/AudioSync.h"

#include "PathUtils.h"
#include "ai/ScriptMatcher.h"
#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "AudioMixdown.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace rt {

void AudioSync::setVoiceGenerationPanel(QWidget* panel)
{
    if (!panel || !m_audioSidePanelStack || panel == m_voiceGenerationPage) return;
    if (m_voiceGenerationPage)
        m_audioSidePanelStack->removeWidget(m_voiceGenerationPage);
    m_voiceGenerationPage = panel;
    m_voiceGenerationPage->setObjectName(QStringLiteral("AudioTtsPanel"));
    m_audioSidePanelStack->insertWidget(5, m_voiceGenerationPage);
}

void AudioSync::showVoiceGenerationPanel()
{
    if (!m_voiceGenerationPage) return;
    showAudioSidePanel(5);
}

QVector<VoiceScriptLine> AudioSync::voiceScriptLines() const
{
    QVector<VoiceScriptLine> result;
    if (!m_script) return result;
    result.reserve(static_cast<qsizetype>(m_script->lines.size()));
    for (const auto& line : m_script->lines) {
        result.push_back({
            line.lineNumber,
            QString::fromStdString(line.character),
            QString::fromStdString(line.dialogue),
            QString::fromStdString(line.segment)
        });
    }
    return result;
}

QVector<VoiceReferenceCandidate> AudioSync::voiceReferenceCandidates() const
{
    return approvedVoiceReferenceCandidates(m_clips);
}

QVector<VoiceReferenceCandidate> AudioSync::approvedVoiceReferenceCandidates(
    const std::vector<SyncClip>& clips)
{
    QVector<VoiceReferenceCandidate> result;
    result.reserve(static_cast<qsizetype>(clips.size()));
    for (const auto& clip : clips) {
        if (clip.matchState != 2 || clip.scriptLineNumber < 0) continue;
        if (clip.sourceFile.empty() || clip.end <= clip.start) continue;
        if (!QFileInfo::exists(QString::fromStdString(clip.sourceFile))) continue;
        QString text = QString::fromStdString(
            clip.editedText.empty() ? clip.transcript : clip.editedText);
        result.push_back({
            clip.id,
            clip.scriptLineNumber,
            QString::fromStdString(clip.character),
            text,
            QString::fromStdString(clip.sourceFile),
            QString::fromStdString(clip.scriptSegment),
            clip.start,
            clip.end,
            clip.confidence
        });
    }
    return result;
}

QVector<VoiceImportedAudioTrack> AudioSync::voiceImportedAudioTracks() const
{
    QVector<VoiceImportedAudioTrack> result;
    result.reserve(static_cast<qsizetype>(m_audioPaths.size()));
    for (const auto& source : m_audioPaths) {
        VoiceImportedAudioTrack track;
        track.sourceFile = QString::fromUtf8(source);
        track.displayName = QFileInfo(track.sourceFile).fileName();
        if (const auto samples = m_audioSamples.find(source);
            samples != m_audioSamples.end() && samples->second.sampleRate > 0) {
            track.duration = static_cast<double>(samples->second.samples.size())
                           / static_cast<double>(samples->second.sampleRate);
        }
        track.approvedClipCount = static_cast<int>(std::count_if(
            m_clips.begin(), m_clips.end(), [&source](const SyncClip& clip) {
                return clip.sourceFile == source && clip.matchState == 2
                    && clip.scriptLineNumber >= 0 && clip.end > clip.start;
            }));
        result.push_back(std::move(track));
    }
    return result;
}

const AudioSampleData* AudioSync::voiceAudioSamples(const QString& path) const
{
    const auto found = m_audioSamples.find(path.toUtf8().toStdString());
    return found == m_audioSamples.end() ? nullptr : &found->second;
}

bool AudioSync::saveApprovedVoiceReference(const QString& character,
                                           QString* savedPath,
                                           QString* error) const
{
    return saveApprovedVoiceReferenceClips(m_clips, character, savedPath, error);
}

bool AudioSync::saveApprovedVoiceReferenceClips(const std::vector<SyncClip>& clips,
                                                const QString& character,
                                                QString* savedPath,
                                                QString* error)
{
    constexpr uint32_t kSampleRate = 44100;
    constexpr double kGapSeconds = 0.12;
    std::vector<float> combined;
    QStringList transcripts;
    int used = 0;

    for (const auto& clip : clips) {
        if (clip.matchState != 2 || clip.scriptLineNumber < 0
            || clip.end <= clip.start || clip.sourceFile.empty()) continue;
        if (QString::fromUtf8(clip.character).compare(character, Qt::CaseInsensitive) != 0)
            continue;

        AudioFile file;
        if (!file.open(clip.sourceFile)) continue;
        const auto channels = std::max<uint16_t>(1, file.info().channels);
        const auto startFrame = static_cast<int64_t>(clip.start * kSampleRate);
        const auto frameCount = static_cast<int64_t>((clip.end - clip.start) * kSampleRate);
        std::vector<float> region;
        const auto framesRead = file.readRegionResampled(
            startFrame, frameCount, kSampleRate, region);
        if (framesRead <= 0 || region.empty()) continue;

        if (!combined.empty())
            combined.insert(combined.end(),
                            static_cast<size_t>(kGapSeconds * kSampleRate), 0.0f);
        for (int64_t frame = 0; frame < framesRead; ++frame) {
            float mono = 0.0f;
            for (uint16_t channel = 0; channel < channels; ++channel)
                mono += region[static_cast<size_t>(frame * channels + channel)];
            combined.push_back(mono / static_cast<float>(channels));
        }
        const QString transcript = QString::fromUtf8(
            clip.editedText.empty() ? clip.transcript : clip.editedText).trimmed();
        if (!transcript.isEmpty()) transcripts.push_back(transcript);
        ++used;
    }

    if (combined.empty()) {
        if (error) *error = QObject::tr(
            "No confirmed clips are available for %1.").arg(character);
        return false;
    }

    QString safeCharacter = character.trimmed();
    safeCharacter.replace(QRegularExpression(QStringLiteral(R"([^\p{L}\p{N}_-]+)")),
                          QStringLiteral("_"));
    if (safeCharacter.isEmpty()) safeCharacter = QStringLiteral("Voice");
    const QString library = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("Voice References"));
    if (!QDir().mkpath(library)) {
        if (error) *error = QObject::tr(
            "Could not create the Voice References library.");
        return false;
    }

    const QString base = QStringLiteral("%1_%2").arg(
        safeCharacter,
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString mp3Path = QDir(library).filePath(base + QStringLiteral(".mp3"));
    MixdownResult mix;
    mix.samples = std::move(combined);
    mix.sampleRate = kSampleRate;
    mix.channels = 1;
    mix.totalFrames = static_cast<int64_t>(mix.samples.size());
    mix.duration = static_cast<double>(mix.totalFrames) / kSampleRate;
    if (!AudioMixdown::writeAudioFile(mix, utf8ToPath(mp3Path.toUtf8().toStdString()),
                                      AudioCodec::MP3, 192000)) {
        if (error) *error = QObject::tr(
            "The approved reference MP3 could not be encoded.");
        return false;
    }

    QJsonObject metadata{
        {QStringLiteral("character"), character},
        {QStringLiteral("transcript"), transcripts.join(QStringLiteral(" "))},
        {QStringLiteral("duration"), mix.duration},
        {QStringLiteral("approvedClips"), used},
        {QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate)}
    };
    QFile metadataFile(QDir(library).filePath(base + QStringLiteral(".json")));
    if (metadataFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        metadataFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));

    if (savedPath) *savedPath = mp3Path;
    return true;
}

void AudioSync::attachGeneratedAudio(const QString& path,
                                     const QString& character,
                                     const QString& dialogue,
                                     int scriptLineNumber,
                                     const QString& segment,
                                     double durationSeconds)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
    const std::string source = path.toUtf8().toStdString();
    const auto duplicate = std::find_if(m_clips.begin(), m_clips.end(),
        [&source](const SyncClip& clip) { return clip.sourceFile == source; });
    if (duplicate != m_clips.end()) return;

    int nextId = 1;
    for (const auto& clip : m_clips) nextId = std::max(nextId, clip.id + 1);

    SyncClip clip;
    clip.id = nextId;
    clip.sourceFile = source;
    clip.character = character.toStdString();
    clip.start = 0.0;
    clip.end = std::max(0.01, durationSeconds);
    clip.transcript = dialogue.toStdString();
    clip.editedText = dialogue.toStdString();
    clip.matchState = scriptLineNumber >= 0 ? 2 : 0;
    clip.confidence = scriptLineNumber >= 0 ? 1.0f : 0.0f;
    clip.scriptLineNumber = scriptLineNumber;
    clip.scriptSegment = segment.toStdString();
    m_clips.push_back(std::move(clip));

    if (std::find(m_audioPaths.begin(), m_audioPaths.end(), source) == m_audioPaths.end()) {
        m_audioPaths.push_back(source);
        addAudioFileListItem(path);
    }
    if (m_audioPath.empty()) m_audioPath = source;
    if (scriptLineNumber >= 0) m_lineAudioFile[scriptLineNumber] = source;

    m_audioImported = true;
    if (scriptLineNumber >= 0) m_syncDone = true;
    loadAudioSamples();
    populateClipList();
    populateLeftList();
    updateWorkflowState();
    updateSmartBar();
    emit voiceContextChanged();
}

bool AudioSync::syncGeneratedAudio(const QString& path,
                                   const QString& character,
                                   const QString& dialogue,
                                   int preferredScriptLine,
                                   const QString& preferredSegment,
                                   double durationSeconds)
{
    const ScriptLine* matchedLine = nullptr;
    if (m_script) {
        // A line explicitly selected in the TTS panel is authoritative only
        // when it belongs to the chosen character.
        for (const auto& line : m_script->lines) {
            if (line.lineNumber == preferredScriptLine
                && QString::fromStdString(line.character).compare(
                       character, Qt::CaseInsensitive) == 0) {
                matchedLine = &line;
                break;
            }
        }

        // Otherwise run the same ScriptMatcher text comparison used by auto
        // sync, but restrict its candidate script to the selected character.
        if (!matchedLine) {
            Script characterScript;
            for (const auto& line : m_script->lines) {
                if (QString::fromStdString(line.character).compare(
                        character, Qt::CaseInsensitive) == 0) {
                    characterScript.lines.push_back(line);
                }
            }
            if (!characterScript.lines.empty()) {
                ScriptMatcher matcher(characterScript);
                const auto [line, confidence] = matcher.matchSegment(
                    dialogue.toStdString());
                if (line && confidence >= matcher.threshold()) {
                    for (const auto& original : m_script->lines) {
                        if (original.lineNumber == line->lineNumber) {
                            matchedLine = &original;
                            break;
                        }
                    }
                }
            }
        }
    }

    attachGeneratedAudio(
        path,
        matchedLine ? QString::fromStdString(matchedLine->character) : character,
        dialogue,
        matchedLine ? matchedLine->lineNumber : -1,
        matchedLine ? QString::fromStdString(matchedLine->segment) : preferredSegment,
        durationSeconds);
    return matchedLine != nullptr;
}

bool AudioSync::auditionVoiceDraft(const QString& path)
{
    if (!m_audioEngine || path.isEmpty()) return false;
    AudioFile file;
    if (!file.open(path.toUtf8().toStdString())) return false;
    const uint16_t channels = std::max<uint16_t>(1, file.info().channels);
    const uint32_t sampleRate = std::max<uint32_t>(1, m_audioEngine->sampleRate());
    auto samples = file.readAllResampled(sampleRate);
    if (samples.empty()) return false;

    stopFilePlayback();
    stopPlayback();
    m_voiceDraftSamples = std::move(samples);

    AudioTrackSource source{};
    source.trackId = 9997;
    source.samples = m_voiceDraftSamples.data();
    source.totalFrames = static_cast<int64_t>(m_voiceDraftSamples.size() / channels);
    source.startFrame = 0;
    source.channels = channels;
    source.sampleRate = sampleRate;
    source.volume = 1.0f;

    if (!m_savedSyncClock) {
        m_savedSyncClock = m_audioEngine->syncClock();
        m_audioEngine->setSyncClock(nullptr);
    }
    m_audioEngine->setPlaybackSpeed(1.0);
    m_audioEngine->setTrackSources({source});
    m_audioEngine->seekToFrame(0);
    m_audioEngine->play();
    m_voiceDraftAuditionActive = true;
    return true;
}

void AudioSync::stopVoiceDraftAudition()
{
    if (!m_voiceDraftAuditionActive) return;
    stopPlayback();
}

} // namespace rt
