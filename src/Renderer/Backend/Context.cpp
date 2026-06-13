#define VMA_IMPLEMENTATION
#include <Veng/Renderer/Context.h>

#include <set>

#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

#include <Veng/Vendor/ImGui.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <Veng/Renderer/ImGuiTexture.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Vendor/IconsMaterialDesign.h>
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

        bool swapChainAdequate = false;
        if (extensionsSupported)
        {
            SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device);
            swapChainAdequate = !swapChainSupport.Formats.empty() &&
                !swapChainSupport.PresentModes.empty();
        }

        return indices.IsComplete() && extensionsSupported && swapChainAdequate && supportedFeatures.samplerAnisotropy;
    }

    void Context::Initialize(const ContextInfo& info, Window* window)
    {
        s_Instance = this;

        Log::Info("Initializing Vulkan context");

        VULKAN_HPP_DEFAULT_DISPATCHER.init();

        VE_ASSERT(window != nullptr, "Context::Initialize requires a window (headless mode is not supported yet)");

        m_Window = window;

        if (glfwVulkanSupported() != GLFW_TRUE)
        {
            VE_ASSERT(false, "Vulkan is not supported on this system!");
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

        instanceCreateInfo.pNext = &debugCreateInfo;
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
        m_Native->DebugMessenger = m_Native->Instance.createDebugUtilsMessengerEXT(debugCreateInfo);
#endif

        DebugMarkers::Initialize();

        window->CreateSurface(*this);

        Log::Info("Initializing surface");

        m_Native->Surface = GetVkSurface(*window);

        m_Native->PhysicalDevice = m_Native->GetPhysicalDevice();

        m_Native->Device = m_Native->CreateDevice();

        VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Native->Device);

        VmaAllocatorCreateInfo allocatorInfo{
            .physicalDevice = m_Native->PhysicalDevice,
            .device = m_Native->Device,
            .instance = m_Native->Instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };

        vmaCreateAllocator(&allocatorInfo, &m_Native->Allocator);

        m_Native->GraphicsQueue = m_Native->Device.getQueue(m_Native->QueueFamilies.GraphicsFamily.value(), 0);
        m_Native->PresentQueue = m_Native->Device.getQueue(m_Native->QueueFamilies.PresentFamily.value(), 0);

        m_Native->SwapChain = SwapChain::Create({
            .MaxImageCount = 2,
            .Width = window->GetWidth(),
            .Height = window->GetHeight(),
            .Format = vk::Format::eR16G16B16A16Sfloat,
        });

        Log::Info("Created {0} swap chain images ({1}x{2})", m_Native->SwapChain->GetImageCount(),
                  m_Native->SwapChain->GetWidth(), m_Native->SwapChain->GetHeight());

        Log::Info("Setting initial render extents:");
        m_RenderExtent = m_Native->SwapChain->GetExtent();
        m_InternalRenderExtent = info.InternalRenderExtent;
        m_OutputFormat = info.OutputFormat;
        m_DepthFormat = info.DepthFormat;
        Log::Info("  Internal: {0}x{1}", m_InternalRenderExtent.x, m_InternalRenderExtent.y);
        Log::Info("  Output: {0}x{1}", m_RenderExtent.x, m_RenderExtent.y);

        Log::Info("Creating command pool");

        m_Native->CommandPool = CommandPool::Create();

        m_Native->DescriptorPool = DescriptorPool::Create({
            .Name = "Primary Pool",
            .MaxSets = 100000,
            .PoolSizes{
                {
                    .type = vk::DescriptorType::eUniformBuffer,
                    .descriptorCount = 10000
                },
                {
                    .type = vk::DescriptorType::eStorageBuffer,
                    .descriptorCount = 10000
                },
                {
                    .type = vk::DescriptorType::eCombinedImageSampler,
                    .descriptorCount = 10000
                }
            },
            .Flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet |
            vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind
        });

        m_Native->ImGuiDescriptorPool = DescriptorPool::Create({
            .Name = "ImGui Descriptor Pool",
            .MaxSets = 100000,
            .PoolSizes = {
                {
                    .type = vk::DescriptorType::eUniformBuffer,
                    .descriptorCount = 100000,
                },
                {
                    .type = vk::DescriptorType::eCombinedImageSampler,
                    .descriptorCount = 100000,
                }
            },
            .Flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet |
            vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind
        });

        m_Native->SynchronizationFrames.resize(m_Native->MaxFramesInFlight);
        m_Native->RetireBins.resize(m_Native->MaxFramesInFlight);


        m_ImGuiRenderPass = RenderPass::Create({
            .Name = "ImGui RenderPass",
            .Attachments = {
                {
                    .Format = Format::RGBA16Sfloat,
                    .LoadOp = LoadOp::Clear,
                    .StoreOp = StoreOp::Store,
                    .InitialLayout = ImageLayout::Undefined,
                    .FinalLayout = ImageLayout::ColorAttachment
                }
            }
        });

        CreateImGuiResources();

        ImGui::CreateContext();
        ImNodes::CreateContext();

        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#ifdef VE_VIEWPORTS
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
#endif
#ifdef VE_DEBUG
        io.ConfigDebugIsDebuggerPresent = true;
#endif

        GLFWwindow* handle = GetGlfwWindow(*window);

        ImGui_ImplGlfw_InitForVulkan(handle, true);

        ImGui_ImplVulkan_InitInfo initInfo = {
            .Instance = m_Native->Instance,
            .PhysicalDevice = m_Native->PhysicalDevice,
            .Device = m_Native->Device,
            .Queue = m_Native->GraphicsQueue,
            .DescriptorPool = m_Native->DescriptorPool->GetVkDescriptorPool(),
            .MinImageCount = m_Native->SwapChain->GetImageCount(),
            .ImageCount = m_Native->SwapChain->GetImageCount(),
            .PipelineInfoMain = {
                .RenderPass = m_ImGuiRenderPass->GetNative().RenderPass,
                .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
            }
        };

        ImGui_ImplVulkan_Init(&initInfo);

        ImGuiStyle& style = ImGui::GetStyle();

        style.TabRounding = 0.0f;
        style.FrameRounding = 4.0f;
        style.FrameBorderSize = 1.0f;
        style.WindowRounding = 6.0f;
        style.PopupRounding = 4.0f;
        style.GrabRounding = 12.0f;
        style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        style.FramePadding = ImVec2(6.0f, 6.0f);

        auto accentColor = ImVec4(0.2f, 0.7f, 0.7f, 1.00f);
        auto grayDarkest = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
        auto grayDarker = ImVec4(0.03f, 0.03f, 0.03f, 1.00f);
        auto grayDark = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
        auto grayMedium = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
        auto grayLight = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        colors[ImGuiCol_TextDisabled] = grayLight;
        colors[ImGuiCol_WindowBg] = grayDarkest;
        colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_PopupBg] = grayDarkest;
        colors[ImGuiCol_Border] = grayMedium;
        colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.4f);
        colors[ImGuiCol_FrameBg] = grayDarker;
        colors[ImGuiCol_FrameBgHovered] = grayMedium;
        colors[ImGuiCol_FrameBgActive] = grayDarkest;
        colors[ImGuiCol_TitleBg] = grayDarker;
        colors[ImGuiCol_TitleBgActive] = grayDarker;
        colors[ImGuiCol_TitleBgCollapsed] = grayDarker;
        colors[ImGuiCol_MenuBarBg] = grayDarker;
        colors[ImGuiCol_ScrollbarBg] = grayDarkest;
        colors[ImGuiCol_ScrollbarGrab] = grayDark;
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
        colors[ImGuiCol_CheckMark] = accentColor;
        colors[ImGuiCol_SliderGrab] = accentColor;
        colors[ImGuiCol_SliderGrabActive] = accentColor;
        colors[ImGuiCol_Button] = grayDark;
        colors[ImGuiCol_ButtonHovered] = grayMedium;
        colors[ImGuiCol_ButtonActive] = grayDarkest;
        colors[ImGuiCol_Header] = grayDark;
        colors[ImGuiCol_HeaderHovered] = accentColor;
        colors[ImGuiCol_HeaderActive] = accentColor;
        colors[ImGuiCol_Separator] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
        colors[ImGuiCol_SeparatorHovered] = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
        colors[ImGuiCol_SeparatorActive] = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.20f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
        colors[ImGuiCol_InputTextCursor] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
        colors[ImGuiCol_Tab] = grayDarker;
        colors[ImGuiCol_TabSelected] = grayDark;
        colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.93f, 0.93f, 0.93f, 0.61f);
        colors[ImGuiCol_TabDimmed] = grayDarker;
        colors[ImGuiCol_TabDimmedSelected] = grayDark;
        colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.93f, 0.93f, 0.93f, 0.61f);
        colors[ImGuiCol_DockingPreview] = accentColor;
        colors[ImGuiCol_DockingEmptyBg] = grayDarkest;
        colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
        colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
        colors[ImGuiCol_TableHeaderBg] = grayDarker;
        colors[ImGuiCol_TableBorderStrong] = grayDark;
        colors[ImGuiCol_TableBorderLight] = grayDarker;
        colors[ImGuiCol_TableRowBg] = grayDarker;
        colors[ImGuiCol_TableRowBgAlt] = grayDarker;
        colors[ImGuiCol_TextLink] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
        colors[ImGuiCol_TreeLines] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
        colors[ImGuiCol_DragDropTarget] = accentColor;
        colors[ImGuiCol_UnsavedMarker] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        colors[ImGuiCol_NavCursor] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

        ImNodesStyle& imnodesStyle = ImNodes::GetStyle();
        imnodesStyle.Colors[ImNodesCol_NodeBackground] = ImColor(grayDarkest);
        imnodesStyle.Colors[ImNodesCol_NodeBackgroundHovered] = ImColor(grayDarkest);
        imnodesStyle.Colors[ImNodesCol_NodeBackgroundSelected] = ImColor(grayDarkest);
        imnodesStyle.Colors[ImNodesCol_NodeOutline] = ImColor(grayMedium);
        imnodesStyle.Colors[ImNodesCol_TitleBar] = ImColor(grayDark);
        imnodesStyle.Colors[ImNodesCol_TitleBarHovered] = ImColor(grayDark);
        imnodesStyle.Colors[ImNodesCol_TitleBarSelected] = ImColor(grayDark);
        imnodesStyle.Colors[ImNodesCol_Link] = ImColor(accentColor);
        imnodesStyle.Colors[ImNodesCol_LinkHovered] = ImColor(accentColor);
        imnodesStyle.Colors[ImNodesCol_LinkSelected] = ImColor(accentColor);
        imnodesStyle.Colors[ImNodesCol_Pin] = ImColor(accentColor);
        imnodesStyle.Colors[ImNodesCol_PinHovered] = ImColor(accentColor);
        imnodesStyle.Colors[ImNodesCol_GridBackground] = ImColor(grayDarkest);
        imnodesStyle.Colors[ImNodesCol_GridLine] = ImColor(grayDark);
        imnodesStyle.Colors[ImNodesCol_GridLinePrimary] = ImColor(grayMedium);

        if (info.DefaultFontPath && std::filesystem::exists(*info.DefaultFontPath))
        {
            io.Fonts->AddFontFromFileTTF(info.DefaultFontPath->string().c_str(), 16.0f);
        }

        if (info.IconFontPath && std::filesystem::exists(*info.IconFontPath))
        {
            ImFontConfig config;
            config.MergeMode = true;
            config.GlyphOffset = ImVec2(0, 5.0f);
            config.GlyphMinAdvanceX = 20.0f; // Use if you want to make the icon monospaced
            static constexpr ImWchar icon_ranges[] = {ICON_MIN_MD, ICON_MAX_16_MD, 0};
            io.Fonts->AddFontFromFileTTF(info.IconFontPath->string().c_str(), 20.0f, &config, icon_ranges);
        }
    }

    void Context::DisposeResources()
    {
        // Application::Run has already waited the GPU idle. Anything the
        // consumer released in OnDispose has retired into the bins — drain them
        // all before the frames (and their command buffers) go away.
        m_Native->DrainAllRetireBins();
        m_Native->SynchronizationFrames.clear();
    }

    void Context::Dispose()
    {
        Log::Info("Disposing rendering context...");
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImNodes::DestroyContext();
        ImGui::DestroyContext();
        DisposeImGuiResources();
        m_ImGuiRenderPass.reset();

        // The context's own resources (ImGui image/view/framebuffers/render
        // pass) just retired into the bins. Drain them while the device,
        // allocator and descriptor pool are still alive, then stop accepting
        // retirements — anything dropped after this point is a leak/bug.
        m_Native->DrainAllRetireBins();
        m_Native->Disposed = true;

        m_Native->ImGuiDescriptorPool.reset();
        m_Native->DescriptorPool.reset();
        m_Native->CommandPool.reset();
        m_Native->SwapChain.reset();
        m_Native->PresentQueue = nullptr;
        m_Native->GraphicsQueue = nullptr;
        vmaDestroyAllocator(m_Native->Allocator);
        m_Native->Device.destroy();
        m_Native->Instance.destroySurfaceKHR(m_Native->Surface);
        m_Native->PhysicalDevice = nullptr;
        if (m_Native->DebugMessenger)
        {
            m_Native->Instance.destroyDebugUtilsMessengerEXT(m_Native->DebugMessenger);
            m_Native->DebugMessenger = nullptr;
        }
        m_Native->Instance.destroy();
        m_Window = nullptr;
        s_Instance = nullptr;
    }

    Context& Context::Instance()
    {
        return *s_Instance;
    }

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

    u32 Context::GetMaxFramesInFlight() const { return m_Native->MaxFramesInFlight; }
    u32 Context::GetCurrentFrameInFlight() const { return m_Native->CurrentFrameInFlight; }

    uvec2 Context::GetSwapChainExtent() const { return m_Native->SwapChain->GetExtent(); }
    Format Context::GetSwapChainFormat() const { return m_Native->SwapChain->GetFormat(); }
    Ref<Image> Context::GetCurrentSwapChainImage() const { return m_Native->SwapChain->GetCurrentImage(); }
    Ref<ImageView> Context::GetCurrentSwapChainImageView() const { return m_Native->SwapChain->GetCurrentImageView(); }

    void Context::ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const
    {
        auto commandBuffer = CommandBuffer::Create();
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
        m_Native->Device.waitIdle();
    }

    void Context::RenderImGui(CommandBuffer& commandBuffer)
    {
        m_ImGuiRenderedThisFrame = true;

        commandBuffer.PipelineBarrier({
            .Image = *m_ImGuiImage,
            .NewLayout = ImageLayout::ColorAttachment,
        });

        vector<ClearValue> clear = {
            ClearColor{1.0f, 0.0f, 0.0f, 0.0f}
        };

        commandBuffer.BeginRenderPass(m_ImGuiRenderPass,
                                      m_ImGuiFramebuffers[m_Native->SwapChain->GetCurrentImageIndex()], clear);

        ImGui::Render();

        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        ImDrawData* drawData = ImGui::GetDrawData();
        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer.GetNative().CommandBuffer);

        commandBuffer.EndRenderPass();

        commandBuffer.PipelineBarrier({
            .Image = *m_ImGuiImage,
            .NewLayout = ImageLayout::ShaderReadOnly,
        });
    }

    void Context::BeginFrame()
    {
        if (!m_ImGuiRenderedThisFrame)
        {
            ImGui::EndFrame();
            ImGui::UpdatePlatformWindows();
        }

        m_ImGuiRenderedThisFrame = false;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    Ref<ImGuiTexture> Context::CreateImGuiTexture(const Sampler& sampler, const ImageView& imageView)
    {
        auto native = CreateUnique<ImGuiTexture::Native>();

        native->Set = ImGui_ImplVulkan_AddTexture(
            sampler.GetNative().Sampler,
            imageView.GetNative().ImageView,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );

        return Ref<ImGuiTexture>(new ImGuiTexture(std::move(native)));
    }

    void Context::DestroyImGuiTexture(const ImGuiTexture& texture)
    {
        ImGui_ImplVulkan_RemoveTexture(texture.GetNative().Set);
    }

    vector<const char*>& Context::Native::GetRequiredExtensions()
    {
        if (RequiredExtensions.empty())
        {
            u32 glfwExtensionCount = 0;
            const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

            RequiredExtensions = vector<const char*>(glfwExtensions,
                                                       glfwExtensions + glfwExtensionCount);

            RequiredExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            RequiredExtensions.emplace_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            RequiredExtensions.emplace_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
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

        int i = 0;
        for (const auto& queueFamily : queueFamilies)
        {
            if (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics)
            {
                QueueFamilies.GraphicsFamily = i;
            }

            if (vk::Bool32 presentSupport = device.getSurfaceSupportKHR(i, Surface).value)
            {
                QueueFamilies.PresentFamily = i;
            }

            if (QueueFamilies.IsComplete())
            {
                break;
            }

            i++;
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
        set<u32> uniqueQueueFamilies{
            indices.GraphicsFamily.value(),
            indices.PresentFamily.value()
        };

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

        vk::PhysicalDeviceVulkan12Features vulkan12Features{
            .pNext = &features2,
            .shaderUniformBufferArrayNonUniformIndexing = vk::True,
            .shaderSampledImageArrayNonUniformIndexing = vk::True,
            .shaderStorageBufferArrayNonUniformIndexing = vk::True,
            .descriptorBindingUniformBufferUpdateAfterBind = vk::True,
            .descriptorBindingSampledImageUpdateAfterBind = vk::True,
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

    void Context::CreateImGuiResources()
    {
        m_ImGuiFramebuffers.resize(m_Native->SwapChain->GetImageCount());

        m_ImGuiImage = Image::Create({
            .Name = "ImGui Image",
            .Extent = {m_RenderExtent.x, m_RenderExtent.y, 1},
            .MipLevels = 1,
            .Layers = 1,
            .Format = Format::RGBA16Sfloat,
            .Type = ImageType::Type2D,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled
        });

        m_ImGuiImageView = ImageView::Create({
            .Name = "ImGui Image View",
            .Image = m_ImGuiImage,
        });

        for (size_t i = 0; i < m_Native->SwapChain->GetImageCount(); i++)
        {
            m_ImGuiFramebuffers[i] = Framebuffer::Create({
                .Name = fmt::format("ImGui Framebuffer {}", i),
                .RenderPass = m_ImGuiRenderPass,
                .Attachments = {m_ImGuiImageView},
                .Width = m_RenderExtent.x,
                .Height = m_RenderExtent.y,
                .Layers = 1,
            });
        }
    }

    void Context::DisposeImGuiResources()
    {
        m_ImGuiImageView.reset();
        m_ImGuiImage.reset();
        m_ImGuiFramebuffers.clear();
    }

    SynchronizationFrame& Context::AcquireNextFrame()
    {
        m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight].GetInFlightFence().Wait();

        // The fence is now signaled, so the GPU has finished the work that was
        // recorded the last time this frame index was current — anything
        // retired then is safe to destroy.
        m_Native->DrainRetireBin(m_Native->RetireBins[m_Native->CurrentFrameInFlight]);

        return m_Native->SynchronizationFrames[m_Native->CurrentFrameInFlight];
    }

    Context::Native::RetireBin& Context::Native::CurrentRetireBin()
    {
        VE_ASSERT(!Disposed,
                  "Context::Retire after teardown — a resource outlived the rendering context");
        return RetireBins[CurrentFrameInFlight];
    }

    void Context::Native::Retire(vk::Buffer buffer, VmaAllocation allocation)
    {
        CurrentRetireBin().Buffers.emplace_back(buffer, allocation);
    }

    void Context::Native::Retire(vk::Image image, VmaAllocation allocation)
    {
        CurrentRetireBin().Images.emplace_back(image, allocation);
    }

    void Context::Native::Retire(vk::ImageView imageView) { CurrentRetireBin().ImageViews.push_back(imageView); }
    void Context::Native::Retire(vk::Sampler sampler) { CurrentRetireBin().Samplers.push_back(sampler); }
    void Context::Native::Retire(vk::ShaderModule shaderModule) { CurrentRetireBin().ShaderModules.push_back(shaderModule); }
    void Context::Native::Retire(vk::Pipeline pipeline) { CurrentRetireBin().Pipelines.push_back(pipeline); }
    void Context::Native::Retire(vk::PipelineLayout pipelineLayout) { CurrentRetireBin().PipelineLayouts.push_back(pipelineLayout); }
    void Context::Native::Retire(vk::RenderPass renderPass) { CurrentRetireBin().RenderPasses.push_back(renderPass); }
    void Context::Native::Retire(vk::Framebuffer framebuffer) { CurrentRetireBin().Framebuffers.push_back(framebuffer); }
    void Context::Native::Retire(vk::DescriptorSet descriptorSet) { CurrentRetireBin().DescriptorSets.push_back(descriptorSet); }

    void Context::Native::DrainRetireBin(RetireBin& bin)
    {
        // Destroy dependents before the objects they reference: descriptor sets
        // and framebuffers first, then views, then the images/buffers backing
        // them. Everything in the bin is already GPU-idle.
        for (auto descriptorSet : bin.DescriptorSets)
            Device.freeDescriptorSets(DescriptorPool->GetVkDescriptorPool(), descriptorSet);
        for (auto framebuffer : bin.Framebuffers)
            Device.destroyFramebuffer(framebuffer);
        for (auto imageView : bin.ImageViews)
            Device.destroyImageView(imageView);
        for (auto& [image, allocation] : bin.Images)
            vmaDestroyImage(Allocator, image, allocation);
        for (auto& [buffer, allocation] : bin.Buffers)
            vmaDestroyBuffer(Allocator, buffer, allocation);
        for (auto renderPass : bin.RenderPasses)
            Device.destroyRenderPass(renderPass);
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

    void Context::SubmitFrame(const SynchronizationFrame& frame) const
    {
        const auto commandBuffer = frame.GetCommandBuffer()->GetNative().CommandBuffer;
        const auto renderFinishedSemaphore = frame.GetRenderFinishedSemaphore().GetNative().Semaphore;
        const auto imageAvailableSemaphore = frame.GetImageAvailableSemaphore().GetNative().Semaphore;

        const vk::PipelineStageFlags mask = vk::PipelineStageFlagBits::eColorAttachmentOutput;

        const vk::SubmitInfo submitInfo{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &imageAvailableSemaphore,
            .pWaitDstStageMask = &mask,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &renderFinishedSemaphore,
        };

        VK_ASSERT(m_Native->GraphicsQueue.submit(1, &submitInfo, frame.GetInFlightFence().GetNative().Fence),
                  "Failed to submit draw command buffer!");
    }

    void Context::PresentFrame(const SynchronizationFrame& frame)
    {
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

        VK_ASSERT(m_Native->GraphicsQueue.submit(1, &submitInfo, VK_NULL_HANDLE), "failed to submit to queue!");

        WaitIdle();
    }

    void Context::UpdateRenderExtent()
    {
        Log::Info("Updating extent {0}x{1}", m_RenderExtent.x, m_RenderExtent.y);

        m_Native->SwapChain->RenderExtentChanged();

        m_RenderExtent = m_Native->SwapChain->GetExtent();

        DisposeImGuiResources();
        CreateImGuiResources();

        m_Native->SwapChain->Invalidated();

#ifdef VE_VIEWPORTS
        ImGui::GetMainViewport()->Size = ImVec2(static_cast<f32>(m_RenderExtent.x),
                                                static_cast<f32>(m_RenderExtent.y));
#endif
        m_RenderExtentChanged = false;
    }
}
