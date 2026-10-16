/*
 * LUT.cpp — 3D Look-Up Table effect implementation.
 * Sprint 3: Effects & Color
 *
 * Parses .cube files (Adobe/Resolve standard format).
 */

#include "effects/LUT.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>

namespace rt {

LUT::LUT()
    : Effect(EffectType::LUT)
{
    addParam("Intensity", 1.0f, 0.0f, 1.0f);
}

std::unique_ptr<Effect> LUT::clone() const
{
    auto copy = std::make_unique<LUT>();
    copy->m_enabled  = m_enabled;
    copy->m_cube.store(cube(), std::memory_order_release);   // immutable: share it
    copy->m_lutPath  = m_lutPath;
    for (size_t i = 0; i < m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

bool LUT::loadCubeFile(const std::string& path)
{
    std::ifstream file(utf8ToPath(path));
    if (!file.is_open()) {
        spdlog::error("LUT: cannot open .cube file: {}", path);
        return false;
    }

    int size = 0;
    std::vector<float> data;
    std::string line;

    while (std::getline(file, line)) {
        // Strip carriage return
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#')
            continue;

        // Parse keywords
        if (line.rfind("TITLE", 0) == 0)
            continue;
        if (line.rfind("DOMAIN_MIN", 0) == 0)
            continue;
        if (line.rfind("DOMAIN_MAX", 0) == 0)
            continue;

        if (line.rfind("LUT_3D_SIZE", 0) == 0) {
            // The size follows the keyword. Searching the whole line for its
            // first digit found the "3" of "3D", so every real cube (33, 65)
            // was read as size 3 and rejected.
            size = std::atoi(line.c_str() + std::strlen("LUT_3D_SIZE"));
            continue;
        }

        // Skip 1D LUT lines
        if (line.rfind("LUT_1D_SIZE", 0) == 0) {
            spdlog::warn("LUT: 1D LUT not supported, only 3D");
            return false;
        }

        // Parse data line: "R G B"
        float r, g, b;
        std::istringstream iss(line);
        if (iss >> r >> g >> b) {
            data.push_back(std::clamp(r, 0.0f, 1.0f));
            data.push_back(std::clamp(g, 0.0f, 1.0f));
            data.push_back(std::clamp(b, 0.0f, 1.0f));
        }
    }

    if (size < 2 || size > 256) {
        spdlog::error("LUT: invalid LUT_3D_SIZE {} in {}", size, path);
        return false;
    }

    const size_t expected = static_cast<size_t>(size) * size * size * 3;
    if (data.size() != expected) {
        spdlog::error("LUT: expected {} values but got {} in {}", expected, data.size(), path);
        return false;
    }

    static std::atomic<uint64_t> s_nextCubeId{1};
    auto cube = std::make_shared<LutCube>();
    cube->size = size;
    cube->data = std::move(data);
    cube->id   = s_nextCubeId.fetch_add(1, std::memory_order_relaxed);
    m_cube.store(std::move(cube), std::memory_order_release);
    m_lutPath = path;

    spdlog::info("LUT: loaded {}x{}x{} 3D LUT from {}", size, size, size, path);
    return true;
}

} // namespace rt
