#include "panels/audio/VoiceReferenceLibrary.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace rt::VoiceReferenceLibrary {

namespace {

struct LibraryCache
{
    bool valid{false};
    QString directory;
    QDateTime modified;
    QVector<SavedVoiceReference> references;
};

LibraryCache& libraryCache()
{
    static LibraryCache cache;
    return cache;
}

QString referenceAudioBeside(const QDir& library, const QString& baseName)
{
    for (const auto* suffix : {".flac", ".mp3"}) {
        const QString candidate = library.filePath(baseName + QLatin1String(suffix));
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return {};
}

QVector<SavedVoiceReference> readLibrary(const QDir& library)
{
    QVector<SavedVoiceReference> result;
    const auto metadataFiles = library.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Time);
    for (const auto& info : metadataFiles) {
        const QString audio = referenceAudioBeside(library, info.completeBaseName());
        if (audio.isEmpty()) continue;
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) continue;
        const auto metadata = document.object();
        result.push_back({
            audio,
            metadata.value(QStringLiteral("character")).toString(),
            metadata.value(QStringLiteral("transcript")).toString(),
            metadata.value(QStringLiteral("duration")).toDouble()
        });
    }
    return result;
}

} // namespace

QString directory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("Voice References"));
}

QVector<SavedVoiceReference> list()
{
    const QString path = directory();
    const QFileInfo folder(path);
    if (!folder.isDir()) return {};

    // Adding or removing a file updates the folder's modification time, so
    // one stat decides whether the sidecars need to be read again.
    auto& cache = libraryCache();
    const QDateTime modified = folder.lastModified();
    if (!cache.valid || cache.directory != path || cache.modified != modified) {
        cache.references = readLibrary(QDir(path));
        cache.directory = path;
        cache.modified = modified;
        cache.valid = true;
    }
    return cache.references;
}

std::optional<SavedVoiceReference> newestFor(const QString& character)
{
    for (const auto& reference : list()) {
        if (reference.character.compare(character, Qt::CaseInsensitive) == 0)
            return reference;
    }
    return std::nullopt;
}

void invalidate()
{
    libraryCache().valid = false;
}

} // namespace rt::VoiceReferenceLibrary
