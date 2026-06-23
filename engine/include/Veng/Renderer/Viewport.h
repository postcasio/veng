#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ViewportRegion.h>

#include <Veng/Scene/Camera.h>

namespace Veng
{
    class Scene;
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;

    /// @brief Selects whether the engine compositor places a viewport's texture into the window.
    ///
    /// Both roles render identically — every viewport renders into its own texture at its
    /// region's resolution. The role gates only engine compositing, nothing about the render.
    enum class ViewportRole : u8
    {
        /// @brief The engine compositor places this viewport's texture into its region.
        ///
        /// At most a handful per app — the game's view, splitscreen quadrants.
        Presented,
        /// @brief A consumer samples this viewport's texture (an ImGui panel, a material).
        Offscreen,
    };

    /// @brief Per-frame render source pushed into a viewport by its owner.
    ///
    /// The input subset of the renderer's internal SceneView — the renderer-written scratch
    /// (light count, cascade matrices, broadphase) is not part of it. The owner sets this
    /// each frame; Render reads it. The viewport retains the camera for screen-to-world mapping.
    struct ViewState
    {
        /// @brief The scene to render; null renders nothing (a closed document).
        const Scene* World = nullptr;
        /// @brief The viewpoint to render from.
        CameraView Camera;
        /// @brief Frame delta time in seconds, forwarded to the renderer.
        f32 Delta = 0.0f;
        /// @brief Exposure scale applied before the tone curve.
        f32 Exposure = 1.0f;
        /// @brief Bloom bright-pass luminance knee.
        f32 BloomThreshold = 1.0f;
        /// @brief Bloom composite mix intensity.
        f32 BloomIntensity = 1.0f;
        /// @brief Bloom upsample spread.
        f32 BloomRadius = 1.0f;
    };

    /// @brief Construction parameters for Viewport.
    struct ViewportInfo
    {
        /// @brief The Vulkan context for resource creation.
        Context& Context;
        /// @brief Asset manager the owned SceneRenderer loads its engine shaders through.
        ///
        /// Must outlive the viewport.
        AssetManager& Assets;
        /// @brief Initial region; its Extent sizes the owned SceneRenderer's render target.
        ViewportRegion Region;
        /// @brief Output target format; resolved to Context::GetOutputFormat() when Undefined.
        Format ColorFormat = Format::Undefined;
        /// @brief Initial topology and sizing knobs for the owned SceneRenderer.
        SceneRendererSettings Settings;
        /// @brief Whether the engine compositor places this viewport into its region.
        ViewportRole Role = ViewportRole::Offscreen;
    };

    /// @brief A region of the window, a renderer, and a role: a renderable view into a world.
    ///
    /// Owns a SceneRenderer, carries a ViewportRegion (its window rectangle, whose extent
    /// drives the render resolution), takes a per-frame ViewState pushed by its owner, and on
    /// Render does the Execute + PrepareForAccess(Sample) pair itself. Its product is a
    /// sampleable Ref<ImageView> (GetOutput) and a bindless TextureHandle (GetOutputHandle).
    ///
    /// Single-owner (Unique); Create is the factory. A region or settings change invalidates
    /// GetOutput()/GetOutputHandle() exactly as the underlying SceneRenderer's Resize/Configure
    /// does — re-fetch both after SetRegion (on an extent change) and after Configure.
    class Viewport
    {
    public:
        /// @brief Creates a Viewport owning a fresh SceneRenderer sized to the region's extent.
        ///
        /// A ColorFormat left Undefined resolves to Context::GetOutputFormat(). The viewport is
        /// constructed unregistered: it is driveable on its own (call Render directly).
        /// @param info  Construction parameters.
        /// @return The owning Unique.
        static Unique<Viewport> Create(const ViewportInfo& info);

        /// @brief Destroys the owned renderer and releases the output's bindless slot.
        ~Viewport();

        Viewport(const Viewport&) = delete;
        Viewport& operator=(const Viewport&) = delete;

        /// @brief Sets the viewport's placement and render resolution together.
        ///
        /// A change to the extent debounces an internal SceneRenderer::Resize to the next
        /// Render, invalidating GetOutput()/GetOutputHandle() (re-fetch after the next Render).
        /// A zero extent is ignored (a collapsed or first-frame panel reports {0,0}), so it
        /// never drives SceneRenderer::Resize(0,0). The offset is stored immediately.
        /// @param region  The new placement and extent in window framebuffer pixels.
        void SetRegion(const ViewportRegion& region);

        /// @brief Binds this frame's render source (stores a copy).
        ///
        /// @param state  The scene, camera, and per-frame tone/bloom knobs to render with.
        void SetViewState(const ViewState& state);

        /// @brief Reconfigures the owned renderer's topology and sizing knobs.
        ///
        /// Invalidates GetOutput()/GetOutputHandle() exactly as SceneRenderer::Configure does;
        /// re-fetch both after this call.
        /// @param settings  The new topology and sizing knobs.
        void Configure(const SceneRendererSettings& settings);

        /// @brief Renders the bound view into the viewport's texture and makes it sampleable.
        ///
        /// Applies any pending region resize, builds the internal SceneView from the bound
        /// ViewState, calls SceneRenderer::Execute, then transitions the output for sampling
        /// (PrepareForAccess(Sample)). A null World (no ViewState set, or a closed document)
        /// is a no-op — the viewport renders nothing rather than dereferencing null.
        /// @param cmd  The command buffer to record into.
        void Render(CommandBuffer& cmd);

        /// @brief Returns the sampleable view of the rendered result.
        ///
        /// Invalidated by an extent change applied in Render and by Configure; re-fetch after.
        [[nodiscard]] Ref<ImageView> GetOutput() const;

        /// @brief Returns the bindless slot naming the rendered result.
        ///
        /// For the compositor, ImGuiLayer::CreateTexture, and Material::SetTextureHandle.
        /// Invalidated alongside GetOutput() by an extent change and by Configure; re-fetch after.
        [[nodiscard]] TextureHandle GetOutputHandle() const;

        /// @brief Returns the viewport's current region (placement + extent).
        [[nodiscard]] const ViewportRegion& GetRegion() const;

        /// @brief Returns the viewport's role.
        [[nodiscard]] ViewportRole GetRole() const;

        /// @brief Returns the owned renderer for its stats and diagnostic surface.
        ///
        /// The escape hatch for GetLastDrawnCount() and the rest of the renderer's read
        /// surface, rather than re-exporting every getter. A mutable reference by the
        /// Native-idiom rule: the wrapper's constness is its own identity, not the GPU state.
        [[nodiscard]] SceneRenderer& GetRenderer() const;

    private:
        explicit Viewport(const ViewportInfo& info);

        /// @brief Re-registers the output view into bindless, releasing the prior slot.
        ///
        /// Called at Create and after every internal Resize/Configure, so GetOutputHandle()
        /// always names the live output. The old slot retires through the per-frame window.
        void RefreshOutputHandle();

        /// @brief The Vulkan context, for bindless registration.
        Context& m_Context;
        /// @brief The owned deferred renderer.
        Unique<SceneRenderer> m_Renderer;
        /// @brief The viewport's window rectangle.
        ViewportRegion m_Region;
        /// @brief Whether the engine compositor places this viewport.
        ViewportRole m_Role;

        /// @brief The bound per-frame render source.
        ViewState m_ViewState;

        /// @brief Pending extent applied at the next Render; zero when none is pending.
        uvec2 m_PendingExtent = {};

        /// @brief Bindless slot naming the current output view; re-registered on resize/Configure.
        TextureHandle m_OutputHandle;
    };
}
