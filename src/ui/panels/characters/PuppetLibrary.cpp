/*
 * PuppetLibrary.cpp — on-disk model for PNG puppet characters.
 */

#include "panels/characters/PuppetLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <spdlog/spdlog.h>

namespace rt {
namespace puppetlib {

namespace {
constexpr const char* kRootDir      = "assets/png_characters";
constexpr const char* kManifestName = "puppet.json";
} // namespace

const std::array<const char*, kFaceCount>& faceKeys()
{
    static const std::array<const char*, kFaceCount> k = {
        "closed_open", "closed_closed", "open_open", "open_closed"
    };
    return k;
}

const std::array<QString, kFaceCount>& faceLabels()
{
    static const std::array<QString, kFaceCount> k = {
        QStringLiteral("Mouth closed / Eyes open  (resting)"),
        QStringLiteral("Mouth closed / Eyes closed  (blink)"),
        QStringLiteral("Mouth open / Eyes open  (talking)"),
        QStringLiteral("Mouth open / Eyes closed  (talk + blink)")
    };
    return k;
}

QString puppetsRootDir()
{
    return QString::fromLatin1(kRootDir);
}

QString manifestPath(const QString& folderName)
{
    return puppetsRootDir() + QStringLiteral("/") + folderName +
           QStringLiteral("/") + QString::fromLatin1(kManifestName);
}

QStringList listPuppetFolders()
{
    QStringList out;
    QDir root(puppetsRootDir());
    if (!root.exists()) return out;
    const QFileInfoList dirs =
        root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& d : dirs) {
        if (QFile::exists(manifestPath(d.fileName())))
            out << d.fileName();
    }
    return out;
}

bool load(const QString& folderName, PuppetManifest& out)
{
    QFile f(manifestPath(folderName));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        spdlog::warn("PuppetLibrary: invalid manifest for '{}': {}",
                     folderName.toStdString(), err.errorString().toStdString());
        return false;
    }

    const QJsonObject root = doc.object();
    out = PuppetManifest{};
    out.folderName  = folderName;
    out.displayName = root.value(QStringLiteral("name")).toString(folderName);

    const QJsonObject variants = root.value(QStringLiteral("variants")).toObject();
    // Preserve creation order when present, else fall back to JSON key order.
    QStringList order;
    const QJsonArray orderArr = root.value(QStringLiteral("variantOrder")).toArray();
    for (const auto& v : orderArr) order << v.toString();
    for (const QString& key : variants.keys())
        if (!order.contains(key)) order << key;

    for (const QString& vName : order) {
        if (!variants.contains(vName)) continue;
        const QJsonObject vobj = variants.value(vName).toObject();
        PuppetVariant variant;
        for (int i = 0; i < kFaceCount; ++i)
            variant.faces[static_cast<size_t>(i)] =
                vobj.value(QString::fromLatin1(faceKeys()[static_cast<size_t>(i)])).toString();
        out.variants.insert(vName, variant);
        out.variantOrder << vName;
    }

    return !out.variantOrder.isEmpty();
}

bool save(const PuppetManifest& m)
{
    if (m.folderName.isEmpty()) return false;

    QDir().mkpath(puppetsRootDir() + QStringLiteral("/") + m.folderName);

    QJsonObject root;
    root.insert(QStringLiteral("name"), m.displayName);

    QJsonArray orderArr;
    for (const QString& v : m.variantOrder) orderArr.append(v);
    root.insert(QStringLiteral("variantOrder"), orderArr);

    QJsonObject variants;
    for (const QString& vName : m.variantOrder) {
        const auto it = m.variants.constFind(vName);
        if (it == m.variants.constEnd()) continue;
        QJsonObject vobj;
        for (int i = 0; i < kFaceCount; ++i)
            vobj.insert(QString::fromLatin1(faceKeys()[static_cast<size_t>(i)]),
                        it->faces[static_cast<size_t>(i)]);
        variants.insert(vName, vobj);
    }
    root.insert(QStringLiteral("variants"), variants);

    QFile f(manifestPath(m.folderName));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        spdlog::warn("PuppetLibrary: cannot write manifest for '{}'",
                     m.folderName.toStdString());
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

bool remove(const QString& folderName)
{
    if (folderName.isEmpty()) return false;
    QDir dir(puppetsRootDir() + QStringLiteral("/") + folderName);
    if (!dir.exists()) return true;
    return dir.removeRecursively();
}

bool removeVariant(const QString& folderName, const QString& variant)
{
    if (folderName.isEmpty() || variant.isEmpty()) return false;

    PuppetManifest m;
    if (!load(folderName, m)) return false;

    if (!m.variants.contains(variant)) return false;

    // Last variant → delete the whole puppet.
    if (m.variantOrder.size() <= 1)
        return remove(folderName);

    m.variants.remove(variant);
    m.variantOrder.removeAll(variant);

    // Remove the variant's image subfolder.
    QDir vdir(puppetsRootDir() + QStringLiteral("/") + folderName +
              QStringLiteral("/") + variant);
    if (vdir.exists())
        vdir.removeRecursively();

    return save(m);
}

bool renameVariantKey(const QString& folderName, const QString& oldKey,
                      const QString& newKey)
{
    if (folderName.isEmpty() || oldKey.isEmpty() || newKey.isEmpty()) return false;
    if (oldKey == newKey) return true;

    PuppetManifest m;
    if (!load(folderName, m)) return false;
    if (!m.variants.contains(oldKey)) return false;
    if (m.variants.contains(newKey))  return false;  // collision

    // Re-key ONLY — do NOT move image folders.  Face paths in the manifest are
    // explicit per-face and every consumer reads them directly, so the on-disk
    // folder name need not match the key.  (Moving directories here was fragile:
    // e.g. renaming an outfit re-keys "Dress" -> "Dress/Dress", whose move is a
    // dir-into-its-own-subdir no-op that left the manifest pointing at a path
    // with no files → blank thumbnails/images.)  Keeping the existing paths is
    // always safe; newly added faces simply get written under the new key.
    PuppetVariant var = m.variants.value(oldKey);
    const int idx = m.variantOrder.indexOf(oldKey);
    m.variants.remove(oldKey);
    m.variants.insert(newKey, var);
    if (idx >= 0) m.variantOrder[idx] = newKey;
    else          m.variantOrder << newKey;

    return save(m);
}

QString makeVariantKey(const QString& outfit, const QString& action)
{
    return outfit + QStringLiteral("/") + action;
}

void splitVariantKey(const QString& key, QString& outfit, QString& action)
{
    const int bar = key.indexOf(QLatin1Char('/'));
    if (bar < 0) {
        outfit = QStringLiteral("Default");
        action = key;
    } else {
        outfit = key.left(bar);
        action = key.mid(bar + 1);
    }
}

QStringList listOutfits(const PuppetManifest& m)
{
    QStringList out;
    for (const QString& key : m.variantOrder) {
        QString o, a;
        splitVariantKey(key, o, a);
        if (!out.contains(o)) out << o;
    }
    return out;
}

QStringList listActions(const PuppetManifest& m, const QString& outfit)
{
    QStringList out;
    for (const QString& key : m.variantOrder) {
        QString o, a;
        splitVariantKey(key, o, a);
        if (o == outfit) out << a;
    }
    return out;
}

QString resolveVariantKey(const PuppetManifest& m, const QString& outfit,
                          const QString& action)
{
    for (const QString& key : m.variantOrder) {
        QString o, a;
        splitVariantKey(key, o, a);
        if (o == outfit && a == action) return key;
    }
    return {};
}

QString sanitizeFolderName(const QString& displayName)
{
    QString s = displayName.trimmed();
    s.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 _\\-]")), QString());
    s.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
    if (s.isEmpty()) s = QStringLiteral("Puppet");
    return s;
}

QString importFaceImage(const QString& folderName, const QString& variant,
                        int faceIndex, const QString& sourcePngPath)
{
    if (folderName.isEmpty() || sourcePngPath.isEmpty())
        return {};
    if (faceIndex < 0 || faceIndex >= kFaceCount)
        return {};

    const QString destDir = puppetsRootDir() + QStringLiteral("/") + folderName +
                            QStringLiteral("/") + variant;
    QDir().mkpath(destDir);

    const QString destRel = destDir + QStringLiteral("/") +
                            QString::fromLatin1(faceKeys()[static_cast<size_t>(faceIndex)]) +
                            QStringLiteral(".png");

    // Overwrite any previous image for this face/variant.
    if (QFile::exists(destRel))
        QFile::remove(destRel);
    if (!QFile::copy(sourcePngPath, destRel)) {
        spdlog::warn("PuppetLibrary: failed to copy '{}' -> '{}'",
                     sourcePngPath.toStdString(), destRel.toStdString());
        return {};
    }
    return destRel;
}

} // namespace puppetlib
} // namespace rt
