/*
 * FrameSignatureLog.cpp — see FrameSignatureLog.h.
 */

#include "diag/FrameSignatureLog.h"
#include "diag/FrameSignature.h"
#include "GpuContext.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace rt {

struct FrameSignatureLog::Impl {
    std::mutex     mtx;
    FrameSignature sig;
    bool           sigTried{false};
    bool           sigReady{false};
    std::ofstream  csv;
    bool           headerWritten{false};
};

FrameSignatureLog::FrameSignatureLog() {
    const char* env = std::getenv("ROUNDTABLE_FRAMEHASH");
    if (!env || env[0] == '\0') return;   // disabled — stays a no-op

    m_impl = new Impl();
    std::string path = env;
    if (path == "1" || path == "on" || path == "true")
        path = "framehash.csv";
    m_impl->csv.open(path, std::ios::out | std::ios::trunc);
    if (!m_impl->csv) {
        spdlog::error("FrameSignatureLog: cannot open {}", path);
        delete m_impl; m_impl = nullptr;
        return;
    }
    m_enabled = true;
    spdlog::warn("FrameSignatureLog: ENABLED -> {} (A/B render-path harness)", path);
}

FrameSignatureLog::~FrameSignatureLog() {
    if (m_impl) {
        if (m_impl->sigReady) m_impl->sig.destroy();
        delete m_impl;
    }
}

FrameSignatureLog& FrameSignatureLog::get() {
    static FrameSignatureLog instance;
    return instance;
}

void FrameSignatureLog::capture(const char* tag, int64_t frame,
                                VkImageView view, VkSampler sampler,
                                uint32_t width, uint32_t height,
                                VkImageLayout layout) {
    if (!m_enabled || !m_impl) return;
    if (view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE) return;

    std::lock_guard<std::mutex> lk(m_impl->mtx);

    if (!m_impl->sigTried) {
        m_impl->sigTried = true;
        m_impl->sigReady = m_impl->sig.init(GpuContext::get());
        if (!m_impl->sigReady)
            spdlog::error("FrameSignatureLog: FrameSignature init failed; disabling");
    }
    if (!m_impl->sigReady) { m_enabled = false; return; }

    std::array<uint32_t, 8> s{};
    if (!m_impl->sig.compute(view, sampler, width, height, s, layout))
        return;

    if (!m_impl->headerWritten) {
        m_impl->csv << "tag,frame,width,height,s0,s1,s2,s3,s4,s5,s6,s7\n";
        m_impl->headerWritten = true;
    }
    m_impl->csv << tag << ',' << frame << ',' << width << ',' << height;
    for (uint32_t v : s) m_impl->csv << ',' << v;
    m_impl->csv << '\n';
    m_impl->csv.flush();
}

} // namespace rt
