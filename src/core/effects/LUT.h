/*
 * LUT — 3D Look-Up Table color grading effect.
 *
 * Loads .cube (Resolve/Adobe) LUT files and applies them via compute shader.
 * Supports intensity parameter for blending between original and graded.
 */

#pragma once

#include "effects/Effect.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

/// One loaded .cube, immutable once published. Render snapshots and GPU
/// texture caches share it; reloading the effect publishes a new cube.
struct LutCube
{
    int size{0};               ///< dimension (e.g. 33 for 33x33x33)
    std::vector<float> data;   ///< size^3 x 3 floats (RGB), R varies fastest
    uint64_t id{0};            ///< unique per load: the GPU texture cache key
};

class LUT : public Effect
{
public:
    LUT();
    ~LUT() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;
protected:
    void copyExtraStateFrom(const Effect& src) override;
public:

    enum Param : size_t {
        Intensity = 0,  // 0–1 (blend between original and LUT-graded)
        ParamCount
    };

    // ── LUT data ────────────────────────────────────────────────────────

    /// Load a .cube LUT file from disk.
    bool loadCubeFile(const std::string& path);

    /// The loaded cube, or null. Safe from any thread: a reload publishes
    /// a new cube and whoever holds the old one keeps it alive.
    [[nodiscard]] std::shared_ptr<const LutCube> cube() const noexcept
    {
        return m_cube.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::string& lutPath() const noexcept { return m_lutPath; }

    [[nodiscard]] bool hasLUT() const noexcept { return cube() != nullptr; }

    // Retain loaded data in undo history, independent of the source file.
    struct Source {
        std::string path;
        std::shared_ptr<const LutCube> cube;
    };
    [[nodiscard]] Source source() const { return {m_lutPath, cube()}; }
    void setSource(const Source& source) {
        m_lutPath = source.path;
        m_cube.store(source.cube, std::memory_order_release);
    }

private:
    std::atomic<std::shared_ptr<const LutCube>> m_cube;
    std::string m_lutPath;
};

} // namespace rt
