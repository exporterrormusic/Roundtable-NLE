// CharacterBrowserNetwork.cpp - Network/download (extracted from CharacterBrowser.cpp).

#include "panels/characters/CharacterBrowser.h"
#include "panels/characters/CharacterThumbnailCache.h"
#include "Theme.h"
#include "QtHelpers.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"


#endif

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QRegularExpression>
#include <spdlog/spdlog.h>

namespace {
    const QString NIKKE_DB_API     = QStringLiteral("https://api.github.com/repos/Nikke-db/nikke-db.github.io/contents/l2d");
    const QString NIKKE_DB_RAW     = QStringLiteral("https://raw.githubusercontent.com/Nikke-db/nikke-db.github.io/main/l2d");
    const QString NIKKE_DB_L2D_JSON = QStringLiteral("https://raw.githubusercontent.com/Nikke-db/nikke-db-vue/main/src/utils/json/l2d.json");
}

namespace rt {

void CharacterBrowser::fetchRemoteCharacterList()
{
    m_statusLabel->setText("Fetching character list from Nikke DB...");

    QNetworkRequest request{QUrl(NIKKE_DB_API)};
    request.setRawHeader("User-Agent", "ROUNDTABLE-NLE/2.0");

    auto* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            spdlog::warn("CharacterBrowser: Failed to fetch remote list: {}",
                         reply->errorString().toStdString());
            m_statusLabel->setText("Failed to fetch remote list");
            m_refreshBtn->setText(QStringLiteral("\xF0\x9F\x94\x84  Refresh"));
            m_refreshBtn->setEnabled(true);
            populateCharacterList();
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isArray()) {
            spdlog::warn("CharacterBrowser: Unexpected API response format");
            m_refreshBtn->setText(QStringLiteral("\xF0\x9F\x94\x84  Refresh"));
            m_refreshBtn->setEnabled(true);
            populateCharacterList();
            return;
        }

        m_remoteCharIds.clear();
        // Only accept directories matching character ID patterns (c###, c###_##)
        // to exclude story CGs, event scenes, backgrounds, and other non-character assets
        static QRegularExpression charIdRe(QStringLiteral("^c\\d+(_\\d+)?$"));
        for (const auto& val : doc.array()) {
            QJsonObject item = val.toObject();
            if (item.value("type").toString() == "dir") {
                QString name = item.value("name").toString();
                if (charIdRe.match(name).hasMatch())
                    m_remoteCharIds.insert(name);
            }
        }

        spdlog::info("CharacterBrowser: Found {} remote characters", m_remoteCharIds.size());

        // Also fetch l2d.json to get display names for new character IDs
        fetchRemoteL2dNames();
    });
}

void CharacterBrowser::fetchRemoteL2dNames()
{
    m_statusLabel->setText("Fetching character names from Nikke DB...");

    QNetworkRequest request{QUrl(NIKKE_DB_L2D_JSON)};
    request.setRawHeader("User-Agent", "ROUNDTABLE-NLE/2.0");

    auto* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            spdlog::warn("CharacterBrowser: Failed to fetch l2d.json: {}",
                         reply->errorString().toStdString());
            m_statusLabel->setText("Character list updated (names unavailable)");
            m_refreshBtn->setText(QStringLiteral("\xF0\x9F\x94\x84  Refresh"));
            m_refreshBtn->setEnabled(true);
            populateCharacterList();
            return;
        }

        QJsonDocument remoteDoc = QJsonDocument::fromJson(reply->readAll());
        if (!remoteDoc.isArray()) {
            spdlog::warn("CharacterBrowser: l2d.json is not an array");
            m_refreshBtn->setText(QStringLiteral("\xF0\x9F\x94\x84  Refresh"));
            m_refreshBtn->setEnabled(true);
            populateCharacterList();
            return;
        }

        // Load the local metadata file
        QJsonObject root;
        QJsonObject chars;
        {
            QFile metaFile("assets/character_metadata.json");
            metaFile.open(QIODevice::ReadOnly);
            if (metaFile.isOpen()) {
                QJsonDocument localDoc = QJsonDocument::fromJson(metaFile.readAll());
                metaFile.close();
                root = localDoc.object();
                chars = root.value("characters").toObject();
            }
        }

        // Merge entries from l2d.json that aren't already in the local metadata
        int added = 0;
        // Regex to detect outfit variant IDs: c501_01, c010_02, c9000_01, etc.
        static QRegularExpression variantRe(QStringLiteral("^(c\\d{2,4})_(\\d{2})$"));

        QJsonArray remoteArray = remoteDoc.array();

        // --- First pass: process base character entries (no _XX suffix) ---
        for (const auto& val : remoteArray) {
            QJsonObject entry = val.toObject();
            QString id   = entry.value("id").toString();
            QString name = entry.value("name").toString();
            if (id.isEmpty() || name.isEmpty()) continue;

            // Skip non-character entries (story CGs, event scenes, etc.)
            if (!id.startsWith("c")) continue;

            // Skip variant IDs in this pass — handled in second pass
            if (variantRe.match(id).hasMatch()) continue;

            if (chars.contains(id)) {
                // If existing entry has a placeholder name (raw ID), upgrade it
                QString existingName = chars[id].toObject().value("name").toString();
                if (existingName == id) {
                    QJsonObject existing = chars[id].toObject();
                    existing["name"] = name;
                    QJsonObject outfits = existing["outfits"].toObject();
                    if (outfits.contains("default")) {
                        QJsonObject def = outfits["default"].toObject();
                        def["display_name"] = name;
                        outfits["default"] = def;
                    }
                    existing["outfits"] = outfits;
                    chars[id] = existing;
                    ++added;
                    spdlog::info("CharacterBrowser: Upgraded placeholder name for {} -> {}",
                                 id.toStdString(), name.toStdString());
                }
                continue;
            }

            // Determine category: c9xxx = NPC, others = Character
            QString category = QStringLiteral("Character");
            if (id.startsWith("c9") && id.length() >= 5)
                category = QStringLiteral("NPC");

            // Build the new metadata entry
            QJsonObject outfitDefault;
            outfitDefault["display_name"] = name;
            outfitDefault["variants"] = QJsonArray({"default", "aim", "cover"});

            QJsonObject outfits;
            outfits["default"] = outfitDefault;

            QJsonObject charEntry;
            charEntry["name"] = name;
            charEntry["category"] = category;
            charEntry["has_mouth_animation"] = true;
            charEntry["outfits"] = outfits;

            chars[id] = charEntry;
            ++added;
        }

        // --- Second pass: merge outfit variants into base character entries ---
        for (const auto& val : remoteArray) {
            QJsonObject entry = val.toObject();
            QString id   = entry.value("id").toString();
            QString name = entry.value("name").toString();
            if (id.isEmpty() || name.isEmpty()) continue;

            QRegularExpressionMatch match = variantRe.match(id);
            if (!match.hasMatch()) continue;

            QString baseId    = match.captured(1);
            QString outfitKey = QStringLiteral("outfit_") + match.captured(2);

            // Ensure base character exists in metadata
            if (!chars.contains(baseId)) continue;

            QJsonObject baseEntry = chars[baseId].toObject();
            QJsonObject outfits = baseEntry["outfits"].toObject();

            if (!outfits.contains(outfitKey)) {
                QJsonObject outfitObj;
                outfitObj["display_name"] = name;
                outfitObj["variants"] = QJsonArray({"default", "aim", "cover"});
                outfits[outfitKey] = outfitObj;
                baseEntry["outfits"] = outfits;
                chars[baseId] = baseEntry;
                ++added;
                spdlog::info("CharacterBrowser: Merged outfit {} -> {} as {}",
                             id.toStdString(), baseId.toStdString(), outfitKey.toStdString());
            }

            // Remove standalone variant entry if it was previously created
            if (chars.contains(id)) {
                chars.remove(id);
                spdlog::info("CharacterBrowser: Removed standalone variant entry {}",
                             id.toStdString());
            }
        }

        // --- Clean up: remove any existing story/CG/scene entries from metadata ---
        QStringList keysToRemove;
        for (auto it = chars.begin(); it != chars.end(); ++it) {
            const QString& key = it.key();
            if (!key.startsWith("c"))
                keysToRemove.append(key);
        }
        for (const auto& key : keysToRemove)
            chars.remove(key);

        // Add placeholder entries for remote IDs not found in any data source
        // (e.g. new characters whose names haven't been added to l2d.json yet)
        int placeholders = 0;
        for (const QString& remoteId : m_remoteCharIds) {
            if (chars.contains(remoteId)) continue;
            if (!remoteId.startsWith("c")) continue;

            // Skip variant IDs — they belong as outfits on the base character
            if (variantRe.match(remoteId).hasMatch()) continue;

            QString category = QStringLiteral("Character");
            if (remoteId.startsWith("c9") && remoteId.length() >= 5)
                category = QStringLiteral("NPC");

            QJsonObject outfitDefault;
            outfitDefault["display_name"] = remoteId;
            outfitDefault["variants"] = QJsonArray({"default", "aim", "cover"});

            QJsonObject outfits;
            outfits["default"] = outfitDefault;

            QJsonObject charEntry;
            charEntry["name"] = remoteId;
            charEntry["category"] = category;
            charEntry["has_mouth_animation"] = true;
            charEntry["outfits"] = outfits;

            chars[remoteId] = charEntry;
            ++placeholders;
        }
        if (placeholders > 0) {
            added += placeholders;
            spdlog::info("CharacterBrowser: Added {} placeholder entries for unnamed remote characters",
                         placeholders);
        }

        if (added > 0) {
            root["characters"] = chars;
            QFile outFile("assets/character_metadata.json");
            if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                outFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                outFile.close();
                spdlog::info("CharacterBrowser: Added {} new characters ({} named, {} unnamed)",
                             added, added - placeholders, placeholders);
            }

            // Reload metadata cache
            m_metadataLoaded = false;
        }

        m_statusLabel->setText(QString("Updated — %1 new characters added").arg(added));
        m_refreshBtn->setText(QStringLiteral("\xF0\x9F\x94\x84  Refresh"));
        m_refreshBtn->setEnabled(true);
        populateCharacterList();
    });
}

void CharacterBrowser::downloadCharacterModel(const QString& repoPath,
                                               const QString& charName,
                                               const QString& outfitName,
                                               std::function<void(bool)> onOutfitComplete)
{
    // Resolve display name if not provided
    QString displayName = charName;
    if (displayName.isEmpty()) {
        displayName = repoPath;
        loadMetadata();
        // repoPath might be "c010" or "c010_02" — base charId is first part
        QString baseId = repoPath.contains('_') ? repoPath.left(repoPath.indexOf('_')) : repoPath;
        if (m_cachedCharacters.contains(baseId)) {
            QString name = m_cachedCharacters.value(baseId).toObject().value("name").toString();
            if (!name.isEmpty())
                displayName = name;
        }
    }

    // Sanitize for Windows filesystem (replace illegal path characters)
    for (QChar& c : displayName) {
        if (c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            c = '_';
    }
    while (displayName.endsWith('.') || displayName.endsWith(' '))
        displayName.chop(1);

    // Models are stored in the program folder's assets/characters/.
    QString targetDir = QDir(rt::downloadedCharacterAssetsDir())
                            .filePath(QString("characters/%1/%2")
                                          .arg(displayName, outfitName));

    spdlog::info("CharacterBrowser: Fetching file list from: l2d/{}", repoPath.toStdString());

    // Fetch the directory listing from GitHub API
    QString apiUrl = NIKKE_DB_API + "/" + repoPath;
    QNetworkRequest request{QUrl(apiUrl)};
    request.setRawHeader("User-Agent", "ROUNDTABLE-NLE/2.0");

    auto* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, repoPath, displayName, targetDir, onOutfitComplete = std::move(onOutfitComplete)]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            auto errStr = reply->errorString();
            spdlog::error("CharacterBrowser: Failed to list files for {}: {}",
                          repoPath.toStdString(), errStr.toStdString());
            m_statusLabel->setText("Download failed: " + errStr);
            QMessageBox::warning(this, "Download Failed",
                QString("Failed to download %1:\n%2").arg(displayName, errStr));
            m_downloadBtn->setEnabled(true);
            m_downloadProgress->setVisible(false);
            if (onOutfitComplete) onOutfitComplete(false);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isArray()) {
            spdlog::error("CharacterBrowser: Unexpected response for {}", repoPath.toStdString());
            m_statusLabel->setText("Download failed: bad response");
            QMessageBox::warning(this, "Download Failed",
                QString("Unexpected API response for %1.\nThe remote repository may have changed.").arg(displayName));
            m_downloadBtn->setEnabled(true);
            m_downloadProgress->setVisible(false);
            if (onOutfitComplete) onOutfitComplete(false);
            return;
        }

        // Collect files to download (.skel, .atlas, .png, .json)
        static const QStringList exts = {".skel", ".atlas", ".png", ".json", ".txt"};
        struct FileToDownload { QString name; QString downloadUrl; };
        std::vector<FileToDownload> files;

        // Also track subdirectories (aim, cover stances)
        QStringList subDirs;

        for (const auto& val : doc.array()) {
            QJsonObject item = val.toObject();
            QString name = item.value("name").toString();
            QString type = item.value("type").toString();

            if (type == "dir") {
                subDirs.append(name);
            } else if (type == "file") {
                for (const auto& ext : exts) {
                    if (name.endsWith(ext, Qt::CaseInsensitive)) {
                        QString url = item.value("download_url").toString();
                        if (url.isEmpty())
                            url = NIKKE_DB_RAW + "/" + repoPath + "/" + name;
                        files.push_back({name, url});
                        break;
                    }
                }
            }
        }

        spdlog::info("CharacterBrowser: Found {} files and {} subdirs for {}",
                     files.size(), subDirs.size(), repoPath.toStdString());

        if (files.empty() && subDirs.isEmpty()) {
            spdlog::warn("CharacterBrowser: No downloadable files for {}", repoPath.toStdString());
            m_statusLabel->setText("No files found for " + displayName);
            QMessageBox::warning(this, "Download",
                QString("No downloadable files found for %1 at path '%2'.")
                    .arg(displayName, repoPath));
            m_downloadBtn->setEnabled(true);
            m_downloadProgress->setVisible(false);
            if (onOutfitComplete) onOutfitComplete(false);
            return;
        }

        // Create target directory only when we have files to download
        QDir().mkpath(targetDir);

        // Shared state for tracking all downloads (main files + subdir files)
        struct DownloadState {
            int totalFiles = 0;
            int downloaded = 0;
            int failCount = 0;
            int subdirListingsPending = 0; // subdir API calls still in flight
            bool completionFired = false;
        };
        auto state = std::make_shared<DownloadState>();
        state->totalFiles = static_cast<int>(files.size());
        state->subdirListingsPending = static_cast<int>(subDirs.size());

        // Helper lambda to check if everything is done
        auto checkComplete = [this, displayName, targetDir, state, onOutfitComplete]() {
            if (state->completionFired) return;
            if (state->downloaded < state->totalFiles) return;
            if (state->subdirListingsPending > 0) return; // still waiting for subdir listings

            state->completionFired = true;
            m_downloadProgress->setVisible(false);

            if (state->failCount > 0) {
                m_statusLabel->setText(QString("Downloaded %1 (%2 failed)")
                    .arg(displayName).arg(state->failCount));
                spdlog::warn("CharacterBrowser: {} files failed for {}",
                             state->failCount, displayName.toStdString());
            } else {
                m_statusLabel->setText(QString("Downloaded %1 (%2 files)")
                    .arg(displayName).arg(state->totalFiles));
                spdlog::info("CharacterBrowser: Downloaded {} ({} files)",
                             displayName.toStdString(), state->totalFiles);
            }

            // Generate persistent thumbnail for the downloaded character
            renderAndCacheCharacterThumbnail(displayName.toStdString());

            if (onOutfitComplete)
                onOutfitComplete(state->failCount == 0);
        };

        // Download each main file
        for (const auto& f : files) {
            QString localPath = targetDir + "/" + f.name;
            spdlog::info("CharacterBrowser: Downloading {} → {}", f.downloadUrl.toStdString(), localPath.toStdString());
            downloadFile(f.downloadUrl, localPath,
                [this, state, displayName, checkComplete](bool ok) {
                    state->downloaded++;
                    if (!ok) state->failCount++;
                    int pct = (state->totalFiles > 0) ? (state->downloaded * 100 / state->totalFiles) : 100;
                    m_downloadProgress->setValue(pct);
                    checkComplete();
                });
        }

        // Download subdirectories (stances like aim, cover) — track them in shared state
        for (const auto& subDir : subDirs) {
            QString subTargetDir = targetDir + "/" + subDir;
            QDir().mkpath(subTargetDir);

            QString subApiUrl = NIKKE_DB_API + "/" + repoPath + "/" + subDir;
            QNetworkRequest subReq{QUrl(subApiUrl)};
            subReq.setRawHeader("User-Agent", "ROUNDTABLE-NLE/2.0");

            auto* subReply = m_networkManager->get(subReq);
            connect(subReply, &QNetworkReply::finished, this,
                [this, subReply, repoPath, subDir, subTargetDir, state, checkComplete]() {
                    subReply->deleteLater();
                    // Decrement pending subdir listings
                    state->subdirListingsPending--;

                    if (subReply->error() != QNetworkReply::NoError) {
                        spdlog::warn("CharacterBrowser: Failed to list subdir {}/{}",
                                     repoPath.toStdString(), subDir.toStdString());
                        checkComplete();
                        return;
                    }

                    QJsonDocument d = QJsonDocument::fromJson(subReply->readAll());
                    if (!d.isArray()) {
                        checkComplete();
                        return;
                    }

                    static const QStringList exts2 = {".skel", ".atlas", ".png", ".json", ".txt"};
                    for (const auto& val : d.array()) {
                        QJsonObject item = val.toObject();
                        if (item.value("type").toString() != "file") continue;
                        QString name = item.value("name").toString();
                        for (const auto& ext : exts2) {
                            if (name.endsWith(ext, Qt::CaseInsensitive)) {
                                QString url = item.value("download_url").toString();
                                if (url.isEmpty())
                                    url = NIKKE_DB_RAW + "/" + repoPath + "/" + subDir + "/" + name;
                                spdlog::info("CharacterBrowser: Downloading subdir file {} → {}",
                                             url.toStdString(), (subTargetDir + "/" + name).toStdString());

                                // Add to total BEFORE starting download
                                state->totalFiles++;
                                downloadFile(url, subTargetDir + "/" + name,
                                    [this, state, checkComplete](bool ok) {
                                        state->downloaded++;
                                        if (!ok) state->failCount++;
                                        int pct = (state->totalFiles > 0)
                                            ? (state->downloaded * 100 / state->totalFiles) : 100;
                                        m_downloadProgress->setValue(pct);
                                        checkComplete();
                                    });
                                break;
                            }
                        }
                    }

                    // Check complete in case subdir had no files
                    checkComplete();
                });
        }
    });
}

void CharacterBrowser::downloadFile(const QString& remotePath, const QString& localPath,
                                     std::function<void(bool)> callback)
{
    QNetworkRequest request{QUrl(remotePath)};
    request.setRawHeader("User-Agent", "ROUNDTABLE-NLE/2.0");

    auto* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this,
        [reply, localPath, cb = std::move(callback)]() {
            reply->deleteLater();

            if (reply->error() != QNetworkReply::NoError) {
                spdlog::warn("Failed to download: {}", localPath.toStdString());
                cb(false);
                return;
            }

            QFile file(localPath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(reply->readAll());
                file.close();
                cb(true);
            } else {
                spdlog::warn("Failed to write: {}", localPath.toStdString());
                cb(false);
            }
        });
}

void CharacterBrowser::resolveCharacterName(const QString& /*charId*/) const
{
    // Utility to resolve character ID to name from metadata
    // (currently unused externally, but available for future use)
}

// ═════════════════════════════════════════════════════════════════════════════
// Metadata caching (P1)
// ═════════════════════════════════════════════════════════════════════════════

void CharacterBrowser::loadMetadata()
{
    if (m_metadataLoaded) return;

    QFile metaFile(rt::userDataDir() + "/character_metadata.json");
    if (!metaFile.open(QIODevice::ReadOnly)) {
        // Fall back to bundled metadata in install directory
        metaFile.setFileName("assets/character_metadata.json");
        metaFile.open(QIODevice::ReadOnly);
    }
    if (metaFile.isOpen()) {
        QJsonDocument doc = QJsonDocument::fromJson(metaFile.readAll());
        metaFile.close();
        m_cachedCharacters = doc.object().value("characters").toObject();
        m_metadataLoaded = true;
        spdlog::info("CharacterBrowser: Cached {} character metadata entries",
                     m_cachedCharacters.size());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Context menu (F4)
// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
