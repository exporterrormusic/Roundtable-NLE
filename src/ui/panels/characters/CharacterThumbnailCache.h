/*
 * CharacterThumbnailCache.h — Persistent on-disk character thumbnail cache.
 *
 * When a character is downloaded, a single frame of the default idle pose
 * is rendered via Spine's software rasterizer and saved as a PNG.
 * makeCharacterThumbnail() checks this cache first, avoiding expensive
 * per-character Spine loads at startup.
 */

#pragma once

#include <string>

#include <QColor>
#include <QRect>

class QImage;
class QPixmap;

namespace rt {

/// Directory name for the thumbnail cache (under userDataDir/cache/)
constexpr const char* kCharacterThumbCacheDir = "character_thumbs";

// ── Shared thumbnail framing helpers ────────────────────────────────────
// Used by BOTH the persistent cache renderer below and ShotComposer's
// video-character thumbnail fallback, so a crop tweak for a character
// looks identical everywhere.

/// Per-character thumbnail crop adjustment.
/// hShift: + moves crop right (character shifts left)
/// vShift: + moves crop down (character shifts up)
/// zoomW/zoomH: > 1 = zoom out (show more), independently per axis.
struct ThumbCropAdj
{
    float hShift{0.0f};
    float vShift{0.0f};
    float zoomW{1.0f};
    float zoomH{1.0f};
};

/// Manual crop adjustment for the given character (identity if untuned).
/// `hasManualAdjustment` reports whether the character is in the tuning
/// table — the cache renderer uses this to apply tall/thin auto-defaults
/// only to untuned characters.
ThumbCropAdj thumbCropAdjustmentFor(const std::string& charName,
                                    bool* hasManualAdjustment = nullptr);

/// Head-and-shoulders crop rect from a content bounding box: 55% of the
/// content height and 80% of its width (modified by `adj`), anchored at
/// the content top and clamped to the frame.
QRect computeThumbCropRect(const QRect& content, int frameW, int frameH,
                           const ThumbCropAdj& adj);

/// Bounding box of pixels with alpha > 10 (any 4-bytes-per-pixel QImage
/// format with alpha in byte 3).  Falls back to the full frame when the
/// image is fully transparent.
QRect thumbContentBoundingBox(const QImage& img);

/// Dominant colourful hue of the image (for the thumbnail background),
/// or a dark neutral when the image has no strong hue.
QColor extractDominantThumbColor(const QImage& img);

/// Render a single idle frame for the given character and save it to the
/// persistent thumbnail cache.  Uses Spine CPU software rasterization.
/// @param charName   Folder name of the character (e.g. "Crown")
/// @param outfit     Outfit name (e.g. "default")
/// @return true if the thumbnail was rendered and saved successfully
bool renderAndCacheCharacterThumbnail(const std::string& charName,
                                      const std::string& outfit = "default");

/// Get the file path for a cached character thumbnail.
/// Does not check if the file exists.
std::string cachedCharacterThumbnailPath(const std::string& charName);

/// Check if a cached thumbnail exists for the given character.
bool hasCachedCharacterThumbnail(const std::string& charName);

/// Load a cached character thumbnail (cropped close-up with background), scaled to the given size.
/// Returns a null QPixmap if no cached thumbnail exists.
QPixmap loadCachedCharacterThumbnail(const std::string& charName, int sz);

/// Load the full-body cached render (uncropped, transparent background) for the given character.
/// Returns a null QPixmap if no cached full-body render exists.
QPixmap loadCachedCharacterFullBody(const std::string& charName);

/// Load the outfit-specific full-body cached render for the given character + outfit combination.
/// Falls back to the generic full-body render if no outfit-specific one exists.
QPixmap loadCachedCharacterOutfitFullBody(const std::string& charName,
                                           const std::string& outfit);

/// Get the file path for the full-body cached render.
std::string cachedCharacterFullBodyPath(const std::string& charName);

/// Get the file path for an outfit-specific full-body cached render.
std::string cachedCharacterOutfitFullBodyPath(const std::string& charName,
                                                const std::string& outfit);

} // namespace rt
