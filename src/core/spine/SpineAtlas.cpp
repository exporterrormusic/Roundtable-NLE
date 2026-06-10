/*
 * SpineAtlas.cpp — Atlas loading via spine-cpp with dummy TextureLoader.
 */

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/SpineAtlas.h"

#include <spine/Atlas.h>
#include <spine/TextureLoader.h>
#include <spine/Extension.h>

#include "PathUtils.h"
#include <spdlog/spdlog.h>

#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

// ─── Dummy texture loader (no GPU at core layer) ────────────────────────────
namespace {

class DummyTextureLoader : public spine::TextureLoader {
public:
    void load(spine::AtlasPage& page, const spine::String& path) override
    {
        // Store the page dimensions from the atlas file; actual texture is
        // created later by the GPU renderer.  We just set a non-null pointer
        // so spine-cpp knows the page is "loaded".
        page.texture = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
        spdlog::debug("SpineAtlas: page '{}' ({}x{}, pma={})",
                       path.buffer(), page.width, page.height, page.pma);
    }

    void unload(void* /*texture*/) override
    {
        // Nothing to free — dummy pointer
    }
};

} // anonymous namespace

// ─── Provide spine-cpp's required getDefaultExtension ───────────────────────
// spine-cpp calls this when SpineExtension::getInstance() is null.
// We only need one global instance.
spine::SpineExtension* spine::getDefaultExtension()
{
    static spine::DefaultSpineExtension ext;
    return &ext;
}

namespace rt {

// ─── Deleter ────────────────────────────────────────────────────────────────
void SpineAtlas::SpineAtlasDeleter::operator()(spine::Atlas* p) const
{
    delete p;
}

// ─── Constructors / Destructor ──────────────────────────────────────────────
SpineAtlas::SpineAtlas() = default;
SpineAtlas::~SpineAtlas() = default;

SpineAtlas::SpineAtlas(SpineAtlas&&) noexcept = default;
SpineAtlas& SpineAtlas::operator=(SpineAtlas&&) noexcept = default;

// ─── Load from file ─────────────────────────────────────────────────────────
bool SpineAtlas::load(const std::string& atlasPath)
{
    fs::path p(atlasPath);
    if (!fs::exists(p)) {
        spdlog::error("SpineAtlas: file not found: {}", atlasPath);
        return false;
    }

    m_directory = pathToUtf8(p.parent_path());

    // Destroy old atlas FIRST while old textureLoader is still alive,
    // because ~Atlas calls textureLoader->unload() for each page.
    m_atlas.reset();
    m_pages.clear();
    m_regions.clear();

    // Create texture loader
    m_textureLoader = std::make_unique<DummyTextureLoader>();

    // Read file ourselves so we can strip a UTF-8 BOM before spine-cpp parses it.
    // Some atlas files (e.g. c900_00.atlas) start with a BOM which spine-cpp
    // bakes into the texture page name, causing texture loads to fail.
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) {
        spdlog::error("SpineAtlas: cannot open file: {}", atlasPath);
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    // Strip UTF-8 BOM (EF BB BF) if present
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    auto* atlas = new spine::Atlas(
        content.data(), static_cast<int>(content.size()),
        m_directory.c_str(),
        m_textureLoader.get()
    );

    if (atlas->getPages().size() == 0) {
        spdlog::error("SpineAtlas: failed to parse atlas: {}", atlasPath);
        delete atlas;
        return false;
    }

    m_atlas.reset(atlas);
    extractMetadata();

    spdlog::info("SpineAtlas: loaded '{}' — {} pages, {} regions",
                 pathToUtf8(p.filename()), m_pages.size(), m_regions.size());
    return true;
}

// ─── Load from memory ───────────────────────────────────────────────────────
bool SpineAtlas::loadFromMemory(const char* data, int length, const std::string& dir)
{
    m_directory = dir;

    // Destroy old atlas FIRST while old textureLoader is still alive
    m_atlas.reset();
    m_pages.clear();
    m_regions.clear();

    m_textureLoader = std::make_unique<DummyTextureLoader>();

    auto* atlas = new spine::Atlas(
        data, length,
        dir.c_str(),
        m_textureLoader.get()
    );

    if (atlas->getPages().size() == 0) {
        spdlog::error("SpineAtlas: failed to parse atlas from memory");
        delete atlas;
        return false;
    }

    m_atlas.reset(atlas);
    extractMetadata();

    spdlog::info("SpineAtlas: loaded from memory — {} pages, {} regions",
                 m_pages.size(), m_regions.size());
    return true;
}

// ─── Extract metadata from spine atlas objects ──────────────────────────────
void SpineAtlas::extractMetadata()
{
    m_pages.clear();
    m_regions.clear();

    if (!m_atlas) return;

    // Pages
    auto& pages = m_atlas->getPages();
    for (size_t i = 0; i < pages.size(); ++i) {
        auto* page = pages[i];
        AtlasPageInfo info;

        // When createTexture=true (our default), texturePath is NOT set by
        // spine-cpp.  Fall back to page->name (the texture filename) and
        // prepend the directory ourselves.
        const char* tpBuf = page->texturePath.buffer();
        if (tpBuf && tpBuf[0] != '\0') {
            info.texturePath = tpBuf;
        } else {
            const char* nameBuf = page->name.buffer();
            if (nameBuf && nameBuf[0] != '\0') {
                // Store just the filename — loadTextures prepends the directory
                info.texturePath = nameBuf;
            }
        }

        info.width       = page->width;
        info.height      = page->height;
        info.pma         = page->pma;
        m_pages.push_back(std::move(info));
    }

    // Regions
    auto& regions = m_atlas->getRegions();
    for (size_t i = 0; i < regions.size(); ++i) {
        auto* region = regions[i];
        AtlasRegionInfo info;
        info.name     = region->name.buffer();

        // Find which page this region belongs to
        for (size_t pi = 0; pi < pages.size(); ++pi) {
            if (region->page == pages[pi]) {
                info.pageIndex = static_cast<int>(pi);
                break;
            }
        }

        info.x      = region->x;
        info.y      = region->y;
        info.width  = region->width;
        info.height = region->height;
        info.originalWidth  = static_cast<int>(region->originalWidth);
        info.originalHeight = static_cast<int>(region->originalHeight);
        info.offsetX = static_cast<int>(region->offsetX);
        info.offsetY = static_cast<int>(region->offsetY);
        info.rotate  = (region->degrees != 0);

        m_regions.push_back(std::move(info));
    }
}

// ─── Find region by name ────────────────────────────────────────────────────
const AtlasRegionInfo* SpineAtlas::findRegion(const std::string& name) const
{
    for (auto& r : m_regions) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE

