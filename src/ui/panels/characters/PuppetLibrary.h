/*
 * PuppetLibrary — on-disk model for Veadotube-style PNG puppet characters.
 *
 * A puppet lives under assets/png_characters/<folder>/ with a puppet.json
 * manifest describing one or more variants (expressions / costumes).  Each
 * variant is a set of 4 PNGs, one per mouth×eyes state, stored in enum order
 * matching rt::PngPuppetClip::Face.
 *
 * Paths in the manifest are stored relative to the project root (e.g.
 * "assets/png_characters/Alice/default/closed_open.png") with forward slashes
 * so projects stay portable.
 *
 * Shared by the creation/listing UI (PuppetLibraryPanel, PuppetCreatorDialog)
 * and the timeline drop handler.
 */

#pragma once

#include <array>

#include <QMap>
#include <QString>
#include <QStringList>

namespace rt {

/// One expression/costume variant: 4 face PNG paths in PngPuppetClip::Face order
/// (0=closed/open, 1=closed/closed, 2=open/open, 3=open/closed).
struct PuppetVariant
{
    std::array<QString, 4> faces{};  ///< project-relative PNG paths
};

struct PuppetManifest
{
    QString folderName;                  ///< on-disk folder under the puppets root
    QString displayName;                 ///< user-facing character name
    QStringList variantOrder;            ///< variant names in creation order
    QMap<QString, PuppetVariant> variants;
};

namespace puppetlib {

/// Number of face images per variant.
inline constexpr int kFaceCount = 4;

/// Stable JSON keys for each face, in PngPuppetClip::Face order.
const std::array<const char*, kFaceCount>& faceKeys();

/// Human-readable labels for the 4 faces (for the creator dialog), in order.
const std::array<QString, kFaceCount>& faceLabels();

/// Root directory that holds all puppet folders (project-relative).
QString puppetsRootDir();

/// Absolute path to a puppet's manifest file.
QString manifestPath(const QString& folderName);

/// List the folder names of all puppets that have a valid manifest.
QStringList listPuppetFolders();

/// Load a puppet manifest by folder name.  Returns false if missing/invalid.
bool load(const QString& folderName, PuppetManifest& out);

/// Write a puppet manifest to disk (creates the folder if needed).
bool save(const PuppetManifest& m);

/// Delete a puppet folder and everything in it.  Returns true on success.
bool remove(const QString& folderName);

/// Delete a single variant (expression/costume) from a puppet: drops it from
/// the manifest and removes its image subfolder.  If it was the last variant,
/// the whole puppet folder is removed.  Returns true on success.
bool removeVariant(const QString& folderName, const QString& variant);

/// Rename a variant key (move its image subfolder, rewrite its face paths, and
/// re-key the manifest in place).  Used to rename outfits/actions.  Returns
/// false on collision or if the source variant is missing.
bool renameVariantKey(const QString& folderName, const QString& oldKey,
                      const QString& newKey);

/// Turn a user-entered display name into a safe folder name.
QString sanitizeFolderName(const QString& displayName);

// ── Outfit / Action variant-key model ───────────────────────────────────────
// A puppet variant is addressed by a two-level key "<outfit>/<action>" so the
// editor can present a Character → Outfit → Action cascade.  Legacy puppets used
// a flat single-token variant; those are treated as outfit="Default", action=key.

/// Compose a variant key from an outfit + action.
QString makeVariantKey(const QString& outfit, const QString& action);

/// Split a variant key into outfit + action.  A slash-less (legacy) key maps to
/// outfit="Default", action=key.
void splitVariantKey(const QString& key, QString& outfit, QString& action);

/// Ordered, de-duplicated outfit names present in a manifest.
QStringList listOutfits(const PuppetManifest& m);

/// Ordered action names for a given outfit.
QStringList listActions(const PuppetManifest& m, const QString& outfit);

/// Resolve (outfit, action) to the manifest's actual raw variant key, or an
/// empty string when no such variant exists.
QString resolveVariantKey(const PuppetManifest& m, const QString& outfit,
                          const QString& action);

/// Copy a chosen source PNG into the puppet's variant folder and return the
/// resulting project-relative path.  `faceIndex` is in PngPuppetClip::Face
/// order.  Returns an empty string on failure.
QString importFaceImage(const QString& folderName, const QString& variant,
                        int faceIndex, const QString& sourcePngPath);

} // namespace puppetlib
} // namespace rt
