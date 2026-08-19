/*
 * VulkanViewportSetup.cpp - GPU resource creation (swapchain, render pass, pipeline).
 * Split from VulkanViewport.cpp.
 */

#include "viewport/VulkanViewport.h"
#include "PathUtils.h"
#include "Theme.h"
#include "GpuContext.h"
#include "vulkan/Swapchain.h"
#include "vulkan/Texture.h"
#include "cache/FrameCache.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <QPainter>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif


namespace rt {

//  Helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

static std::vector<uint32_t> loadSpirv(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint32_t> spirv(static_cast<size_t>(size) / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(spirv.data()), size);
    return spirv;
}

static VkShaderModule createShaderModule(VkDevice device,
                                          const std::vector<uint32_t>& spirv)
{
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode    = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Construction / Destruction

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool VulkanViewport::initGpu()
{
#ifdef _WIN32
    auto& gpu = GpuContext::get();
    if (!gpu.isInitialized()) return false;


    // 1. Create a native QWindow â†’ get HWND â†’ create VkSurfaceKHR
    m_nativeWindow = new QWindow();
    m_nativeWindow->setSurfaceType(QSurface::VulkanSurface);
    m_nativeWindow->create();

    HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow->winId());
    if (!hwnd) {
        spdlog::error("VulkanViewport: Failed to get native HWND");
        delete m_nativeWindow;
        m_nativeWindow = nullptr;
        return false;
    }

    VkWin32SurfaceCreateInfoKHR surfaceCI{};
    surfaceCI.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceCI.hinstance = GetModuleHandleW(nullptr);
    surfaceCI.hwnd      = hwnd;

    VkResult res = vkCreateWin32SurfaceKHR(gpu.vkInstance().handle(), &surfaceCI,
                                            nullptr, &m_surface);
    if (res != VK_SUCCESS) {
        spdlog::error("VulkanViewport: vkCreateWin32SurfaceKHR failed ({})",
                      static_cast<int>(res));
        delete m_nativeWindow;
        m_nativeWindow = nullptr;
        return false;
    }

    // 2. Verify present support on graphics queue
    VkBool32 presentSupport = VK_FALSE;
    uint32_t graphicsFamily = gpu.device().queueFamilies().graphics.value();
    vkGetPhysicalDeviceSurfaceSupportKHR(gpu.physicalDevice(), graphicsFamily,
                                          m_surface, &presentSupport);
    if (!presentSupport) {
        spdlog::error("VulkanViewport: Graphics queue does not support present");
        vkDestroySurfaceKHR(gpu.vkInstance().handle(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
        delete m_nativeWindow;
        m_nativeWindow = nullptr;
        return false;
    }

    // 3. Embed the native window in this QWidget
    m_windowContainer = QWidget::createWindowContainer(m_nativeWindow, this);
    m_windowContainer->setMinimumSize(160, 90);
    // Deliver hover (no-button) mouse-move events so the transform
    // overlay can update the cursor (scale/move/rotate affordance)
    // without requiring a pressed button.
    m_windowContainer->setMouseTracking(true);
    setMouseTracking(true);

    // Forward wheel/double-click events from the native QWindow to this
    // widget. createWindowContainer() delivers all input to the QWindow,
    // so the container widget never sees wheel events. Installing the
    // filter on the QWindow itself intercepts them at the right level.
    m_nativeWindow->installEventFilter(this);

    // Subclass the HWND so WM_SETCURSOR can force our transform cursors
    // (Qt's QWindow::setCursor is ignored for this native surface).
    installCursorSubclass();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_windowContainer);

    // 4. Create swapchain and rendering resources
    if (!createSwapchainResources()) {
        spdlog::error("VulkanViewport: Failed to create swapchain resources");
        vkDestroySurfaceKHR(gpu.vkInstance().handle(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
        return false;
    }

    m_gpuActive = true;
    m_gpuInitialized = true;
    spdlog::info("VulkanViewport: GPU display initialized (native swapchain)");
    return true;
#else
    return false;
#endif
}

void VulkanViewport::shutdownGpu()
{
    if (!m_gpuInitialized) return;

    auto& gpu = GpuContext::get();
    if (!gpu.isInitialized()) return;

    VkDevice device = gpu.vkDevice();
    const bool deviceLost = gpu.gpuState() != GpuState::Healthy;
    if (!deviceLost)
        vkDeviceWaitIdle(device);
    else
        spdlog::warn("VulkanViewport: skipping device-idle wait after device loss");

    // Destroy recycled semaphores after normal idle, or after the lost device
    // has been declared permanently unusable.
    for (VkSemaphore sem : m_recycledSemaphores) {
        if (sem != VK_NULL_HANDLE)
            vkDestroySemaphore(device, sem, nullptr);
    }
    m_recycledSemaphores.clear();

    // Destroy private upload textures before swapchain/surface teardown
    for (auto& slot : m_uploadSlots) {
        slot.texture.reset();
    }

    destroySwapchainResources();

    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(gpu.vkInstance().handle(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }

    m_gpuActive = false;
    m_gpuInitialized = false;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Swapchain + Pipeline Resources
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

bool VulkanViewport::createSwapchainResources()
{
    auto& gpu = GpuContext::get();

    // Swapchain
    m_swapchain = std::make_unique<Swapchain>();
    SwapchainConfig scCfg;
    scCfg.surface         = m_surface;
    scCfg.width           = std::max(1u, static_cast<uint32_t>(width()));
    scCfg.height          = std::max(1u, static_cast<uint32_t>(height()));
    scCfg.vsync           = false; // low-latency mailbox preferred
    scCfg.preferredFormat = VK_FORMAT_B8G8R8A8_UNORM; // match compositor BGRA
    scCfg.imageCount      = 3; // triple buffer â€” prevents vkAcquireNextImageKHR blocking UI thread

    if (!m_swapchain->create(gpu.device(), scCfg)) {
        spdlog::error("VulkanViewport: Failed to create swapchain");
        return false;
    }

    // Render pass, pipeline, descriptors, sync, framebuffers
    if (!createRenderPass())         return false;
    if (!createDescriptorResources()) return false;
    if (!createPipeline())           return false;
    if (!createSyncObjects())        return false;
    if (!createFramebuffers())       return false;

    return true;
}

void VulkanViewport::destroySwapchainResources()
{
    auto& gpu = GpuContext::get();
    if (!gpu.isInitialized()) return;
    VkDevice device = gpu.vkDevice();

    for (auto fb : m_framebuffers)
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    m_framebuffers.clear();

    if (m_inFlightFence)    { vkDestroyFence(device, m_inFlightFence, nullptr);     m_inFlightFence = VK_NULL_HANDLE; }
    for (auto& slot : m_uploadSlots) {
        slot.texture.reset();
        if (slot.pendingStagingBuffer != VK_NULL_HANDLE && slot.pendingStagingAllocator) {
            vmaDestroyBuffer(static_cast<VmaAllocator>(slot.pendingStagingAllocator),
                             slot.pendingStagingBuffer,
                             static_cast<VmaAllocation>(slot.pendingStagingAlloc));
            slot.pendingStagingBuffer    = VK_NULL_HANDLE;
            slot.pendingStagingAlloc     = nullptr;
            slot.pendingStagingAllocator = nullptr;
        }
        if (slot.fence) {
            vkDestroyFence(device, slot.fence, nullptr);
            slot.fence = VK_NULL_HANDLE;
        }
        slot.commandBuffer = VK_NULL_HANDLE;
    }
    m_uploadSlots.clear();
    m_nextUploadSlot = 0;
    for (VkSemaphore sem : m_renderFinishedByImage) {
        if (sem != VK_NULL_HANDLE)
            vkDestroySemaphore(device, sem, nullptr);
    }
    m_renderFinishedByImage.clear();
    if (m_imageAvailable)   { vkDestroySemaphore(device, m_imageAvailable, nullptr); m_imageAvailable = VK_NULL_HANDLE; }
    if (m_commandPool)      { vkDestroyCommandPool(device, m_commandPool, nullptr);  m_commandPool = VK_NULL_HANDLE; }

    if (m_pipeline)        { vkDestroyPipeline(device, m_pipeline, nullptr);        m_pipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout)  { vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool)  { vkDestroyDescriptorPool(device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorSetLayout) { vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr); m_descriptorSetLayout = VK_NULL_HANDLE; }
    if (m_fallbackSampler) { vkDestroySampler(device, m_fallbackSampler, nullptr);  m_fallbackSampler = VK_NULL_HANDLE; }
    if (m_vertShader)      { vkDestroyShaderModule(device, m_vertShader, nullptr);  m_vertShader = VK_NULL_HANDLE; }
    if (m_fragShader)      { vkDestroyShaderModule(device, m_fragShader, nullptr);  m_fragShader = VK_NULL_HANDLE; }
    if (m_renderPass)      { vkDestroyRenderPass(device, m_renderPass, nullptr);    m_renderPass = VK_NULL_HANDLE; }

    if (m_swapchain) {
        m_swapchain->destroy(device);
        m_swapchain.reset();
    }
}

bool VulkanViewport::createRenderPass()
{
    VkDevice device = GpuContext::get().vkDevice();

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = m_swapchain->format();
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 1;
    rpCI.pAttachments    = &colorAttachment;
    rpCI.subpassCount    = 1;
    rpCI.pSubpasses      = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies   = &dep;

    if (vkCreateRenderPass(device, &rpCI, nullptr, &m_renderPass) != VK_SUCCESS) {
        spdlog::error("VulkanViewport: Failed to create render pass");
        return false;
    }
    return true;
}

bool VulkanViewport::createDescriptorResources()
{
    VkDevice device = GpuContext::get().vkDevice();

    // Sampler â€” use CLAMP_TO_BORDER with opaque-black border so that
    // when the viewport is zoomed in and the fullscreen triangle extends
    // beyond the image boundary, out-of-range samples return black instead
    // of stretching the edge pixels (which creates ghost image artifacts).
    VkSamplerCreateInfo samplerCI{};
    samplerCI.sType       = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCI.magFilter   = VK_FILTER_LINEAR;
    samplerCI.minFilter   = VK_FILTER_LINEAR;
    samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerCI.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    vkCreateSampler(device, &samplerCI, nullptr, &m_fallbackSampler);

    // Descriptor set layout (1 combined image sampler)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;
    vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_descriptorSetLayout);

    // Descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    vkCreateDescriptorPool(device, &poolCI, nullptr, &m_descriptorPool);

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorSetLayout;
    vkAllocateDescriptorSets(device, &allocInfo, &m_descriptorSet);

    return true;
}

bool VulkanViewport::createPipeline()
{
    VkDevice device = GpuContext::get().vkDevice();

    // Find compiled SPIR-V shaders
    namespace fs = std::filesystem;
    fs::path searchDirs[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "build" / "shaders",
        fs::current_path() / "build" / "shaders",
        fs::current_path() / "shaders",
    };

    fs::path vertPath, fragPath;
    for (auto& dir : searchDirs) {
        auto vp = dir / "quad.vert.spv";
        auto fp = dir / "quad.frag.spv";
        if (fs::exists(vp) && fs::exists(fp)) {
            vertPath = vp;
            fragPath = fp;
            break;
        }
    }
    if (vertPath.empty()) {
        spdlog::error("VulkanViewport: quad.vert.spv / quad.frag.spv not found");
        return false;
    }

    auto vertSpirv = loadSpirv(pathToUtf8(vertPath));
    auto fragSpirv = loadSpirv(pathToUtf8(fragPath));
    m_vertShader = createShaderModule(device, vertSpirv);
    m_fragShader = createShaderModule(device, fragSpirv);
    if (!m_vertShader || !m_fragShader) return false;

    // Pipeline layout
    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts    = &m_descriptorSetLayout;
    vkCreatePipelineLayout(device, &plCI, nullptr, &m_pipelineLayout);

    // Shader stages
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_vertShader;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_fragShader;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAsm{};
    inputAsm.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAtt.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments    = &blendAtt;

    std::array<VkDynamicState, 2> dynStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynamicState.pDynamicStates    = dynStates.data();

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.stageCount          = static_cast<uint32_t>(stages.size());
    pipelineCI.pStages             = stages.data();
    pipelineCI.pVertexInputState   = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAsm;
    pipelineCI.pViewportState      = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState   = &msaa;
    pipelineCI.pColorBlendState    = &blendState;
    pipelineCI.pDynamicState       = &dynamicState;
    pipelineCI.layout              = m_pipelineLayout;
    pipelineCI.renderPass          = m_renderPass;
    pipelineCI.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                 &pipelineCI, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        spdlog::error("VulkanViewport: Failed to create graphics pipeline ({})",
                      static_cast<int>(result));
        return false;
    }

    spdlog::info("VulkanViewport: Graphics pipeline created");
    return true;
}

bool VulkanViewport::createSyncObjects()
{
    auto& gpu = GpuContext::get();
    VkDevice device = gpu.vkDevice();

    // Command pool (graphics queue family)
    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = gpu.device().queueFamilies().graphics.value();
    vkCreateCommandPool(device, &poolCI, nullptr, &m_commandPool);

    // Main present command buffer
    VkCommandBufferAllocateInfo cbAI{};
    cbAI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAI.commandPool        = m_commandPool;
    cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAI.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cbAI, &m_commandBuffer);

    // Semaphores + fence
    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device, &semCI, nullptr, &m_imageAvailable) != VK_SUCCESS)
        return false;
    if (!createPresentSemaphores())
        return false;

    VkFenceCreateInfo fenceCI{};
    fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT; // start signaled
    vkCreateFence(device, &fenceCI, nullptr, &m_inFlightFence);

    m_uploadSlots.clear();
    m_uploadSlots.resize(kUploadSlotCount);
    for (auto& slot : m_uploadSlots) {
        VkCommandBufferAllocateInfo uploadAI{};
        uploadAI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        uploadAI.commandPool        = m_commandPool;
        uploadAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        uploadAI.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &uploadAI, &slot.commandBuffer);

        VkFenceCreateInfo uploadFenceCI{};
        uploadFenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        uploadFenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device, &uploadFenceCI, nullptr, &slot.fence);
    }

    return true;
}

bool VulkanViewport::createPresentSemaphores()
{
    if (!m_swapchain || m_swapchain->imageCount() == 0)
        return false;

    VkDevice device = GpuContext::get().vkDevice();
    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    std::vector<VkSemaphore> replacement(m_swapchain->imageCount(),
                                          VK_NULL_HANDLE);
    for (auto& sem : replacement) {
        if (vkCreateSemaphore(device, &semCI, nullptr, &sem) != VK_SUCCESS) {
            for (VkSemaphore created : replacement) {
                if (created != VK_NULL_HANDLE)
                    vkDestroySemaphore(device, created, nullptr);
            }
            return false;
        }
    }

    // Resize drains the present queue before calling this function. During
    // initial setup the old vector is empty.
    for (VkSemaphore sem : m_renderFinishedByImage) {
        if (sem != VK_NULL_HANDLE)
            vkDestroySemaphore(device, sem, nullptr);
    }
    m_renderFinishedByImage = std::move(replacement);
    return true;
}

bool VulkanViewport::createFramebuffers()
{
    VkDevice device = GpuContext::get().vkDevice();

    m_framebuffers.resize(m_swapchain->imageCount());
    for (uint32_t i = 0; i < m_swapchain->imageCount(); ++i) {
        VkImageView attachment = m_swapchain->imageView(i);

        VkFramebufferCreateInfo fbCI{};
        fbCI.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbCI.renderPass      = m_renderPass;
        fbCI.attachmentCount = 1;
        fbCI.pAttachments    = &attachment;
        fbCI.width           = m_swapchain->extent().width;
        fbCI.height          = m_swapchain->extent().height;
        fbCI.layers          = 1;

        if (vkCreateFramebuffer(device, &fbCI, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            spdlog::error("VulkanViewport: Failed to create framebuffer {}", i);
            return false;
        }
    }
    return true;
}


} // namespace rt
