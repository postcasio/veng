#define VMA_IMPLEMENTATION
#include <Veng/Renderer/Backend/Context.h>

#include <set>

#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <Veng/Renderer/Backend/ImGuiTexture.h>
#include <Veng/Vendor/IconsMaterialDesign.h>

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



    vk::PhysicalDevice Context::GetPhysicalDevice()
    {
        auto physicalDevices = m_Instance.enumeratePhysicalDevices().value;

        for (auto& device : physicalDevices)
        {
            if (IsDeviceSuitable(device))
            {
                return device;
            }
        }

        VE_ASSERT(false, "Failed to find a suitable GPU!");
    }

    bool Context::IsDeviceSuitable(vk::PhysicalDevice device)
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

        auto extensions = GetRequiredExtensions();

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
            static_cast<u32>(m_ValidationLayers.size());
        instanceCreateInfo.ppEnabledLayerNames = m_ValidationLayers.data();
#else
        instanceCreateInfo.pNext = nullptr;
        instanceCreateInfo.enabledLayerCount = 0;
        instanceCreateInfo.ppEnabledLayerNames = nullptr;
#endif

        instanceCreateInfo.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;

        m_Instance = createInstance(instanceCreateInfo).value;

        VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance);

#ifdef VE_ENABLE_VALIDATION_LAYERS
        m_DebugMessenger = m_Instance.createDebugUtilsMessengerEXT(debugCreateInfo);
#endif

        DebugMarkers::Initialize();

        window->CreateSurface(*this);

        Log::Info("Initializing surface");

        m_Surface = window->GetSurface();

        m_PhysicalDevice = GetPhysicalDevice();

        m_Device = CreateDevice();

        VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Device);

        VmaAllocatorCreateInfo allocatorInfo{
            .physicalDevice = m_PhysicalDevice,
            .device = m_Device,
            .instance = m_Instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };

        vmaCreateAllocator(&allocatorInfo, &m_Allocator);

        m_GraphicsQueue = m_Device.getQueue(m_QueueFamilies.GraphicsFamily.value(), 0);
        m_PresentQueue = m_Device.getQueue(m_QueueFamilies.PresentFamily.value(), 0);

        m_SwapChain = SwapChain::Create({
            .MaxImageCount = 2,
            .Width = window->GetWidth(),
            .Height = window->GetHeight(),
            .Format = vk::Format::eR16G16B16A16Sfloat,
        });

        Log::Info("Created {0} swap chain images ({1}x{2})", m_SwapChain->GetImageCount(),
                  m_SwapChain->GetWidth(), m_SwapChain->GetHeight());

        Log::Info("Setting initial render extents:");
        m_RenderExtent = m_SwapChain->GetExtent();
        m_InternalRenderExtent = info.InternalRenderExtent;
        m_OutputFormat = info.OutputFormat;
        m_DepthFormat = info.DepthFormat;
        Log::Info("  Internal: {0}x{1}", m_InternalRenderExtent.x, m_InternalRenderExtent.y);
        Log::Info("  Output: {0}x{1}", m_RenderExtent.x, m_RenderExtent.y);

        Log::Info("Creating command pool");

        m_CommandPool = CommandPool::Create();

        m_DescriptorPool = DescriptorPool::Create({
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

        m_ImGuiDescriptorPool = DescriptorPool::Create({
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

        m_SynchronizationFrames.resize(m_MaxFramesInFlight);
        m_RetireBins.resize(m_MaxFramesInFlight);


        m_ImGuiRenderPass = RenderPass::Create({
            .Name = "ImGui RenderPass",
            .Attachments = {
                {
                    .format = vk::Format::eR16G16B16A16Sfloat,
                    .samples = vk::SampleCountFlagBits::e1,
                    .loadOp = vk::AttachmentLoadOp::eClear,
                    .storeOp = vk::AttachmentStoreOp::eStore,
                    .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
                    .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                    .initialLayout = vk::ImageLayout::eUndefined,
                    .finalLayout = vk::ImageLayout::eColorAttachmentOptimal
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

        GLFWwindow* handle = window->GetHandle();

        ImGui_ImplGlfw_InitForVulkan(handle, true);

        ImGui_ImplVulkan_InitInfo initInfo = {
            .Instance = GetVkInstance(),
            .PhysicalDevice = GetVkPhysicalDevice(),
            .Device = GetVkDevice(),
            .Queue = GetVkGraphicsQueue(),
            .DescriptorPool = m_DescriptorPool->GetVkDescriptorPool(),
            .MinImageCount = m_SwapChain->GetImageCount(),
            .ImageCount = m_SwapChain->GetImageCount(),
            .PipelineInfoMain = {
                .RenderPass = m_ImGuiRenderPass->GetVkRenderPass(),
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
        DrainAllRetireBins();
        m_SynchronizationFrames.clear();
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
        DrainAllRetireBins();
        m_Disposed = true;

        m_ImGuiDescriptorPool.reset();
        m_DescriptorPool.reset();
        m_CommandPool.reset();
        m_SwapChain.reset();
        m_PresentQueue = nullptr;
        m_GraphicsQueue = nullptr;
        vmaDestroyAllocator(m_Allocator);
        m_Device.destroy();
        m_Instance.destroySurfaceKHR(m_Surface);
        m_PhysicalDevice = nullptr;
        if (m_DebugMessenger)
        {
            m_Instance.destroyDebugUtilsMessengerEXT(m_DebugMessenger);
            m_DebugMessenger = nullptr;
        }
        m_Instance.destroy();
        m_Window = nullptr;
        s_Instance = nullptr;
    }

    Context& Context::Instance()
    {
        return *s_Instance;
    }

    void Context::ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const
    {
        auto commandBuffer = CommandBuffer::Create();
        commandBuffer->Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        function(*commandBuffer);

        commandBuffer->End();
        SubmitImmediateCommands(*commandBuffer);
    }

    void Context::AcquireNextImage(Semaphore& semaphore)
    {
        auto result = m_SwapChain->AcquireNextImage(semaphore);

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
        m_Device.waitIdle();
    }

    void Context::RenderImGui(CommandBuffer& commandBuffer)
    {
        m_ImGuiRenderedThisFrame = true;

        commandBuffer.PipelineBarrier({
            .Image = *m_ImGuiImage,
            .NewLayout = vk::ImageLayout::eColorAttachmentOptimal,
        });

        vector<vk::ClearValue> clear = {
            {.color = vk::ClearColorValue(std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f})}
        };

        commandBuffer.BeginRenderPass(m_ImGuiRenderPass,
                                      m_ImGuiFramebuffers[GetSwapChain().GetCurrentImageIndex()], clear);

        ImGui::Render();

        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        ImDrawData* drawData = ImGui::GetDrawData();
        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer.GetVkCommandBuffer());

        commandBuffer.EndRenderPass();

        commandBuffer.PipelineBarrier({
            .Image = *m_ImGuiImage,
            .NewLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
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
        return ImGuiTexture::Create(
            ImGui_ImplVulkan_AddTexture(
                sampler.GetVkSampler(),
                imageView.GetVkImageView(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            )
        );
    }

    void Context::DestroyImGuiTexture(const ImGuiTexture& texture)
    {
        ImGui_ImplVulkan_RemoveTexture(texture.GetDescriptorSet());
    }

    vector<const char*>& Context::GetRequiredExtensions()
    {
        if (m_RequiredExtensions.empty())
        {
            u32 glfwExtensionCount = 0;
            const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

            m_RequiredExtensions = vector<const char*>(glfwExtensions,
                                                       glfwExtensions + glfwExtensionCount);

            m_RequiredExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            m_RequiredExtensions.emplace_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            m_RequiredExtensions.emplace_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
            m_RequiredExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        return m_RequiredExtensions;
    }

    QueueFamilyIndices& Context::FindQueueFamilies(vk::PhysicalDevice device)
    {
        m_QueueFamilies = {};

        u32 queueFamilyCount = 0;
        device.getQueueFamilyProperties(&queueFamilyCount, nullptr);

        vector<vk::QueueFamilyProperties> queueFamilies(queueFamilyCount);
        device.getQueueFamilyProperties(&queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto& queueFamily : queueFamilies)
        {
            if (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics)
            {
                m_QueueFamilies.GraphicsFamily = i;
            }

            if (vk::Bool32 presentSupport = device.getSurfaceSupportKHR(i, m_Surface).value)
            {
                m_QueueFamilies.PresentFamily = i;
            }

            if (m_QueueFamilies.IsComplete())
            {
                break;
            }

            i++;
        }

        return m_QueueFamilies;
    }

    SwapChainSupportDetails Context::QuerySwapChainSupport(vk::PhysicalDevice device) const
    {
        return SwapChainSupportDetails{
            .Capabilities = device.getSurfaceCapabilitiesKHR(m_Surface).value,
            .Formats = device.getSurfaceFormatsKHR(m_Surface).value,
            .PresentModes = device.getSurfacePresentModesKHR(m_Surface).value
        };
    }

    bool Context::CheckDeviceExtensionSupport(vk::PhysicalDevice device) const
    {
        auto availableExtensions = device.enumerateDeviceExtensionProperties(nullptr).value;

        auto& deviceExtensions = m_DeviceExtensions;

        set<string> requiredExtensions(deviceExtensions.begin(),
                                       deviceExtensions.end());

        for (const auto& extension : availableExtensions)
        {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    vk::Device Context::CreateDevice()
    {
        QueueFamilyIndices indices = FindQueueFamilies(m_PhysicalDevice);

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

        auto deviceExtensions = m_DeviceExtensions;

        vk::PhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{
            .dynamicRendering = vk::True
        };

        vk::PhysicalDeviceFeatures2 features2{
            .pNext = &dynamicRenderingFeatures,
            .features = deviceFeatures
        };

        m_PhysicalDevice.getFeatures2(&features2);

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

        return m_PhysicalDevice.createDevice(deviceCreateInfo, nullptr).value;
    }

    void Context::CreateImGuiResources()
    {
        m_ImGuiFramebuffers.resize(m_SwapChain->GetImageCount());

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

        for (size_t i = 0; i < m_SwapChain->GetImageCount(); i++)
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
        m_SynchronizationFrames[m_CurrentFrameInFlight].GetInFlightFence().Wait();

        // The fence is now signaled, so the GPU has finished the work that was
        // recorded the last time this frame index was current — anything
        // retired then is safe to destroy.
        DrainRetireBin(m_RetireBins[m_CurrentFrameInFlight]);

        return m_SynchronizationFrames[m_CurrentFrameInFlight];
    }

    Context::RetireBin& Context::CurrentRetireBin()
    {
        VE_ASSERT(!m_Disposed,
                  "Context::Retire after teardown — a resource outlived the rendering context");
        return m_RetireBins[m_CurrentFrameInFlight];
    }

    void Context::Retire(vk::Buffer buffer, VmaAllocation allocation)
    {
        CurrentRetireBin().Buffers.emplace_back(buffer, allocation);
    }

    void Context::Retire(vk::Image image, VmaAllocation allocation)
    {
        CurrentRetireBin().Images.emplace_back(image, allocation);
    }

    void Context::Retire(vk::ImageView imageView) { CurrentRetireBin().ImageViews.push_back(imageView); }
    void Context::Retire(vk::Sampler sampler) { CurrentRetireBin().Samplers.push_back(sampler); }
    void Context::Retire(vk::ShaderModule shaderModule) { CurrentRetireBin().ShaderModules.push_back(shaderModule); }
    void Context::Retire(vk::Pipeline pipeline) { CurrentRetireBin().Pipelines.push_back(pipeline); }
    void Context::Retire(vk::PipelineLayout pipelineLayout) { CurrentRetireBin().PipelineLayouts.push_back(pipelineLayout); }
    void Context::Retire(vk::RenderPass renderPass) { CurrentRetireBin().RenderPasses.push_back(renderPass); }
    void Context::Retire(vk::Framebuffer framebuffer) { CurrentRetireBin().Framebuffers.push_back(framebuffer); }
    void Context::Retire(vk::DescriptorSet descriptorSet) { CurrentRetireBin().DescriptorSets.push_back(descriptorSet); }

    void Context::DrainRetireBin(RetireBin& bin)
    {
        // Destroy dependents before the objects they reference: descriptor sets
        // and framebuffers first, then views, then the images/buffers backing
        // them. Everything in the bin is already GPU-idle.
        for (auto descriptorSet : bin.DescriptorSets)
            m_Device.freeDescriptorSets(m_DescriptorPool->GetVkDescriptorPool(), descriptorSet);
        for (auto framebuffer : bin.Framebuffers)
            m_Device.destroyFramebuffer(framebuffer);
        for (auto imageView : bin.ImageViews)
            m_Device.destroyImageView(imageView);
        for (auto& [image, allocation] : bin.Images)
            vmaDestroyImage(m_Allocator, image, allocation);
        for (auto& [buffer, allocation] : bin.Buffers)
            vmaDestroyBuffer(m_Allocator, buffer, allocation);
        for (auto renderPass : bin.RenderPasses)
            m_Device.destroyRenderPass(renderPass);
        for (auto pipeline : bin.Pipelines)
            m_Device.destroyPipeline(pipeline);
        for (auto pipelineLayout : bin.PipelineLayouts)
            m_Device.destroyPipelineLayout(pipelineLayout);
        for (auto sampler : bin.Samplers)
            m_Device.destroySampler(sampler);
        for (auto shaderModule : bin.ShaderModules)
            m_Device.destroyShaderModule(shaderModule);

        bin = {};
    }

    void Context::DrainAllRetireBins()
    {
        for (auto& bin : m_RetireBins)
            DrainRetireBin(bin);
    }

    void Context::SubmitFrame(const SynchronizationFrame& frame) const
    {
        const auto commandBuffer = frame.GetCommandBuffer()->GetVkCommandBuffer();
        const auto renderFinishedSemaphore = frame.GetRenderFinishedSemaphore().GetVkSemaphore();
        const auto imageAvailableSemaphore = frame.GetImageAvailableSemaphore().GetVkSemaphore();

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

        VK_ASSERT(m_GraphicsQueue.submit(1, &submitInfo, frame.GetInFlightFence().GetVkFence()),
                  "Failed to submit draw command buffer!");
    }

    void Context::PresentFrame(const SynchronizationFrame& frame)
    {
        auto renderFinishedSemaphore = frame.GetRenderFinishedSemaphore().GetVkSemaphore();
        auto swapChain = m_SwapChain->GetVkSwapChain();
        auto imageIndex = m_SwapChain->GetCurrentImageIndex();

        vk::PresentInfoKHR presentInfo{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &renderFinishedSemaphore,
            .swapchainCount = 1,
            .pSwapchains = &swapChain,
            .pImageIndices = &imageIndex
        };

        auto result = m_PresentQueue.presentKHR(presentInfo);

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

        m_CurrentFrameInFlight = (m_CurrentFrameInFlight + 1) % m_MaxFramesInFlight;
    }

    void Context::SubmitImmediateCommands(CommandBuffer& commandBuffer) const
    {
        const auto commandBufferHandle = commandBuffer.GetVkCommandBuffer();

        const vk::SubmitInfo submitInfo = {
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBufferHandle
        };

        VK_ASSERT(m_GraphicsQueue.submit(1, &submitInfo, VK_NULL_HANDLE), "failed to submit to queue!");

        WaitIdle();
    }

    vk::PresentModeKHR Context::GetPresentMode(
        const vector<vk::PresentModeKHR>& availablePresentModes)
    {
        for (const auto& availablePresentMode : availablePresentModes)
        {
            if (availablePresentMode == vk::PresentModeKHR::eMailbox)
            {
                return availablePresentMode;
            }
        }

        return vk::PresentModeKHR::eFifo;
    }

    void Context::UpdateRenderExtent()
    {
        Log::Info("Updating extent {0}x{1}", m_RenderExtent.x, m_RenderExtent.y);

        m_SwapChain->RenderExtentChanged();

        m_RenderExtent = m_SwapChain->GetExtent();

        DisposeImGuiResources();
        CreateImGuiResources();

        m_SwapChain->Invalidated();

#ifdef VE_VIEWPORTS
        ImGui::GetMainViewport()->Size = ImVec2(static_cast<f32>(m_RenderExtent.x),
                                                static_cast<f32>(m_RenderExtent.y));
#endif
        m_RenderExtentChanged = false;
    }
}
