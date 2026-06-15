#define VMA_IMPLEMENTATION
#include <Veng/Renderer/Context.h>

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
                  const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                  void* pUserData)
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

    Context::Context() : m_Native(CreateUnique<Native>())
    {
    }

    Context::~Context() = default;

    Context::Native& Context::GetNative() const { return *m_Native; }

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
        QueueFamilyIndices indices = FindQueueFamilies(device);

        auto supportedFeatures = device.getFeatures();

        bool extensionsSupported = CheckDeviceExtensionSupport(device);

        // Headless needs neither a present queue nor an adequate swapchain.
        if (Headless)
        {
            return indices.IsComplete() && extensionsSupported && supportedFeatures.samplerAnisotropy;
        }

        bool swapChainAdequate = false;
        if (extensionsSupported)
        {
            SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device);
            swapChainAdequate = !swapChainSupport.Formats.empty() &&
                !swapChainSupport.PresentModes.empty();
        }

        return indices.IsComplete() && indices.CanPresent() && extensionsSupported && swapChainAdequate &&
            supportedFeatures.samplerAnisotropy;
    }

    void Context::Initialize(const ContextInfo& info, Window* window)
    {
        const bool headless = (window == nullptr);
        m_Native->Headless = headless;

        Log::Info(headless ? "Initializing Vulkan context (headless)" : "Initializing Vulkan context");

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
            {
                return std::strcmp(extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            });
        }

        auto extensions = m_Native->GetRequiredExtensions();

        const vk::ApplicationInfo appInfo{
            .pApplicationName = info.ApplicationName.c_str(),
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName = info.EngineName.c_str(),
            .engineVersion = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion = VK_API_VERSION_1_3
        };

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
            .pfnUserCallback = DebugCallback
        };

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
        instanceCreateInfo.enabledLayerCount =
            static_cast<u32>(m_Native->ValidationLayers.size());
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
        m_Native->DebugMessenger = m_Native->Instance.createDebugUtilsMessengerEXT(debugCreateInfo).value;
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
                file.read(reinterpret_cast<char*>(initial.data()), static_cast<std::streamsize>(size));
            }
        }

        vk::PipelineCacheCreateInfo cacheInfo{
            .initialDataSize = initial.size(),
            .pInitialData = initial.empty() ? nullptr : initial.data(),
        };
        m_Native->PipelineCache = m_Native->Device.createPipelineCache(cacheInfo).value;

        VmaAllocatorCreateInfo allocatorInfo{
            .physicalDevice = m_Native->PhysicalDevice,
            .device = m_Native->Device,
            .instance = m_Native->Instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };

        vmaCreateAllocator(&allocatorInfo, &m_Native->Allocator);

        m_Native->GraphicsQueue = m_Native->Device.getQueue(m_Native->QueueFamilies.GraphicsFamily.value(), 0);

        // Transfer queue. On MoltenVK this resolves to the same family (and
        // queue index 0) as graphics, so TransferQueue and GraphicsQueue are the
        // same handle and the submission lock is what keeps them safe.
        m_Native->TransferQueue = m_Native->Device.getQueue(m_Native->QueueFamilies.TransferFamily.value(), 0);

        Log::Info("Queue families: graphics={0}, transfer={1}{2}",
                  m_Native->QueueFamilies.GraphicsFamily.value(),
                  m_Native->QueueFamilies.TransferFamily.value(),
                  m_Native->QueueFamilies.TransferIsGraphics()
                      ? " (transfer collapsed onto graphics)"
                      : " (dedicated transfer family)");

        if (!headless)
        {
            m_Native->PresentQueue = m_Native->Device.getQueue(m_Native->QueueFamilies.PresentFamily.value(), 0);

            m_Native->SwapChain = SwapChain::Create(*this, {
                .MaxImageCount = 2,
                .Width = window->GetWidth(),
                .Height = window->GetHeight(),
                .Format = vk::Format::eR16G16B16A16Sfloat,
            });

            Log::Info("Created {0} swap chain images ({1}x{2})", m_Native->SwapChain->GetImageCount(),
                      m_Native->SwapChain->GetWidth(), m_Native->SwapChain->GetHeight());

            m_RenderExtent = m_Native->SwapChain->GetExtent();
        }
        else
        {
            // No swapchain to derive an extent from; fall back to the requested
            // internal extent for GetRenderExtent() consumers.
            m_RenderExtent = info.InternalRenderExtent;
        }

        Log::Info("Setting initial render extents:");
        m_InternalRenderExtent = info.InternalRenderExtent;
        m_OutputFormat = info.OutputFormat;
        m_DepthFormat = info.DepthFormat;
        Log::Info("  Internal: {0}x{1}", m_InternalRenderExtent.x, m_InternalRenderExtent.y);
        Log::Info("  Output: {0}x{1}", m_RenderExtent.x, m_RenderExtent.y);

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

        m_Native->DescriptorPool = DescriptorPool::Create(*this, {
            .Name = "Primary Pool",
            .MaxSets = 100000,
            .PoolSizes = poolSizes,
            .Flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet |
            vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind
        });

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
    }

    void Context::DisposeResources()
    {
        // Drop any bindless acquires that never reached a frame (a context torn
        // down without rendering): their views retire into the current bin, which
        // the drain below reclaims while the device and allocator are still alive.
        m_PendingBindlessAcquires.clear();

        // Application::Run has already waited the GPU idle. Anything the
        // consumer released in OnDispose has retired into the bins — drain them
        // all before the frames (and their command buffers) go away.
        m_Native->DrainAllRetireBins();

        // Upload scratch pinned to the transfer timeline never sees an
        // AcquireNextFrame after the last upload (the headless smoke renders one
        // frame then exits). The GPU is idle, so every value has been reached:
        // host-wait the timeline's last signalled value, then reclaim all entries.
        m_Native->TransferTimeline->Wait(m_Native->TransferTimelineValue);
        m_Native->DrainTransferRetireList();

        m_Native->SynchronizationFrames.clear();
    }

    void Context::Dispose()
    {
        Log::Info("Disposing rendering context...");

        // Drop the registry's set/layout and registered-resource Refs first —
        // their destructors retire handles into the current bins, which the
        // drain below then cleans up while the device is still alive.
        m_Native->Bindless.reset();

        // Drain any handles still in the bins while the device, allocator and
        // descriptor pool are alive. Then host-wait the transfer timeline and
        // reclaim any upload scratch still pinned to it — both before flipping
        // Disposed, so a late retire/release is caught rather than leaked. After
        // this point anything dropped is a leak/bug.
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
                file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            }
            else
            {
                Log::Warn("pipeline cache write failed: {}", m_Native->PipelineCachePath->string());
            }
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

        // Create each pool on the worker that owns it. A VkCommandPool may only be
        // accessed by one thread at a time; creating worker i's pool on worker i
        // keeps that invariant from the first touch and matches the thread that
        // records uploads into it. Pool creation is independent per worker, so the
        // ForEachWorker bodies run concurrently with no shared state.
        taskSystem.ForEachWorker([this, transferFamily](u32 workerIndex)
        {
            m_Native->TransferPools[workerIndex].Pool = m_Native->Device.createCommandPool(vk::CommandPoolCreateInfo{
                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                .queueFamilyIndex = transferFamily,
            }).value;
        });

        // Allocate each worker's transfer command buffer from its own pool, serially
        // on this thread. AllocationPoolOverride is shared mutable state, so it must
        // not be written from the concurrent ForEachWorker bodies above. Each buffer
        // comes from a distinct pool no worker is touching yet, so the worker that
        // later records into it remains the only thread accessing that pool.
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

    void Context::Native::LockedSubmit(vk::Queue queue, const vk::SubmitInfo& submitInfo, vk::Fence fence)
    {
        std::lock_guard lock(SubmitMutex);
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
                  "SubmitTransfer: worker index {} out of range ({} transfer pools)",
                  workerIndex, m_Native->TransferPools.size());

        auto& pool = m_Native->TransferPools[workerIndex];
        pool.CommandBuffer->End();

        const auto vkCommandBuffer = pool.CommandBuffer->GetNative().CommandBuffer;
        const vk::Semaphore vkTimeline = timeline.GetNative().Semaphore;

        // The value is allocated, the signal-info built, and the submit issued all
        // under one lock: a timeline must signal strictly increasing values, so a
        // worker must not compute its value and then race another worker for the
        // queue (which could submit the lower value second).
        std::lock_guard lock(m_Native->SubmitMutex);

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

    bool Context::IsHeadless() const { return m_Native->Headless; }

    const QueueFamilyIndices& Context::GetQueueFamilies() const
    {
        return m_Native->QueueFamilies;
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

        // Acquire any bindless-sampled resources that went resident since the
        // last frame onto the graphics queue before any pass records — these are
        // invisible to the RenderGraph (sampled through set 0), so the graph
        // can't derive the transition itself. The acquire is idempotent and only
        // ever does work the first frame after a resource becomes resident.
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
            Backend::TransitionImage(*commandBuffer, *GetCurrentSwapChainImage(), ImageLayout::PresentSrc);
        }

        commandBuffer->End();

        SubmitFrame(frame);

        PresentFrame(frame);
    }

    u32 Context::GetMaxFramesInFlight() const { return m_Native->MaxFramesInFlight; }
    u32 Context::GetCurrentFrameInFlight() const { return m_Native->CurrentFrameInFlight; }

    uvec2 Context::GetSwapChainExtent() const { VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)"); return m_Native->SwapChain->GetExtent(); }
    Format Context::GetSwapChainFormat() const { VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)"); return m_Native->SwapChain->GetFormat(); }
    Ref<Image> Context::GetCurrentSwapChainImage() const { VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)"); return m_Native->SwapChain->GetCurrentImage(); }
    Ref<ImageView> Context::GetCurrentSwapChainImageView() const { VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)"); return m_Native->SwapChain->GetCurrentImageView(); }
    u32 Context::GetSwapChainImageCount() const { VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)"); return m_Native->SwapChain->GetImageCount(); }
    u32 Context::GetCurrentSwapChainImageIndex() const { VE_ASSERT(m_Native->SwapChain, "no swapchain (headless)"); return m_Native->SwapChain->GetCurrentImageIndex(); }

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
        auto result = m_Native->SwapChain->AcquireNextImage(semaphore);

        if (result == vk::Result::eErrorOutOfDateKHR)
        {
            Log::Warn("Out of date swap chain image!");

            UpdateRenderExtent();
        }
        else if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR)
        {
            VE_ASSERT(false, "failed to acquire swap chain image!");
        }
    }

    void Context::WaitIdle() const
    {
        VK_ASSERT(m_Native->Device.waitIdle(), "failed to wait for device idle!");
    }

    BindlessRegistry& Context::GetBindlessRegistry() const { return *m_Native->Bindless; }

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
                const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

                RequiredExtensions = vector<const char*>(glfwExtensions,
                                                           glfwExtensions + glfwExtensionCount);

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
            .PresentModes = device.getSurfacePresentModesKHR(Surface).value
        };
    }

    bool Context::Native::CheckDeviceExtensionSupport(vk::PhysicalDevice device) const
    {
        auto availableExtensions = device.enumerateDeviceExtensionProperties(nullptr).value;

        set<string> requiredExtensions(DeviceExtensions.begin(),
                                       DeviceExtensions.end());

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

        f32 queuePriority = 1.0f;
        for (u32 queueFamily : uniqueQueueFamilies)
        {
            vk::DeviceQueueCreateInfo queueCreateInfo{
                .queueFamilyIndex = queueFamily,
                .queueCount = 1,
                .pQueuePriorities = &queuePriority
            };

            queueCreateInfos.push_back(queueCreateInfo);
        }

        vk::PhysicalDeviceFeatures deviceFeatures{
            .sampleRateShading = VK_TRUE,
            .samplerAnisotropy = VK_TRUE,
            .shaderSampledImageArrayDynamicIndexing = VK_TRUE
        };

        auto deviceExtensions = DeviceExtensions;

        vk::PhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{
            .dynamicRendering = vk::True
        };

        vk::PhysicalDeviceFeatures2 features2{
            .pNext = &dynamicRenderingFeatures,
            .features = deviceFeatures
        };

        PhysicalDevice.getFeatures2(&features2);

        // Timeline semaphores are a hard requirement for the loader↔render sync
        // channel — not a fallback. Query support explicitly and fatal-assert if
        // the device lacks it before enabling the feature below.
        vk::PhysicalDeviceVulkan12Features supportedVulkan12Features{};
        vk::PhysicalDeviceFeatures2 supportedFeatures2{ .pNext = &supportedVulkan12Features };
        PhysicalDevice.getFeatures2(&supportedFeatures2);
        VE_ASSERT(supportedVulkan12Features.timelineSemaphore,
                  "Physical device does not support timeline semaphores!");

        vk::PhysicalDeviceVulkan12Features vulkan12Features{
            .pNext = &features2,
            .timelineSemaphore = vk::True,
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
            .bufferDeviceAddress = vk::True,
            .shaderOutputViewportIndex = vk::True,
            .shaderOutputLayer = vk::True,
        };

        vk::DeviceCreateInfo deviceCreateInfo{
            .pNext = &vulkan12Features,
            .queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size()),
            .pQueueCreateInfos = queueCreateInfos.data(),
            .enabledExtensionCount = static_cast<u32>(deviceExtensions.size()),
            .ppEnabledExtensionNames = deviceExtensions.data()
        };

        return PhysicalDevice.createDevice(deviceCreateInfo, nullptr).value;
    }

    SynchronizationFrame& Context::AcquireNextFrame()
    {
        m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight].GetInFlightFence().Wait();

        // The fence is now signaled, so the GPU has finished the work that was
        // recorded the last time this frame index was current — anything
        // retired then is safe to destroy.
        m_Native->DrainRetireBin(m_Native->RetireBins[m_Native->CurrentFrameInFlight]);

        // Reclaim any upload scratch whose transfer-timeline value the GPU has
        // now reached — keyed on the transfer timeline, not this frame's fence.
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
        std::lock_guard lock(RetireMutex);
        CurrentRetireBin().Buffers.emplace_back(buffer, allocation);
    }

    void Context::Native::Retire(vk::Image image, VmaAllocation allocation)
    {
        std::lock_guard lock(RetireMutex);
        CurrentRetireBin().Images.emplace_back(image, allocation);
    }

    void Context::Native::Retire(vk::ImageView imageView) { std::lock_guard lock(RetireMutex); CurrentRetireBin().ImageViews.push_back(imageView); }
    void Context::Native::Retire(vk::Sampler sampler) { std::lock_guard lock(RetireMutex); CurrentRetireBin().Samplers.push_back(sampler); }
    void Context::Native::Retire(vk::ShaderModule shaderModule) { std::lock_guard lock(RetireMutex); CurrentRetireBin().ShaderModules.push_back(shaderModule); }
    void Context::Native::Retire(vk::Pipeline pipeline) { std::lock_guard lock(RetireMutex); CurrentRetireBin().Pipelines.push_back(pipeline); }
    void Context::Native::Retire(vk::PipelineLayout pipelineLayout) { std::lock_guard lock(RetireMutex); CurrentRetireBin().PipelineLayouts.push_back(pipelineLayout); }
    void Context::Native::Retire(vk::DescriptorSet descriptorSet) { std::lock_guard lock(RetireMutex); CurrentRetireBin().DescriptorSets.push_back(descriptorSet); }

    void Context::Native::RetireOnTransfer(vk::Buffer buffer, VmaAllocation allocation, u64 timelineValue)
    {
        std::lock_guard lock(RetireMutex);
        TransferRetireList.emplace_back(buffer, allocation, timelineValue);
    }

    void Context::Native::DrainTransferRetireList()
    {
        std::lock_guard lock(RetireMutex);

        // Hold the lock across the GetValue() read and the erase so a concurrent
        // RetireOnTransfer cannot append between them and be missed or double-freed.
        const u64 reached = TransferTimeline->GetValue();

        std::erase_if(TransferRetireList, [this, reached](const TransferRetireEntry& entry)
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
        std::lock_guard lock(RetireMutex);

        // Destroy dependents before the objects they reference: descriptor sets
        // first, then views, then the images/buffers backing them. Everything in
        // the bin is already GPU-idle.
        for (auto descriptorSet : bin.DescriptorSets)
            VK_ASSERT(Device.freeDescriptorSets(DescriptorPool->GetVkDescriptorPool(), descriptorSet),
                      "failed to free descriptor set!");
        for (auto imageView : bin.ImageViews)
            Device.destroyImageView(imageView);
        for (auto& [image, allocation] : bin.Images)
            vmaDestroyImage(Allocator, image, allocation);
        for (auto& [buffer, allocation] : bin.Buffers)
            vmaDestroyBuffer(Allocator, buffer, allocation);
        for (auto pipeline : bin.Pipelines)
            Device.destroyPipeline(pipeline);
        for (auto pipelineLayout : bin.PipelineLayouts)
            Device.destroyPipelineLayout(pipelineLayout);
        for (auto sampler : bin.Samplers)
            Device.destroySampler(sampler);
        for (auto shaderModule : bin.ShaderModules)
            Device.destroyShaderModule(shaderModule);

        bin = {};
    }

    void Context::Native::DrainAllRetireBins()
    {
        for (auto& bin : RetireBins)
            DrainRetireBin(bin);
    }

    void Context::AddFrameTransferWait(const TimelineSemaphore& timeline, const u64 value)
    {
        std::lock_guard lock(m_Native->SubmitMutex);
        m_Native->PendingFrameTransferWaits.emplace_back(timeline.GetNative().Semaphore, value);
    }

    void Context::SubmitFrame(const SynchronizationFrame& frame) const
    {
        const auto commandBuffer = frame.GetCommandBuffer()->GetNative().CommandBuffer;

        // Drain the accumulated transfer-timeline waits under SubmitMutex; the
        // submit itself re-takes the lock in LockedSubmit. Cleared each frame so
        // a wait satisfied this frame never re-blocks the next.
        vector<vk::Semaphore> waitSemaphores;
        vector<vk::PipelineStageFlags> waitStages;
        vector<u64> waitValues;
        {
            std::lock_guard lock(m_Native->SubmitMutex);
            for (const auto& [semaphore, value] : m_Native->PendingFrameTransferWaits)
            {
                waitSemaphores.push_back(semaphore);
                // Sample an async-uploaded resource at the fragment-shader stage.
                waitStages.push_back(vk::PipelineStageFlagBits::eFragmentShader);
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

            // Binary present/acquire semaphores need no timeline value, so a value
            // is supplied for every wait semaphore (ignored for binary ones); here
            // every wait is a timeline wait.
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

        const auto renderFinishedSemaphore = frame.GetRenderFinishedSemaphore().GetNative().Semaphore;
        const auto imageAvailableSemaphore = frame.GetImageAvailableSemaphore().GetNative().Semaphore;

        // The image-available binary semaphore leads the wait arrays; the timeline
        // waits ride alongside it, never replacing it. Its wait stage must match
        // the srcStage each swapchain image's first transition is seeded with
        // (SwapChain::AcquireWaitStage). The binary semaphore's slot in the
        // timeline-values array is ignored by the driver but must be present so the
        // value array length matches the semaphore array.
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

        m_Native->LockedSubmit(m_Native->GraphicsQueue, submitInfo, frame.GetInFlightFence().GetNative().Fence);
    }

    void Context::PresentFrame(const SynchronizationFrame& frame)
    {
        // Headless: nothing to present — just advance to the next in-flight frame.
        if (m_Native->Headless)
        {
            m_Native->CurrentFrameInFlight = (m_Native->CurrentFrameInFlight + 1) % m_Native->MaxFramesInFlight;
            return;
        }

        auto renderFinishedSemaphore = frame.GetRenderFinishedSemaphore().GetNative().Semaphore;
        auto swapChain = m_Native->SwapChain->GetVkSwapChain();
        auto imageIndex = m_Native->SwapChain->GetCurrentImageIndex();

        vk::PresentInfoKHR presentInfo{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &renderFinishedSemaphore,
            .swapchainCount = 1,
            .pSwapchains = &swapChain,
            .pImageIndices = &imageIndex
        };

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

        m_Native->CurrentFrameInFlight = (m_Native->CurrentFrameInFlight + 1) % m_Native->MaxFramesInFlight;
    }

    void Context::SubmitImmediateCommands(CommandBuffer& commandBuffer) const
    {
        const auto commandBufferHandle = commandBuffer.GetNative().CommandBuffer;

        const vk::SubmitInfo submitInfo = {
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBufferHandle
        };

        m_Native->LockedSubmit(m_Native->GraphicsQueue, submitInfo, VK_NULL_HANDLE);

        WaitIdle();
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
