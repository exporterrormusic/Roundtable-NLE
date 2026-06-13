/*
 * AudioStreamLabels.h — shared formatting for the audio-stream picker.
 *
 * Header-only so both the Properties-panel combo and the timeline right-click
 * "Audio Stream" submenu format stream labels identically. Keep this the ONE
 * place that turns an AudioStreamDesc into a human label.
 */

#pragma once

#include "audio/AudioFile.h"   // rt::AudioStreamDesc

#include <QString>
#include <QObject>   // QObject::tr

namespace rt {

/// Human label for one audio stream, e.g. "Audio 1 — stereo · eng" or
/// "Audio 2 — mono · lav (default)". Ordinals are shown 1-based to match how
/// editors number tracks; the stored value remains the 0-based ordinal.
inline QString audioStreamLabel(const AudioStreamDesc& d)
{
    QString channels;
    switch (d.channels) {
        case 1:  channels = QObject::tr("mono");   break;
        case 2:  channels = QObject::tr("stereo"); break;
        case 6:  channels = QStringLiteral("5.1");  break;
        case 8:  channels = QStringLiteral("7.1");  break;
        default: channels = QObject::tr("%1 ch").arg(d.channels); break;
    }

    QString label = QObject::tr("Audio %1 — %2").arg(d.ordinal + 1).arg(channels);

    // Prefer an explicit title, else the language tag, as a disambiguator.
    const QString tag = !d.title.empty()    ? QString::fromStdString(d.title)
                      : !d.language.empty() ? QString::fromStdString(d.language)
                                            : QString();
    if (!tag.isEmpty())
        label += QStringLiteral(" · ") + tag;
    if (d.isDefault)
        label += QStringLiteral(" ") + QObject::tr("(default)");
    return label;
}

} // namespace rt
