#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/Ray.h>
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
        /// @brief Initial placement region; its Extent times RenderScale sizes the render target.
        ViewportRegion Region;
        /// @brief Output target format; resolved to Context::GetOutputFormat() when Undefined.
        Format ColorFormat = Format::Undefined;
        /// @brief Initial topology and sizing knobs for the owned SceneRenderer.
        SceneRendererSettings Settings;
        /// @brief Uniform render-resolution multiplier on the region extent.
        ///
        /// The SceneRenderer is sized to round(Region.Extent * RenderScale); the placement region
        /// is unchanged, so the compositor scales the result to fill it. (0,1] renders below the
        /// region for dynamic resolution scaling and is upscaled; >1 supersamples. Uniform, so the
        /// render aspect matches the region. Must be > 0.
        f32 RenderScale = 1.0f;
        /// @brief Whether the engine compositor places this viewport into its region.
        ViewportRole Role = ViewportRole::Offscreen;
    };

    /// @brief A region of the window, a renderer, and a role: a renderable view into a world.
    ///
    /// Owns a SceneRenderer, carries a ViewportRegion (its window placement rectangle) and a
    /// RenderScale (its render target is the region extent times the scale), takes a per-frame
    /// ViewState pushed by its owner, and on Render does the Execute + PrepareForAccess(Sample)
    /// pair itself. Its product is a sampleable Ref<ImageView> (GetOutput) and a bindless
    /// TextureHandle (GetOutputHandle).
    ///
    /// Single-owner (Unique); Create is the factory. A region or settings change invalidates
    /// GetOutput()/GetOutputHandle() exactly as the underlying SceneRenderer's Resize/Configure
    /// does — re-fetch both after SetRegion (on an extent change) and after Configure.
    class Viewport
    {
    public:
        /// @brief Creates a Viewport owning a fresh SceneRenderer sized to the region's extent × RenderScale.
        ///
        /// A ColorFormat left Undefined resolves to Context::GetOutputFormat(). The viewport is
        /// constructed unregistered: it is driveable on its own (call Render directly) until an
        /// Application::RegisterViewport hands it a drive-list back-reference.
        /// @param info  Construction parameters.
        /// @return The owning Unique.
        static Unique<Viewport> Create(const ViewportInfo& info);

        /// @brief Releases the output's bindless slot and self-unregisters from its drive-list.
        ///
        /// If this viewport was registered (Application::RegisterViewport), it removes its own
        /// pointer from the drive-list via the stored back-reference — an order-preserving erase,
        /// so dropping the owning Unique is the whole of cleanup with no explicit unregister.
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

        /// @brief Sets the uniform render-resolution multiplier on the region extent.
        ///
        /// The render target becomes round(GetRegion().Extent * scale) while the placement region
        /// is unchanged; the compositor scales the result to fill the region. A real change
        /// debounces an internal SceneRenderer::Resize to the next Render, invalidating
        /// GetOutput()/GetOutputHandle() (re-fetch after). Drives dynamic resolution scaling.
        /// @param scale  The multiplier; (0,1] reduces resolution, >1 supersamples.
        /// @pre scale > 0 — asserted otherwise.
        void SetRenderScale(f32 scale);

        /// @brief Returns the current render-resolution multiplier.
        [[nodiscard]] f32 GetRenderScale() const;

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
        /// @warning A consumer viewport sampling this handle (e.g. a material bound through
        ///          Material::SetTextureHandle) must be registered after the producer:
        ///          registration order is render order, so a producer registered first ends its
        ///          Render with the output in Sample layout before the consumer's Render reads it.
        ///          Both halves record on the single graphics queue in submission order, so the
        ///          handoff needs no ring and no semaphore and the output stays single-copy;
        ///          the producer's next-frame Execute transitions it back to ColorAttachment.
        /// @see SceneRenderer::GetOutput for the frames-in-flight / single-copy output contract.
        [[nodiscard]] TextureHandle GetOutputHandle() const;

        /// @brief Returns the viewport's current region (placement + extent).
        [[nodiscard]] const ViewportRegion& GetRegion() const;

        /// @brief Returns the viewport's role.
        [[nodiscard]] ViewportRole GetRole() const;

        /// @brief Maps a window point into this viewport's region as normalized coordinates.
        ///
        /// Hit-tests windowPoint (window framebuffer pixels) against GetRegion(); on a hit,
        /// remaps it to normalized [0,1] coordinates across the region — (0,0) is the region's
        /// top-left, (1,1) its bottom-right — independent of the region's window offset. The
        /// pointer-to-viewport seam: a router hit-tests a click against each registered
        /// viewport's region to find the one it belongs to, and the editor uses the fraction
        /// for hover/pick coordinates. Gameplay-agnostic — the viewport imports no Viewer or
        /// PlayerInput; the viewport-to-seat association lives in the router/app.
        /// @param windowPoint  The point to test, in window framebuffer pixels.
        /// @return The point's normalized [0,1] position within the region, or nullopt when it
        ///         lies outside the region (or the region has a zero extent).
        [[nodiscard]] optional<vec2> WindowToViewport(ivec2 windowPoint) const;

        /// @brief Unprojects a window point into a world-space ray through the retained camera.
        ///
        /// Composes WindowToViewport with the camera retained from the last SetViewState: maps
        /// the [0,1] point to NDC and unprojects it through glm::inverse(camera.ViewProjection())
        /// into a world-space Ray whose origin is the camera and whose normalized direction passes
        /// through the pixel. Self-contained — the viewport already holds the camera, so picking
        /// needs no external CameraView. The viewport supplies the ray; what the ray hits (a scene
        /// raycast) is editor or gameplay code, not the viewport.
        /// @param windowPoint  The point to unproject, in window framebuffer pixels.
        /// @return The world-space ray through windowPoint, or nullopt when the point lies outside
        ///         the region or no ViewState has been set yet.
        [[nodiscard]] optional<Ray> ScreenToWorldRay(ivec2 windowPoint) const;

        /// @brief Returns the owned renderer for its stats and diagnostic surface.
        ///
        /// The escape hatch for GetLastDrawnCount() and the rest of the renderer's read
        /// surface, rather than re-exporting every getter. A mutable reference by the
        /// Native-idiom rule: the wrapper's constness is its own identity, not the GPU state.
        [[nodiscard]] SceneRenderer& GetRenderer() const;

        /// @brief Binds this viewport to a drive-list it self-unregisters from on destruction.
        ///
        /// Application::RegisterViewport calls this with its drive-list after appending this
        /// viewport's pointer; ~Viewport then erases that pointer, order-preserving. Registering
        /// an already-registered viewport is a fatal assert (the back-reference would leak the
        /// prior membership).
        /// @param driveList  The Application drive-list this viewport now belongs to.
        /// @pre This viewport is not already attached to a drive-list.
        void AttachToDriveList(vector<Viewport*>& driveList);

    private:
        explicit Viewport(const ViewportInfo& info);

        /// @brief Re-registers the output view into bindless, releasing the prior slot.
        ///
        /// Called at Create and after every internal Resize/Configure, so GetOutputHandle()
        /// always names the live output. The old slot retires through the per-frame window.
        void RefreshOutputHandle();

        /// @brief The render-target extent: the region extent scaled by RenderScale, clamped to ≥ 1.
        ///
        /// @return round(m_Region.Extent * m_RenderScale), never below {1,1}.
        [[nodiscard]] uvec2 ScaledExtent() const;

        /// @brief The Vulkan context, for bindless registration.
        Context& m_Context;
        /// @brief The owned deferred renderer.
        Unique<SceneRenderer> m_Renderer;
        /// @brief The viewport's window placement rectangle.
        ViewportRegion m_Region;
        /// @brief Uniform render-resolution multiplier on the region extent.
        f32 m_RenderScale = 1.0f;
        /// @brief Whether the engine compositor places this viewport.
        ViewportRole m_Role;

        /// @brief The bound per-frame render source.
        ViewState m_ViewState;

        /// @brief True once SetViewState has bound a render source.
        ///
        /// Gates ScreenToWorldRay: before any ViewState the retained camera is the default,
        /// so picking returns nullopt rather than unprojecting through an unset view.
        bool m_HasViewState = false;

        /// @brief Pending extent applied at the next Render; zero when none is pending.
        uvec2 m_PendingExtent = {};

        /// @brief Bindless slot naming the current output view; re-registered on resize/Configure.
        TextureHandle m_OutputHandle;

        /// @brief The drive-list this viewport is registered into; null when unregistered.
        ///
        /// Set by AttachToDriveList; ~Viewport erases this viewport's pointer from it.
        vector<Viewport*>* m_DriveList = nullptr;
    };
}
