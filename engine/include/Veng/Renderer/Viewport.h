#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/Ray.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/DynamicResolution.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ViewportRegion.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Entity.h>

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
        /// @brief Environment map driving image-based lighting and the skybox; empty for none.
        AssetHandle<Environment> Environment;
        /// @brief Scales the IBL ambient + skybox radiance.
        f32 EnvironmentIntensity = 1.0f;
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
        /// The SceneRenderer is sized to round(Region.Extent * upper-bound-scale) — the static
        /// RenderScale here, or the dynamic-resolution MaxScale once SetDynamicResolution is called;
        /// the placement region is unchanged, so the compositor scales the result to fill it. (0,1]
        /// renders below the region and is upscaled; >1 supersamples. Uniform, so the render aspect
        /// matches the region. Must be > 0.
        f32 RenderScale = 1.0f;
        /// @brief Caps the allocation extent to this fraction of the region's pixels.
        ///
        /// A ceiling on the allocation relative to the region's backing extent: the SceneRenderer is
        /// allocated no larger than round(Region.Extent * MaxAllocationScale * upper-bound-scale). The
        /// default 1.0 allocates at the full region — on a 2× HiDPI display that is native resolution
        /// at the backing pixels, not supersampling. A value < 1.0 is a deliberate lower ceiling (a
        /// fixed perf budget); reclaiming footprint under sustained load is the allocation-tier outer
        /// loop's job, not this cap's. It is the outermost of the three multiplicative scales — the
        /// cap, then the upper-bound (MaxScale or static) allocation scale, then the per-frame
        /// sub-rect — so it bounds them. Must be > 0.
        f32 MaxAllocationScale = 1.0f;
        /// @brief Whether the engine compositor places this viewport into its region.
        ViewportRole Role = ViewportRole::Offscreen;
        /// @brief Renders only on a frame its owner pushed a fresh ViewState, instead of every frame.
        ///
        /// Off by default: the drive-list renders the viewport every frame regardless of input, the
        /// plug-and-play game path. On — set by a consumer whose source appears and disappears, an
        /// editor panel that hides when its dock tab is inactive — Render is a no-op on any frame no
        /// SetViewState landed since the last, so a hidden panel (whose draw, and thus its
        /// per-frame SetViewState, did not run) stops driving the renderer rather than re-rendering
        /// stale content into the shared bindless targets behind the visible panels.
        bool RenderOnDemand = false;
    };

    /// @brief A region of the window, a renderer, and a role: a renderable view into a world.
    ///
    /// Owns a SceneRenderer, carries a ViewportRegion (its window placement rectangle) and a
    /// RenderScale (its render target is the region extent times the upper-bound scale — the static
    /// scale, or the dynamic-resolution MaxScale — with the current scale rendering a sub-rect),
    /// takes a per-frame ViewState pushed by its owner, and on Render does the Execute +
    /// PrepareForAccess(Sample) pair itself. Its product is a sampleable Ref<ImageView> (GetOutput)
    /// and a bindless TextureHandle (GetOutputHandle).
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
        /// While dynamic resolution is enabled this is the current (per-frame) scale: the render
        /// target is sized to the controller's MaxScale, so a scale at or below it only adjusts the
        /// rendered sub-rect — no resize. With dynamic resolution off the scale is the allocation
        /// ceiling, so the target becomes round(GetRegion().Extent * scale) and a real change
        /// debounces an internal SceneRenderer::Resize to the next Render, invalidating
        /// GetOutput()/GetOutputHandle() (re-fetch after).
        /// @param scale  The multiplier; (0,1] reduces resolution, >1 supersamples.
        /// @pre scale > 0 — asserted otherwise.
        void SetRenderScale(f32 scale);

        /// @brief Returns the current render-resolution multiplier.
        [[nodiscard]] f32 GetRenderScale() const;

        /// @brief Returns the live allocation scale the render target is sized at.
        ///
        /// The fraction of the (capped) region the SceneRenderer is allocated to. With the
        /// allocation-tier controller enabled this is the current tier's scale (it follows the
        /// sustained sub-rect through StepAllocationTier); otherwise it is the static ceiling
        /// (MaxScale while dynamic resolution is on, else the static RenderScale). Reads with
        /// GetRenderScale() as "rendering at GetRenderScale() of an allocation that is this of the
        /// region."
        /// @return The allocation scale, > 0.
        [[nodiscard]] f32 GetAllocationScale() const;

        /// @brief Returns the live allocation-tier index, or 0 when the tier controller is disabled.
        ///
        /// The outer loop's quantized state: an index into the AllocationTierSettings::Tiers array
        /// (0 = baseline / full allocation, increasing = smaller). 0 when no tier controller is
        /// configured (the allocation is the static ceiling).
        /// @return The current tier index.
        [[nodiscard]] u32 GetAllocationTierIndex() const;

        /// @brief Returns the renderer's current allocation extent in pixels.
        ///
        /// round(GetRegion().Extent * MaxAllocationScale * GetAllocationScale()), clamped to ≥ {1,1}:
        /// the size every render-graph target is allocated at. The rendered sub-rect is this times
        /// GetRenderScale()/GetAllocationScale() (reported by SceneRenderer::GetValidExtent()).
        /// @return The allocation extent.
        [[nodiscard]] uvec2 GetAllocationExtent() const;

        /// @brief Enables automatic render-scale control from measured GPU frame time.
        ///
        /// Each Render the viewport reads Context::GetLastGpuFrameTimeMs() and eases its
        /// RenderScale toward the settings' frame budget (ComputeDynamicResolutionScale), rendering
        /// into a sub-rect of the target without a resize. The render target is sized to the
        /// allocation scale — by default MaxScale, the ceiling the inner loop can reach — so this
        /// call resizes the SceneRenderer images whenever that scale moves the integer extent
        /// (invalidating GetOutput()/GetOutputHandle(); re-fetch after), and a sub-1 allocation
        /// scale shrinks them rather than allocating full-region. The current scale is clamped into
        /// [MinScale, MaxScale]. The control is inert — the scale is left as set — when
        /// Context::IsGpuTimingSupported() is false. The whole-frame GPU time drives it, so it is
        /// meaningful for the dominant (primary) viewport; on a device with several heavy viewports
        /// the measurement is not attributed per viewport.
        ///
        /// Passing tierSettings additionally enables the outer-loop allocation-tier controller: the
        /// viewport folds its per-frame RenderScale into a long EMA and steps a quantized allocation
        /// tier (StepAllocationTier) after each inner-loop update, so the allocation follows the
        /// sustained sub-rect rather than sitting at MaxScale. A tier change debounces a
        /// SceneRenderer::Resize to the next Render. With tierSettings omitted the allocation is the
        /// static MaxScale (the prior behavior). The inner loop is sized so the sub-rect always fits
        /// inside the chosen tier, so a tier change is visually continuous (the rendered pixel count
        /// is preserved across the resize).
        /// @param settings      The inner-loop tuning (budget, scale bounds, deadband, rate limit).
        /// @param tierSettings  The outer-loop tier tuning; omitted leaves the allocation at MaxScale.
        void
        SetDynamicResolution(const DynamicResolutionSettings& settings,
                             const optional<AllocationTierSettings>& tierSettings = std::nullopt);

        /// @brief Disables automatic render-scale control, holding the current RenderScale.
        ///
        /// The allocation ceiling reverts from MaxScale to the held static scale, which may move the
        /// extent and debounce a resize (invalidating GetOutput()/GetOutputHandle(); re-fetch after).
        void ClearDynamicResolution();

        /// @brief Returns whether automatic render-scale control is enabled.
        [[nodiscard]] bool IsDynamicResolutionEnabled() const;

        /// @brief Returns whether the outer-loop allocation-tier controller is enabled.
        ///
        /// True when SetDynamicResolution was passed tier settings: the allocation follows the
        /// sustained sub-rect across quantized tiers. False when the allocation is the static
        /// MaxScale ceiling (the inner loop alone, or no controller at all).
        [[nodiscard]] bool IsAllocationTierEnabled() const;

        /// @brief Returns a counter bumped whenever the output view/handle is replaced.
        ///
        /// GetOutput()/GetOutputHandle() are invalidated by a resize (a region or render-scale
        /// change applied in Render, including an automatic dynamic-resolution adjustment) and by
        /// Configure. A consumer caching an ImGui texture or a material binding from the output
        /// compares this counter frame to frame to know when to re-fetch, rather than tracking
        /// every mutation itself. Monotonic for the viewport's lifetime.
        [[nodiscard]] u64 GetOutputGeneration() const;

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

        /// @brief Resolves the entity under a window point through the id-buffer picking pass.
        ///
        /// Hit-tests @p windowPoint against the region (WindowToViewport); on a miss the callback
        /// fires immediately with nullopt. On a hit it forwards the texel to the owned
        /// SceneRenderer's picking pass; the GPU readback resolves a frame or two later (never a
        /// stall), and @p onResolved fires from a later Render on the render thread with the picked
        /// Entity — the front-most mesh under the cursor (depth-tested), with a small screen-space
        /// search radius for "click-near" forgiveness — or nullopt over background.
        ///
        /// Because the resolve lands later, it carries a scene-epoch + scene-pointer guard: the
        /// callback fires with nullopt (rather than a wrong entity) if the bound scene was swapped or
        /// cleared between the click and the resolve. The picked slot index is mapped back to the
        /// live Entity at resolve time (Scene::GetLiveEntityAtIndex), so a recycled slot resolves to
        /// its live occupant or to none. A new Pick replaces any still-pending one.
        ///
        /// @pre The owned renderer was created with SceneRendererSettings::Picking set — otherwise
        ///      the picking pass never runs and the callback never fires. A ViewState with a live
        ///      World must be bound (the resolve maps the texel against it).
        /// @param windowPoint  The window-framebuffer-pixel point to pick.
        /// @param onResolved   Invoked once with the resolved Entity, or nullopt for background / a
        ///                     miss / a scene swap.
        void Pick(ivec2 windowPoint, function<void(optional<Entity>)> onResolved);

        /// @brief Returns the owned renderer for its stats and diagnostic surface.
        ///
        /// The escape hatch for GetLastDrawnCount() and the rest of the renderer's read
        /// surface, rather than re-exporting every getter. A mutable reference by the
        /// Native-idiom rule: the wrapper's constness is its own identity, not the GPU state.
        [[nodiscard]] SceneRenderer& GetRenderer() const;

        /// @brief Returns the owned renderer's immediate-mode debug-draw accumulator.
        ///
        /// The per-viewport channel for pushing debug lines/billboards: the accumulator and the
        /// depth target the flush pass tests against are per-renderer, so split-screen viewports
        /// each composite their own gizmos. Forwards to SceneRenderer::GetDebugDraw; the pass runs
        /// only when SceneRendererSettings::DebugDraw is enabled.
        /// @return The viewport renderer's DebugDraw accumulator.
        [[nodiscard]] DebugDraw& GetDebugDraw() const;

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

        /// @brief Advances an in-flight pick each Render: forward → poll → resolve, with the guard.
        ///
        /// Called at the end of Render. Bails (fires nullopt) if the bound scene changed since the
        /// pick was issued; otherwise forwards the texel to the renderer on the first frame, then
        /// polls the readback and, once ready, maps the pick id to a live Entity and fires the
        /// callback. A no-op when no pick is pending.
        void ServicePendingPick();

        /// @brief The renderer's allocation extent for a given scale, capped and clamped to ≥ 1.
        ///
        /// @param scale  The render-resolution multiplier to apply.
        /// @return round(m_Region.Extent * m_MaxAllocationScale * scale), never below {1,1}.
        [[nodiscard]] uvec2 ExtentForScale(f32 scale) const;

        /// @brief The renderer's allocation extent at the current allocation scale, clamped to ≥ 1.
        ///
        /// @return ExtentForScale(GetAllocationScale()).
        [[nodiscard]] uvec2 ScaledExtent() const;

        /// @brief The per-frame render fraction of the allocation pushed into SceneView::RenderScale.
        ///
        /// m_RenderScale / GetAllocationScale(): the fraction of the (allocation-sized) target to
        /// render this frame. At the allocation scale it is 1 (renders the full target); below it a
        /// sub-rect. Clamped to ≤ 1 against a transient current-scale-above-ceiling window.
        /// @return The sub-rect fraction in (0, 1].
        [[nodiscard]] f32 GetViewRenderScale() const;

        /// @brief Debounces a SceneRenderer::Resize to the next Render when the allocation moved.
        ///
        /// Compares the allocation extent computed after a scale/bound change against the caller's
        /// captured prior extent; on a real change (and a non-zero region) it sets m_PendingExtent.
        /// @param priorAlloc  The allocation extent captured before the change.
        void DebounceAllocationResize(uvec2 priorAlloc);

        /// @brief Advances the render scale from measured GPU frame time when control is enabled.
        ///
        /// Called at the top of Render. Reads Context::GetLastGpuFrameTimeMs(), computes the next
        /// scale, and debounces a resize (via SetRenderScale) only when the integer render extent
        /// would change — so a steady-state micro-adjustment within a pixel never resizes. A no-op
        /// when control is disabled or timing is unsupported.
        void UpdateDynamicResolution();

        /// @brief Steps the allocation tier from the live RenderScale when the tier controller is on.
        ///
        /// Called in Render after UpdateDynamicResolution, so the outer loop reads the inner loop's
        /// freshly-updated sub-rect scale. Folds m_RenderScale into the long EMA and advances the
        /// tier via StepAllocationTier; on a tier-index change it debounces a SceneRenderer::Resize
        /// to the next Render (the allocation now follows the sustained sub-rect). A no-op when no
        /// tier controller is configured.
        /// @param delta  The frame delta in seconds, from the bound ViewState.
        void UpdateAllocationTier(f32 delta);

        /// @brief The Vulkan context, for bindless registration.
        Context& m_Context;
        /// @brief The owned deferred renderer.
        Unique<SceneRenderer> m_Renderer;
        /// @brief The viewport's window placement rectangle.
        ViewportRegion m_Region;
        /// @brief Uniform render-resolution multiplier on the region extent.
        f32 m_RenderScale = 1.0f;
        /// @brief Ceiling on the allocation extent as a fraction of the region's backing pixels.
        f32 m_MaxAllocationScale = 1.0f;
        /// @brief Automatic render-scale controller tuning; unset when control is disabled.
        optional<DynamicResolutionSettings> m_DynamicResolution;
        /// @brief Outer-loop allocation-tier controller tuning; unset when the tier controller is off.
        ///
        /// Set only alongside m_DynamicResolution (the inner loop feeds the outer loop). When unset
        /// the allocation is the static MaxScale; when set GetAllocationScale() is the live tier.
        optional<AllocationTierSettings> m_TierSettings;
        /// @brief Caller-held state for the allocation-tier controller; meaningful only with m_TierSettings.
        AllocationTierState m_TierState;
        /// @brief Whether the engine compositor places this viewport.
        ViewportRole m_Role;

        /// @brief Whether Render is gated on a fresh per-frame ViewState (see ViewportInfo).
        bool m_RenderOnDemand = false;

        /// @brief Set by SetViewState, cleared by Render: a ViewState landed since the last render.
        ///
        /// Only read when m_RenderOnDemand is set, where it skips a render for a frame whose owner
        /// pushed no source (a hidden editor panel that did not draw).
        bool m_ViewStateFresh = false;

        /// @brief The bound per-frame render source.
        ViewState m_ViewState;

        /// @brief True once SetViewState has bound a render source.
        ///
        /// Gates ScreenToWorldRay: before any ViewState the retained camera is the default,
        /// so picking returns nullopt rather than unprojecting through an unset view.
        bool m_HasViewState = false;

        /// @brief Monotonic epoch bumped whenever the bound ViewState World pointer changes.
        ///
        /// A pick captures this at issue time; the resolve bails (fires nullopt) if it no longer
        /// matches, so a late readback never lands an id against a scene that was swapped (a Play
        /// Clone()) or cleared between the click and the resolve.
        u64 m_SceneEpoch = 0;

        /// @brief A pick request awaiting service + readback, with the guard captured at issue.
        struct PendingPick
        {
            /// @brief The render-target texel (allocation pixels) to resolve.
            uvec2 Texel;
            /// @brief Whether the texel has been forwarded to the renderer's RequestPick yet.
            bool Forwarded = false;
            /// @brief The scene the pick was issued against (pointer-identity guard).
            const Scene* World = nullptr;
            /// @brief The scene epoch captured at issue (matches m_SceneEpoch only if unchanged).
            u64 Epoch = 0;
            /// @brief The callback fired once on resolve.
            function<void(optional<Entity>)> OnResolved;
        };
        /// @brief The single in-flight pick, or unset when none is pending.
        optional<PendingPick> m_PendingPick;

        /// @brief Pending extent applied at the next Render; zero when none is pending.
        uvec2 m_PendingExtent = {};

        /// @brief Bindless slot naming the current output view; re-registered on resize/Configure.
        TextureHandle m_OutputHandle;

        /// @brief Bumped each time the output view/handle is replaced; read via GetOutputGeneration.
        u64 m_OutputGeneration = 0;

        /// @brief The drive-list this viewport is registered into; null when unregistered.
        ///
        /// Set by AttachToDriveList; ~Viewport erases this viewport's pointer from it.
        vector<Viewport*>* m_DriveList = nullptr;
    };
}
