#define VMA_IMPLEMENTATION
#include <Veng/Renderer/Context.h>

#include <array>
#include <cstring>
#include <fstream>
#include <set>

#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Window.h>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

// #define VE_VIEWPORTS 1

namespace Veng::Renderer
{
#ifdef VE_ENABLE_VALIDATION_LAYERS
    static VKAPI_ATTR VkBool32 VKAPI_CALL
    DebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                  vk::DebugUtilsMessageTypeFlagsEXT messageType,
                  const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
    {
        // Throwing here would unwind through the Vulkan C ABI, so only log.
        if (messageSeverity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
        {
            Log::Error("Vulkan validation: {}", pCallbackData->pMessage);
        }
        else if (messageSeverity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
        {
            Log::Warn("Vulkan validation: {}", pCallbackData->pMessage);
        }

        return VK_FALSE;
    }
#endif

    Context::Context() : m_Native(CreateUnique<Native>()) {}

    Context::~Context() = default;

    Context::Native& Context::GetNative() const
    {
        return *m_Native;
    }

    vk::PhysicalDevice Context::Native::GetPhysicalDevice()
    {
        auto physicalDevices = Instance.enumeratePhysicalDevices().value;

        for (auto& device : physicalDevices)
        {
            if (IsDeviceSuitable(device))
            {
                return device;
            }
        }

        VE_ASSERT(false, "Failed to find a suitable GPU!");
    }

    bool Context::Native::IsDeviceSuitable(vk::PhysicalDevice device)
    {
        const QueueFamilyIndices indices = FindQueueFamilies(device);

        auto supportedFeatures = device.getFeatures();

        const bool extensionsSupported = CheckDeviceExtensionSupport(device);

        // Headless needs neither a present queue nor an adequate swapchain.
        if (Headless)
        {
            return indices.IsComplete() && extensionsSupported &&
                   supportedFeatures.samplerAnisotropy;
        }

        bool swapChainAdequate = false;
        if (extensionsSupported)
        {
            const SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device);
            swapChainAdequate =
                !swapChainSupport.Formats.empty() && !swapChainSupport.PresentModes.empty();
        }

        return indices.IsComplete() && indices.CanPresent() && extensionsSupported &&
               swapChainAdequate && supportedFeatures.samplerAnisotropy;
    }

    void Context::Initialize(const ContextInfo& info, Window* window)
    {
        const bool headless = (window == nullptr);
        m_Native->Headless = headless;

        Log::Info(headless ? "Initializing Vulkan context (headless)"
                           : "Initializing Vulkan context");

        VULKAN_HPP_DEFAULT_DISPATCHER.init();

        m_Window = window;

        if (!headless && glfwVulkanSupported() != GLFW_TRUE)
        {
            VE_ASSERT(false, "Vulkan is not supported on this system!");
        }

        if (headless)
        {
            // No surface ⇒ the swapchain device extension isn't needed (or wanted).
            std::erase_if(m_Native->DeviceExtensions, [](const char* extension)
                          { return std::strcmp(extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; });
        }

        auto extensions = m_Native->GetRequiredExtensions();

        const vk::ApplicationInfo appInfo{.pApplicationName = info.ApplicationName.c_str(),
                                          .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                          .pEngineName = info.EngineName.c_str(),
                                          .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                                          .apiVersion = VK_API_VERSION_1_3};

        vk::InstanceCreateInfo instanceCreateInfo{
            .pApplicationInfo = &appInfo,
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

#ifdef VE_ENABLE_VALIDATION_LAYERS
        Log::Info("Enabling validation layers");
        vk::DebugUtilsMessengerCreateInfoEXT debugCreateInfo{
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                               vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                               vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
            .pfnUserCallback = DebugCallback};

        // Turn on synchronization validation so missing/incorrect barriers from
        // the render graph are caught at runtime.
        constexpr vk::ValidationFeatureEnableEXT enabledValidationFeatures[] = {
            vk::ValidationFeatureEnableEXT::eSynchronizationValidation,
        };
        const vk::ValidationFeaturesEXT validationFeatures{
            .pNext = &debugCreateInfo,
            .enabledValidationFeatureCount = static_cast<u32>(std::size(enabledValidationFeatures)),
            .pEnabledValidationFeatures = enabledValidationFeatures,
        };

        instanceCreateInfo.pNext = &validationFeatures;
        instanceCreateInfo.enabledLayerCount = static_cast<u32>(m_Native->ValidationLayers.size());
        instanceCreateInfo.ppEnabledLayerNames = m_Native->ValidationLayers.data();
#else
        instanceCreateInfo.pNext = nullptr;
        instanceCreateInfo.enabledLayerCount = 0;
        instanceCreateInfo.ppEnabledLayerNames = nullptr;
#endif

        instanceCreateInfo.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;

        m_Native->Instance = createInstance(instanceCreateInfo).value;

        VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Native->Instance);

#ifdef VE_ENABLE_VALIDATION_LAYERS
        m_Native->DebugMessenger =
            m_Native->Instance.createDebugUtilsMessengerEXT(debugCreateInfo).value;
#endif

        DebugMarkers::Initialize(m_Native->Instance);

        if (!headless)
        {
            Log::Info("Initializing surface");
            window->CreateSurface(*this);
            m_Native->Surface = GetVkSurface(*window);
        }

        m_Native->PhysicalDevice = m_Native->GetPhysicalDevice();

        m_Native->Device = m_Native->CreateDevice();

        VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Native->Device);

        m_Native->PipelineCachePath = info.PipelineCachePath;

        // Seed the cache from disk when a path is given and the file exists. A
        // missing file is the first run, not an error; a stale/foreign/truncated
        // blob is safe — the driver validates the cache header and silently
        // starts cold on a mismatch. veng never parses or validates the bytes.
        vector<u8> initial;
        if (m_Native->PipelineCachePath)
        {
            std::ifstream file(*m_Native->PipelineCachePath, std::ios::binary | std::ios::ate);
            if (file)
            {
                const auto size = static_cast<usize>(file.tellg());
                initial.resize(size);
                file.seekg(0);
                file.read(reinterpret_cast<char*>(initial.data()),
                          static_cast<std::streamsize>(size));
            }
        }

        const vk::PipelineCacheCreateInfo cacheInfo{
            .initialDataSize = initial.size(),
            .pInitialData = initial.empty() ? nullptr : initial.data(),
        };
        m_Native->PipelineCache = m_Native->Device.createPipelineCache(cacheInfo).value;

        const VmaAllocatorCreateInfo allocatorInfo{
            .physicalDevice = m_Native->PhysicalDevice,
            .device = m_Native->Device,
            .instance = m_Native->Instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };

        vmaCreateAllocator(&allocatorInfo, &m_Native->Allocator);

        m_Native->GraphicsQueue =
            m_Native->Device.getQueue(m_Native->QueueFamilies.GraphicsFamily.value(), 0);

        // Transfer queue. On MoltenVK this resolves to the same family (and
        // queue index 0) as graphics, so TransferQueue and GraphicsQueue are the
        // same handle and the submission lock is what keeps them safe.
        m_Native->TransferQueue =
            m_Native->Device.getQueue(m_Native->QueueFamilies.TransferFamily.value(), 0);

        Log::Info("Queue families: graphics={0}, transfer={1}{2}",
                  m_Native->QueueFamilies.GraphicsFamily.value(),
                  m_Native->QueueFamilies.TransferFamily.value(),
                  m_Native->QueueFamilies.TransferIsGraphics()
                      ? " (transfer collapsed onto graphics)"
                      : " (dedicated transfer family)");

        if (!headless)
        {
            m_Native->PresentQueue =
                m_Native->Device.getQueue(m_Native->QueueFamilies.PresentFamily.value(), 0);

            m_Native->SwapChain = SwapChain::Create(*this, {
                                                               // Triple-buffered: under FIFO a
                                                               // two-image swapchain half-rate
                                                               // locks (30 on a 60Hz display) the
                                                               // moment a frame overruns one
                                                               // vblank; a spare image lets the
                                                               // present engine pace at the GPU
                                                               // rate instead.
                                                               .MaxImageCount = 3,
                                                               .Width = window->GetWidth(),
                                                               .Height = window->GetHeight(),
                                                               .Mode = info.RequestedDisplayMode,
                                                           });

            Log::Info("Created {0} swap chain images ({1}x{2})",
                      m_Native->SwapChain->GetImageCount(), m_Native->SwapChain->GetWidth(),
                      m_Native->SwapChain->GetHeight());

            m_RenderExtent = m_Native->SwapChain->GetExtent();
        }
        else
        {
            // No swapchain to derive an extent from; fall back to the configured
            // headless extent for GetRenderExtent() consumers.
            m_RenderExtent = info.HeadlessExtent;
        }

        m_OutputFormat = info.OutputFormat;
        m_DepthFormat = info.DepthFormat;
        Log::Info("Render-target extent: {0}x{1}", m_RenderExtent.x, m_RenderExtent.y);

        Log::Info("Creating command pool");

        m_Native->CommandPool = CommandPool::Create(*this);

        // One pool size per DescriptorType, budgeted from the single source
        // of truth (GetDescriptorTypeInfo) so every type the engine knows
        // about has a budget — adding a DescriptorType updates the pool too.
        vector<vk::DescriptorPoolSize> poolSizes;
        poolSizes.reserve(AllDescriptorTypes.size());
        for (const auto descriptorType : AllDescriptorTypes)
        {
            const auto typeInfo = GetDescriptorTypeInfo(descriptorType);
            poolSizes.push_back({.type = typeInfo.VkType, .descriptorCount = typeInfo.PoolBudget});
        }

        m_Native->DescriptorPool = DescriptorPool::Create(
            *this, {.Name = "Primary Pool",
                    .MaxSets = 100000,
                    .PoolSizes = poolSizes,
                    .Flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet |
                             vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind});

        m_Native->SynchronizationFrames.reserve(m_Native->MaxFramesInFlight);
        for (u32 i = 0; i < m_Native->MaxFramesInFlight; ++i)
        {
            m_Native->SynchronizationFrames.emplace_back(*this);
        }

        m_Native->RetireBins.resize(m_Native->MaxFramesInFlight);

        // The single transfer timeline: worker upload submits signal it, the
        // frame submit and the transfer-keyed retire drain wait/poll it.
        m_Native->TransferTimeline = TimelineSemaphore::Create(*this);

        m_Native->Bindless = CreateUnique<BindlessRegistry>(*this);

        // GPU frame timing: a (start, end) timestamp pair per frame-in-flight. Available only
        // when the graphics queue family reports valid timestamp bits and the device a non-zero
        // period; otherwise GetLastGpuFrameTimeMs() stays zero and BeginFrame/EndFrame skip the
        // writes (so dynamic-resolution control is inert rather than wrong).
        const f32 timestampPeriod = m_Native->PhysicalDevice.getProperties().limits.timestampPeriod;
        const u32 graphicsFamily = m_Native->QueueFamilies.GraphicsFamily.value();
        const u32 validBits =
            m_Native->PhysicalDevice.getQueueFamilyProperties()[graphicsFamily].timestampValidBits;
        if (timestampPeriod > 0.0f && validBits > 0)
        {
            m_GpuTimingSupported = true;
            m_Native->TimestampPeriodNs = timestampPeriod;
            m_Native->TimestampValidMask =
                validBits >= 64 ? ~0ULL : ((static_cast<u64>(1) << validBits) - 1);
            m_Native->TimestampWritten.assign(m_Native->MaxFramesInFlight, false);
            m_Native->ScopeNames.resize(m_Native->MaxFramesInFlight);

            // Per frame slot: the frame (start,end) pair plus MaxGpuScopes (start,end) scope pairs.
            const u32 queriesPerFrame = 2 + 2 * Native::MaxGpuScopes;
            m_Native->TimestampPool =
                m_Native->Device
                    .createQueryPool({
                        .queryType = vk::QueryType::eTimestamp,
                        .queryCount = m_Native->MaxFramesInFlight * queriesPerFrame,
                    })
                    .value;
        }
    }

    void Context::DisposeResources()
    {
        // Pending bindless acquires that never reached a frame retire into the
        // current bin; the drain below reclaims them while the device is alive.
        m_PendingBindlessAcquires.clear();

        // GPU is idle (Application::Run already waited). Drain all retire bins
        // before the sync frames and their command buffers go away.
        m_Native->DrainAllRetireBins();

        // The transfer timeline may have upload scratch that was never reclaimed
        // by AcquireNextFrame. The GPU is idle, so every value has been reached —
        // wait the last signalled value and drain the list.
        m_Native->TransferTimeline->Wait(m_Native->TransferTimelineValue);
        m_Native->DrainTransferRetireList();

        m_Native->SynchronizationFrames.clear();
    }

    void Context::Dispose()
    {
        Log::Info("Disposing rendering context...");

        // Drop bindless first — its destructors retire handles into the bins,
        // which the drain below cleans up while the device is still alive.
        m_Native->Bindless.reset();

        // Drain bins and transfer scratch before flipping Disposed, so a late
        // retire is caught rather than leaked. After this point, any drop is a bug.
        m_Native->DrainAllRetireBins();
        m_Native->TransferTimeline->Wait(m_Native->TransferTimelineValue);
        m_Native->DrainTransferRetireList();
        m_Native->Disposed = true;

        // Workers have been joined by Application before Dispose, so destroying
        // the per-worker transfer pools here is unraced.
        for (auto& transferPool : m_Native->TransferPools)
        {
            transferPool.CommandBuffer.reset();
            if (transferPool.Pool)
            {
                m_Native->Device.destroyCommandPool(transferPool.Pool);
            }
        }
        m_Native->TransferPools.clear();

        m_Native->TransferTimeline.reset();
        m_Native->DescriptorPool.reset();
        m_Native->CommandPool.reset();
        m_Native->SwapChain.reset();
        m_Native->PresentQueue = nullptr;
        m_Native->GraphicsQueue = nullptr;
        m_Native->TransferQueue = nullptr;
        vmaDestroyAllocator(m_Native->Allocator);

        // Persist the warm cache before destroying it, when a path is set. The
        // Vulkan fetch is fatal on failure (the .value unwrap asserts) like any
        // Vulkan call; the file write is recoverable — a warm cache is an
        // optimization, not correctness, so a failed write logs and teardown
        // continues.
        if (m_Native->PipelineCachePath)
        {
            const auto data = m_Native->Device.getPipelineCacheData(m_Native->PipelineCache).value;
            std::ofstream file(*m_Native->PipelineCachePath, std::ios::binary | std::ios::trunc);
            if (file)
            {
                file.write(reinterpret_cast<const char*>(data.data()),
                           static_cast<std::streamsize>(data.size()));
            }
            else
            {
                Log::Warn("pipeline cache write failed: {}", m_Native->PipelineCachePath->string());
            }
        }

        if (m_Native->TimestampPool)
        {
            m_Native->Device.destroyQueryPool(m_Native->TimestampPool);
            m_Native->TimestampPool = nullptr;
        }

        m_Native->Device.destroyPipelineCache(m_Native->PipelineCache);
        m_Native->Device.destroy();
        if (m_Native->Surface)
        {
            m_Native->Instance.destroySurfaceKHR(m_Native->Surface);
        }
        m_Native->PhysicalDevice = nullptr;
        if (m_Native->DebugMessenger)
        {
            m_Native->Instance.destroyDebugUtilsMessengerEXT(m_Native->DebugMessenger);
            m_Native->DebugMessenger = nullptr;
        }
        m_Native->Instance.destroy();
        m_Window = nullptr;
    }

    void Context::InitializeTransferPools(TaskSystem& taskSystem)
    {
        const u32 workerCount = taskSystem.GetWorkerCount();
        const u32 transferFamily = m_Native->QueueFamilies.TransferFamily.value();

        m_Native->TransferPools.resize(workerCount);

        // Create each pool on the worker that owns it — a VkCommandPool may only be
        // accessed by one thread at a time, and the creating thread is the one that
        // later records uploads. Bodies run concurrently with no shared state.
        taskSystem.ForEachWorker(
            [this, transferFamily](u32 workerIndex)
            {
                m_Native->TransferPools[workerIndex].Pool =
                    m_Native->Device
                        .createCommandPool(vk::CommandPoolCreateInfo{
                            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                            .queueFamilyIndex = transferFamily,
                        })
                        .value;
            });

        // Allocate command buffers serially: AllocationPoolOverride is shared
        // mutable state and must not be written from concurrent ForEachWorker bodies.
        for (u32 workerIndex = 0; workerIndex < workerCount; ++workerIndex)
        {
            auto& transferPool = m_Native->TransferPools[workerIndex];
            m_Native->AllocationPoolOverride = transferPool.Pool;
            transferPool.CommandBuffer = CommandBuffer::Create(*this);
            m_Native->AllocationPoolOverride = nullptr;
        }

        Log::Info("Created {0} per-worker transfer command pool(s)", workerCount);
    }

    CommandBuffer& Context::GetTransferCommandBuffer(u32 workerIndex)
    {
        VE_ASSERT(workerIndex < m_Native->TransferPools.size(),
                  "GetTransferCommandBuffer: worker index {} out of range ({} transfer pools) — "
                  "InitializeTransferPools must run first",
                  workerIndex, m_Native->TransferPools.size());

        return *m_Native->TransferPools[workerIndex].CommandBuffer;
    }

    void Context::Native::LockedSubmit(vk::Queue queue, const vk::SubmitInfo& submitInfo,
                                       vk::Fence fence)
    {
        const std::scoped_lock lock(SubmitMutex);
        VK_ASSERT(queue.submit(1, &submitInfo, fence), "failed to submit to queue!");
    }

    CommandBuffer& Context::BeginTransferRecording(u32 workerIndex)
    {
        VE_ASSERT(workerIndex < m_Native->TransferPools.size(),
                  "BeginTransferRecording: worker index {} out of range ({} transfer pools)",
                  workerIndex, m_Native->TransferPools.size());

        auto& pool = m_Native->TransferPools[workerIndex];

        // Reuse is timeline-gated: wait the worker's last upload to finish before
        // resetting its (single, reused) command buffer.
        if (pool.LastSubmittedValue != 0)
        {
            m_Native->TransferTimeline->Wait(pool.LastSubmittedValue);
        }

        pool.CommandBuffer->Reset();
        pool.CommandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);
        return *pool.CommandBuffer;
    }

    u64 Context::SubmitTransfer(u32 workerIndex, const TimelineSemaphore& timeline)
    {
        VE_ASSERT(workerIndex < m_Native->TransferPools.size(),
                  "SubmitTransfer: worker index {} out of range ({} transfer pools)", workerIndex,
                  m_Native->TransferPools.size());

        auto& pool = m_Native->TransferPools[workerIndex];
        pool.CommandBuffer->End();

        const auto vkCommandBuffer = pool.CommandBuffer->GetNative().CommandBuffer;
        const vk::Semaphore vkTimeline = timeline.GetNative().Semaphore;

        // Allocate the value, build the signal-info, and submit all under one lock:
        // a timeline must signal strictly increasing values, so value allocation and
        // submit must be atomic — two workers racing could submit a lower value second.
        const std::scoped_lock lock(m_Native->SubmitMutex);

        const u64 value = ++m_Native->TransferTimelineValue;

        const vk::TimelineSemaphoreSubmitInfo timelineInfo{
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &value,
        };

        const vk::SubmitInfo submitInfo{
            .pNext = &timelineInfo,
            .commandBufferCount = 1,
            .pCommandBuffers = &vkCommandBuffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &vkTimeline,
        };

        VK_ASSERT(m_Native->TransferQueue.submit(1, &submitInfo, VK_NULL_HANDLE),
                  "failed to submit transfer commands!");

        pool.LastSubmittedValue = value;
        return value;
    }

    TimelineSemaphore& Context::GetTransferTimeline() const
    {
        return *m_Native->TransferTimeline;
    }

    bool Context::IsHeadless() const
    {
        return m_Native->Headless;
    }

    const QueueFamilyIndices& Context::GetQueueFamilies() const
    {
        return m_Native->QueueFamilies;
    }

    bool Context::IsGpuDrivenCullingSupported() const
    {
        return m_Native->GpuDrivenCullingSupported;
    }

    bool Context::IsGpuTimingSupported() const
    {
        return m_GpuTimingSupported;
    }

    f32 Context::GetLastGpuFrameTimeMs() const
    {
        return m_GpuFrameTimeMs;
    }

    std::span<const Context::GpuPassTiming> Context::GetLastGpuPassTimings() const
    {
        return m_GpuPassTimings;
    }

    void Context::BeginGpuScope(CommandBuffer& cmd, const string_view name)
    {
        // Only inside a driven frame are the queries reset and writable; a graph executed
        // out-of-frame (ImmediateCommands, a one-shot render) records no scopes.
        if (!m_GpuTimingSupported || !m_GpuScopeRecording)
        {
            return;
        }

        // Past the budget: still push a sentinel so the matching EndGpuScope balances the stack.
        if (m_Native->CurrentScopeNames.size() >= Native::MaxGpuScopes)
        {
            m_Native->OpenScopeStack.push_back(Native::MaxGpuScopes);
            return;
        }

        const u32 slot = m_Native->CurrentFrameInFlight;
        const u32 scopeBase = (slot * (2 + 2 * Native::MaxGpuScopes)) + 2;
        const auto index = static_cast<u32>(m_Native->CurrentScopeNames.size());

        m_Native->CurrentScopeNames.emplace_back(name);
        m_Native->OpenScopeStack.push_back(index);
        GetVkCommandBuffer(cmd).writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                               m_Native->TimestampPool, scopeBase + (index * 2));
    }

    void Context::EndGpuScope(CommandBuffer& cmd)
    {
        // Symmetric with BeginGpuScope: out-of-frame the matching Begin pushed nothing, so the
        // stack stays balanced without a pop here.
        if (!m_GpuTimingSupported || !m_GpuScopeRecording)
        {
            return;
        }

        VE_ASSERT(!m_Native->OpenScopeStack.empty(),
                  "Context::EndGpuScope called without a matching BeginGpuScope");
        const u32 index = m_Native->OpenScopeStack.back();
        m_Native->OpenScopeStack.pop_back();

        // The sentinel marks a scope opened past the per-frame budget — nothing to close.
        if (index == Native::MaxGpuScopes)
        {
            return;
        }

        const u32 slot = m_Native->CurrentFrameInFlight;
        const u32 scopeBase = (slot * (2 + 2 * Native::MaxGpuScopes)) + 2;
        GetVkCommandBuffer(cmd).writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                               m_Native->TimestampPool,
                                               scopeBase + (index * 2) + 1);
    }

    bool Context::IsFormatLinearFilterSupported(const Format format) const
    {
        const vk::FormatProperties props =
            m_Native->PhysicalDevice.getFormatProperties(ToVk(format));
        return static_cast<bool>(props.optimalTilingFeatures &
                                 vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
    }

    SynchronizationFrame& Context::GetCurrentFrame()
    {
        return m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight];
    }

    CommandBuffer& Context::GetCurrentCommandBuffer()
    {
        return *GetCurrentFrame().GetCommandBuffer();
    }

    CommandBuffer& Context::BeginFrame()
    {
        auto& frame = AcquireNextFrame();

        // Headless has no swapchain image to acquire.
        if (!IsHeadless())
        {
            AcquireNextImage(frame.GetImageAvailableSemaphore());
        }

        frame.GetInFlightFence().Reset();

        auto commandBuffer = frame.GetCommandBuffer();

        commandBuffer->Reset();

        commandBuffer->Begin();

        // GPU timing: read back the queries this slot wrote on its previous cycle (its fence
        // was just waited by AcquireNextFrame, so the results are ready), then reset the slot's
        // whole query run and write the frame-start timestamp. The slot is read only after it
        // has been written once.
        if (m_GpuTimingSupported)
        {
            const u32 slot = m_Native->CurrentFrameInFlight;
            const u32 queriesPerFrame = 2 + 2 * Native::MaxGpuScopes;
            const u32 frameBase = slot * queriesPerFrame;
            const u32 scopeBase = frameBase + 2;
            const u64 mask = m_Native->TimestampValidMask;
            const auto ticksToMs = [this](const u64 ticks)
            { return static_cast<f32>(ticks) * m_Native->TimestampPeriodNs * 1e-6f; };

            if (m_Native->TimestampWritten[slot])
            {
                std::array<u64, 2> frameStamps{};
                const vk::Result frameResult = m_Native->Device.getQueryPoolResults(
                    m_Native->TimestampPool, frameBase, 2, sizeof(frameStamps), frameStamps.data(),
                    sizeof(u64), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
                if (frameResult == vk::Result::eSuccess)
                {
                    const u64 ticks = (frameStamps[1] - frameStamps[0]) & mask;
                    m_GpuFrameTimeMs = ticksToMs(ticks);
                }

                // The scope names recorded into this slot last cycle give both the count of
                // scope pairs to read (only the written ones — an eWait read of an unwritten
                // query would hang) and the label for each measured pass.
                const vector<string>& names = m_Native->ScopeNames[slot];
                m_GpuPassTimings.clear();
                if (!names.empty())
                {
                    vector<u64> scopeStamps(names.size() * 2);
                    const vk::Result scopeResult = m_Native->Device.getQueryPoolResults(
                        m_Native->TimestampPool, scopeBase, static_cast<u32>(scopeStamps.size()),
                        scopeStamps.size() * sizeof(u64), scopeStamps.data(), sizeof(u64),
                        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
                    if (scopeResult == vk::Result::eSuccess)
                    {
                        m_GpuPassTimings.reserve(names.size());
                        for (usize i = 0; i < names.size(); i++)
                        {
                            const u64 ticks =
                                (scopeStamps[(i * 2) + 1] - scopeStamps[i * 2]) & mask;
                            m_GpuPassTimings.push_back({
                                .Name = names[i],
                                .Milliseconds = ticksToMs(ticks),
                            });
                        }
                    }
                }
            }

            const vk::CommandBuffer vkCmd = GetVkCommandBuffer(*commandBuffer);
            vkCmd.resetQueryPool(m_Native->TimestampPool, frameBase, queriesPerFrame);
            vkCmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_Native->TimestampPool,
                                 frameBase);

            // Open a fresh recording for this slot's new frame; scopes append as passes record.
            // The reset above makes the slot's queries writable, so scope writes are now legal.
            m_Native->CurrentScopeNames.clear();
            m_Native->OpenScopeStack.clear();
            m_GpuScopeRecording = true;
        }

        // Transition any resources that went resident since last frame into Sample
        // layout before passes record. The RenderGraph cannot derive this transition
        // — bindless resources are invisible to it (sampled through set 0).
        for (const Ref<ImageView>& view : m_PendingBindlessAcquires)
        {
            commandBuffer->PrepareForAccess(view, AccessKind::Sample);
        }
        m_PendingBindlessAcquires.clear();

        return *commandBuffer;
    }

    void Context::EndFrame()
    {
        auto& frame = GetCurrentFrame();

        auto commandBuffer = frame.GetCommandBuffer();

        // Headless has no swapchain image to transition for presentation.
        if (!IsHeadless())
        {
            Backend::TransitionImage(*commandBuffer, *GetCurrentSwapChainImage(),
                                     ImageLayout::PresentSrc);
        }

        // GPU frame timing: the frame-end timestamp, capturing the whole command buffer's work.
        // The scope names recorded this frame travel to the slot so its next readback can pair
        // each duration with its pass.
        if (m_GpuTimingSupported)
        {
            const u32 slot = m_Native->CurrentFrameInFlight;
            const u32 frameBase = slot * (2 + 2 * Native::MaxGpuScopes);
            GetVkCommandBuffer(*commandBuffer)
                .writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_Native->TimestampPool,
                                frameBase + 1);
            m_Native->ScopeNames[slot] = std::move(m_Native->CurrentScopeNames);
            m_Native->TimestampWritten[slot] = true;
            m_GpuScopeRecording = false;
        }

        commandBuffer->End();

        SubmitFrame(frame);

        PresentFrame(frame);
    }

    u32 Context::GetMaxFramesInFlight() const
    {
        return m_Native->MaxFramesInFlight;
    }
    u32 Context::GetCurrentFrameInFlight() const
    {
        return m_Native->CurrentFrameInFlight;
    }

    u32 Context::GetMaxImageDimension2D() const
    {
        return m_Native->PhysicalDevice.getProperties().limits.maxImageDimension2D;
    }

    uvec2 Context::GetSwapChainExtent() const
    {
        VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)");
        return m_Native->SwapChain->GetExtent();
    }
    Format Context::GetSwapChainFormat() const
    {
        VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)");
        return m_Native->SwapChain->GetFormat();
    }
    DisplayMode Context::GetActiveDisplayMode() const
    {
        VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)");
        return m_Native->SwapChain->GetDisplayMode();
    }
    DisplayColorSpace Context::GetActiveDisplayColorSpace() const
    {
        VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)");
        return m_Native->SwapChain->GetDisplayColorSpace();
    }
    Ref<Image> Context::GetCurrentSwapChainImage() const
    {
        VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)");
        return m_Native->SwapChain->GetCurrentImage();
    }
    Ref<ImageView> Context::GetCurrentSwapChainImageView() const
    {
        VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)");
        return m_Native->SwapChain->GetCurrentImageView();
    }
    u32 Context::GetSwapChainImageCount() const
    {
        VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)");
        return m_Native->SwapChain->GetImageCount();
    }
    u32 Context::GetCurrentSwapChainImageIndex() const
    {
        VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)");
        return m_Native->SwapChain->GetCurrentImageIndex();
    }

    void Context::AddSwapChainInvalidationCallback(std::function<void()> callback)
    {
        m_Native->SwapChain->AddInvalidationCallback(std::move(callback));
    }

    void Context::ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const
    {
        auto commandBuffer = CommandBuffer::Create(const_cast<Context&>(*this));
        commandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);

        function(*commandBuffer);

        commandBuffer->End();
        SubmitImmediateCommands(*commandBuffer);
    }

    void Context::AcquireNextImage(Semaphore& semaphore)
    {
        // On eErrorOutOfDateKHR the semaphore is left unsignaled and no image index
        // is produced, so the swap chain must be recreated and the image re-acquired
        // before submit — submitting a frame whose wait is this unsignaled semaphore
        // deadlocks the queue (its fence never signals, hanging the next frame's wait).
        auto result = m_Native->SwapChain->AcquireNextImage(semaphore);

        while (result == vk::Result::eErrorOutOfDateKHR)
        {
            Log::Warn("Out of date swap chain image!");

            m_Window->SpinUntilValidSize();
            WaitIdle();
            UpdateRenderExtent();

            result = m_Native->SwapChain->AcquireNextImage(semaphore);
        }

        if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR)
        {
            VE_ASSERT(false, "failed to acquire swap chain image!");
        }
    }

    void Context::WaitIdle() const
    {
        VK_ASSERT(m_Native->Device.waitIdle(), "failed to wait for device idle!");
    }

    BindlessRegistry& Context::GetBindlessRegistry() const
    {
        return *m_Native->Bindless;
    }

    void Context::EnqueueBindlessAcquire(const Ref<ImageView>& view)
    {
        m_PendingBindlessAcquires.push_back(view);
    }

    vector<const char*>& Context::Native::GetRequiredExtensions()
    {
        if (RequiredExtensions.empty())
        {
            // Surface/window-related instance extensions (GLFW's list and the
            // swapchain colour-space extension) are only needed with a window.
            if (!Headless)
            {
                u32 glfwExtensionCount = 0;
                const char** glfwExtensions =
                    glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

                RequiredExtensions =
                    vector<const char*>(glfwExtensions, glfwExtensions + glfwExtensionCount);

                RequiredExtensions.emplace_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
            }

            RequiredExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            RequiredExtensions.emplace_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            RequiredExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        return RequiredExtensions;
    }

    QueueFamilyIndices& Context::Native::FindQueueFamilies(vk::PhysicalDevice device)
    {
        QueueFamilies = {};

        u32 queueFamilyCount = 0;
        device.getQueueFamilyProperties(&queueFamilyCount, nullptr);

        vector<vk::QueueFamilyProperties> queueFamilies(queueFamilyCount);
        device.getQueueFamilyProperties(&queueFamilyCount, queueFamilies.data());

        for (u32 i = 0; i < queueFamilyCount; ++i)
        {
            const auto& queueFamily = queueFamilies[i];

            if (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics)
            {
                QueueFamilies.GraphicsFamily = i;
            }

            // Prefer a transfer-capable family that is NOT also graphics — the
            // dedicated DMA path on discrete GPUs. The graphics family also
            // implicitly supports transfer (the spec guarantees it), so it is the
            // fallback below when no transfer-only family exists.
            if ((queueFamily.queueFlags & vk::QueueFlagBits::eTransfer) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                QueueFamilies.TransferFamily = i;
            }

            // Present support is only meaningful when there's a surface.
            if (Surface && device.getSurfaceSupportKHR(i, Surface).value)
            {
                QueueFamilies.PresentFamily = i;
            }
        }

        // MoltenVK (and any GPU with no transfer-only family): collapse transfer
        // onto graphics. This is the primary dev platform and the tested path —
        // TransferIsGraphics() is true and no cross-queue ownership transfer is
        // needed; the submission lock alone serializes the shared queue.
        if (!QueueFamilies.TransferFamily.has_value())
        {
            QueueFamilies.TransferFamily = QueueFamilies.GraphicsFamily;
        }

        return QueueFamilies;
    }

    SwapChainSupportDetails Context::Native::QuerySwapChainSupport(vk::PhysicalDevice device) const
    {
        return SwapChainSupportDetails{
            .Capabilities = device.getSurfaceCapabilitiesKHR(Surface).value,
            .Formats = device.getSurfaceFormatsKHR(Surface).value,
            .PresentModes = device.getSurfacePresentModesKHR(Surface).value};
    }

    bool Context::Native::CheckDeviceExtensionSupport(vk::PhysicalDevice device) const
    {
        auto availableExtensions = device.enumerateDeviceExtensionProperties(nullptr).value;

        set<string> requiredExtensions(DeviceExtensions.begin(), DeviceExtensions.end());

        for (const auto& extension : availableExtensions)
        {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    vk::Device Context::Native::CreateDevice()
    {
        QueueFamilyIndices indices = FindQueueFamilies(PhysicalDevice);

        vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
        set<u32> uniqueQueueFamilies{indices.GraphicsFamily.value()};
        if (indices.CanPresent())
        {
            uniqueQueueFamilies.insert(indices.PresentFamily.value());
        }
        // No-op when transfer collapses onto graphics (MoltenVK); adds the
        // dedicated transfer family on a discrete GPU.
        uniqueQueueFamilies.insert(indices.TransferFamily.value());

        const f32 queuePriority = 1.0f;
        for (const u32 queueFamily : uniqueQueueFamilies)
        {
            const vk::DeviceQueueCreateInfo queueCreateInfo{.queueFamilyIndex = queueFamily,
                                                            .queueCount = 1,
                                                            .pQueuePriorities = &queuePriority};

            queueCreateInfos.push_back(queueCreateInfo);
        }

        const vk::PhysicalDeviceFeatures deviceFeatures{.sampleRateShading = VK_TRUE,
                                                        .samplerAnisotropy = VK_TRUE,
                                                        .shaderSampledImageArrayDynamicIndexing =
                                                            VK_TRUE};

        auto deviceExtensions = DeviceExtensions;

        // VK_KHR_portability_subset must be enabled iff the device advertises it (a
        // non-conformant implementation such as MoltenVK); a conformant native driver does
        // not expose it and must not be asked for it. Add it per-device here rather than in
        // the always-required list so desktop GPUs are not wrongly rejected as unsuitable.
        {
            const auto available = PhysicalDevice.enumerateDeviceExtensionProperties(nullptr).value;
            for (const auto& extension : available)
            {
                if (std::strcmp(extension.extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) ==
                    0)
                {
                    deviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
                    break;
                }
            }
        }

        vk::PhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{.dynamicRendering =
                                                                                vk::True};

        vk::PhysicalDeviceFeatures2 features2{.pNext = &dynamicRenderingFeatures,
                                              .features = deviceFeatures};

        PhysicalDevice.getFeatures2(&features2);

        // The GPU-driven cull issues a multiDrawIndirect command buffer and carries each
        // candidate's index in the command's firstInstance (read back as an instance-rate
        // vertex attribute), which needs drawIndirectFirstInstance. Enable the pair only
        // when both are supported, so a half-supported device never enables one alone;
        // GpuDrivenCullingSupported gates CullMode::GPU, with the CPU path as the fallback.
        // MoltenVK lacks drawIndirectCount, so the count-buffer (vkCmdDrawIndexedIndirectCount)
        // path is out — the buffer holds a fixed candidate maximum and culled slots no-op.
        const bool gpuDrivenCullingSupported =
            features2.features.multiDrawIndirect && features2.features.drawIndirectFirstInstance;
        GpuDrivenCullingSupported = gpuDrivenCullingSupported;
        features2.features.multiDrawIndirect = gpuDrivenCullingSupported ? vk::True : vk::False;
        features2.features.drawIndirectFirstInstance =
            gpuDrivenCullingSupported ? vk::True : vk::False;

        // Timeline semaphores are required for the async-upload sync channel.
        // Fatal-assert if the device lacks support before enabling the feature.
        vk::PhysicalDeviceVulkan12Features supportedVulkan12Features{};
        vk::PhysicalDeviceFeatures2 supportedFeatures2{.pNext = &supportedVulkan12Features};
        PhysicalDevice.getFeatures2(&supportedFeatures2);
        VE_ASSERT(supportedVulkan12Features.timelineSemaphore,
                  "Physical device does not support timeline semaphores!");

        // Designators must be in declaration order (MSVC enforces the C++ rule; clang is
        // lenient). timelineSemaphore is declared late in PhysicalDeviceVulkan12Features,
        // after the descriptor-indexing block and before bufferDeviceAddress.
        vk::PhysicalDeviceVulkan12Features vulkan12Features{
            .pNext = &features2,
            .shaderUniformBufferArrayNonUniformIndexing = vk::True,
            .shaderSampledImageArrayNonUniformIndexing = vk::True,
            .shaderStorageBufferArrayNonUniformIndexing = vk::True,
            .descriptorBindingUniformBufferUpdateAfterBind = vk::True,
            .descriptorBindingSampledImageUpdateAfterBind = vk::True,
            .descriptorBindingStorageImageUpdateAfterBind = vk::True,
            .descriptorBindingStorageBufferUpdateAfterBind = vk::True,
            .descriptorBindingUpdateUnusedWhilePending = vk::True,
            .descriptorBindingPartiallyBound = vk::True,
            .runtimeDescriptorArray = vk::True,
            .timelineSemaphore = vk::True,
            .bufferDeviceAddress = vk::True,
            .shaderOutputViewportIndex = vk::True,
            .shaderOutputLayer = vk::True,
        };

        const vk::DeviceCreateInfo deviceCreateInfo{
            .pNext = &vulkan12Features,
            .queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size()),
            .pQueueCreateInfos = queueCreateInfos.data(),
            .enabledExtensionCount = static_cast<u32>(deviceExtensions.size()),
            .ppEnabledExtensionNames = deviceExtensions.data()};

        return PhysicalDevice.createDevice(deviceCreateInfo, nullptr).value;
    }

    SynchronizationFrame& Context::AcquireNextFrame()
    {
        m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight].GetInFlightFence().Wait();

        // Fence signalled: GPU finished the work from the last use of this frame
        // index — everything retired then is safe to destroy now.
        m_Native->DrainRetireBin(m_Native->RetireBins[m_Native->CurrentFrameInFlight]);

        // Reclaim upload scratch whose transfer-timeline value the GPU has now reached.
        m_Native->DrainTransferRetireList();

        m_Native->Bindless->OnFrameAcquired(m_Native->CurrentFrameInFlight);

        return m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight];
    }

    Context::Native::RetireBin& Context::Native::CurrentRetireBin()
    {
        VE_ASSERT(!Disposed,
                  "Context::Retire after teardown — a resource outlived the rendering context");
        return RetireBins[CurrentFrameInFlight];
    }

    // Every Retire overload, RetireOnTransfer, and both drains hold RetireMutex:
    // a worker dropping upload scratch must not race the main thread's bin reads,
    // the bin vectors' reallocation, or the transfer-list drain.
    void Context::Native::Retire(vk::Buffer buffer, VmaAllocation allocation)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().Buffers.emplace_back(buffer, allocation);
    }

    void Context::Native::Retire(vk::Image image, VmaAllocation allocation)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().Images.emplace_back(image, allocation);
    }

    void Context::Native::Retire(vk::ImageView imageView)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().ImageViews.push_back(imageView);
    }
    void Context::Native::Retire(vk::Sampler sampler)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().Samplers.push_back(sampler);
    }
    void Context::Native::Retire(vk::ShaderModule shaderModule)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().ShaderModules.push_back(shaderModule);
    }
    void Context::Native::Retire(vk::Pipeline pipeline)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().Pipelines.push_back(pipeline);
    }
    void Context::Native::Retire(vk::PipelineLayout pipelineLayout)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().PipelineLayouts.push_back(pipelineLayout);
    }
    void Context::Native::Retire(vk::DescriptorSet descriptorSet)
    {
        const std::scoped_lock lock(RetireMutex);
        CurrentRetireBin().DescriptorSets.push_back(descriptorSet);
    }

    void Context::Native::RetireOnTransfer(vk::Buffer buffer, VmaAllocation allocation,
                                           u64 timelineValue)
    {
        const std::scoped_lock lock(RetireMutex);
        TransferRetireList.emplace_back(buffer, allocation, timelineValue);
    }

    void Context::Native::DrainTransferRetireList()
    {
        const std::scoped_lock lock(RetireMutex);

        // Hold the lock across the GetValue() read and the erase so a concurrent
        // RetireOnTransfer cannot append between them and be missed or double-freed.
        const u64 reached = TransferTimeline->GetValue();

        std::erase_if(TransferRetireList,
                      [this, reached](const TransferRetireEntry& entry)
                      {
                          if (entry.TimelineValue > reached)
                          {
                              return false;
                          }

                          vmaDestroyBuffer(Allocator, entry.Buffer, entry.Allocation);
                          return true;
                      });
    }

    void Context::Native::DrainRetireBin(RetireBin& bin)
    {
        const std::scoped_lock lock(RetireMutex);

        // Destroy dependents before the objects they reference: descriptor sets
        // first, then views, then the images/buffers backing them. Everything in
        // the bin is already GPU-idle.
        for (auto descriptorSet : bin.DescriptorSets)
        {
            VK_ASSERT(
                Device.freeDescriptorSets(DescriptorPool->GetVkDescriptorPool(), descriptorSet),
                "failed to free descriptor set!");
        }
        for (auto imageView : bin.ImageViews)
        {
            Device.destroyImageView(imageView);
        }
        for (auto& [image, allocation] : bin.Images)
        {
            vmaDestroyImage(Allocator, image, allocation);
        }
        for (auto& [buffer, allocation] : bin.Buffers)
        {
            vmaDestroyBuffer(Allocator, buffer, allocation);
        }
        for (auto pipeline : bin.Pipelines)
        {
            Device.destroyPipeline(pipeline);
        }
        for (auto pipelineLayout : bin.PipelineLayouts)
        {
            Device.destroyPipelineLayout(pipelineLayout);
        }
        for (auto sampler : bin.Samplers)
        {
            Device.destroySampler(sampler);
        }
        for (auto shaderModule : bin.ShaderModules)
        {
            Device.destroyShaderModule(shaderModule);
        }

        bin = {};
    }

    void Context::Native::DrainAllRetireBins()
    {
        for (auto& bin : RetireBins)
        {
            DrainRetireBin(bin);
        }
    }

    void Context::AddFrameTransferWait(const TimelineSemaphore& timeline, const u64 value)
    {
        const std::scoped_lock lock(m_Native->SubmitMutex);
        m_Native->PendingFrameTransferWaits.emplace_back(timeline.GetNative().Semaphore, value);
    }

    void Context::SubmitFrame(const SynchronizationFrame& frame) const
    {
        const auto commandBuffer = frame.GetCommandBuffer()->GetNative().CommandBuffer;

        // Drain accumulated transfer-timeline waits under SubmitMutex before
        // LockedSubmit re-takes it. Cleared each frame — a satisfied wait must
        // not re-block the next frame.
        vector<vk::Semaphore> waitSemaphores;
        vector<vk::PipelineStageFlags> waitStages;
        vector<u64> waitValues;
        {
            const std::scoped_lock lock(m_Native->SubmitMutex);
            for (const auto& [semaphore, value] : m_Native->PendingFrameTransferWaits)
            {
                waitSemaphores.push_back(semaphore);
                // Sample an async-uploaded resource at the fragment-shader stage.
                waitStages.emplace_back(vk::PipelineStageFlagBits::eFragmentShader);
                waitValues.push_back(value);
            }
            m_Native->PendingFrameTransferWaits.clear();
        }

        const bool hasTransferWaits = !waitSemaphores.empty();

        // Headless: no swapchain image to wait on or signal for present. The
        // transfer waits, if any, are the only waits this branch carries.
        if (m_Native->Headless)
        {
            if (!hasTransferWaits)
            {
                const vk::SubmitInfo headlessSubmit{
                    .commandBufferCount = 1,
                    .pCommandBuffers = &commandBuffer,
                };

                m_Native->LockedSubmit(m_Native->GraphicsQueue, headlessSubmit,
                                       frame.GetInFlightFence().GetNative().Fence);
                return;
            }

            // Headless has no binary semaphores; every wait here is a timeline wait.
            const vk::TimelineSemaphoreSubmitInfo timelineInfo{
                .waitSemaphoreValueCount = static_cast<u32>(waitValues.size()),
                .pWaitSemaphoreValues = waitValues.data(),
            };

            const vk::SubmitInfo headlessSubmit{
                .pNext = &timelineInfo,
                .waitSemaphoreCount = static_cast<u32>(waitSemaphores.size()),
                .pWaitSemaphores = waitSemaphores.data(),
                .pWaitDstStageMask = waitStages.data(),
                .commandBufferCount = 1,
                .pCommandBuffers = &commandBuffer,
            };

            m_Native->LockedSubmit(m_Native->GraphicsQueue, headlessSubmit,
                                   frame.GetInFlightFence().GetNative().Fence);
            return;
        }

        const auto renderFinishedSemaphore =
            frame.GetRenderFinishedSemaphore().GetNative().Semaphore;
        const auto imageAvailableSemaphore =
            frame.GetImageAvailableSemaphore().GetNative().Semaphore;

        // The image-available binary semaphore leads the arrays; timeline waits
        // follow. Its stage must match SwapChain::AcquireWaitStage. The binary
        // semaphore's slot in the timeline-values array is ignored by the driver
        // but must be present to keep the array lengths equal.
        waitSemaphores.insert(waitSemaphores.begin(), imageAvailableSemaphore);
        waitStages.insert(waitStages.begin(), SwapChain::AcquireWaitStage);
        waitValues.insert(waitValues.begin(), 0);

        const vk::TimelineSemaphoreSubmitInfo timelineInfo{
            .waitSemaphoreValueCount = static_cast<u32>(waitValues.size()),
            .pWaitSemaphoreValues = waitValues.data(),
        };

        const vk::SubmitInfo submitInfo{
            .pNext = hasTransferWaits ? &timelineInfo : nullptr,
            .waitSemaphoreCount = static_cast<u32>(waitSemaphores.size()),
            .pWaitSemaphores = waitSemaphores.data(),
            .pWaitDstStageMask = waitStages.data(),
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &renderFinishedSemaphore,
        };

        m_Native->LockedSubmit(m_Native->GraphicsQueue, submitInfo,
                               frame.GetInFlightFence().GetNative().Fence);
    }

    void Context::PresentFrame(const SynchronizationFrame& frame)
    {
        // Headless: nothing to present — just advance to the next in-flight frame.
        if (m_Native->Headless)
        {
            m_Native->CurrentFrameInFlight =
                (m_Native->CurrentFrameInFlight + 1) % m_Native->MaxFramesInFlight;
            return;
        }

        auto renderFinishedSemaphore = frame.GetRenderFinishedSemaphore().GetNative().Semaphore;
        auto swapChain = m_Native->SwapChain->GetVkSwapChain();
        auto imageIndex = m_Native->SwapChain->GetCurrentImageIndex();

        const vk::PresentInfoKHR presentInfo{.waitSemaphoreCount = 1,
                                             .pWaitSemaphores = &renderFinishedSemaphore,
                                             .swapchainCount = 1,
                                             .pSwapchains = &swapChain,
                                             .pImageIndices = &imageIndex};

        auto result = m_Native->PresentQueue.presentKHR(presentInfo);

        if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR ||
            m_RenderExtentChanged)
        {
            m_RenderExtentChanged = false;

            m_Window->SpinUntilValidSize();

            WaitIdle();

            UpdateRenderExtent();
        }
        else if (result != vk::Result::eSuccess)
        {
            VE_ASSERT(false, "failed to present swap chain image!");
        }

        m_Native->CurrentFrameInFlight =
            (m_Native->CurrentFrameInFlight + 1) % m_Native->MaxFramesInFlight;
    }

    void Context::SubmitImmediateCommands(CommandBuffer& commandBuffer) const
    {
        const auto commandBufferHandle = commandBuffer.GetNative().CommandBuffer;

        const vk::SubmitInfo submitInfo = {.commandBufferCount = 1,
                                           .pCommandBuffers = &commandBufferHandle};

        // Block on this submission's own fence, not the whole device: an immediate
        // submit must outlive only its work, not drain every queue (a device-wide
        // WaitIdle here stalls in-flight frame and transfer work too).
        const Unique<Fence> fence = Fence::Create(const_cast<Context&>(*this), "Immediate Submit");

        m_Native->LockedSubmit(m_Native->GraphicsQueue, submitInfo, fence->GetNative().Fence);

        fence->Wait();
    }

    void Context::UpdateRenderExtent()
    {
        Log::Info("Updating extent {0}x{1}", m_RenderExtent.x, m_RenderExtent.y);

        m_Native->SwapChain->RenderExtentChanged();

        m_RenderExtent = m_Native->SwapChain->GetExtent();

        m_Native->SwapChain->Invalidated();

        m_RenderExtentChanged = false;
    }
}
