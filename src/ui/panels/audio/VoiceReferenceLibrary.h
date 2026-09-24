#pragma once

#include <QString>
#include <QVector>

#include <optional>

namespace rt {

/// A reusable voice reference saved by "Save Approved...": one audio file
/// (FLAC; older libraries may hold MP3) plus a JSON transcript sidecar.
struct SavedVoiceReference
{
    QString path;
    QString character;
    QString transcript;
    double duration{0.0};
};

namespace VoiceReferenceLibrary {

/// Application-wide folder shared by every project.
[[nodiscard]] QString directory();
/// Newest first. Cached until the folder's contents change, so callers on
/// hot UI paths do not re-read every sidecar.
[[nodiscard]] QVector<SavedVoiceReference> list();
[[nodiscard]] std::optional<SavedVoiceReference> newestFor(const QString& character);
/// Drop the cache after writing to the library.
void invalidate();

} // namespace VoiceReferenceLibrary

} // namespace rt
