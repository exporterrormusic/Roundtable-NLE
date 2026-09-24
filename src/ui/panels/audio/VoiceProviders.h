#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace rt {

/// Static description of one local TTS engine. Everything that differs per
/// engine in the UI or the worker launch lives here, so adding an engine is
/// one table row plus its Python worker class.
struct VoiceProvider
{
    QString key;                 ///< Worker/protocol id: "breeze", "omnivoice", "fish-s2"
    QString displayName;         ///< Short name for status text
    QString menuLabel;           ///< Engine combo entry
    double referenceSeconds{20.0}; ///< Automatic reference length target
    bool supportsSpeed{false};
    bool supportsDuration{false};
    bool modelIsFile{false};     ///< Model path is one file rather than a directory
    QStringList requiredModelFiles; ///< Relative to the model directory
    QString loadingStatus;
    QString readyHint;
};

/// Engines compiled into this build, in menu order (default first).
[[nodiscard]] const QList<VoiceProvider>& voiceProviders();
/// Lookup by key; nullptr when the engine is unknown or not built.
[[nodiscard]] const VoiceProvider* findVoiceProvider(const QString& key);

} // namespace rt
