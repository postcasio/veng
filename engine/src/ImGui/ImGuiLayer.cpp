#include <Veng/ImGui/ImGuiLayer.h>

#include <filesystem>

#include <Veng/Event.h>
#include <Veng/InputEvents.h>
#include <Veng/Window.h>
#include <Veng/WindowEvents.h>
#include <Veng/UI/Theme.h>
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

#include <GLFW/glfw3.h>

#ifdef VENG_HAS_DEFAULT_FONT
namespace Veng
{
    extern const unsigned char g_DefaultFont[];
    extern const unsigned long g_DefaultFontSize;
}
#endif

namespace Veng
{
    using Renderer::Context;

    struct ImGuiLayer::PendingTextureRemoval
    {
        vk::DescriptorSet Set;
        u32 FramesRemaining;
    };

    Unique<ImGuiLayer> ImGuiLayer::Create(const ImGuiLayerInfo& info, Context& context,
                                          Window& window)
    {
        return Unique<ImGuiLayer>(new ImGuiLayer(info, context, window));
    }

    ImGuiLayer::ImGuiLayer(const ImGuiLayerInfo& info, Context& context, Window& window)
        : m_Context(context), m_Window(window)
    {
        using namespace Renderer;

        m_DescriptorPool = DescriptorPool::Create(
            m_Context, {.Name = "ImGui Descriptor Pool",
                        .MaxSets = 100000,
                        .PoolSizes = {{
                                          .type = vk::DescriptorType::eUniformBuffer,
                                          .descriptorCount = 100000,
                                      },
                                      {
                                          .type = vk::DescriptorType::eCombinedImageSampler,
                                          .descriptorCount = 100000,
                                      }},
                        .Flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet |
                                 vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind});

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

        // Callbacks off: the engine owns the GLFW callbacks and the InputRouter forwards
        // events to the backend through ForwardEvent, so input is routed by focus rather
        // than ingested behind the engine's back.
        ImGui_ImplGlfw_InitForVulkan(handle, false);

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
            .PipelineInfoMain =
                {
                    .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
                    .PipelineRenderingCreateInfo = pipelineRenderingCreateInfo,
                },
            .UseDynamicRendering = true,
        };

        ImGui_ImplVulkan_Init(&initInfo);

        ApplyTheme();

        if (info.DefaultFontPath && std::filesystem::exists(*info.DefaultFontPath))
        {
            io.Fonts->AddFontFromFileTTF(info.DefaultFontPath->string().c_str(), 16.0f);
        }
#ifdef VENG_HAS_DEFAULT_FONT
        else
        {
            // Embedded Roboto is the engine default UI font; ImGui's built-in
            // bitmap font is reached only when this embed is compiled out.
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false; // static embed; the atlas must not free it
            io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(g_DefaultFont),
                                           static_cast<int>(g_DefaultFontSize), 16.0f, &config);
        }
#endif

        if (info.IconFontPath && std::filesystem::exists(*info.IconFontPath))
        {
            ImFontConfig config;
            config.MergeMode = true;
            config.GlyphOffset = ImVec2(0, 5.0f);
            config.GlyphMinAdvanceX = 20.0f;
            static constexpr ImWchar icon_ranges[] = {ICON_MIN_MD, ICON_MAX_16_MD, 0};
            io.Fonts->AddFontFromFileTTF(info.IconFontPath->string().c_str(), 20.0f, &config,
                                         icon_ranges);
        }

        // Swap-chain recreation changes the render extent; recreate the offscreen image to match.
        m_Context.AddSwapChainInvalidationCallback(
            [this]
            {
                DisposeResources();
                CreateResources();
#ifdef VE_VIEWPORTS
                const uvec2 extent = m_Context.GetRenderExtent();
                ImGui::GetMainViewport()->Size = vec2(extent);
#endif
            });
    }

    void ImGuiLayer::ApplyTheme()
    {
        const UI::Theme& theme = UI::GetTheme();

        // Returns the role color with its alpha overridden — for the translucent overlays
        // (selection bands, resize grips, dim layers) ImGui expects.
        const auto alpha = [](vec4 color, f32 a)
        {
            color.a = a;
            return color;
        };

        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowRounding = theme.WindowRounding;
        style.ChildRounding = theme.ChildRounding;
        style.FrameRounding = theme.FrameRounding;
        style.PopupRounding = theme.PopupRounding;
        style.GrabRounding = theme.GrabRounding;
        style.TabRounding = theme.TabRounding;
        style.ScrollbarRounding = theme.ScrollbarRounding;
        style.WindowBorderSize = theme.BorderSize;
        style.FrameBorderSize = theme.BorderSize;
        style.PopupBorderSize = theme.BorderSize;
        style.WindowPadding = theme.WindowPadding;
        style.FramePadding = theme.FramePadding;
        style.ItemSpacing = theme.ItemSpacing;
        style.ItemInnerSpacing = theme.ItemInnerSpacing;
        style.ScrollbarSize = theme.ScrollbarSize;
        style.GrabMinSize = theme.GrabMinSize;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.SeparatorTextBorderSize = 1.0f;
        style.DockingSeparatorSize = 1.0f;

        // Reset every slot to ImGui's sRGB defaults first, so the slots not overridden below
        // still get linearized from a known sRGB value and the linearize pass stays idempotent
        // across repeated ApplyTheme calls.
        ImGui::StyleColorsDark(&style);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = theme.Text;
        colors[ImGuiCol_TextDisabled] = theme.TextDisabled;
        colors[ImGuiCol_WindowBg] = theme.Background;
        colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colors[ImGuiCol_PopupBg] = theme.Surface;
        colors[ImGuiCol_Border] = theme.Border;
        colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colors[ImGuiCol_FrameBg] = theme.Surface;
        colors[ImGuiCol_FrameBgHovered] = theme.SurfaceHovered;
        colors[ImGuiCol_FrameBgActive] = theme.SurfaceActive;
        colors[ImGuiCol_TitleBg] = theme.Background;
        colors[ImGuiCol_TitleBgActive] = theme.Surface;
        colors[ImGuiCol_TitleBgCollapsed] = theme.Background;
        colors[ImGuiCol_MenuBarBg] = theme.Background;
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colors[ImGuiCol_ScrollbarGrab] = theme.SurfaceRaised;
        colors[ImGuiCol_ScrollbarGrabHovered] = theme.SurfaceHovered;
        colors[ImGuiCol_ScrollbarGrabActive] = theme.SurfaceActive;
        colors[ImGuiCol_CheckMark] = theme.Accent;
        colors[ImGuiCol_SliderGrab] = theme.Accent;
        colors[ImGuiCol_SliderGrabActive] = theme.AccentHovered;
        colors[ImGuiCol_Button] = theme.SurfaceRaised;
        colors[ImGuiCol_ButtonHovered] = theme.SurfaceHovered;
        colors[ImGuiCol_ButtonActive] = theme.SurfaceActive;
        colors[ImGuiCol_Header] = theme.AccentMuted;
        colors[ImGuiCol_HeaderHovered] = alpha(theme.Accent, 0.40f);
        colors[ImGuiCol_HeaderActive] = alpha(theme.Accent, 0.55f);
        colors[ImGuiCol_Separator] = theme.Border;
        colors[ImGuiCol_SeparatorHovered] = theme.AccentHovered;
        colors[ImGuiCol_SeparatorActive] = theme.Accent;
        colors[ImGuiCol_ResizeGrip] = alpha(theme.Accent, 0.20f);
        colors[ImGuiCol_ResizeGripHovered] = alpha(theme.Accent, 0.55f);
        colors[ImGuiCol_ResizeGripActive] = alpha(theme.Accent, 0.85f);
        colors[ImGuiCol_InputTextCursor] = theme.Text;
        colors[ImGuiCol_Tab] = theme.Background;
        colors[ImGuiCol_TabHovered] = theme.SurfaceHovered;
        colors[ImGuiCol_TabSelected] = theme.Surface;
        colors[ImGuiCol_TabSelectedOverline] = theme.Accent;
        colors[ImGuiCol_TabDimmed] = theme.Background;
        colors[ImGuiCol_TabDimmedSelected] = theme.Surface;
        colors[ImGuiCol_TabDimmedSelectedOverline] = alpha(theme.Accent, 0.40f);
        colors[ImGuiCol_DockingPreview] = alpha(theme.Accent, 0.70f);
        colors[ImGuiCol_DockingEmptyBg] = theme.Background;
        colors[ImGuiCol_PlotLines] = theme.TextMuted;
        colors[ImGuiCol_PlotLinesHovered] = theme.Accent;
        colors[ImGuiCol_PlotHistogram] = theme.Accent;
        colors[ImGuiCol_PlotHistogramHovered] = theme.AccentHovered;
        colors[ImGuiCol_TableHeaderBg] = theme.Surface;
        colors[ImGuiCol_TableBorderStrong] = theme.BorderStrong;
        colors[ImGuiCol_TableBorderLight] = theme.Border;
        colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colors[ImGuiCol_TableRowBgAlt] = alpha(theme.Surface, 0.40f);
        colors[ImGuiCol_TextLink] = theme.Accent;
        colors[ImGuiCol_TextSelectedBg] = alpha(theme.Accent, 0.35f);
        colors[ImGuiCol_TreeLines] = theme.Border;
        colors[ImGuiCol_DragDropTarget] = theme.Accent;
        colors[ImGuiCol_UnsavedMarker] = theme.Warning;
        colors[ImGuiCol_NavCursor] = theme.Accent;
        colors[ImGuiCol_NavWindowingHighlight] = alpha(theme.Text, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = alpha(theme.Background, 0.60f);
        colors[ImGuiCol_ModalWindowDimBg] = alpha(theme.Background, 0.60f);

        // The UI flows through a linear pipeline (linear float overlay, display re-encodes to
        // sRGB at scanout), so every sRGB-authored slot is linearized to round-trip correctly.
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            colors[i] = UI::SrgbToLinear(colors[i]);
        }

        // imnodes colors take the same linearization before packing to ImU32.
        const auto node = [](vec4 color) { return ImColor(UI::SrgbToLinear(color)); };
        ImNodesStyle& imnodesStyle = ImNodes::GetStyle();
        imnodesStyle.Colors[ImNodesCol_NodeBackground] = node(theme.Surface);
        imnodesStyle.Colors[ImNodesCol_NodeBackgroundHovered] = node(theme.SurfaceRaised);
        imnodesStyle.Colors[ImNodesCol_NodeBackgroundSelected] = node(theme.SurfaceRaised);
        imnodesStyle.Colors[ImNodesCol_NodeOutline] = node(theme.Border);
        imnodesStyle.Colors[ImNodesCol_TitleBar] = node(theme.SurfaceRaised);
        imnodesStyle.Colors[ImNodesCol_TitleBarHovered] = node(theme.SurfaceHovered);
        imnodesStyle.Colors[ImNodesCol_TitleBarSelected] = node(theme.SurfaceHovered);
        imnodesStyle.Colors[ImNodesCol_Link] = node(theme.Accent);
        imnodesStyle.Colors[ImNodesCol_LinkHovered] = node(theme.AccentHovered);
        imnodesStyle.Colors[ImNodesCol_LinkSelected] = node(theme.AccentHovered);
        imnodesStyle.Colors[ImNodesCol_Pin] = node(theme.Accent);
        imnodesStyle.Colors[ImNodesCol_PinHovered] = node(theme.AccentHovered);
        imnodesStyle.Colors[ImNodesCol_GridBackground] = node(theme.Background);
        imnodesStyle.Colors[ImNodesCol_GridLine] = node(theme.Border);
        imnodesStyle.Colors[ImNodesCol_GridLinePrimary] = node(theme.BorderStrong);
    }

    ImGuiLayer::~ImGuiLayer()
    {
        // The device is idle by teardown (Application waits before dropping the
        // layer), so any sets still in the retire queue free immediately. Must run
        // before the backend shuts down, while the sets are still valid.
        for (const PendingTextureRemoval& removal : m_PendingTextureRemovals)
        {
            ImGui_ImplVulkan_RemoveTexture(removal.Set);
        }
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

        m_Image =
            Image::Create(m_Context, {.Name = "ImGui Image",
                                      .Extent = {extent.x, extent.y, 1},
                                      .MipLevels = 1,
                                      .Layers = 1,
                                      .Format = Format::RGBA16Sfloat,
                                      .Type = ImageType::Type2D,
                                      .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled});

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
        // The free runs outside the erase_if predicate: a hardened STL may evaluate
        // the predicate more than once per element, which would free the same set twice.
        vector<vk::DescriptorSet> setsToFree;
        std::erase_if(m_PendingTextureRemovals,
                      [&setsToFree](PendingTextureRemoval& removal)
                      {
                          if (removal.FramesRemaining == 0)
                          {
                              setsToFree.push_back(removal.Set);
                              return true;
                          }
                          --removal.FramesRemaining;
                          return false;
                      });
        for (const vk::DescriptorSet set : setsToFree)
        {
            ImGui_ImplVulkan_RemoveTexture(set);
        }

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

    void ImGuiLayer::ForwardEvent(const Event& event)
    {
        GLFWwindow* handle = GetGlfwWindow(m_Window);
        switch (event.GetEventType())
        {
        case EventType::KeyPressed:
        {
            const auto& key = static_cast<const KeyPressedEvent&>(event);
            ImGui_ImplGlfw_KeyCallback(handle, static_cast<int>(key.GetKey()), key.GetScancode(),
                                       GLFW_PRESS, key.GetMods());
            break;
        }
        case EventType::KeyReleased:
        {
            const auto& key = static_cast<const KeyReleasedEvent&>(event);
            ImGui_ImplGlfw_KeyCallback(handle, static_cast<int>(key.GetKey()), key.GetScancode(),
                                       GLFW_RELEASE, key.GetMods());
            break;
        }
        case EventType::KeyTyped:
            ImGui_ImplGlfw_CharCallback(handle,
                                        static_cast<const KeyTypedEvent&>(event).GetCodepoint());
            break;
        case EventType::MouseButtonPressed:
        {
            const auto& button = static_cast<const MouseButtonPressedEvent&>(event);
            ImGui_ImplGlfw_MouseButtonCallback(handle, static_cast<int>(button.GetButton()),
                                               GLFW_PRESS, button.GetMods());
            break;
        }
        case EventType::MouseButtonReleased:
        {
            const auto& button = static_cast<const MouseButtonReleasedEvent&>(event);
            ImGui_ImplGlfw_MouseButtonCallback(handle, static_cast<int>(button.GetButton()),
                                               GLFW_RELEASE, button.GetMods());
            break;
        }
        case EventType::MouseMoved:
        {
            const vec2 position = static_cast<const MouseMovedEvent&>(event).GetPosition();
            ImGui_ImplGlfw_CursorPosCallback(handle, position.x, position.y);
            break;
        }
        case EventType::MouseScrolled:
        {
            const vec2 offset = static_cast<const MouseScrolledEvent&>(event).GetOffset();
            ImGui_ImplGlfw_ScrollCallback(handle, offset.x, offset.y);
            break;
        }
        case EventType::MouseEntered:
            ImGui_ImplGlfw_CursorEnterCallback(
                handle,
                static_cast<const MouseEnteredEvent&>(event).HasEntered() ? GLFW_TRUE : GLFW_FALSE);
            break;
        case EventType::WindowFocus:
            ImGui_ImplGlfw_WindowFocusCallback(
                handle,
                static_cast<const WindowFocusEvent&>(event).IsFocused() ? GLFW_TRUE : GLFW_FALSE);
            break;
        default:
            break;
        }
    }

    void ImGuiLayer::Render(Renderer::CommandBuffer& commandBuffer)
    {
        using namespace Renderer;

        m_RenderedThisFrame = true;

        Backend::TransitionImage(commandBuffer, *m_Image, ImageLayout::ColorAttachment);

        commandBuffer.BeginRendering({
            .Extent = m_Context.GetRenderExtent(),
            .ColorAttachments = {{
                .ImageView = m_ImageView,
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            }},
        });

        ImGui::Render();

        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        ImDrawData* drawData = ImGui::GetDrawData();
        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer.GetNative().CommandBuffer);

        commandBuffer.EndRendering();

        Backend::TransitionImage(commandBuffer, *m_Image, ImageLayout::ShaderReadOnly);
    }

    Ref<ImGuiTexture> ImGuiLayer::CreateTexture(const Renderer::Sampler& sampler,
                                                const Renderer::ImageView& imageView)
    {
        auto native = CreateUnique<ImGuiTexture::Native>();

        native->Set = ImGui_ImplVulkan_AddTexture(
            GetVkSampler(sampler), GetVkImageView(imageView),
            static_cast<VkImageLayout>(vk::ImageLayout::eShaderReadOnlyOptimal));

        return Ref<ImGuiTexture>(new ImGuiTexture(std::move(native), *this));
    }

    void ImGuiLayer::DestroyTexture(const ImGuiTexture& texture)
    {
        // The drain runs in BeginFrame before this frame's fence is waited, so the
        // count must cover the frame in flight at queueing time plus the subsequent
        // MaxFramesInFlight cycles — hence MaxFramesInFlight + 1.
        m_PendingTextureRemovals.push_back(
            {texture.GetNative().Set, m_Context.GetMaxFramesInFlight() + 1});
    }
}
