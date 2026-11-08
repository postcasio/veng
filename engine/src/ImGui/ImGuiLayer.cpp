#include <Veng/ImGui/ImGuiLayer.h>

#include <cstddef>
#include <cstring>
#include <filesystem>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Event.h>
#include <Veng/InputEvents.h>
#include <Veng/Window.h>
#include <Veng/WindowEvents.h>
#include <Veng/UI/Theme.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/VertexBufferLayout.h>
#include "ImGuiTextureNative.h"

#include <Veng/Vendor/ImGui.h>
#include <Veng/Vendor/FontAwesome7.h>

#include <backends/imgui_impl_glfw.h>

#include <GLFW/glfw3.h>

#ifdef VENG_HAS_DEFAULT_FONT
namespace Veng
{
    extern const unsigned char g_DefaultFont[];
    extern const unsigned long g_DefaultFontSize;
}
#endif

#ifdef VENG_HAS_ICON_FONT
namespace Veng
{
    extern const unsigned char g_IconFont[];
    extern const unsigned long g_IconFontSize;
}
#endif

namespace Veng
{
    using Renderer::Context;

    namespace
    {
        // Core pack ImGui stages.
        constexpr AssetId ImGuiVertId{0xE38A4ECAF908D31FULL};
        constexpr AssetId ImGuiFragId{0x19BC9581419ECB59ULL};

        // The display-to-clip transform and the draw's bindless texture and sampler slots. Matches
        // imgui.vert and imgui.frag PushConstants byte-for-byte.
        struct ImGuiPushConstants
        {
            vec2 Scale;
            vec2 Translate;
            u32 Texture;
            u32 Sampler;
        };

        // The vertex layout below mirrors ImDrawVert field for field.
        static_assert(sizeof(ImDrawVert) == 20 && offsetof(ImDrawVert, pos) == 0 &&
                          offsetof(ImDrawVert, uv) == 8 && offsetof(ImDrawVert, col) == 16,
                      "ImDrawVert no longer matches the ImGui vertex layout");
        static_assert(sizeof(ImDrawIdx) == 2, "the index ring binds ImDrawIdx as 16-bit indices");

        // The ring grows to at least this many bytes, so the first frames do not reallocate per
        // widget added.
        constexpr u64 MinRingBytes = 64 * 1024;

        // Registered as ImGui's reset-render-state callback; the render loop recognizes it and
        // re-applies its state rather than calling it.
        void ResetRenderStateCallback(const ImDrawList* /*list*/, const ImDrawCmd* /*cmd*/) {}
    }

    /// @brief One of ImGui's own textures, created on its request.
    struct ImGuiDynamicTexture
    {
        Ref<Renderer::Image> Image;
        Ref<Renderer::ImageView> View;
        Renderer::TextureHandle Handle;
    };

    struct ImGuiLayer::DrawResources
    {
        /// @brief The vertex and index storage one frame in flight writes.
        struct FrameRing
        {
            Ref<Renderer::Buffer> Vertices;
            Ref<Renderer::Buffer> Indices;
        };

        Ref<Renderer::ShaderModule> VertexModule;
        Ref<Renderer::ShaderModule> FragmentModule;
        Ref<Renderer::PipelineLayout> Layout;
        /// @brief One pipeline per target format, built on first use.
        vector<std::pair<Renderer::Format, Ref<Renderer::GraphicsPipeline>>> Pipelines;
        /// @brief Indexed by frame in flight.
        vector<FrameRing> Rings;
        /// @brief The sampler ImGui's own textures are drawn through.
        Renderer::SamplerHandle LinearSampler;
        /// @brief ImGui's own textures, keyed by the request record ImGui owns.
        map<const ImTextureData*, ImGuiDynamicTexture> Textures;

        /// @brief Returns the pipeline rendering into @p format, building it on first use.
        const Ref<Renderer::GraphicsPipeline>& PipelineFor(Context& context,
                                                           const Renderer::Format format)
        {
            for (const auto& [built, pipeline] : Pipelines)
            {
                if (built == format)
                {
                    return pipeline;
                }
            }

            using namespace Renderer;
            const Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
                context,
                {
                    .Name = "ImGui Pipeline",
                    .ColorAttachments = {{.Format = format, .Blend = BlendState::AlphaBlend()}},
                    .VertexBufferLayout = VertexBufferLayout{{Format::RG32Sfloat, "a_Position"},
                                                             {Format::RG32Sfloat, "a_UV"},
                                                             {Format::RGBA8Unorm, "a_Color"}},
                    .PipelineLayout = Layout,
                    .ShaderStages =
                        {
                            {.Stage = ShaderStage::Vertex, .Module = VertexModule},
                            {.Stage = ShaderStage::Fragment, .Module = FragmentModule},
                        },
                });
            return Pipelines.emplace_back(format, pipeline).second;
        }
    };

    namespace
    {
        // Returns @p buffer when it holds @p bytes, else a replacement that does; the replaced
        // buffer's destruction is deferred past the frames that may still read it.
        Ref<Renderer::Buffer> EnsureRing(Context& context, const Ref<Renderer::Buffer>& buffer,
                                         const u64 bytes, const Renderer::BufferUsage usage,
                                         const char* name)
        {
            if (buffer != nullptr && buffer->GetSize() >= bytes)
            {
                return buffer;
            }
            const u64 grown = buffer != nullptr ? buffer->GetSize() * 2 : MinRingBytes;
            return Renderer::Buffer::Create(context, {
                                                         .Name = name,
                                                         .Size = std::max(bytes, grown),
                                                         .Usage = usage,
                                                         .HostMapped = true,
                                                     });
        }
    }

    Unique<ImGuiLayer> ImGuiLayer::Create(const ImGuiLayerInfo& info, Context& context,
                                          AssetManager& assets, Window& window)
    {
        return Unique<ImGuiLayer>(new ImGuiLayer(info, context, assets, window));
    }

    ImGuiLayer::ImGuiLayer(const ImGuiLayerInfo& info, Context& context, AssetManager& assets,
                           Window& window)
        : m_Context(context), m_Window(window), m_Draw(CreateUnique<DrawResources>())
    {
        using namespace Renderer;

        // The modules are held rather than the asset handles, so the layer does not pin the
        // manager that loaded them.
        const AssetResult<AssetHandle<Shader>> vs = assets.LoadSync<Shader>(ImGuiVertId);
        VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
        m_Draw->VertexModule = vs->Get()->Module;
        const AssetResult<AssetHandle<Shader>> fs = assets.LoadSync<Shader>(ImGuiFragId);
        VE_ASSERT(fs.has_value(), "{}", fs.error().Detail);
        m_Draw->FragmentModule = fs->Get()->Module;

        m_Draw->Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "ImGui Layout",
                           .PushConstantRanges = {PushConstantRange::Of<ImGuiPushConstants>(
                               ShaderStage::Vertex | ShaderStage::Fragment)},
                       });
        m_Draw->Rings.resize(m_Context.GetMaxFramesInFlight());

        // ImGui's own textures sample linearly and clamp, as its reference renderers do: the font
        // atlas packs glyphs edge to edge, so a wrap would bleed one into its neighbour.
        m_Draw->LinearSampler = m_Context.GetBindlessRegistry()
                                    .AcquireSampler({
                                        .Name = "ImGui Linear Sampler",
                                        .MagFilter = Filter::Linear,
                                        .MinFilter = Filter::Linear,
                                        .MipmapMode = MipmapMode::Linear,
                                        .AddressModeU = AddressMode::ClampToEdge,
                                        .AddressModeV = AddressMode::ClampToEdge,
                                        .AddressModeW = AddressMode::ClampToEdge,
                                        .AnisotropyEnabled = false,
                                        .MinLod = -1000.0f,
                                        .MaxLod = 1000.0f,
                                    })
                                    .Handle;

        CreateResources();
        (void)m_Draw->PipelineFor(m_Context, m_Image->GetFormat());

        ImGui::CreateContext();
        ImNodes::CreateContext();

        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // ImGui borrows this pointer rather than copying the string, so the path is held by the
        // layer. Its own default is the relative `imgui.ini`, resolved against the working
        // directory — which an application has no guarantee it may write to, and which inside a
        // macOS bundle is the bundle itself. So the path is always explicit here, and no path at
        // all means no persistence rather than a write to wherever the process happens to stand.
        if (info.IniPath)
        {
            m_IniPath = info.IniPath->string();
            io.IniFilename = m_IniPath.c_str();
        }
        else
        {
            io.IniFilename = nullptr;
        }
#ifdef VE_DEBUG
        io.ConfigDebugIsDebuggerPresent = true;
#endif

        GLFWwindow* handle = GetGlfwWindow(window);

        // Callbacks off: the engine owns the GLFW callbacks and the InputRouter forwards
        // events to the backend through ForwardEvent, so input is routed by focus rather
        // than ingested behind the engine's back.
        ImGui_ImplGlfw_InitForOther(handle, false);

        // The renderer honours ImDrawCmd::VtxOffset, so a draw list may pass 64k vertices with
        // 16-bit indices, and services ImGui's texture requests, so the font atlas grows on demand.
        io.BackendRendererName = "veng";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::GetPlatformIO().DrawCallback_ResetRenderState = ResetRenderStateCallback;

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

        // FontAwesome merged onto the default font, so the FontAwesome7.h ICON_FA_* glyphs
        // render inline with text. The dynamic font atlas loads glyphs on demand, so no glyph
        // range is supplied. Sized just under the 16px body text so icons sit level with it
        // rather than towering over a label; the advance keeps them roughly monospaced.
        constexpr f32 iconSize = 14.0f;
        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.GlyphOffset = ImVec2(0, 2.0f);
        iconConfig.GlyphMinAdvanceX = 16.0f;

        if (info.IconFontPath && std::filesystem::exists(*info.IconFontPath))
        {
            io.Fonts->AddFontFromFileTTF(info.IconFontPath->string().c_str(), iconSize,
                                         &iconConfig);
        }
#ifdef VENG_HAS_ICON_FONT
        else
        {
            iconConfig.FontDataOwnedByAtlas = false; // static embed; the atlas must not free it
            io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(g_IconFont),
                                           static_cast<int>(g_IconFontSize), iconSize, &iconConfig);
        }
#endif

        // Swap-chain recreation changes the render extent; recreate the offscreen image to match.
        m_Context.AddSwapChainInvalidationCallback(
            [this]
            {
                ReleaseResources();
                CreateResources();
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
        style.WindowMenuButtonPosition =
            theme.ShowWindowCollapseButton ? ImGuiDir_Left : ImGuiDir_None;
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
        // ImGui's own textures are released while their request records are still alive, so each
        // is marked destroyed for the context that is about to drop it.
        Renderer::BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
        {
            const auto found = m_Draw->Textures.find(texture);
            if (found != m_Draw->Textures.end())
            {
                bindless.Release(found->second.Handle);
                m_Draw->Textures.erase(found);
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        for (const auto& [record, texture] : m_Draw->Textures)
        {
            bindless.Release(texture.Handle);
        }
        m_Draw->Textures.clear();

        ImGui::GetIO().BackendRendererName = nullptr;
        ImGui::GetIO().BackendFlags &=
            ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
        ImGui::GetPlatformIO().ClearRendererHandlers();

        ImGui_ImplGlfw_Shutdown();
        ImNodes::DestroyContext();
        ImGui::DestroyContext();

        ReleaseResources();
        m_Draw.reset();
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

    void ImGuiLayer::ReleaseResources()
    {
        m_ImageView.reset();
        m_Image.reset();
    }

    void ImGuiLayer::BeginFrame()
    {
        if (!m_RenderedThisFrame)
        {
            ImGui::EndFrame();
        }

        ImGuiIO& io = ImGui::GetIO();
        if (m_Skipped)
        {
            // A key or button released while no frame ran never reached ImGui; this frame's own
            // events are still queued and apply on top of the cleared state.
            io.ClearInputKeys();
            io.ClearInputMouse();
            m_Skipped = false;
        }

        m_RenderedThisFrame = false;
        m_FrameOpen = true;
        {
            // The platform half issues the per-frame window-system queries (size, focus, hover,
            // cursor), apart from ImGui's own NewFrame so a capture attributes the two separately.
            VE_PROFILE_SCOPE("ImGui/PlatformNewFrame");
            ImGui_ImplGlfw_NewFrame();
        }
        {
            VE_PROFILE_SCOPE("ImGui/NewFrame");
            ImGui::NewFrame();
        }

        // The GLFW backend re-applies the OS cursor shape each NewFrame for a free cursor,
        // which would undo Window::SetCursorVisible(false); defaulting this frame's cursor to
        // None makes the backend apply the hidden mode instead. A widget that sets a shape
        // later in the frame still wins, so ImGui's own cursors work whenever the app shows
        // the cursor.
        if (!m_Window.IsMouseCaptured() && !m_Window.IsCursorVisible())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        }
    }

    bool ImGuiLayer::ForwardEvent(const Event& event)
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
        case EventType::KeyRepeat:
            // ImGui derives its own repeat from each key's held duration, so a platform repeat is
            // not forwarded: the backend would either discard it or double the repetition ImGui
            // already synthesizes. The KeyPressed/KeyReleased pair above is the whole held state
            // ImGui needs.
            break;
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

        // Cooperative except over the keyboard ImGui holds: a key typed into one of its widgets is
        // not also a Gui edit or a navigation press. Every other event falls through.
        const EventType type = event.GetEventType();
        const bool keyboard = type == EventType::KeyPressed || type == EventType::KeyRepeat ||
                              type == EventType::KeyTyped;
        return keyboard && ImGui::GetIO().WantCaptureKeyboard;
    }

    void ImGuiLayer::OnCursorCaptured(bool captured)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!captured)
        {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        }
        else
        {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;

            // Drop any in-progress interaction before input is swallowed: a focus switch on
            // mouse-down (e.g. click-to-capture) sends the down to ImGui but routes the
            // matching up to the game, which would otherwise leave a button stuck held and
            // break ImGui input on the next release.
            io.ClearInputMouse();
        }
    }

    void ImGuiLayer::SkipFrame()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.WantCaptureMouse = false;
        io.WantCaptureKeyboard = false;
        io.WantTextInput = false;

        m_DrewOutput = false;
        m_FrameOpen = false;
        m_Skipped = true;
    }

    void ImGuiLayer::Render(Renderer::CommandBuffer& commandBuffer)
    {
        using namespace Renderer;

        m_RenderedThisFrame = true;
        m_FrameOpen = false;

        ImGui::Render();
        const ImDrawData* drawData = ImGui::GetDrawData();

        // Texture requests are serviced whether or not anything draws: an empty frame may still
        // be the one ImGui retires a texture in.
        UpdateTextures(commandBuffer, *drawData);

        m_DrewOutput = drawData->CmdListsCount > 0 && drawData->TotalVtxCount > 0;
        if (!m_DrewOutput)
        {
            return;
        }

        commandBuffer.PrepareForAccess(m_ImageView, AccessKind::ColorAttachment);

        commandBuffer.BeginRendering({
            .Extent = m_Context.GetRenderExtent(),
            .ColorAttachments = {{
                .ImageView = m_ImageView,
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            }},
        });

        RecordDrawData(commandBuffer, *drawData);

        commandBuffer.EndRendering();

        commandBuffer.PrepareForAccess(m_ImageView, AccessKind::SampleGraphics);
    }

    void ImGuiLayer::UpdateTextures(Renderer::CommandBuffer& cmd, const ImDrawData& drawData)
    {
        using namespace Renderer;

        if (drawData.Textures == nullptr)
        {
            return;
        }

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        for (ImTextureData* texture : *drawData.Textures)
        {
            if (texture->Status == ImTextureStatus_OK)
            {
                continue;
            }

            if (texture->Status == ImTextureStatus_WantDestroy)
            {
                // The slot and the image retire past the frames in flight that may still sample
                // them, so the request is honoured at once.
                const auto found = m_Draw->Textures.find(texture);
                if (found != m_Draw->Textures.end())
                {
                    bindless.Release(found->second.Handle);
                    m_Draw->Textures.erase(found);
                }
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
                continue;
            }

            const bool create = texture->Status == ImTextureStatus_WantCreate;
            if (create)
            {
                VE_ASSERT(texture->Format == ImTextureFormat_RGBA32,
                          "ImGuiLayer: texture {} requests format {}; only RGBA32 is served",
                          texture->UniqueID, static_cast<i32>(texture->Format));
                const Ref<Image> image = Image::Create(
                    m_Context, {
                                   .Name = fmt::format("ImGui Texture {}", texture->UniqueID),
                                   .Extent = {static_cast<u32>(texture->Width),
                                              static_cast<u32>(texture->Height), 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
                const Ref<ImageView> view =
                    ImageView::Create(m_Context, {.Name = image->GetName(), .Image = image});
                const TextureHandle handle = bindless.Register(view);
                m_Draw->Textures.insert_or_assign(
                    texture, ImGuiDynamicTexture{.Image = image, .View = view, .Handle = handle});
                texture->SetTexID(PackImGuiTextureId(handle, m_Draw->LinearSampler));
            }

            const auto found = m_Draw->Textures.find(texture);
            VE_ASSERT(found != m_Draw->Textures.end(),
                      "ImGuiLayer: texture {} requests an update before it was created",
                      texture->UniqueID);
            const ImGuiDynamicTexture& dynamic = found->second;

            // A create uploads the whole texture, which also clears it; an update uploads the
            // bounding rectangle of the changed regions, which ImGui keeps in UpdateRect.
            const u32 x = create ? 0u : texture->UpdateRect.x;
            const u32 y = create ? 0u : texture->UpdateRect.y;
            const u32 width = create ? static_cast<u32>(texture->Width) : texture->UpdateRect.w;
            const u32 height = create ? static_cast<u32>(texture->Height) : texture->UpdateRect.h;
            const usize pitch = static_cast<usize>(width) * texture->BytesPerPixel;

            vector<u8> pixels(pitch * height);
            for (u32 row = 0; row < height; ++row)
            {
                std::memcpy(pixels.data() + (pitch * row),
                            texture->GetPixelsAt(static_cast<int>(x), static_cast<int>(y + row)),
                            pitch);
            }
            const Ref<Buffer> staging =
                Buffer::Create(m_Context, {
                                              .Name = "ImGui Texture Upload",
                                              .Size = pixels.size(),
                                              .Usage = BufferUsage::TransferSrc,
                                          });
            staging->UploadSync(pixels);

            const BufferImageCopyRegion region{
                .BufferOffset = 0,
                .MipLevel = 0,
                .ImageOffset = {x, y, 0},
                .Extent = {width, height, 1},
            };
            cmd.PrepareForAccess(dynamic.View, AccessKind::TransferDst);
            cmd.CopyBufferToImage(staging, dynamic.Image, {&region, 1});
            cmd.PrepareForAccess(dynamic.View, AccessKind::SampleGraphics);
            // The staging Ref drops here; deferred destruction keeps it alive until the frame
            // fence, so the recorded copy still has its source when the GPU runs it.

            texture->SetStatus(ImTextureStatus_OK);
        }
    }

    void ImGuiLayer::RecordDrawData(Renderer::CommandBuffer& cmd, const ImDrawData& drawData)
    {
        using namespace Renderer;

        // Scissor rectangles are projected into framebuffer pixels, which differ from ImGui's
        // display units on a scaled display.
        const vec2 clipOffset = drawData.DisplayPos;
        const vec2 clipScale = drawData.FramebufferScale;
        const vec2 framebuffer = vec2(drawData.DisplaySize) * clipScale;
        if (framebuffer.x <= 0.0f || framebuffer.y <= 0.0f)
        {
            return;
        }
        const uvec2 framebufferExtent = uvec2(framebuffer);

        // Every list is packed into one vertex and one index stream this frame slot owns; a draw
        // then addresses its list through the running offsets.
        DrawResources::FrameRing& ring = m_Draw->Rings[m_Context.GetCurrentFrameInFlight()];
        const u64 vertexBytes = static_cast<u64>(drawData.TotalVtxCount) * sizeof(ImDrawVert);
        const u64 indexBytes = static_cast<u64>(drawData.TotalIdxCount) * sizeof(ImDrawIdx);
        ring.Vertices = EnsureRing(m_Context, ring.Vertices, vertexBytes, BufferUsage::Vertex,
                                   "ImGui Vertices");
        ring.Indices =
            EnsureRing(m_Context, ring.Indices, indexBytes, BufferUsage::Index, "ImGui Indices");

        auto* vertices = static_cast<ImDrawVert*>(ring.Vertices->GetMappedData());
        auto* indices = static_cast<ImDrawIdx*>(ring.Indices->GetMappedData());
        for (const ImDrawList* list : drawData.CmdLists)
        {
            std::memcpy(vertices, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
            std::memcpy(indices, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
            vertices += list->VtxBuffer.Size;
            indices += list->IdxBuffer.Size;
        }

        // The display rectangle [DisplayPos, DisplayPos + DisplaySize] maps onto clip [-1, 1].
        const vec2 scale = 2.0f / vec2(drawData.DisplaySize);
        const vec2 translate = -1.0f - (vec2(drawData.DisplayPos) * scale);

        const Ref<GraphicsPipeline>& pipeline =
            m_Draw->PipelineFor(m_Context, m_Image->GetFormat());
        const auto setupRenderState = [&]
        {
            cmd.BindPipeline(pipeline);
            m_Context.GetBindlessRegistry().Bind(cmd);
            cmd.BindVertexBuffer(ring.Vertices);
            cmd.BindIndexBuffer(ring.Indices, IndexType::U16);
            cmd.SetViewport({0, 0}, framebufferExtent);
        };
        setupRenderState();

        u32 vertexBase = 0;
        u32 indexBase = 0;
        for (const ImDrawList* list : drawData.CmdLists)
        {
            for (const ImDrawCmd& draw : list->CmdBuffer)
            {
                if (draw.UserCallback != nullptr)
                {
                    if (draw.UserCallback == ResetRenderStateCallback)
                    {
                        setupRenderState();
                    }
                    else
                    {
                        draw.UserCallback(list, &draw);
                    }
                    continue;
                }

                // Clamped to the framebuffer, since a scissor may not leave it.
                const vec2 clipMin = glm::max(
                    (vec2(draw.ClipRect.x, draw.ClipRect.y) - clipOffset) * clipScale, vec2(0.0f));
                const vec2 clipMax = glm::min(
                    (vec2(draw.ClipRect.z, draw.ClipRect.w) - clipOffset) * clipScale, framebuffer);
                if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
                {
                    continue;
                }
                cmd.SetScissor(ivec2(clipMin), uvec2(clipMax - clipMin));

                const ImGuiTextureSlots slots = UnpackImGuiTextureId(draw.GetTexID());
                cmd.PushConstants(ImGuiPushConstants{
                    .Scale = scale,
                    .Translate = translate,
                    .Texture = slots.Texture,
                    .Sampler = slots.Sampler,
                });
                cmd.DrawIndexed(draw.ElemCount, 1, draw.IdxOffset + indexBase,
                                static_cast<i32>(draw.VtxOffset + vertexBase), 0);
            }
            vertexBase += static_cast<u32>(list->VtxBuffer.Size);
            indexBase += static_cast<u32>(list->IdxBuffer.Size);
        }

        // A later draw that sets no scissor of its own inherits the whole target, not the last
        // clip rectangle.
        cmd.SetScissor({0, 0}, framebufferExtent);
    }

    Ref<ImGuiTexture> ImGuiLayer::CreateTexture(const Renderer::Sampler& sampler,
                                                const Renderer::ImageView& imageView)
    {
        using namespace Renderer;

        // The registration owns a view of its own over the same subresources, so the slot keeps
        // the image alive for as long as a draw may sample it.
        const Ref<ImageView> view =
            ImageView::Create(m_Context, {
                                             .Name = imageView.GetName(),
                                             .Image = imageView.GetImage(),
                                             .ViewType = imageView.GetViewType(),
                                             .BaseMipLevel = imageView.GetBaseMipLevel(),
                                             .MipLevels = imageView.GetMipLevels(),
                                             .BaseArrayLayer = imageView.GetBaseArrayLayer(),
                                             .ArrayLayers = imageView.GetArrayLayers(),
                                         });

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        auto native = CreateUnique<ImGuiTexture::Native>(ImGuiTexture::Native{
            .Texture = bindless.Register(view),
            .Sampler = bindless.AcquireSampler(sampler.GetInfo()).Handle,
        });

        return Ref<ImGuiTexture>(new ImGuiTexture(std::move(native), *this));
    }

    void ImGuiLayer::DestroyTexture(const ImGuiTexture& texture)
    {
        // The registry defers the slot's reuse past every frame in flight that may still sample
        // it; the sampler slot is the registry's shared one and is not the texture's to release.
        m_Context.GetBindlessRegistry().Release(texture.GetNative().Texture);
    }
}
