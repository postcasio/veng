#include <Veng/ImGui/ImGuiLayer.h>

#include <filesystem>

#include <Veng/Window.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/DescriptorPool.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

#include <Veng/Vendor/ImGui.h>
#include <Veng/Vendor/IconsMaterialDesign.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

namespace Veng
{
    using Renderer::Context;

    struct ImGuiLayer::PendingTextureRemoval
    {
        vk::DescriptorSet Set;
        u32 FramesRemaining;
    };

    Unique<ImGuiLayer> ImGuiLayer::Create(const ImGuiLayerInfo& info, Context& context, Window& window)
    {
        return Unique<ImGuiLayer>(new ImGuiLayer(info, context, window));
    }

    ImGuiLayer::ImGuiLayer(const ImGuiLayerInfo& info, Context& context, Window& window) : m_Context(context)
    {
        using namespace Renderer;

        m_DescriptorPool = DescriptorPool::Create(m_Context, {
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

        CreateResources();

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

        GLFWwindow* handle = GetGlfwWindow(window);

        ImGui_ImplGlfw_InitForVulkan(handle, true);

        // ImGui draws into the offscreen image via dynamic rendering rather than
        // a dedicated render pass/framebuffer: the only attachment is
        // the RGBA16Sfloat color attachment that backs m_Image.
        const vk::Format colorAttachmentFormat = ToVk(Format::RGBA16Sfloat);

        const vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo = {
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorAttachmentFormat,
        };

        // The ImGui Vulkan backend creates its own command pool on this family; it
        // must be the device's graphics family (not the default 0, which need not be
        // a created queue family on every device, e.g. MoltenVK).
        const u32 graphicsFamily = context.GetQueueFamilies().GraphicsFamily.value();

        ImGui_ImplVulkan_InitInfo initInfo = {
            .Instance = GetVkInstance(context),
            .PhysicalDevice = GetVkPhysicalDevice(context),
            .Device = GetVkDevice(context),
            .QueueFamily = graphicsFamily,
            .Queue = GetVkGraphicsQueue(context),
            .DescriptorPool = m_DescriptorPool->GetVkDescriptorPool(),
            .MinImageCount = context.GetSwapChainImageCount(),
            .ImageCount = context.GetSwapChainImageCount(),
            .PipelineInfoMain = {
                .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
                .PipelineRenderingCreateInfo = pipelineRenderingCreateInfo,
            },
            .UseDynamicRendering = true,
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

        // Recreate the offscreen image/view whenever the swap chain is recreated
        // (resize).
        m_Context.AddSwapChainInvalidationCallback([this]
        {
            DisposeResources();
            CreateResources();
#ifdef VE_VIEWPORTS
            const uvec2 extent = m_Context.GetRenderExtent();
            ImGui::GetMainViewport()->Size = vec2(extent);
#endif
        });
    }

    ImGuiLayer::~ImGuiLayer()
    {
        // The device is idle by teardown (Application waits before dropping the
        // layer), so any sets still in the retire queue free immediately. Must run
        // before the backend shuts down, while the sets are still valid.
        for (const PendingTextureRemoval& removal : m_PendingTextureRemovals)
            ImGui_ImplVulkan_RemoveTexture(removal.Set);
        m_PendingTextureRemovals.clear();

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImNodes::DestroyContext();
        ImGui::DestroyContext();

        DisposeResources();
        m_DescriptorPool.reset();
    }

    void ImGuiLayer::CreateResources()
    {
        using namespace Renderer;

        const uvec2 extent = m_Context.GetRenderExtent();

        m_Image = Image::Create(m_Context, {
            .Name = "ImGui Image",
            .Extent = {extent.x, extent.y, 1},
            .MipLevels = 1,
            .Layers = 1,
            .Format = Format::RGBA16Sfloat,
            .Type = ImageType::Type2D,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled
        });

        m_ImageView = ImageView::Create(m_Context, {
            .Name = "ImGui Image View",
            .Image = m_Image,
        });
    }

    void ImGuiLayer::DisposeResources()
    {
        m_ImageView.reset();
        m_Image.reset();
    }

    void ImGuiLayer::BeginFrame()
    {
        // Free descriptor sets whose retire window has elapsed: a set queued at
        // DestroyTexture is freed only after every frame in flight that could have
        // referenced it has completed. The free runs outside the erase_if predicate
        // because a hardened STL may evaluate the predicate more than once per
        // element, which would free the set twice.
        vector<vk::DescriptorSet> setsToFree;
        std::erase_if(m_PendingTextureRemovals, [&setsToFree](PendingTextureRemoval& removal)
        {
            if (removal.FramesRemaining == 0)
            {
                setsToFree.push_back(removal.Set);
                return true;
            }
            --removal.FramesRemaining;
            return false;
        });
        for (vk::DescriptorSet set : setsToFree)
            ImGui_ImplVulkan_RemoveTexture(set);

        if (!m_RenderedThisFrame)
        {
            ImGui::EndFrame();
            ImGui::UpdatePlatformWindows();
        }

        m_RenderedThisFrame = false;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiLayer::Render(Renderer::CommandBuffer& commandBuffer)
    {
        using namespace Renderer;

        m_RenderedThisFrame = true;

        Backend::TransitionImage(commandBuffer, *m_Image, ImageLayout::ColorAttachment);

        commandBuffer.BeginRendering({
            .Extent = m_Context.GetRenderExtent(),
            .ColorAttachments = {
                {
                    .ImageView = m_ImageView,
                    .LoadOp = LoadOp::Clear,
                    .StoreOp = StoreOp::Store,
                    .ClearValue = ClearColor{1.0f, 0.0f, 0.0f, 0.0f},
                }
            },
        });

        ImGui::Render();

        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        ImDrawData* drawData = ImGui::GetDrawData();
        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer.GetNative().CommandBuffer);

        commandBuffer.EndRendering();

        Backend::TransitionImage(commandBuffer, *m_Image, ImageLayout::ShaderReadOnly);
    }

    Ref<ImGuiTexture> ImGuiLayer::CreateTexture(const Renderer::Sampler& sampler, const Renderer::ImageView& imageView)
    {
        auto native = CreateUnique<ImGuiTexture::Native>();

        native->Set = ImGui_ImplVulkan_AddTexture(
            GetVkSampler(sampler),
            GetVkImageView(imageView),
            static_cast<VkImageLayout>(vk::ImageLayout::eShaderReadOnlyOptimal));

        return Ref<ImGuiTexture>(new ImGuiTexture(std::move(native), *this));
    }

    void ImGuiLayer::DestroyTexture(const ImGuiTexture& texture)
    {
        // The descriptor set may still be read by command buffers in flight; defer
        // its free until those frames complete, or the GPU faults on a set whose
        // pool slot has been reused. The drain runs in BeginFrame, before
        // Context::BeginFrame waits this frame's fence, so the count must cover the
        // frame in flight at the time of queueing plus the MaxFramesInFlight cycles
        // that follow — hence MaxFramesInFlight + 1.
        m_PendingTextureRemovals.push_back({texture.GetNative().Set, m_Context.GetMaxFramesInFlight() + 1});
    }
}
