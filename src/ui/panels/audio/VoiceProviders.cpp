#include "panels/audio/VoiceProviders.h"

#include <QCoreApplication>

namespace rt {

namespace {

QString voiceText(const char* text)
{
    return QCoreApplication::translate("VoiceProviders", text);
}

QList<VoiceProvider> buildVoiceProviders()
{
    QList<VoiceProvider> providers;
#ifdef ROUNDTABLE_HAS_BREEZE
    providers.push_back({
        QStringLiteral("breeze"),
        QStringLiteral("Breeze-TTS-2"),
        voiceText("Breeze-TTS-2 (Q8 / CUDA)"),
        20.0, true, false, true, {},
        voiceText("Loading Breeze-TTS-2 Q8 on CUDA..."),
        voiceText("Breeze-TTS-2 ready. Approved clips and their automatic transcripts "
                  "are combined exactly and converted to a 24 kHz reference.")
    });
#endif
#ifdef ROUNDTABLE_HAS_OMNIVOICE
    providers.push_back({
        QStringLiteral("omnivoice"),
        QStringLiteral("OmniVoice"),
        voiceText("OmniVoice (Apache-2.0)"),
        8.0, true, true, false,
        {QStringLiteral("model.safetensors"), QStringLiteral("config.json"),
         QStringLiteral("audio_tokenizer")},
        voiceText("Loading OmniVoice..."),
        voiceText("OmniVoice ready. For the strongest clone, use a clean 3-10 second "
                  "reference and its exact transcript. Duration targeting is available.")
    });
#endif
#ifdef ROUNDTABLE_HAS_FISH_S2
    providers.push_back({
        QStringLiteral("fish-s2"),
        QStringLiteral("Fish S2 Pro"),
        voiceText("Fish S2 Pro (personal / non-commercial)"),
        20.0, false, false, false,
        {QStringLiteral("codec.pth"), QStringLiteral("config.json"),
         QStringLiteral("model.safetensors.index.json")},
        voiceText("Loading Fish S2 Pro (this can take a few minutes)..."),
        voiceText("Fish S2 Pro ready. For the strongest clone, use a clean 10-30 second "
                  "reference and its exact transcript. Close other GPU-heavy apps "
                  "before loading.")
    });
#endif
    return providers;
}

} // namespace

const QList<VoiceProvider>& voiceProviders()
{
    static const QList<VoiceProvider> providers = buildVoiceProviders();
    return providers;
}

const VoiceProvider* findVoiceProvider(const QString& key)
{
    for (const auto& provider : voiceProviders()) {
        if (provider.key == key) return &provider;
    }
    return nullptr;
}

} // namespace rt
