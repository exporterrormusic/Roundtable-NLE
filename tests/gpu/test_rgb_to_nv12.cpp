/*
 * test_rgb_to_nv12.cpp — RgbToNv12Converter (shaders/rgb_to_nv12.comp)
 * converts the compositor's BGRA output to the NV12 the CPU path produces:
 * swscale BGRA → NV12 with BT.709, full-range RGB in, limited-range out
 * (FfmpegEncoderBase).
 */
#include <gtest/gtest.h>

#include "GpuContext.h"
#include "RgbToNv12Converter.h"
#include "decode/ThreadedSws.h"
#include "vulkan/Buffer.h"
#include "vulkan/Texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

using namespace rt;

namespace {

struct Nv12
{
    uint32_t width{0}, height{0}, stride{0};
    std::vector<uint8_t> bytes;  // Y rows, then UV rows, `stride` bytes each
    uint8_t y(uint32_t x, uint32_t yy) const { return bytes[yy * stride + x]; }
    uint8_t cb(uint32_t x, uint32_t yy) const
    {
        return bytes[stride * height + (yy / 2) * stride + (x / 2) * 2];
    }
    uint8_t cr(uint32_t x, uint32_t yy) const
    {
        return bytes[stride * height + (yy / 2) * stride + (x / 2) * 2 + 1];
    }
};

/// Colour bars on top, a smooth gradient in the middle, noise at the bottom.
std::vector<uint8_t> testPattern(uint32_t w, uint32_t h)
{
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    std::mt19937 rng(7);
    const uint8_t bars[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
                                {255, 0, 255},   {0, 0, 255},   {255, 0, 0},   {0, 0, 0}};
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &bgra[(static_cast<size_t>(y) * w + x) * 4];
            if (y < h / 3) {
                const auto* bar = bars[x * 8 / w];
                p[0] = bar[0]; p[1] = bar[1]; p[2] = bar[2];
            } else if (y < 2 * h / 3) {
                p[0] = static_cast<uint8_t>(255 * x / (w - 1));
                p[1] = static_cast<uint8_t>(255 * (y - h / 3) / (h / 3));
                p[2] = static_cast<uint8_t>(255 - 255 * x / (w - 1));
            } else {
                p[0] = static_cast<uint8_t>(rng());
                p[1] = static_cast<uint8_t>(rng());
                p[2] = static_cast<uint8_t>(rng());
            }
            p[3] = static_cast<uint8_t>(rng());  // alpha is ignored
        }
    }
    return bgra;
}

bool convertOnGpu(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, Nv12& out)
{
    auto& gpu = GpuContext::get();
    RgbToNv12Converter converter;
    if (!converter.init(gpu.device().handle())) return false;

    TextureConfig cfg;
    cfg.width  = w;
    cfg.height = h;
    cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    cfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Texture image;
    if (!image.createFromData(gpu.allocator().handle(), gpu.device().handle(), cfg,
                              bgra.data(), bgra.size(), gpu.cmdPool(), gpu.computeQueue()))
        return false;

    Buffer dst;
    const VkDeviceSize size = RgbToNv12Converter::byteSize(w, h);
    if (!dst.create(gpu.allocator().handle(), size, BufferUsage::StorageReadback))
        return false;

    VkCommandBuffer cmd = gpu.cmdPool().beginSingleTime();
    image.transitionLayout(cmd, image.layout(), VK_IMAGE_LAYOUT_GENERAL);
    const bool recorded = converter.record(cmd, image.imageView(), w, h,
                                           /*swizzledRB=*/true, dst.handle(), 0);
    gpu.cmdPool().endSingleTime(cmd, gpu.computeQueue(), &gpu.computeQueueMutex());
    if (!recorded) return false;

    out.width = w;
    out.height = h;
    out.stride = RgbToNv12Converter::stride(w);
    out.bytes.resize(static_cast<size_t>(size));
    void* mapped = dst.map();
    if (!mapped) return false;
    std::memcpy(out.bytes.data(), mapped, out.bytes.size());
    dst.unmap();
    converter.shutdown();
    return true;
}

/// The CPU path's conversion, set up as FfmpegEncoderBase sets it up.
Nv12 convertWithSwscale(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h)
{
    Nv12 out;
    out.width = w;
    out.height = h;
    out.stride = RgbToNv12Converter::stride(w);
    out.bytes.assign(static_cast<size_t>(RgbToNv12Converter::byteSize(w, h)), 0);
    SwsContext* ctx = createSwsContext(static_cast<int>(w), static_cast<int>(h), AV_PIX_FMT_BGRA,
                                       static_cast<int>(w), static_cast<int>(h), AV_PIX_FMT_NV12,
                                       SWS_BILINEAR, /*threads=*/1);
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(ctx, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
    const uint8_t* src[] = {bgra.data()};
    const int srcStride[] = {static_cast<int>(w * 4)};
    uint8_t* dst[] = {out.bytes.data(), out.bytes.data() + out.stride * h};
    const int dstStride[] = {static_cast<int>(out.stride), static_cast<int>(out.stride)};
    sws_scale(ctx, src, srcStride, 0, static_cast<int>(h), dst, dstStride);
    sws_freeContext(ctx);
    return out;
}

/// Exact BT.709 limited-range values from the shader's definition.
Nv12 convertReference(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h)
{
    Nv12 out;
    out.width = w;
    out.height = h;
    out.stride = RgbToNv12Converter::stride(w);
    out.bytes.assign(static_cast<size_t>(RgbToNv12Converter::byteSize(w, h)), 0);
    auto rgb = [&](uint32_t x, uint32_t y, double c[3]) {
        const uint8_t* p = &bgra[(static_cast<size_t>(y) * w + std::min(x, w - 1)) * 4];
        c[0] = p[2] / 255.0; c[1] = p[1] / 255.0; c[2] = p[0] / 255.0;
    };
    auto byte = [](double v) {
        return static_cast<uint8_t>(std::clamp(std::floor(v + 0.5), 0.0, 255.0));
    };
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            double c[3];
            rgb(x, y, c);
            out.bytes[y * out.stride + x] =
                byte(16.0 + 219.0 * (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]));
        }
    // Chroma: the two columns of each pair, rows y-1..y+2 weighted 1,3,3,1
    // (swscale's 2:1 bilinear vertical chroma filter), edges clamped.
    const double rowWeight[4] = {1.0 / 8, 3.0 / 8, 3.0 / 8, 1.0 / 8};
    for (uint32_t y = 0; y < h; y += 2)
        for (uint32_t x = 0; x < w; x += 2) {
            double m[3] = {0, 0, 0};
            for (int r = 0; r < 4; ++r) {
                const int yy = std::clamp(static_cast<int>(y) - 1 + r, 0, static_cast<int>(h) - 1);
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    double c[3];
                    rgb(x + dx, static_cast<uint32_t>(yy), c);
                    for (int i = 0; i < 3; ++i) m[i] += c[i] * rowWeight[r] / 2.0;
                }
            }
            uint8_t* uv = &out.bytes[out.stride * h + (y / 2) * out.stride + x];
            uv[0] = byte(128.0 + 224.0 * (-0.2126 * m[0] - 0.7152 * m[1] + 0.9278 * m[2]) / 1.8556);
            uv[1] = byte(128.0 + 224.0 * (0.7874 * m[0] - 0.7152 * m[1] - 0.0722 * m[2]) / 1.5748);
        }
    return out;
}

struct Diff { int maxY{0}, maxC{0}; double meanY{0}, meanC{0}; };

Diff compare(const Nv12& a, const Nv12& b, uint32_t y0, uint32_t y1)
{
    Diff d;
    size_t ny = 0, nc = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = 0; x < a.width; ++x) {
            const int dy = std::abs(a.y(x, y) - b.y(x, y));
            d.maxY = std::max(d.maxY, dy);
            d.meanY += dy;
            ++ny;
            if ((x & 1) == 0 && (y & 1) == 0) {
                const int dc = std::max(std::abs(a.cb(x, y) - b.cb(x, y)),
                                        std::abs(a.cr(x, y) - b.cr(x, y)));
                d.maxC = std::max(d.maxC, dc);
                d.meanC += dc;
                ++nc;
            }
        }
    d.meanY /= static_cast<double>(std::max<size_t>(ny, 1));
    d.meanC /= static_cast<double>(std::max<size_t>(nc, 1));
    return d;
}

class RgbToNv12Test : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { s_gpu = GpuContext::get().init(); }
    static void TearDownTestSuite() { if (s_gpu) GpuContext::get().shutdown(); }
    static inline bool s_gpu = false;
};

} // namespace

TEST_F(RgbToNv12Test, MatchesTheExactBt709Definition)
{
    if (!s_gpu) GTEST_SKIP() << "No Vulkan device";
    for (const auto [w, h] : {std::pair{128u, 96u}, std::pair{130u, 66u}}) {
        const auto bgra = testPattern(w, h);
        Nv12 gpu;
        ASSERT_TRUE(convertOnGpu(bgra, w, h, gpu)) << w << 'x' << h;
        const Diff d = compare(gpu, convertReference(bgra, w, h), 0, h);
        EXPECT_LE(d.maxY, 1) << w << 'x' << h;
        EXPECT_LE(d.maxC, 1) << w << 'x' << h;
    }
}

TEST_F(RgbToNv12Test, MatchesTheCpuEncoderConversion)
{
    if (!s_gpu) GTEST_SKIP() << "No Vulkan device";
    const uint32_t w = 128, h = 96;
    const auto bgra = testPattern(w, h);
    Nv12 gpu;
    ASSERT_TRUE(convertOnGpu(bgra, w, h, gpu));
    const Nv12 cpu = convertWithSwscale(bgra, w, h);
    // Same values within fixed-point rounding everywhere, noise included:
    // noise has a different chroma at every pixel, so any difference in how
    // the chroma sample is filtered would show there.
    const Diff smooth = compare(gpu, cpu, 0, 2 * h / 3);
    const Diff noise = compare(gpu, cpu, 2 * h / 3, h);
    std::cout << "RGB_TO_NV12 vs swscale: smooth maxY=" << smooth.maxY
              << " maxC=" << smooth.maxC << " meanC=" << smooth.meanC
              << " | noise maxY=" << noise.maxY << " maxC=" << noise.maxC
              << " meanC=" << noise.meanC << std::endl;
    EXPECT_LE(smooth.maxY, 1);
    EXPECT_LE(noise.maxY, 1);
    EXPECT_LE(smooth.maxC, 1);
    EXPECT_LE(noise.maxC, 1);
}

